// XPLogin10 - the welcome screen state machine.
//
// Every screen transition XP could make is described here as data. The renderer
// asks "what state am I in", the credential provider asks "may I serialize
// now", and the tests drive the whole thing without a window or an LSA.
#pragma once

#include "xplogin/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xplogin {

enum class UiState {
    Hidden = 0,      // provider constructed but not shown
    UserList,        // welcome screen: "To begin, click your user name"
    PasswordEntry,   // tile slid left, password box visible
    Authenticating,  // credentials submitted, waiting for LSA
    AuthFailed,      // red error text + tile shake, back to PasswordEntry on key
    TurnOffDialog,   // "Turn off computer" overlay
    PowerActionPending, // shutdown/restart/standby handed to the OS
    LoggedOn,        // terminal - LogonUI is tearing us down
};

enum class UiEvent {
    Show = 0,
    Hide,
    SelectUser,        // payload: user index
    CancelSelection,   // green back arrow / Escape
    SubmitCredentials, // Enter or the green go button
    AuthSucceeded,
    AuthFailedEvent,
    AuthCancelled,
    OpenTurnOffDialog,
    CloseTurnOffDialog,
    ChoosePowerAction, // payload: PowerAction
    PowerActionStarted,
    DismissError,      // any key press while the error is showing
    Reset,             // LogonUI called SetUsageScenario again
};

// Everything the renderer needs in order to draw a frame, and everything the
// tests assert on.
struct UiSnapshot {
    UiState      state = UiState::Hidden;
    int          selectedUser = -1;      // index into the user list
    PowerAction  pendingAction = PowerAction::None;
    AuthStatus   lastAuthStatus = AuthStatus::Success;
    std::wstring statusText;             // instruction / error line
    int          failedAttempts = 0;
    bool         passwordBoxVisible = false;
    bool         backButtonVisible = false;
    bool         turnOffButtonEnabled = true;
    bool         busy = false;           // spinner / disabled input
};

// Policy knobs the machine has to honour. Populated from Config on Windows.
struct StateMachinePolicy {
    UsageScenario scenario = UsageScenario::Logon;
    bool allowShutdown = true;      // ShutdownWithoutLogon policy
    bool allowUserSwitch = true;    // false in CPUS_UNLOCK -> no back button
    bool allowBlankPassword = true;
    int  maxFailedAttempts = 0;     // 0 = unlimited (LSA still enforces lockout)
};

// A rejected transition tells the caller *why*, which makes both the UI beep
// and the test assertions specific.
enum class TransitionResult {
    Accepted = 0,
    IgnoredWrongState,
    RejectedNoSelection,
    RejectedInvalidUser,
    RejectedAccountUnusable,
    RejectedPasswordRequired,
    RejectedShutdownNotAllowed,
    RejectedSwitchNotAllowed,
    RejectedBusy,
};

class LogonStateMachine {
public:
    LogonStateMachine();

    void SetPolicy(const StateMachinePolicy& policy);
    const StateMachinePolicy& Policy() const { return policy_; }

    // The machine only needs to know which accounts are selectable.
    void SetUsers(const std::vector<UserAccount>& users);
    const std::vector<UserAccount>& Users() const { return users_; }

    // Pre-selects a tile without entering PasswordEntry (CPUS_UNLOCK gives us
    // the locked user up front).
    void PreselectUser(int index);

    TransitionResult Handle(UiEvent event);
    TransitionResult HandleSelectUser(int index);
    TransitionResult HandlePowerChoice(PowerAction action);
    TransitionResult HandleAuthResult(const AuthResult& result);

    UiState State() const { return snapshot_.state; }
    const UiSnapshot& Snapshot() const { return snapshot_; }

    // True when the credential provider is allowed to hand a blob to LogonUI.
    bool CanSerialize() const;

    // The account the user is trying to log on as, or nullptr.
    const UserAccount* SelectedUser() const;

    // Human readable, used by logs and by the test failure messages.
    static const char* StateName(UiState state);
    static const char* EventName(UiEvent event);

private:
    void EnterState(UiState state);
    void RefreshDerivedFlags();
    static std::wstring MessageForStatus(AuthStatus status);

    StateMachinePolicy       policy_;
    std::vector<UserAccount> users_;
    UiSnapshot               snapshot_;
    UiState                  stateBeforeTurnOff_ = UiState::UserList;
};

} // namespace xplogin
