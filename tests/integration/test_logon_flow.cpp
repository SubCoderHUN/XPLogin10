// Integration tests: the whole logon flow end to end.
//
// The controller under test here is byte-for-byte the one the credential
// provider drives on a real machine; only the OS behind it is simulated. These
// cover the credential-provider handshake (serialize -> LSA -> ReportResult),
// the power path and the observer callbacks the renderer relies on.
#include "xplogin/CredentialPacker.h"
#include "xplogin/LogonController.h"
#include "xplogin/ShellStatus.h"
#include "Mocks.h"
#include "xptest.h"

#include <memory>

using namespace xplogin;
using namespace xplogin::testing;

namespace {

struct Rig {
    std::shared_ptr<FakeAccountDatabase> db =
        std::make_shared<FakeAccountDatabase>(L"WINBOX");
    std::shared_ptr<FakeAuthenticator> auth;
    std::shared_ptr<FakeUserEnumerator> users;
    // Optional blank-password prober. Left null by default so the existing
    // tests keep their flag-based behaviour; the auto sign-in tests set one.
    std::shared_ptr<FakeAuthenticator> prober;
    std::shared_ptr<FakeSessionManager> sessions = std::make_shared<FakeSessionManager>();
    std::shared_ptr<FakePowerController> power = std::make_shared<FakePowerController>();
    std::shared_ptr<FakeSoundPlayer> sound = std::make_shared<FakeSoundPlayer>();
    std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
    std::shared_ptr<FakeStateStore> store = std::make_shared<FakeStateStore>();
    RecordingObserver observer;
    AppConfig config;

    Rig() {
        db->Add(L"Bill", L"hunter2");
        db->Add(L"Alice", L"opensesame");
        db->Add(L"Kiosk", L"");
        auth = std::make_shared<FakeAuthenticator>(db);
        users = std::make_shared<FakeUserEnumerator>(db);
    }

    std::unique_ptr<LogonController> Make() {
        ControllerDependencies deps;
        deps.authenticator = auth;
        deps.userEnumerator = users;
        deps.sessionManager = sessions;
        deps.powerController = power;
        deps.soundPlayer = sound;
        deps.clock = clock;
        deps.stateStore = store;
        deps.blankPasswordProber = prober;

        auto controller = std::make_unique<LogonController>(deps, config);
        controller->SetObserver(&observer);
        return controller;
    }
};

int IndexOf(const LogonController& controller, const wchar_t* name) {
    for (size_t i = 0; i < controller.Users().size(); ++i) {
        if (EqualsNoCase(controller.Users()[i].username, name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST(LogonFlow, WelcomeScreenComesUpWithTheAccountList) {
    Rig rig;
    auto controller = rig.Make();
    CHECK(controller->Initialize(UsageScenario::Logon));

    CHECK_EQ(controller->State(), UiState::UserList);
    CHECK_EQ(controller->Users().size(), size_t(3));
    CHECK_EQ(rig.observer.usersChangedCount, 1);
    // See StateMachine.ShowEntersUserList: the instruction above the list is a
    // theme string, not a status, and is empty by default.
    CHECK(controller->Snapshot().statusText.empty());
}

TEST(LogonFlow, ClickTypePressEnter) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    const int bill = IndexOf(*controller, L"Bill");
    REQUIRE_GE(bill, 0);

    CHECK_EQ(static_cast<int>(controller->SelectUser(bill)),
             static_cast<int>(TransitionResult::Accepted));
    CHECK_EQ(controller->State(), UiState::PasswordEntry);

    controller->SetPassword(L"hunter2");
    controller->Submit();

    CHECK_EQ(controller->State(), UiState::LoggedOn);
    CHECK_EQ(rig.observer.logonCompleteCount, 1);
    CHECK(rig.observer.SawTransitionTo(UiState::Authenticating));
    CHECK(rig.observer.SawTransitionTo(UiState::LoggedOn));
}

TEST(LogonFlow, ObserverSeesTheTransitionsInOrder) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    controller->SelectUser(0);
    controller->SetPassword(L"wrong");
    controller->Submit();

    REQUIRE_GE(rig.observer.transitions.size(), size_t(3));
    // Hidden -> UserList -> PasswordEntry -> Authenticating -> AuthFailed
    CHECK_EQ(static_cast<int>(rig.observer.transitions.front().second),
             static_cast<int>(UiState::UserList));
    CHECK_EQ(static_cast<int>(rig.observer.transitions.back().second),
             static_cast<int>(UiState::AuthFailed));
    REQUIRE_EQ(rig.observer.authErrors.size(), size_t(1));
    CHECK_EQ(static_cast<int>(rig.observer.authErrors[0]),
             static_cast<int>(AuthStatus::BadPassword));
}

TEST(LogonFlow, CredentialsAreBuiltFromTheSelectedTile) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    const int alice = IndexOf(*controller, L"Alice");
    controller->SelectUser(alice);
    controller->SetPassword(L"opensesame");

    const LogonRequest request = controller->CurrentRequest();
    CHECK_EQ(request.username, std::wstring(L"Alice"));
    CHECK_EQ(request.domain, std::wstring(L"WINBOX"));
    CHECK_EQ(request.password, std::wstring(L"opensesame"));
    CHECK_EQ(static_cast<int>(request.scenario),
             static_cast<int>(UsageScenario::Logon));
}

TEST(LogonFlow, PasswordIsClearedAfterEveryAttempt) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    controller->SelectUser(0);

    controller->SetPassword(L"wrong");
    controller->Submit();
    CHECK(controller->CurrentRequest().password.empty());

    controller->SetPassword(L"hunter2");
    controller->Submit();
    CHECK(controller->CurrentRequest().password.empty());
}

TEST(LogonFlow, SwitchingTilesForgetsTheTypedPassword) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    controller->SelectUser(0);
    controller->SetPassword(L"typed-for-the-wrong-person");
    controller->SelectUser(1);

