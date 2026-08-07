// Unit tests: the welcome screen state machine.
//
// Required coverage: lock, login and shutdown states. Every transition the UI
// can trigger is exercised here, including the ones that must be refused.
#include "xplogin/LogonStateMachine.h"
#include "xptest.h"

#include <vector>

using namespace xplogin;

namespace {

UserAccount MakeUser(const wchar_t* name, bool enabled = true) {
    UserAccount account;
    account.username = name;
    account.displayName = name;
    account.domain = L"WINBOX";
    account.disabled = !enabled;
    return account;
}

std::vector<UserAccount> ThreeUsers() {
    return {MakeUser(L"Bill"), MakeUser(L"Alice"), MakeUser(L"Kiosk")};
}

// A machine already sitting on the welcome screen with three tiles.
LogonStateMachine ShownMachine(UsageScenario scenario = UsageScenario::Logon) {
    LogonStateMachine machine;
    StateMachinePolicy policy;
    policy.scenario = scenario;
    policy.allowUserSwitch = scenario != UsageScenario::UnlockWorkstation;
    machine.SetPolicy(policy);
    machine.SetUsers(ThreeUsers());
    machine.Handle(UiEvent::Show);
    return machine;
}

int S(UiState state) { return static_cast<int>(state); }
int R(TransitionResult result) { return static_cast<int>(result); }

} // namespace

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

TEST(StateMachine, StartsHidden) {
    LogonStateMachine machine;
    CHECK_EQ(S(machine.State()), S(UiState::Hidden));
    CHECK_EQ(machine.Snapshot().selectedUser, -1);
    CHECK_FALSE(machine.Snapshot().passwordBoxVisible);
}

TEST(StateMachine, ShowEntersUserList) {
    LogonStateMachine machine = ShownMachine();
    CHECK_EQ(S(machine.State()), S(UiState::UserList));
    // The account list carries no status text of its own. Its instruction line
    // is XpTheme::textClickUserName, so it can be translated or switched off
    // from XPLogin.theme.ini; a literal here would silently win over both.
    CHECK(machine.Snapshot().statusText.empty());
    CHECK_FALSE(machine.Snapshot().passwordBoxVisible);
    CHECK(machine.Snapshot().turnOffButtonEnabled);
}

TEST(StateMachine, ShowIsIgnoredWhenAlreadyVisible) {
    LogonStateMachine machine = ShownMachine();
    CHECK_EQ(R(machine.Handle(UiEvent::Show)), R(TransitionResult::IgnoredWrongState));
}

// ---------------------------------------------------------------------------
// Selecting a user
// ---------------------------------------------------------------------------

TEST(StateMachine, SelectingUserOpensPasswordEntry) {
    LogonStateMachine machine = ShownMachine();
    CHECK_EQ(R(machine.HandleSelectUser(1)), R(TransitionResult::Accepted));

    CHECK_EQ(S(machine.State()), S(UiState::PasswordEntry));
    CHECK_EQ(machine.Snapshot().selectedUser, 1);
    CHECK(machine.Snapshot().passwordBoxVisible);
    CHECK(machine.Snapshot().backButtonVisible);
    REQUIRE(machine.SelectedUser() != nullptr);
    CHECK_EQ(machine.SelectedUser()->username, std::wstring(L"Alice"));
}

TEST(StateMachine, SelectingOutOfRangeUserIsRejected) {
    LogonStateMachine machine = ShownMachine();
    CHECK_EQ(R(machine.HandleSelectUser(-1)), R(TransitionResult::RejectedInvalidUser));
    CHECK_EQ(R(machine.HandleSelectUser(99)), R(TransitionResult::RejectedInvalidUser));
    CHECK_EQ(S(machine.State()), S(UiState::UserList));
}

TEST(StateMachine, SelectingDisabledAccountShowsErrorAndStaysOnList) {
    LogonStateMachine machine;
    machine.SetUsers({MakeUser(L"Bill"), MakeUser(L"Retired", false)});
    machine.Handle(UiEvent::Show);

    CHECK_EQ(R(machine.HandleSelectUser(1)),
             R(TransitionResult::RejectedAccountUnusable));
    CHECK_EQ(S(machine.State()), S(UiState::UserList));
    CHECK_EQ(static_cast<int>(machine.Snapshot().lastAuthStatus),
             static_cast<int>(AuthStatus::AccountDisabled));
    CHECK_FALSE(machine.Snapshot().passwordBoxVisible);
}

