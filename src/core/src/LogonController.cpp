#include "xplogin/LogonController.h"

#include "xplogin/CredentialPacker.h"
#include "xplogin/SigninOptions.h"
#include "xplogin/Logging.h"
#include "xplogin/StringUtil.h"

namespace xplogin {

LogonController::LogonController(ControllerDependencies deps, AppConfig config)
    : deps_(std::move(deps)),
      config_(std::move(config)),
      directory_(deps_.userEnumerator, config_.users) {}

LogonController::~LogonController() {
    SecureClear(password_);
}

bool LogonController::Initialize(UsageScenario scenario) {
    scenario_ = scenario;
    // LogonUI calls SetUsageScenario again on every screensaver timeout and
    // every return from the shutdown dialog. Anything typed before that point
    // must not survive into the new screen.
    SecureClear(password_);
    machine_.Handle(UiEvent::Reset);

    StateMachinePolicy policy;
    policy.scenario = scenario;
    policy.allowShutdown = config_.allowShutdown && scenario != UsageScenario::CredUI;
    // During an unlock XP offered no other account: only the owner of the
    // locked session may come back in.
    policy.allowUserSwitch = scenario != UsageScenario::UnlockWorkstation;
    policy.maxFailedAttempts = config_.maxFailedAttempts;
    machine_.SetPolicy(policy);

    RefreshUsers();

    if (scenario == UsageScenario::UnlockWorkstation) {
        int index = -1;
        if (!lockedUserSid_.empty()) {
            index = directory_.IndexOfSid(lockedUserSid_);
        }
        if (index < 0 && !lockedUserName_.empty()) {
            index = directory_.IndexOfUser(lockedUserName_);
        }
        if (index >= 0) {
            // Show only the locking account, exactly like XP's unlock screen.
            std::vector<UserAccount> single{directory_.Users()[static_cast<size_t>(index)]};
            machine_.SetUsers(single);
            machine_.PreselectUser(0);
        }
    }

    const UiState previous = machine_.State();
    machine_.Handle(UiEvent::Show);
    ApplyStateChange(previous);

    initialized_ = true;
    XPLOG_INFO("controller initialized: scenario=%d users=%d",
               static_cast<int>(scenario), static_cast<int>(machine_.Users().size()));
    return true;
}

void LogonController::SetLockedUserSid(const std::wstring& sid) {
    lockedUserSid_ = sid;
}

void LogonController::SetLockedUserName(const std::wstring& qualifiedName) {
    lockedUserName_ = qualifiedName;
}

void LogonController::Shutdown() {
    SecureClear(password_);
    machine_.Handle(UiEvent::Hide);
    initialized_ = false;
}

void LogonController::SetUserEnumerator(std::shared_ptr<IUserEnumerator> enumerator) {
    if (!enumerator) {
        return;
    }
    deps_.userEnumerator = std::move(enumerator);
    directory_ = UserDirectory(deps_.userEnumerator, config_.users);
}

void LogonController::RefreshUsers() {
    directory_.Refresh();
    if (scenario_ != UsageScenario::UnlockWorkstation) {
        machine_.SetUsers(directory_.Users());
    }
    if (observer_) {
        observer_->OnUsersChanged();
    }
}

void LogonController::ApplyStateChange(UiState previous) {
    const UiState current = machine_.State();
    if (previous != current && observer_) {
        observer_->OnStateChanged(previous, current);
    }
}

TransitionResult LogonController::SelectUser(int index) {
    const UiState previous = machine_.State();
    TransitionResult result = machine_.HandleSelectUser(index);
    if (result == TransitionResult::Accepted) {
        SecureClear(password_);
        if (deps_.soundPlayer && config_.sounds.enabled) {
            deps_.soundPlayer->Play(SoundEvent::Click);
        }
    }
    ApplyStateChange(previous);
    return result;
}

TransitionResult LogonController::CancelSelection() {
    const UiState previous = machine_.State();
    SecureClear(password_);
    TransitionResult result = machine_.Handle(UiEvent::CancelSelection);
    ApplyStateChange(previous);
    return result;
}

TransitionResult LogonController::CancelAuthentication() {
    const UiState previous = machine_.State();
    SecureClear(password_);
    if (previous != UiState::Authenticating) {
        return TransitionResult::IgnoredWrongState;
    }
    XPLOG_INFO("abandoning a submission that never came back");
    TransitionResult result = machine_.Handle(UiEvent::AuthCancelled);
    ApplyStateChange(previous);
    return result;
}

void LogonController::SetPassword(const std::wstring& password) {
    SecureClear(password_);
    password_ = password;
    // Typing dismisses the previous error, like XP's shake-then-retry.
    if (machine_.State() == UiState::AuthFailed) {
        const UiState previous = machine_.State();
        machine_.Handle(UiEvent::DismissError);
        ApplyStateChange(previous);
    }
}

bool LogonController::SignsInAutomatically() const {
    return config_.users.autoLogonBlankPassword &&
           scenario_ == UsageScenario::Logon &&
           SignsInWithoutAsking(machine_.Users());
}

LogonRequest LogonController::CurrentRequest() const {
    LogonRequest request;
    request.scenario = scenario_;
    const UserAccount* user = machine_.SelectedUser();
    if (user) {
        request.username = user->username;
        request.domain = user->domain;
    }
    request.password = password_;
    return request;
}

TransitionResult LogonController::Submit() {
    const UserAccount* user = machine_.SelectedUser();
    if (!user) {
        return TransitionResult::RejectedNoSelection;
    }
    // XP refused an empty password unless the account genuinely had none.
    if (password_.empty() && !user->blankPassword &&
        !config_.users.autoLogonBlankPassword) {
        // Fall through: LSA is the authority on whether a blank password works,
        // and refusing here would leak which accounts have no password. The
        // policy flag only exists so a deployment can be stricter.
    }

    const UiState previous = machine_.State();
    TransitionResult result = machine_.Handle(UiEvent::SubmitCredentials);
    if (result != TransitionResult::Accepted) {
        ApplyStateChange(previous);
        return result;
    }
    ApplyStateChange(previous);

    if (!deps_.authenticator) {
        OnAuthReported(AuthResult::Fail(AuthStatus::InternalError,
                                        L"No authentication provider is available."));
        return TransitionResult::Accepted;
    }

    AuthResult authResult = deps_.authenticator->Authenticate(CurrentRequest());
    if (authResult.status == AuthStatus::Pending) {
        // The credential provider path: LogonUI takes the blob from
        // BuildSerialization() and calls back through ReportResult.
        return TransitionResult::Accepted;
    }
    OnAuthReported(authResult);
    return TransitionResult::Accepted;
}

void LogonController::OnAuthReported(const AuthResult& result) {
    const UiState previous = machine_.State();
    machine_.HandleAuthResult(result);
    ApplyStateChange(previous);

    if (result.Ok()) {
        CompleteLogon();
        return;
    }

    if (result.status != AuthStatus::Cancelled) {
        SecureClear(password_);
        if (deps_.soundPlayer && config_.sounds.enabled) {
            deps_.soundPlayer->Play(SoundEvent::Error);
        }
        if (observer_) {
            observer_->OnAuthError(result);
        }
        XPLOG_INFO("authentication rejected: status=%d nt=0x%08x",
                   static_cast<int>(result.status),
                   static_cast<unsigned>(result.ntStatus));
    }
}

void LogonController::CompleteLogon() {
    const UserAccount* user = machine_.SelectedUser();
    SecureClear(password_);

    if (deps_.soundPlayer && config_.sounds.enabled) {
        deps_.soundPlayer->Play(SoundEvent::Logon);
    }

    // Only a host that owns the logon may do this, and the credential provider
    // is not one: under LogonUI, Winlogon connects the session, and doing it a
    // second time from here breaks the sign-in it is in the middle of.
    // MakeWin32Dependencies therefore leaves sessionManager null on purpose -
    // the comment there has the full account. This branch is for a standalone
    // host that really is the thing performing the logon. Starting the shell
    // is not among the things it may do; see ISessionManager.
    if (user && deps_.sessionManager) {
        uint32_t sessionId =
            deps_.sessionManager->FindSessionForUser(user->domain, user->username);
        if (sessionId != 0) {
            // Fast user switching: reattach instead of building a new session.
            deps_.sessionManager->ConnectSession(sessionId);
        } else {
            sessionId = deps_.sessionManager->CurrentSessionId();
        }
        logonSessionId_ = sessionId;
    }

    if (deps_.stateStore && deps_.clock) {
        HealthMonitor health(deps_.stateStore, deps_.clock, config_.health);
        health.ReportSuccessfulLogon();
    }

    if (observer_ && user) {
        observer_->OnLogonComplete(*user, logonSessionId_);
    }
    XPLOG_INFO("logon complete, session=%u", static_cast<unsigned>(logonSessionId_));
}

TransitionResult LogonController::OpenTurnOffDialog() {
    const UiState previous = machine_.State();
    TransitionResult result = machine_.Handle(UiEvent::OpenTurnOffDialog);
    ApplyStateChange(previous);
    return result;
}

TransitionResult LogonController::CloseTurnOffDialog() {
    const UiState previous = machine_.State();
    TransitionResult result = machine_.Handle(UiEvent::CloseTurnOffDialog);
    ApplyStateChange(previous);
    return result;
}

TransitionResult LogonController::ChoosePowerAction(PowerAction action) {
    if (action != PowerAction::None && deps_.powerController &&
        !deps_.powerController->IsActionAvailable(action)) {
        return TransitionResult::RejectedShutdownNotAllowed;
    }

    const UiState previous = machine_.State();
    TransitionResult result = machine_.HandlePowerChoice(action);
    ApplyStateChange(previous);
    if (result != TransitionResult::Accepted || action == PowerAction::None) {
        return result;
    }

    if (deps_.soundPlayer && config_.sounds.enabled &&
        (action == PowerAction::TurnOff || action == PowerAction::Restart)) {
        deps_.soundPlayer->Play(SoundEvent::Shutdown);
    }
    if (observer_) {
        observer_->OnPowerAction(action);
    }

    // The screen, before Windows is told anything.
    //
    // Everything above this line has already happened in memory - the state is
    // PowerActionPending, so the account list and the turn-off dialog are gone
    // from the layout and the XP shell-status line has taken their place. None
    // of that is on the glass until somebody paints it, and after the next line
    // this thread may never get another chance: Execute hands the machine to
    // Windows, LogonUI is torn down with the session, and a queued WM_PAINT is
    // simply never reached.
    //
    // That gap is what a shutdown from the sign-in screen actually looked like:
    // Windows' spinner, and then the XP screen with the turn-off dialog still
    // open on it - the last frame painted before the click - held there for the
    // several seconds until the power went.
    if (presentPowerAction_) {
        presentPowerAction_();
    }

    if (deps_.powerController) {
        deps_.powerController->Execute(action);
    }
    machine_.Handle(UiEvent::PowerActionStarted);
    return TransitionResult::Accepted;
}

bool LogonController::BuildSerialization(uint64_t logonId, size_t pointerSize,
                                         PackedCredential* out) const {
    if (!machine_.CanSerialize() || !out) {
        return false;
    }
    LogonRequest request = CurrentRequest();
    return PackKerbInteractiveUnlockLogon(request, logonId, pointerSize, out) ==
           PackStatus::Ok;
}

} // namespace xplogin