    CHECK(controller->CurrentRequest().password.empty());
}

TEST(LogonFlow, BackButtonReturnsToTheTileList) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    controller->SelectUser(0);
    controller->SetPassword(L"something");
    CHECK_EQ(static_cast<int>(controller->CancelSelection()),
             static_cast<int>(TransitionResult::Accepted));

    CHECK_EQ(controller->State(), UiState::UserList);
    CHECK(controller->SelectedUser() == nullptr);
    CHECK(controller->CurrentRequest().password.empty());
}

TEST(LogonFlow, BlankPasswordAccountSignsIn) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    const int kiosk = IndexOf(*controller, L"Kiosk");
    REQUIRE_GE(kiosk, 0);
    controller->SelectUser(kiosk);
    controller->Submit(); // no SetPassword at all

    CHECK_EQ(controller->State(), UiState::LoggedOn);
}

// ---------------------------------------------------------------------------
// The credential provider handshake
// ---------------------------------------------------------------------------

TEST(LogonFlow, SerializationIsOnlyAvailableWhileAuthenticating) {
    Rig rig;
    rig.auth->asynchronous = true;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    PackedCredential packed;
    CHECK_FALSE(controller->CanSerialize());
    CHECK_FALSE(controller->BuildSerialization(0, 8, &packed));

    controller->SelectUser(0);
    CHECK_FALSE(controller->CanSerialize());

    controller->SetPassword(L"hunter2");
    controller->Submit();
    // Asynchronous: we are parked in Authenticating waiting for LSA.
    CHECK_EQ(controller->State(), UiState::Authenticating);
    CHECK(controller->CanSerialize());
    CHECK(controller->BuildSerialization(0, 8, &packed));
    CHECK_FALSE(packed.empty());
}

TEST(LogonFlow, SerializedBlobCarriesTheTypedCredentials) {
    Rig rig;
    rig.auth->asynchronous = true;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    const int alice = IndexOf(*controller, L"Alice");
    controller->SelectUser(alice);
    controller->SetPassword(L"opensesame");
    controller->Submit();

    PackedCredential packed;
    REQUIRE(controller->BuildSerialization(0, 8, &packed));
    CHECK(ValidatePackedCredential(packed.bytes, 8));

    const KerbLayout layout = KerbLayout::For(8);
    std::u16string value;
    REQUIRE(ReadPackedString(packed, layout.domainOffset, 8, &value));
    CHECK_EQ(value, WideToU16(L"WINBOX"));
    REQUIRE(ReadPackedString(packed, layout.userOffset, 8, &value));
    CHECK_EQ(value, WideToU16(L"Alice"));
    REQUIRE(ReadPackedString(packed, layout.passwordOffset, 8, &value));
    CHECK_EQ(value, WideToU16(L"opensesame"));
}