TEST(StateMachine, SwitchingTilesClearsPreviousError) {
    LogonStateMachine machine = ShownMachine();
    machine.HandleSelectUser(0);
    machine.Handle(UiEvent::SubmitCredentials);
    machine.HandleAuthResult(AuthResult::Fail(AuthStatus::BadPassword));
    CHECK_EQ(machine.Snapshot().failedAttempts, 1);

    machine.HandleSelectUser(1);
    CHECK_EQ(machine.Snapshot().failedAttempts, 0);
    CHECK_EQ(static_cast<int>(machine.Snapshot().lastAuthStatus),
             static_cast<int>(AuthStatus::Success));
    CHECK_EQ(S(machine.State()), S(UiState::PasswordEntry));
}

TEST(StateMachine, ReselectingTheSameTileKeepsAttemptCount) {
    LogonStateMachine machine = ShownMachine();
    machine.HandleSelectUser(0);
    machine.Handle(UiEvent::SubmitCredentials);
    machine.HandleAuthResult(AuthResult::Fail(AuthStatus::BadPassword));

    machine.HandleSelectUser(0);
    CHECK_EQ(machine.Snapshot().failedAttempts, 1);
}

TEST(StateMachine, BackButtonReturnsToUserList) {
    LogonStateMachine machine = ShownMachine();
    machine.HandleSelectUser(0);
    CHECK_EQ(R(machine.Handle(UiEvent::CancelSelection)), R(TransitionResult::Accepted));

    CHECK_EQ(S(machine.State()), S(UiState::UserList));
    CHECK_EQ(machine.Snapshot().selectedUser, -1);
    CHECK(machine.SelectedUser() == nullptr);
}

// ---------------------------------------------------------------------------
// Login
// ---------------------------------------------------------------------------

TEST(StateMachine, SubmitEntersAuthenticating) {
    LogonStateMachine machine = ShownMachine();
    machine.HandleSelectUser(0);
    CHECK_EQ(R(machine.Handle(UiEvent::SubmitCredentials)),
             R(TransitionResult::Accepted));

    CHECK_EQ(S(machine.State()), S(UiState::Authenticating));
    CHECK(machine.Snapshot().busy);
    CHECK(machine.CanSerialize());
    // The power button is disabled mid-authentication: XP did the same.
    CHECK_FALSE(machine.Snapshot().turnOffButtonEnabled);
}

TEST(StateMachine, SubmitWithoutSelectionIsRejected) {
    LogonStateMachine machine = ShownMachine();
    CHECK_EQ(R(machine.Handle(UiEvent::SubmitCredentials)),
             R(TransitionResult::IgnoredWrongState));
    CHECK_FALSE(machine.CanSerialize());
}

TEST(StateMachine, SuccessfulAuthReachesLoggedOn) {
    LogonStateMachine machine = ShownMachine();
    machine.HandleSelectUser(0);
    machine.Handle(UiEvent::SubmitCredentials);
    CHECK_EQ(R(machine.HandleAuthResult(AuthResult::Success())),
             R(TransitionResult::Accepted));

    CHECK_EQ(S(machine.State()), S(UiState::LoggedOn));
    CHECK_EQ(machine.Snapshot().failedAttempts, 0);
    CHECK_FALSE(machine.CanSerialize());
}

TEST(StateMachine, FailedAuthShowsErrorAndCountsTheAttempt) {
    LogonStateMachine machine = ShownMachine();
    machine.HandleSelectUser(0);
    machine.Handle(UiEvent::SubmitCredentials);
    machine.HandleAuthResult(AuthResult::Fail(AuthStatus::BadPassword));

    CHECK_EQ(S(machine.State()), S(UiState::AuthFailed));
    CHECK_EQ(machine.Snapshot().failedAttempts, 1);
    CHECK(machine.Snapshot().passwordBoxVisible);
    CHECK_FALSE(machine.Snapshot().statusText.empty());
    // Still the same tile: the user retypes rather than reselecting.
    CHECK_EQ(machine.Snapshot().selectedUser, 0);
}

TEST(StateMachine, TypingAfterAnErrorReturnsToPasswordEntry) {
    LogonStateMachine machine = ShownMachine();
    machine.HandleSelectUser(0);
    machine.Handle(UiEvent::SubmitCredentials);
    machine.HandleAuthResult(AuthResult::Fail(AuthStatus::BadPassword));

    CHECK_EQ(R(machine.Handle(UiEvent::DismissError)), R(TransitionResult::Accepted));
    CHECK_EQ(S(machine.State()), S(UiState::PasswordEntry));
    CHECK(machine.Snapshot().statusText.empty() ||
          machine.Snapshot().statusText == std::wstring(L"Type your password"));
}

