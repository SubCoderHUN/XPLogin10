// XPLogin10 - the logon flow, with no Windows in sight.
//
// This is the object the credential provider owns. It sequences: enumerate
// accounts -> user picks a tile -> password typed -> hand credentials to LSA ->
// play the XP logon sound -> make sure the shell is running. The exact same
// object is driven by tests/integration/, which is why it takes every OS
// service as an injected interface.
#pragma once

#include "xplogin/Config.h"
#include "xplogin/CredentialPacker.h"
#include "xplogin/Interfaces.h"
#include "xplogin/LogonStateMachine.h"
#include "xplogin/UserDirectory.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xplogin {

// The UI implements this to know when to repaint or animate.
class IControllerObserver {
public:
    virtual ~IControllerObserver() = default;
    virtual void OnStateChanged(UiState /*from*/, UiState /*to*/) {}
    virtual void OnUsersChanged() {}
    virtual void OnAuthError(const AuthResult& /*result*/) {}
    virtual void OnLogonComplete(const UserAccount& /*user*/, uint32_t /*sessionId*/) {}
    virtual void OnPowerAction(PowerAction /*action*/) {}
};

struct ControllerDependencies {
    std::shared_ptr<IAuthenticator>  authenticator;
    std::shared_ptr<IUserEnumerator> userEnumerator;
    std::shared_ptr<ISessionManager> sessionManager;
    std::shared_ptr<IPowerController> powerController;
    std::shared_ptr<ISoundPlayer>    soundPlayer;
    std::shared_ptr<IClock>          clock;
    std::shared_ptr<IStateStore>     stateStore;

    // Answers one question, before the screen is shown: does the sole account
    // really accept a blank password? A synchronous authenticator (LogonUserW
    // on Windows) is asked with an empty password; success means the account
    // has none. This is the only reliable signal - the SAM's UF_PASSWD_NOTREQD
    // flag is a hint, not a fact, and it does not survive the account list
    // LogonUI hands over in SetUserArray. Optional: when null, the account's
    // blankPassword hint is trusted instead (which is what the unit tests do).
    std::shared_ptr<IAuthenticator>  blankPasswordProber;
};

class LogonController {
public:
    LogonController(ControllerDependencies deps, AppConfig config);
    ~LogonController();

    // Mirrors ICredentialProvider::SetUsageScenario.
    bool Initialize(UsageScenario scenario);

    // CPUS_UNLOCK gives us the SID of the account that locked the workstation.
    void SetLockedUserSid(const std::wstring& sid);
    void SetLockedUserName(const std::wstring& qualifiedName);

    void Shutdown();

    // ---- input from the UI -------------------------------------------------
    TransitionResult SelectUser(int index);
    TransitionResult CancelSelection();
    void SetPassword(const std::wstring& password);
    TransitionResult Submit();
    TransitionResult OpenTurnOffDialog();
    TransitionResult CloseTurnOffDialog();
    TransitionResult ChoosePowerAction(PowerAction action);

    // ---- input from LogonUI ------------------------------------------------
    // ICredentialProviderCredential::ReportResult forwards the NTSTATUS here
    // when the authenticator is asynchronous.
    void OnAuthReported(const AuthResult& result);

    // Abandons a submission that never came back.
    //
    // The screen hands its credentials to LogonUI and waits in Authenticating
    // for a ReportResult that is not guaranteed to arrive: LogonUI can decide
    // to re-select the tile, hand the logon to another provider, or simply
    // drop the attempt. Nothing else leaves that state, so without this the
    // screen sits there refusing keystrokes and holding the password the user
    // typed - which is exactly what "it stopped taking the password and would
    // not clear the box" looks like from the outside.
    //
    // Returns Accepted only when there was something to cancel; the password
    // is cleared either way, because a password kept past the attempt it was
    // typed for is a password kept for no reason.
    TransitionResult CancelAuthentication();

    // ---- queries -----------------------------------------------------------
    const UiSnapshot& Snapshot() const { return machine_.Snapshot(); }
    UiState State() const { return machine_.State(); }
    const std::vector<UserAccount>& Users() const { return directory_.Users(); }
    const UserAccount* SelectedUser() const { return machine_.SelectedUser(); }
    int SelectedIndex() const { return machine_.Snapshot().selectedUser; }
    const AppConfig& Config() const { return config_; }
    uint32_t LogonSessionId() const { return logonSessionId_; }