TEST(LogonFlow, UnlockScenarioSerializesTheUnlockMessageType) {
    Rig rig;
    rig.auth->asynchronous = true;
    auto controller = rig.Make();
    controller->SetLockedUserSid(rig.db->Find(L"", L"Bill")->account.sid);
    controller->Initialize(UsageScenario::UnlockWorkstation);

    controller->SetPassword(L"hunter2");
    controller->Submit();

    PackedCredential packed;
    REQUIRE(controller->BuildSerialization(0x1234, 8, &packed));

    const KerbLayout layout = KerbLayout::For(8);
    uint32_t messageType = 0;
    for (size_t i = 0; i < 4; ++i) {
        messageType |= static_cast<uint32_t>(packed.bytes[layout.messageTypeOffset + i])
                       << (8 * i);
    }
    CHECK_EQ(messageType,
             static_cast<uint32_t>(KerbMessageType::WorkstationUnlockLogon));
}

TEST(LogonFlow, ReportResultDrivesTheAsynchronousOutcome) {
    Rig rig;
    rig.auth->asynchronous = true;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    controller->SelectUser(IndexOf(*controller, L"Bill"));
    controller->SetPassword(L"hunter2");
    controller->Submit();
    CHECK_EQ(controller->State(), UiState::Authenticating);

    // LogonUI calls ReportResult once LSA has spoken.
    controller->OnAuthReported(rig.auth->ResolvePending());

    CHECK_EQ(controller->State(), UiState::LoggedOn);
    CHECK_EQ(rig.observer.logonCompleteCount, 1);
    CHECK_EQ(rig.sound->CountOf(SoundEvent::Logon), 1);
}

TEST(LogonFlow, ReportResultCanAlsoDeliverAFailure) {
    Rig rig;
    rig.auth->asynchronous = true;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    controller->SelectUser(0);
    controller->SetPassword(L"wrong");
    controller->Submit();
    controller->OnAuthReported(rig.auth->ResolvePending());

    CHECK_EQ(controller->State(), UiState::AuthFailed);
    CHECK_FALSE(controller->Snapshot().statusText.empty());
}

TEST(LogonFlow, ReportResultArrivingLateIsIgnored) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    // Nothing is in flight; a stray ReportResult must not fake a logon.
    controller->OnAuthReported(AuthResult::Success());
    CHECK_EQ(controller->State(), UiState::UserList);
    CHECK_EQ(rig.observer.logonCompleteCount, 0);
}

// ---------------------------------------------------------------------------
// Turn off computer
// ---------------------------------------------------------------------------

TEST(LogonFlow, TurnOffDialogExecutesTheChosenAction) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    CHECK_EQ(static_cast<int>(controller->OpenTurnOffDialog()),
             static_cast<int>(TransitionResult::Accepted));
    CHECK_EQ(controller->State(), UiState::TurnOffDialog);

    CHECK_EQ(static_cast<int>(controller->ChoosePowerAction(PowerAction::Restart)),
             static_cast<int>(TransitionResult::Accepted));

    REQUIRE_EQ(rig.power->executed.size(), size_t(1));
    CHECK_EQ(static_cast<int>(rig.power->executed[0]),
             static_cast<int>(PowerAction::Restart));
    REQUIRE_EQ(rig.observer.powerActions.size(), size_t(1));
    CHECK_EQ(static_cast<int>(rig.observer.powerActions[0]),
             static_cast<int>(PowerAction::Restart));
}

// The one ordering in this program that a machine's last few seconds depend on.
//
// ChoosePowerAction moves the state to PowerActionPending and then hands the
// machine to Windows. After the handover, the thread that draws the screen may
// never run again - LogonUI is torn down with the session - so anything merely
// queued by then is never painted. A shutdown from the sign-in screen showed
// exactly that: Windows' spinner, and then the XP screen with the turn-off
// dialog still open on it, held there until the power went, because that was
// the last frame painted before the click.
TEST(LogonFlow, TheStatusScreenIsPaintedBeforeWindowsIsTold) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    int paints = 0;
    UiState stateWhenPainted = UiState::Hidden;
    bool paintedBeforeExecute = false;

    controller->SetPowerActionPresenter([&]() {
        ++paints;
        stateWhenPainted = controller->State();
        paintedBeforeExecute = rig.power->executed.empty();
    });

    controller->OpenTurnOffDialog();
    controller->ChoosePowerAction(PowerAction::TurnOff);

    CHECK_EQ(paints, 1);
    CHECK(paintedBeforeExecute);
    // And painted with the dialog already gone from the state, so what lands on
    // the glass is the XP shell-status screen rather than the dialog.
    CHECK_EQ(stateWhenPainted, UiState::PowerActionPending);
    CHECK_EQ(ShellStatusFor(stateWhenPainted, PowerAction::TurnOff),
             ShellStatus::ShuttingDown);
}

