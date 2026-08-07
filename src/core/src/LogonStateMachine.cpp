#include "xplogin/LogonStateMachine.h"

namespace xplogin {

LogonStateMachine::LogonStateMachine() {
    snapshot_.state = UiState::Hidden;
    RefreshDerivedFlags();
}

void LogonStateMachine::SetPolicy(const StateMachinePolicy& policy) {
    policy_ = policy;
    RefreshDerivedFlags();
}

void LogonStateMachine::SetUsers(const std::vector<UserAccount>& users) {
    users_ = users;
    if (snapshot_.selectedUser >= static_cast<int>(users_.size())) {
        snapshot_.selectedUser = -1;
        if (snapshot_.state == UiState::PasswordEntry ||
            snapshot_.state == UiState::AuthFailed) {
            EnterState(UiState::UserList);
        }
    }
    RefreshDerivedFlags();
}

void LogonStateMachine::PreselectUser(int index) {
    if (index >= 0 && index < static_cast<int>(users_.size())) {
        snapshot_.selectedUser = index;
    }
    RefreshDerivedFlags();
}

const UserAccount* LogonStateMachine::SelectedUser() const {
    if (snapshot_.selectedUser < 0 ||
        snapshot_.selectedUser >= static_cast<int>(users_.size())) {
        return nullptr;
    }
    return &users_[static_cast<size_t>(snapshot_.selectedUser)];
}

bool LogonStateMachine::CanSerialize() const {
    return snapshot_.state == UiState::Authenticating && SelectedUser() != nullptr;
}

void LogonStateMachine::EnterState(UiState state) {
    snapshot_.state = state;
    RefreshDerivedFlags();
}

void LogonStateMachine::RefreshDerivedFlags() {
    const UiState s = snapshot_.state;

    snapshot_.passwordBoxVisible =
        (s == UiState::PasswordEntry || s == UiState::Authenticating ||
         s == UiState::AuthFailed);

    // XP showed the green "back" arrow only when there was something to go back
    // to. During an unlock there is exactly one account, so there isn't.
    snapshot_.backButtonVisible =
        snapshot_.passwordBoxVisible && policy_.allowUserSwitch && users_.size() > 0;

    snapshot_.turnOffButtonEnabled =
        policy_.allowShutdown && s != UiState::Authenticating &&
        s != UiState::PowerActionPending && s != UiState::LoggedOn;

    snapshot_.busy =
        (s == UiState::Authenticating || s == UiState::PowerActionPending);

    switch (s) {
        case UiState::UserList:
            // No text of its own. The account list's instruction line is a
            // *theme* string (XpTheme::textClickUserName), not a status: a
            // literal here would win over the theme and could not be changed
            // or switched off without a rebuild. An error message that brought
            // us back to the list stays, which is why this only clears after a
            // clean start.
            if (snapshot_.lastAuthStatus == AuthStatus::Success) {
                snapshot_.statusText.clear();
            }
            break;
        case UiState::PasswordEntry:
            snapshot_.statusText = L"Type your password";
            break;
        case UiState::Authenticating:
            snapshot_.statusText = L"Loading your personal settings...";
            break;
        case UiState::TurnOffDialog:
            snapshot_.statusText = L"Turn off computer";
            break;
        case UiState::LoggedOn:
            // Deliberately keeps whatever Authenticating put there, which is
            // "Loading your personal settings..." - XP's welcome text, and the
            // thing the screen should be saying while Winlogon builds the
            // session. Spelled out rather than left to `default` because it is
            // a decision, not an omission.
            break;
        default:
            break;
    }
}

std::wstring LogonStateMachine::MessageForStatus(AuthStatus status) {
    switch (status) {
        case AuthStatus::BadPassword:
            return L"Did you forget your password?\r\n"
                   L"You can click the \"?\" button to see your password hint.";
        case AuthStatus::UnknownUser:
            return L"The user name you typed does not exist on this computer.";
        case AuthStatus::AccountDisabled:
            return L"Your account has been disabled. Please see your system "
                   L"administrator.";
        case AuthStatus::AccountLockedOut:
            return L"Your account has been locked out. Please see your system "
                   L"administrator.";
        case AuthStatus::PasswordExpired:
        case AuthStatus::PasswordMustChange:
            return L"Your password has expired and must be changed.";
        case AuthStatus::LogonTypeNotGranted:
            return L"You cannot log on interactively with this account.";
        case AuthStatus::TimeRestriction:
            return L"Your account has time restrictions that prevent you from "
                   L"logging on at this time.";
        case AuthStatus::Cancelled:
            return L"";
        case AuthStatus::Success:
            return L"";
        case AuthStatus::Pending:
        case AuthStatus::InternalError:
        default:
            return L"The system could not log you on. Please try again.";
    }
}

TransitionResult LogonStateMachine::HandleSelectUser(int index) {
    if (snapshot_.state != UiState::UserList &&
        snapshot_.state != UiState::PasswordEntry &&
        snapshot_.state != UiState::AuthFailed) {
        return TransitionResult::IgnoredWrongState;
    }
    if (index < 0 || index >= static_cast<int>(users_.size())) {
        return TransitionResult::RejectedInvalidUser;
    }

    const UserAccount& account = users_[static_cast<size_t>(index)];
    if (!account.CanLogOn()) {
        // XP still moved the highlight but refused to accept a password.
        snapshot_.selectedUser = index;
        snapshot_.lastAuthStatus = account.lockedOut ? AuthStatus::AccountLockedOut
                                                     : AuthStatus::AccountDisabled;
        snapshot_.statusText = MessageForStatus(snapshot_.lastAuthStatus);
        EnterState(UiState::UserList);
        return TransitionResult::RejectedAccountUnusable;
    }

    // Switching to a different tile clears the previous error and attempt count.
    if (snapshot_.selectedUser != index) {
        snapshot_.failedAttempts = 0;
        snapshot_.lastAuthStatus = AuthStatus::Success;
        snapshot_.statusText.clear();
    }
    snapshot_.selectedUser = index;
    EnterState(UiState::PasswordEntry);
    return TransitionResult::Accepted;
}

TransitionResult LogonStateMachine::HandlePowerChoice(PowerAction action) {
    if (snapshot_.state != UiState::TurnOffDialog) {
        return TransitionResult::IgnoredWrongState;
    }
    if (!policy_.allowShutdown) {
        return TransitionResult::RejectedShutdownNotAllowed;
    }
    if (action == PowerAction::None) {
        EnterState(stateBeforeTurnOff_);
        return TransitionResult::Accepted;
    }
    snapshot_.pendingAction = action;
    EnterState(UiState::PowerActionPending);
    return TransitionResult::Accepted;
}

TransitionResult LogonStateMachine::HandleAuthResult(const AuthResult& result) {
    if (snapshot_.state != UiState::Authenticating) {
        return TransitionResult::IgnoredWrongState;
    }

    if (result.Ok()) {
        snapshot_.lastAuthStatus = AuthStatus::Success;
        snapshot_.failedAttempts = 0;
        snapshot_.statusText.clear();
        EnterState(UiState::LoggedOn);
        return TransitionResult::Accepted;
    }

    if (result.status == AuthStatus::Cancelled) {
        snapshot_.lastAuthStatus = AuthStatus::Cancelled;
        EnterState(UiState::PasswordEntry);
        return TransitionResult::Accepted;
    }

    snapshot_.lastAuthStatus = result.status;
    ++snapshot_.failedAttempts;
    snapshot_.statusText =
        result.message.empty() ? MessageForStatus(result.status) : result.message;
    EnterState(UiState::AuthFailed);

    // Too many misses: XP dropped back to the tile list so the next person
    // could try. LSA's own lockout policy is unaffected by this.
    if (policy_.maxFailedAttempts > 0 &&
        snapshot_.failedAttempts >= policy_.maxFailedAttempts) {
        snapshot_.selectedUser = -1;
        snapshot_.failedAttempts = 0;
        EnterState(UiState::UserList);
    }
    return TransitionResult::Accepted;
}

TransitionResult LogonStateMachine::Handle(UiEvent event) {
    switch (event) {
        case UiEvent::Show:
            if (snapshot_.state != UiState::Hidden) {
                return TransitionResult::IgnoredWrongState;
            }
            // An unlock arrives with the locked account already chosen: XP went
            // straight to the password box for it.
            if (policy_.scenario == UsageScenario::UnlockWorkstation &&
                snapshot_.selectedUser >= 0) {
                EnterState(UiState::PasswordEntry);
            } else {
                EnterState(UiState::UserList);
            }
            return TransitionResult::Accepted;

        case UiEvent::Hide:
            EnterState(UiState::Hidden);
            return TransitionResult::Accepted;

        case UiEvent::Reset:
            snapshot_ = UiSnapshot{};
            snapshot_.state = UiState::Hidden;
            stateBeforeTurnOff_ = UiState::UserList;
            RefreshDerivedFlags();
            return TransitionResult::Accepted;

        case UiEvent::CancelSelection:
            if (snapshot_.state != UiState::PasswordEntry &&
                snapshot_.state != UiState::AuthFailed) {
                return TransitionResult::IgnoredWrongState;
            }
            if (!policy_.allowUserSwitch) {
                return TransitionResult::RejectedSwitchNotAllowed;
            }
            snapshot_.selectedUser = -1;
            snapshot_.failedAttempts = 0;
            snapshot_.lastAuthStatus = AuthStatus::Success;
            snapshot_.statusText.clear();
            EnterState(UiState::UserList);
            return TransitionResult::Accepted;

        case UiEvent::SubmitCredentials: {
            if (snapshot_.state != UiState::PasswordEntry &&
                snapshot_.state != UiState::AuthFailed) {
                return TransitionResult::IgnoredWrongState;
            }
            const UserAccount* user = SelectedUser();
            if (!user) {
                return TransitionResult::RejectedNoSelection;
            }
            if (!user->CanLogOn()) {
                return TransitionResult::RejectedAccountUnusable;
            }
            EnterState(UiState::Authenticating);
            return TransitionResult::Accepted;
        }

        case UiEvent::AuthSucceeded:
            return HandleAuthResult(AuthResult::Success());

        case UiEvent::AuthFailedEvent:
            return HandleAuthResult(AuthResult::Fail(AuthStatus::BadPassword));

        case UiEvent::AuthCancelled:
            return HandleAuthResult(AuthResult::Fail(AuthStatus::Cancelled));

        case UiEvent::DismissError:
            if (snapshot_.state != UiState::AuthFailed) {
                return TransitionResult::IgnoredWrongState;
            }
            snapshot_.statusText.clear();
            EnterState(UiState::PasswordEntry);
            return TransitionResult::Accepted;

        case UiEvent::OpenTurnOffDialog:
            if (snapshot_.state == UiState::Authenticating ||
                snapshot_.state == UiState::PowerActionPending ||
                snapshot_.state == UiState::LoggedOn ||
                snapshot_.state == UiState::Hidden) {
                return TransitionResult::RejectedBusy;
            }
            if (!policy_.allowShutdown) {
                return TransitionResult::RejectedShutdownNotAllowed;
            }
            stateBeforeTurnOff_ = snapshot_.state;
            EnterState(UiState::TurnOffDialog);
            return TransitionResult::Accepted;

        case UiEvent::CloseTurnOffDialog:
            if (snapshot_.state != UiState::TurnOffDialog) {
                return TransitionResult::IgnoredWrongState;
            }
            EnterState(stateBeforeTurnOff_);
            return TransitionResult::Accepted;

        case UiEvent::ChoosePowerAction:
            // Callers must use HandlePowerChoice; this keeps the enum total.
            return TransitionResult::RejectedNoSelection;

        case UiEvent::PowerActionStarted:
            if (snapshot_.state != UiState::PowerActionPending) {
                return TransitionResult::IgnoredWrongState;
            }
            return TransitionResult::Accepted;

        case UiEvent::SelectUser:
            return TransitionResult::RejectedInvalidUser;
    }
    return TransitionResult::IgnoredWrongState;
}

const char* LogonStateMachine::StateName(UiState state) {
    switch (state) {
        case UiState::Hidden: return "Hidden";
        case UiState::UserList: return "UserList";
        case UiState::PasswordEntry: return "PasswordEntry";
        case UiState::Authenticating: return "Authenticating";
        case UiState::AuthFailed: return "AuthFailed";
        case UiState::TurnOffDialog: return "TurnOffDialog";
        case UiState::PowerActionPending: return "PowerActionPending";
        case UiState::LoggedOn: return "LoggedOn";
    }
    return "?";
}

const char* LogonStateMachine::EventName(UiEvent event) {
    switch (event) {
        case UiEvent::Show: return "Show";
        case UiEvent::Hide: return "Hide";
        case UiEvent::SelectUser: return "SelectUser";
        case UiEvent::CancelSelection: return "CancelSelection";
        case UiEvent::SubmitCredentials: return "SubmitCredentials";
        case UiEvent::AuthSucceeded: return "AuthSucceeded";
        case UiEvent::AuthFailedEvent: return "AuthFailed";
        case UiEvent::AuthCancelled: return "AuthCancelled";
        case UiEvent::OpenTurnOffDialog: return "OpenTurnOffDialog";
        case UiEvent::CloseTurnOffDialog: return "CloseTurnOffDialog";
        case UiEvent::ChoosePowerAction: return "ChoosePowerAction";
        case UiEvent::PowerActionStarted: return "PowerActionStarted";
        case UiEvent::DismissError: return "DismissError";
        case UiEvent::Reset: return "Reset";
    }
    return "?";
}

} // namespace xplogin