TEST(StateMachine, RetryAfterFailureIsAllowed) {
    LogonStateMachine machine = ShownMachine();
    machine.HandleSelectUser(0);
    machine.Handle(UiEvent::SubmitCredentials);
    machine.HandleAuthResult(AuthResult::Fail(AuthStatus::BadPassword));

    // Submitting straight from AuthFailed, without dismissing first.
    CHECK_EQ(R(machine.Handle(UiEvent::SubmitCredentials)),
             R(TransitionResult::Accepted));
    CHECK_EQ(S(machine.State()), S(UiState::Authenticating));
}

TEST(StateMachine, ExceedingMaxAttemptsDropsBackToTheUserList) {
    LogonStateMachine machine;
    StateMachinePolicy policy;
    policy.maxFailedAttempts = 3;
    machine.SetPolicy(policy);
    machine.SetUsers(ThreeUsers());
    machine.Handle(UiEvent::Show);
    machine.HandleSelectUser(0);

    for (int i = 0; i < 2; ++i) {
        machine.Handle(UiEvent::SubmitCredentials);
        machine.HandleAuthResult(AuthResult::Fail(AuthStatus::BadPassword));
        CHECK_EQ(S(machine.State()), S(UiState::AuthFailed));
    }

    machine.Handle(UiEvent::SubmitCredentials);
    machine.HandleAuthResult(AuthResult::Fail(AuthStatus::BadPassword));
    CHECK_EQ(S(machine.State()), S(UiState::UserList));
    CHECK_EQ(machine.Snapshot().selectedUser, -1);
}

TEST(StateMachine, CancelledAuthReturnsToPasswordEntryWithoutAStrike) {
    LogonStateMachine machine = ShownMachine();
    machine.HandleSelectUser(0);
    machine.Handle(UiEvent::SubmitCredentials);
    machine.HandleAuthResult(AuthResult::Fail(AuthStatus::Cancelled));

    CHECK_EQ(S(machine.State()), S(UiState::PasswordEntry));
    CHECK_EQ(machine.Snapshot().failedAttempts, 0);
}

TEST(StateMachine, AuthResultOutsideAuthenticatingIsIgnored) {
    LogonStateMachine machine = ShownMachine();
    CHECK_EQ(R(machine.HandleAuthResult(AuthResult::Success())),
             R(TransitionResult::IgnoredWrongState));
    CHECK_EQ(S(machine.State()), S(UiState::UserList));
}

TEST(StateMachine, ErrorMessageDiffersPerFailureReason) {
    LogonStateMachine machine = ShownMachine();

    machine.HandleSelectUser(0);
    machine.Handle(UiEvent::SubmitCredentials);
    machine.HandleAuthResult(AuthResult::Fail(AuthStatus::BadPassword));
    const std::wstring badPassword = machine.Snapshot().statusText;

    machine.Handle(UiEvent::SubmitCredentials);
    machine.HandleAuthResult(AuthResult::Fail(AuthStatus::AccountLockedOut));
    const std::wstring lockedOut = machine.Snapshot().statusText;

    CHECK_FALSE(badPassword.empty());
    CHECK_FALSE(lockedOut.empty());
    CHECK_NE(badPassword, lockedOut);
}

TEST(StateMachine, CustomErrorMessageOverridesTheDefault) {
    LogonStateMachine machine = ShownMachine();
    machine.HandleSelectUser(0);
    machine.Handle(UiEvent::SubmitCredentials);
    machine.HandleAuthResult(
        AuthResult::Fail(AuthStatus::InternalError, L"Domain controller unreachable"));
    CHECK_EQ(machine.Snapshot().statusText,
             std::wstring(L"Domain controller unreachable"));
}

// ---------------------------------------------------------------------------
// Lock / unlock scenario
// ---------------------------------------------------------------------------