// The same guarantee from the other side: whatever the presenter does, Windows
// is told afterwards. A presenter that faults or hangs is a bug in the UI, but
// a presenter that silently stopped the shutdown would be a machine that will
// not turn off.
TEST(LogonFlow, TheShutdownHappensEvenWithNoPresenter) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    controller->OpenTurnOffDialog();
    controller->ChoosePowerAction(PowerAction::Restart);

    REQUIRE_EQ(rig.power->executed.size(), size_t(1));
    CHECK_EQ(static_cast<int>(rig.power->executed[0]),
             static_cast<int>(PowerAction::Restart));
}

// Nothing is painted for a choice that was refused, because nothing changed.
TEST(LogonFlow, ARefusedPowerActionPaintsNothing) {
    Rig rig;
    rig.power->hibernateSupported = false;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    int paints = 0;
    controller->SetPowerActionPresenter([&]() { ++paints; });

    controller->OpenTurnOffDialog();
    controller->ChoosePowerAction(PowerAction::Hibernate);

    CHECK_EQ(paints, 0);
    CHECK(rig.power->executed.empty());
    CHECK_EQ(controller->State(), UiState::TurnOffDialog);
}

TEST(LogonFlow, CancellingTheDialogChangesNothing) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    controller->OpenTurnOffDialog();
    CHECK_EQ(static_cast<int>(controller->CloseTurnOffDialog()),
             static_cast<int>(TransitionResult::Accepted));

    CHECK_EQ(controller->State(), UiState::UserList);
    CHECK(rig.power->executed.empty());
}

TEST(LogonFlow, UnsupportedPowerActionIsRefused) {
    Rig rig;
    rig.power->hibernateSupported = false;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    controller->OpenTurnOffDialog();

    CHECK_EQ(static_cast<int>(controller->ChoosePowerAction(PowerAction::Hibernate)),
             static_cast<int>(TransitionResult::RejectedShutdownNotAllowed));
    CHECK(rig.power->executed.empty());
    // Still on the dialog: the user can pick something else.
    CHECK_EQ(controller->State(), UiState::TurnOffDialog);
}

TEST(LogonFlow, ShutdownPolicyDisablesTheWholeButton) {
    Rig rig;
    rig.config.allowShutdown = false;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    CHECK_FALSE(controller->Snapshot().turnOffButtonEnabled);
    CHECK_EQ(static_cast<int>(controller->OpenTurnOffDialog()),
             static_cast<int>(TransitionResult::RejectedShutdownNotAllowed));
    CHECK(rig.power->executed.empty());
}

TEST(LogonFlow, ShutdownPlaysTheShutdownSound) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    controller->OpenTurnOffDialog();
    controller->ChoosePowerAction(PowerAction::TurnOff);

    CHECK_EQ(rig.sound->CountOf(SoundEvent::Shutdown), 1);
}

TEST(LogonFlow, StandByDoesNotPlayTheShutdownSound) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    controller->OpenTurnOffDialog();
    controller->ChoosePowerAction(PowerAction::StandBy);

    CHECK_EQ(rig.sound->CountOf(SoundEvent::Shutdown), 0);
    REQUIRE_EQ(rig.power->executed.size(), size_t(1));
    CHECK_EQ(static_cast<int>(rig.power->executed[0]),
             static_cast<int>(PowerAction::StandBy));
}

// ---------------------------------------------------------------------------
// Lifecycle and robustness
// ---------------------------------------------------------------------------

TEST(LogonFlow, ReinitializingResetsCleanly) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    controller->SelectUser(0);
    controller->SetPassword(L"hunter2");

    // LogonUI calls SetUsageScenario again, e.g. after a screensaver timeout.
    controller->Initialize(UsageScenario::Logon);

    CHECK_EQ(controller->State(), UiState::UserList);
    CHECK(controller->SelectedUser() == nullptr);
    CHECK(controller->CurrentRequest().password.empty());
    CHECK_EQ(controller->Snapshot().failedAttempts, 0);
}

TEST(LogonFlow, ShutdownWipesTheTypedPassword) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    controller->SelectUser(0);
    controller->SetPassword(L"hunter2");

    controller->Shutdown();
    CHECK_EQ(controller->State(), UiState::Hidden);
    CHECK(controller->CurrentRequest().password.empty());
}