    // The credential blob for LogonUI. Only valid while CanSerialize() is true.
    bool BuildSerialization(uint64_t logonId, size_t pointerSize,
                            PackedCredential* out) const;
    bool CanSerialize() const { return machine_.CanSerialize(); }

    // Current credentials, for the direct-LogonUserW path.
    LogonRequest CurrentRequest() const;

    // True when this screen should sign in on its own, showing XP's welcome
    // message and never an account list. See SignsInWithoutAsking.
    //
    // Decided once, when the account list settles (Initialize / RefreshUsers),
    // and cached: it is read on every frame, and a blank-password probe is a
    // real logon that must not be repeated per paint. An automatic sign-in that
    // LSA turns down clears this, so the screen falls back to an ordinary one.
    //
    // Sign-in only. An unlock has to stay a screen even for an account with no
    // password: Win+L that let go of itself the moment it was pressed would be
    // a lock that does not lock.
    bool SignsInAutomatically() const;

    // Drives the automatic sign-in: selects the sole account and hands LSA an
    // empty password, along the very same path a typed password takes. Returns
    // true when the controller is left ready for GetSerialization to pack the
    // blob (Authenticating, with a user selected). The credential provider
    // calls this once, the first time the screen appears for an account that
    // SignsInAutomatically(); a no-op and false in any other state.
    bool PrepareAutomaticSignIn();

    void SetObserver(IControllerObserver* observer) { observer_ = observer; }

    // Draw the shell-status screen, now, before this thread does anything else.
    //
    // The one place in this program where a repaint has to be synchronous, and
    // the reason is not performance. ChoosePowerAction moves the state to
    // PowerActionPending and then hands the action to Windows, and the handover
    // is the last thing this thread is reliably able to finish: LogonUI is torn
    // down with the session, so a WM_PAINT merely *queued* before it is never
    // processed. Measured, on a machine shutting down from the sign-in screen:
    // the last thing on the glass for the several seconds before the power went
    // was the XP turn-off dialog, still open, because that was the last frame
    // painted before the click.
    //
    // So the UI installs a presenter here, and it is called while the state is
    // already PowerActionPending and before Windows is told anything. What it
    // must do is paint and not return until the pixels are on the screen.
    void SetPowerActionPresenter(std::function<void()> present) {
        presentPowerAction_ = std::move(present);
    }

    // Forces a re-query of the account list (the watchdog calls this when it
    // sees a SAM change notification).
    void RefreshUsers();

    // Swaps in a different source of accounts. The credential provider needs
    // this because LogonUI hands over its authoritative account list in
    // SetUserArray, which it calls *after* SetUsageScenario - so the
    // controller has already been built against the SAM by then.
    void SetUserEnumerator(std::shared_ptr<IUserEnumerator> enumerator);

private:
    void ApplyStateChange(UiState previous);
    void CompleteLogon();

    // Recomputes the cached SignsInAutomatically() decision. Runs whenever the
    // account list changes.
    void EvaluateAutomaticSignIn();
    // Asks the prober whether `account` signs in with a blank password, caching
    // the answer for the life of the screen so the same account is not probed
    // (i.e. logged on and off) again on the next RefreshUsers.
    bool ProbeBlankPassword(const UserAccount& account);

    ControllerDependencies deps_;
    AppConfig              config_;
    UserDirectory          directory_;
    LogonStateMachine      machine_;
    IControllerObserver*   observer_ = nullptr;
    std::function<void()>  presentPowerAction_;

    std::wstring password_;
    std::wstring lockedUserSid_;
    std::wstring lockedUserName_;
    UsageScenario scenario_ = UsageScenario::Invalid;
    uint32_t      logonSessionId_ = 0;
    bool          initialized_ = false;

    // Cached SignsInAutomatically() decision and its guard rails.
    bool          autoSignIn_ = false;
    // Set when an automatic sign-in is rejected, so it is not armed again for
    // the rest of this screen. Cleared on Initialize.
    bool          autoSignInBlocked_ = false;
    // ProbeBlankPassword's one-entry cache, keyed by qualified account name.
    std::wstring  blankProbeName_;
    bool          blankProbeResult_ = false;
    bool          blankProbeValid_ = false;
};

} // namespace xplogin