TEST(StateMachine, UnlockScenarioGoesStraightToPasswordEntry) {
    LogonStateMachine machine;
    StateMachinePolicy policy;
    policy.scenario = UsageScenario::UnlockWorkstation;
    policy.allowUserSwitch = false;
    machine.SetPolicy(policy);
    machine.SetUsers({MakeUser(L"Bill")});
    machine.PreselectUser(0);
    machine.Handle(UiEvent::Show);

    CHECK_EQ(S(machine.State()), S(UiState::PasswordEntry));
    CHECK_EQ(machine.Snapshot().selectedUser, 0);
    CHECK(machine.Snapshot().passwordBoxVisible);
}

TEST(StateMachine, UnlockScenarioHidesTheBackButton) {
    LogonStateMachine machine = ShownMachine(UsageScenario::UnlockWorkstation);
    machine.PreselectUser(0);
    machine.HandleSelectUser(0);

    CHECK_FALSE(machine.Snapshot().backButtonVisible);
    CHECK_EQ(R(machine.Handle(UiEvent::CancelSelection)),
             R(TransitionResult::RejectedSwitchNotAllowed));
    // Still on the password box: there is nowhere else to go.
    CHECK_EQ(S(machine.State()), S(UiState::PasswordEntry));
}

TEST(StateMachine, UnlockScenarioStillAuthenticatesNormally) {
    LogonStateMachine machine = ShownMachine(UsageScenario::UnlockWorkstation);
    machine.PreselectUser(0);
    machine.HandleSelectUser(0);
    machine.Handle(UiEvent::SubmitCredentials);
    CHECK(machine.CanSerialize());
    machine.HandleAuthResult(AuthResult::Success());
    CHECK_EQ(S(machine.State()), S(UiState::LoggedOn));
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

TEST(StateMachine, TurnOffDialogOpensAndCancels) {
    LogonStateMachine machine = ShownMachine();
    CHECK_EQ(R(machine.Handle(UiEvent::OpenTurnOffDialog)),
             R(TransitionResult::Accepted));
    CHECK_EQ(S(machine.State()), S(UiState::TurnOffDialog));

    CHECK_EQ(R(machine.Handle(UiEvent::CloseTurnOffDialog)),
             R(TransitionResult::Accepted));
    CHECK_EQ(S(machine.State()), S(UiState::UserList));
}

TEST(StateMachine, TurnOffDialogReturnsToWhereItWasOpenedFrom) {
    LogonStateMachine machine = ShownMachine();
    machine.HandleSelectUser(1);
    machine.Handle(UiEvent::OpenTurnOffDialog);
    machine.Handle(UiEvent::CloseTurnOffDialog);

    // Back on the password box for the same user, not the tile list.
    CHECK_EQ(S(machine.State()), S(UiState::PasswordEntry));
    CHECK_EQ(machine.Snapshot().selectedUser, 1);
}

TEST(StateMachine, ChoosingShutDownEntersPowerActionPending) {
    LogonStateMachine machine = ShownMachine();
    machine.Handle(UiEvent::OpenTurnOffDialog);
    CHECK_EQ(R(machine.HandlePowerChoice(PowerAction::TurnOff)),
             R(TransitionResult::Accepted));

    CHECK_EQ(S(machine.State()), S(UiState::PowerActionPending));
    CHECK_EQ(static_cast<int>(machine.Snapshot().pendingAction),
             static_cast<int>(PowerAction::TurnOff));
    CHECK(machine.Snapshot().busy);
}

TEST(StateMachine, ChoosingRestartAndStandByAreDistinct) {
    LogonStateMachine restart = ShownMachine();
    restart.Handle(UiEvent::OpenTurnOffDialog);
    restart.HandlePowerChoice(PowerAction::Restart);
    CHECK_EQ(static_cast<int>(restart.Snapshot().pendingAction),
             static_cast<int>(PowerAction::Restart));

    LogonStateMachine standby = ShownMachine();
    standby.Handle(UiEvent::OpenTurnOffDialog);
    standby.HandlePowerChoice(PowerAction::StandBy);
    CHECK_EQ(static_cast<int>(standby.Snapshot().pendingAction),
             static_cast<int>(PowerAction::StandBy));
}

TEST(StateMachine, ChoosingNoneCancelsTheDialog) {
    LogonStateMachine machine = ShownMachine();
    machine.Handle(UiEvent::OpenTurnOffDialog);
    CHECK_EQ(R(machine.HandlePowerChoice(PowerAction::None)),
             R(TransitionResult::Accepted));
    CHECK_EQ(S(machine.State()), S(UiState::UserList));
}

TEST(StateMachine, PowerChoiceOutsideTheDialogIsIgnored) {
    LogonStateMachine machine = ShownMachine();
    CHECK_EQ(R(machine.HandlePowerChoice(PowerAction::TurnOff)),
             R(TransitionResult::IgnoredWrongState));
    CHECK_EQ(S(machine.State()), S(UiState::UserList));
}

TEST(StateMachine, ShutdownIsRefusedWhenPolicyForbidsIt) {
    LogonStateMachine machine;
    StateMachinePolicy policy;
    policy.allowShutdown = false;
    machine.SetPolicy(policy);
    machine.SetUsers(ThreeUsers());
    machine.Handle(UiEvent::Show);

    CHECK_FALSE(machine.Snapshot().turnOffButtonEnabled);
    CHECK_EQ(R(machine.Handle(UiEvent::OpenTurnOffDialog)),
             R(TransitionResult::RejectedShutdownNotAllowed));
    CHECK_EQ(S(machine.State()), S(UiState::UserList));
}

TEST(StateMachine, TurnOffIsRefusedWhileAuthenticating) {
    LogonStateMachine machine = ShownMachine();
    machine.HandleSelectUser(0);
    machine.Handle(UiEvent::SubmitCredentials);

    CHECK_EQ(R(machine.Handle(UiEvent::OpenTurnOffDialog)),
             R(TransitionResult::RejectedBusy));
    CHECK_EQ(S(machine.State()), S(UiState::Authenticating));
}

// ---------------------------------------------------------------------------
// Resets and edge cases
// ---------------------------------------------------------------------------

TEST(StateMachine, ResetClearsEverything) {
    LogonStateMachine machine = ShownMachine();
    machine.HandleSelectUser(2);
    machine.Handle(UiEvent::SubmitCredentials);
    machine.HandleAuthResult(AuthResult::Fail(AuthStatus::BadPassword));

    CHECK_EQ(R(machine.Handle(UiEvent::Reset)), R(TransitionResult::Accepted));
    CHECK_EQ(S(machine.State()), S(UiState::Hidden));
    CHECK_EQ(machine.Snapshot().selectedUser, -1);
    CHECK_EQ(machine.Snapshot().failedAttempts, 0);
    CHECK_EQ(static_cast<int>(machine.Snapshot().pendingAction),
             static_cast<int>(PowerAction::None));
}

TEST(StateMachine, ShrinkingUserListDropsAnInvalidSelection) {
    LogonStateMachine machine = ShownMachine();
    machine.HandleSelectUser(2);
    CHECK_EQ(S(machine.State()), S(UiState::PasswordEntry));

    // A user was deleted while the welcome screen was up.
    machine.SetUsers({MakeUser(L"Bill")});
    CHECK_EQ(machine.Snapshot().selectedUser, -1);
    CHECK_EQ(S(machine.State()), S(UiState::UserList));
}

TEST(StateMachine, EmptyUserListStillRendersTheList) {
    LogonStateMachine machine;
    machine.SetUsers({});
    machine.Handle(UiEvent::Show);
    CHECK_EQ(S(machine.State()), S(UiState::UserList));
    CHECK_EQ(R(machine.HandleSelectUser(0)), R(TransitionResult::RejectedInvalidUser));
}

TEST(StateMachine, HideFromAnyStateWorks) {
    LogonStateMachine machine = ShownMachine();
    machine.HandleSelectUser(0);
    machine.Handle(UiEvent::SubmitCredentials);
    CHECK_EQ(R(machine.Handle(UiEvent::Hide)), R(TransitionResult::Accepted));
    CHECK_EQ(S(machine.State()), S(UiState::Hidden));
}

TEST(StateMachine, StateAndEventNamesAreAllDistinct) {
    // Cheap guard against a copy-paste slip in the logging helpers.
    const UiState states[] = {UiState::Hidden,        UiState::UserList,
                              UiState::PasswordEntry, UiState::Authenticating,
                              UiState::AuthFailed,    UiState::TurnOffDialog,
                              UiState::PowerActionPending, UiState::LoggedOn};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        for (size_t j = i + 1; j < sizeof(states) / sizeof(states[0]); ++j) {
            CHECK_NE(std::string(LogonStateMachine::StateName(states[i])),
                     std::string(LogonStateMachine::StateName(states[j])));
        }
    }
    CHECK_NE(std::string(LogonStateMachine::EventName(UiEvent::Show)),
             std::string(LogonStateMachine::EventName(UiEvent::Hide)));
}