TEST(LogonFlow, RefreshUsersKeepsTheScreenLive) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    CHECK_EQ(controller->Users().size(), size_t(3));

    rig.db->Add(L"NewHire", L"pw");
    controller->RefreshUsers();

    CHECK_EQ(controller->Users().size(), size_t(4));
    CHECK_EQ(rig.observer.usersChangedCount, 2);
    CHECK_GE(IndexOf(*controller, L"NewHire"), 0);
}

TEST(LogonFlow, MissingAuthenticatorFailsGracefully) {
    Rig rig;
    ControllerDependencies deps;
    deps.userEnumerator = rig.users;
    deps.sessionManager = rig.sessions;
    deps.clock = rig.clock;
    deps.stateStore = rig.store;
    // No authenticator at all - the machine must show an error, not crash.
    LogonController controller(deps, rig.config);
    controller.SetObserver(&rig.observer);
    controller.Initialize(UsageScenario::Logon);

    controller.SelectUser(0);
    controller.SetPassword(L"hunter2");
    controller.Submit();

    CHECK_EQ(controller.State(), UiState::AuthFailed);
    REQUIRE_EQ(rig.observer.authErrors.size(), size_t(1));
    CHECK_EQ(static_cast<int>(rig.observer.authErrors[0]),
             static_cast<int>(AuthStatus::InternalError));
}

TEST(LogonFlow, ControllerWorksWithNoOptionalDependencies) {
    Rig rig;
    ControllerDependencies deps;
    deps.authenticator = rig.auth;
    deps.userEnumerator = rig.users;
    // No session manager, sound, power, clock or state store.
    LogonController controller(deps, rig.config);
    controller.Initialize(UsageScenario::Logon);

    controller.SelectUser(IndexOf(controller, L"Bill"));
    controller.SetPassword(L"hunter2");
    controller.Submit();

    // Authentication still succeeds; the optional side effects are skipped.
    CHECK_EQ(controller.State(), UiState::LoggedOn);
    CHECK_EQ(controller.LogonSessionId(), 0u);
}

TEST(LogonFlow, MaxFailedAttemptsSendsTheUserBackToTheTileList) {
    Rig rig;
    rig.config.maxFailedAttempts = 3;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    controller->SelectUser(0);

    for (int i = 0; i < 3; ++i) {
        controller->SetPassword(L"wrong");
        controller->Submit();
    }

    CHECK_EQ(controller->State(), UiState::UserList);
    CHECK(controller->SelectedUser() == nullptr);
    CHECK_EQ(rig.auth->attempts, 3);
}

TEST(LogonFlow, DisabledAccountCannotBeSelected) {
    Rig rig;
    rig.db->Find(L"", L"Alice")->account.disabled = true;
    rig.config.users.showDisabledAccounts = true;

    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    const int alice = IndexOf(*controller, L"Alice");
    REQUIRE_GE(alice, 0);
    CHECK_EQ(static_cast<int>(controller->SelectUser(alice)),
             static_cast<int>(TransitionResult::RejectedAccountUnusable));
    CHECK_EQ(controller->State(), UiState::UserList);
    CHECK_EQ(rig.auth->attempts, 0);
}

TEST(LogonFlow, ClickSoundPlaysOnTileSelection) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    controller->SelectUser(0);
    controller->SelectUser(1);

    CHECK_EQ(rig.sound->CountOf(SoundEvent::Click), 2);
}

// ---------------------------------------------------------------------------
// The machine that signs itself in
// ---------------------------------------------------------------------------

namespace {

// One account, no password, which is the whole condition.
struct SoleAccountRig : Rig {
    SoleAccountRig() {
        db->Accounts().clear();
        db->Add(L"Kiosk", L"");
    }
};

} // namespace

TEST(LogonFlow, OneAccountWithNoPasswordSignsInWithoutAsking) {
    SoleAccountRig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    CHECK(controller->SignsInAutomatically());
}

// An unlock stays a screen. A Win+L that let go of itself the moment it was
// pressed would be a lock that does not lock.
TEST(LogonFlow, AnUnlockNeverSignsInByItself) {
    SoleAccountRig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::UnlockWorkstation);

    CHECK_FALSE(controller->SignsInAutomatically());
}

TEST(LogonFlow, TheAutomaticSignInCanBeTurnedOff) {
    SoleAccountRig rig;
    rig.config.users.autoLogonBlankPassword = false;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    CHECK_FALSE(controller->SignsInAutomatically());
    // And the screen it falls back to is the ordinary one.
    CHECK_EQ(controller->State(), UiState::UserList);
}

TEST(LogonFlow, ASecondAccountBringsTheScreenBack) {
    SoleAccountRig rig;
    rig.db->Add(L"Bill", L"hunter2");
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    CHECK_FALSE(controller->SignsInAutomatically());
}

// The account list LogonUI hands over in SetUserArray does not carry the
// blankPassword hint, so a passwordless account arrives looking as though it
// has one. The probe is what recovers the truth - and it is why the welcome
// screen stopped signing itself in.
TEST(LogonFlow, TheProbeSignsInWhenTheAccountHintIsMissing) {
    SoleAccountRig rig;
    rig.db->Accounts().front().account.blankPassword = false; // as SetUserArray leaves it
    rig.prober = std::make_shared<FakeAuthenticator>(rig.db);

    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    CHECK(controller->SignsInAutomatically());
}

// And the other way: the hint is optimistic but the account really does have a
// password. LSA - the probe - overrules the hint, so the screen does not throw
// a doomed blank password at LogonUI.
TEST(LogonFlow, TheProbeRefusesWhenTheAccountReallyHasAPassword) {
    SoleAccountRig rig;
    rig.db->Accounts().front().password = L"hunter2";
    rig.db->Accounts().front().account.blankPassword = true; // stale hint
    rig.prober = std::make_shared<FakeAuthenticator>(rig.db);

    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    CHECK_FALSE(controller->SignsInAutomatically());
}

// A probe is a real logon; it must not happen again every time the account list
// is re-queried (SetUserArray, then the watchdog).
TEST(LogonFlow, TheBlankPasswordProbeIsCached) {
    SoleAccountRig rig;
    rig.db->Accounts().front().account.blankPassword = false;
    rig.prober = std::make_shared<FakeAuthenticator>(rig.db);

    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    controller->RefreshUsers();
    controller->RefreshUsers();

    CHECK(controller->SignsInAutomatically());
    CHECK_EQ(rig.prober->attempts, 1);
}

// The whole point: the automatic sign-in hands LSA the account name with an
// empty password, down the same path a typed one takes.
TEST(LogonFlow, TheAutomaticSignInHandsLsaTheBlankPassword) {
    SoleAccountRig rig;
    rig.auth->asynchronous = true;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    REQUIRE(controller->SignsInAutomatically());

    // What the credential provider does the first time the screen appears.
    CHECK(controller->PrepareAutomaticSignIn());
    CHECK_EQ(controller->State(), UiState::Authenticating);
    REQUIRE(controller->CanSerialize());

    PackedCredential packed;
    REQUIRE(controller->BuildSerialization(0, 8, &packed));
    const KerbLayout layout = KerbLayout::For(8);
    std::u16string password = u"unset";
    REQUIRE(ReadPackedString(packed, layout.passwordOffset, 8, &password));
    CHECK(password.empty());

    // LSA takes it and the desktop comes up.
    controller->OnAuthReported(rig.auth->ResolvePending());
    CHECK_EQ(controller->State(), UiState::LoggedOn);
    CHECK_EQ(rig.observer.logonCompleteCount, 1);
}

TEST(LogonFlow, TheAutomaticSignInCompletesEndToEnd) {
    SoleAccountRig rig; // Kiosk, no password, synchronous LSA
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    REQUIRE(controller->SignsInAutomatically());

    controller->PrepareAutomaticSignIn();

    CHECK_EQ(controller->State(), UiState::LoggedOn);
    CHECK_EQ(rig.observer.logonCompleteCount, 1);
    CHECK(EqualsNoCase(rig.observer.logonUser, L"Kiosk"));
}

// If the blank password is turned down at the real logon, the screen has to
// stop being the busy welcome screen and become an ordinary one - otherwise it
// sits on "Welcome" with no password box and no way in.
TEST(LogonFlow, ARejectedAutomaticSignInFallsBackToThePasswordScreen) {
    SoleAccountRig rig;
    rig.auth->asynchronous = true;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    REQUIRE(controller->SignsInAutomatically());
    REQUIRE(controller->PrepareAutomaticSignIn());

    controller->OnAuthReported(AuthResult::Fail(AuthStatus::BadPassword));

    CHECK_FALSE(controller->SignsInAutomatically()); // disarmed
    CHECK_EQ(controller->State(), UiState::AuthFailed);

    // And it is not armed again when the account list is re-queried.
    controller->RefreshUsers();
    CHECK_FALSE(controller->SignsInAutomatically());
}
