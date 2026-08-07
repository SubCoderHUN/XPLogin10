// Integration tests: session handling.
//
// Required coverage: "session kezeles szimulacioja". These drive a simulated
// Terminal Services session table through the transitions a real machine makes:
// first logon, Win+L, unlock, fast user switching and logoff.
#include "xplogin/LogonController.h"
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

        auto controller = std::make_unique<LogonController>(deps, config);
        controller->SetObserver(&observer);
        return controller;
    }
};

// Signs `user` in through the full UI flow. Returns true on success.
bool SignIn(LogonController& controller, const wchar_t* user, const wchar_t* password) {
    const int index = -1 + [&] {
        int found = 0;
        for (size_t i = 0; i < controller.Users().size(); ++i) {
            if (EqualsNoCase(controller.Users()[i].username, user)) {
                found = static_cast<int>(i) + 1;
                break;
            }
        }
        return found;
    }();
    if (index < 0) {
        return false;
    }
    controller.SelectUser(index);
    controller.SetPassword(password);
    controller.Submit();
    return controller.State() == UiState::LoggedOn;
}

} // namespace

// ---------------------------------------------------------------------------
// First logon
// ---------------------------------------------------------------------------

TEST(SessionSimulation, SigningInNeverStartsAShell) {
    // Winlogon starts the shell. Nothing here may, and there is no longer an
    // operation for it - this is the invariant that replaced one.
    //
    // The version that did start one was written as a fallback "in case
    // Winlogon had not", guarded by a check for a running explorer.exe. The
    // check was made at the moment the session logged on, which is before
    // Winlogon gets there, so it passed every time: every sign-in launched a
    // second explorer.exe with no arguments, and that opens a My Computer
    // window over the user's desktop.
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    REQUIRE(SignIn(*controller, L"Bill", L"hunter2"));

    CHECK_EQ(rig.observer.logonCompleteCount, 1);
    CHECK_EQ(rig.observer.logonUser, std::wstring(L"Bill"));
    // The session table is left exactly as it was found.
    CHECK_FALSE(rig.sessions->Get(1)->shellRunning);
}

TEST(SessionSimulation, FailedLogonNeverTouchesTheSessionTable) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    controller->SelectUser(controller->Users().size() > 1 ? 1 : 0);
    controller->SetPassword(L"wrong");
    controller->Submit();

    CHECK_EQ(controller->State(), UiState::AuthFailed);
    CHECK_EQ(rig.sessions->connectCount, 0);
    CHECK_EQ(rig.observer.logonCompleteCount, 0);
}

TEST(SessionSimulation, LogonPlaysTheXpLogonSound) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    REQUIRE(SignIn(*controller, L"Bill", L"hunter2"));

    CHECK_EQ(rig.sound->CountOf(SoundEvent::Logon), 1);
    CHECK_EQ(rig.sound->CountOf(SoundEvent::Error), 0);
}

TEST(SessionSimulation, FailedLogonPlaysTheErrorSoundInstead) {
    Rig rig;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);

    controller->SelectUser(0);
    controller->SetPassword(L"nope");
    controller->Submit();

    CHECK_EQ(rig.sound->CountOf(SoundEvent::Logon), 0);
    CHECK_EQ(rig.sound->CountOf(SoundEvent::Error), 1);
}

TEST(SessionSimulation, SoundsCanBeDisabledEntirely) {
    Rig rig;
    rig.config.sounds.enabled = false;
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    REQUIRE(SignIn(*controller, L"Bill", L"hunter2"));

    CHECK(rig.sound->played.empty());
}

// ---------------------------------------------------------------------------
// Lock and unlock
// ---------------------------------------------------------------------------

TEST(SessionSimulation, LockingMarksTheSessionLocked) {
    Rig rig;
    rig.sessions->AssignCurrentSession(L"WINBOX", L"Bill");
    CHECK(rig.sessions->LockSession(1));

    const SessionInfo* session = rig.sessions->Get(1);
    REQUIRE(session != nullptr);
    CHECK(session->locked);
    CHECK_EQ(rig.sessions->lockCount, 1);
}

TEST(SessionSimulation, UnlockScreenShowsOnlyTheLockingUser) {
    Rig rig;
    auto controller = rig.Make();

    // Win+L: the locked session belongs to Bill.
    const std::wstring billSid =
        rig.db->Find(L"", L"Bill")->account.sid;
    controller->SetLockedUserSid(billSid);
    controller->Initialize(UsageScenario::UnlockWorkstation);

    CHECK_EQ(controller->State(), UiState::PasswordEntry);
    REQUIRE(controller->SelectedUser() != nullptr);
    CHECK_EQ(controller->SelectedUser()->username, std::wstring(L"Bill"));
    // Alice must not be offered a way into Bill's locked session.
    CHECK_FALSE(controller->Snapshot().backButtonVisible);
}

TEST(SessionSimulation, UnlockFallsBackToTheUserNameWhenTheSidIsUnknown) {
    Rig rig;
    auto controller = rig.Make();

    controller->SetLockedUserSid(L"S-1-5-21-does-not-exist");
    controller->SetLockedUserName(L"WINBOX\\Alice");
    controller->Initialize(UsageScenario::UnlockWorkstation);

    REQUIRE(controller->SelectedUser() != nullptr);
    CHECK_EQ(controller->SelectedUser()->username, std::wstring(L"Alice"));
}

TEST(SessionSimulation, WrongPasswordLeavesTheSessionLocked) {
    Rig rig;
    rig.sessions->AssignCurrentSession(L"WINBOX", L"Bill");
    rig.sessions->LockSession(1);

    auto controller = rig.Make();
    controller->SetLockedUserSid(rig.db->Find(L"", L"Bill")->account.sid);
    controller->Initialize(UsageScenario::UnlockWorkstation);

    controller->SetPassword(L"wrong");
    controller->Submit();

    CHECK_EQ(controller->State(), UiState::AuthFailed);
    CHECK(rig.sessions->Get(1)->locked);
    CHECK_EQ(rig.sessions->connectCount, 0);
}

TEST(SessionSimulation, CorrectPasswordUnlocksAndReconnects) {
    Rig rig;
    rig.sessions->AssignCurrentSession(L"WINBOX", L"Bill");
    rig.sessions->LockSession(1);
    rig.sessions->Get(1)->shellRunning = true;

    auto controller = rig.Make();
    controller->SetLockedUserSid(rig.db->Find(L"", L"Bill")->account.sid);
    controller->Initialize(UsageScenario::UnlockWorkstation);

    controller->SetPassword(L"hunter2");
    controller->Submit();

    CHECK_EQ(controller->State(), UiState::LoggedOn);
    const SessionInfo* session = rig.sessions->Get(1);
    REQUIRE(session != nullptr);
    CHECK_FALSE(session->locked);
    CHECK_EQ(static_cast<int>(session->state), static_cast<int>(SessionState::Active));
    // The desktop was already there and stays exactly as it was.
    CHECK(session->shellRunning);
}

TEST(SessionSimulation, UnlockRetriesUntilTheRightPassword) {
    Rig rig;
    rig.sessions->AssignCurrentSession(L"WINBOX", L"Bill");
    rig.sessions->LockSession(1);

    auto controller = rig.Make();
    controller->SetLockedUserSid(rig.db->Find(L"", L"Bill")->account.sid);
    controller->Initialize(UsageScenario::UnlockWorkstation);

    for (int attempt = 0; attempt < 3; ++attempt) {
        controller->SetPassword(L"wrong");
        controller->Submit();
        CHECK_EQ(controller->State(), UiState::AuthFailed);
    }
    CHECK_EQ(controller->Snapshot().failedAttempts, 3);

    controller->SetPassword(L"hunter2");
    controller->Submit();
    CHECK_EQ(controller->State(), UiState::LoggedOn);
}

// ---------------------------------------------------------------------------
// Fast user switching
// ---------------------------------------------------------------------------

TEST(SessionSimulation, SecondUserGetsTheirOwnSession) {
    Rig rig;
    // Bill is already signed in and disconnected (he used Switch User).
    const uint32_t billSession = rig.sessions->CreateUserSession(L"WINBOX", L"Bill");
    rig.sessions->Get(billSession)->state = SessionState::Disconnected;
    rig.sessions->Get(billSession)->shellRunning = true;

    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    REQUIRE(SignIn(*controller, L"Alice", L"opensesame"));

    // Alice lands in the console session, not in Bill's.
    CHECK_EQ(controller->LogonSessionId(), rig.sessions->currentSessionId);
    CHECK_NE(controller->LogonSessionId(), billSession);
    CHECK_EQ(static_cast<int>(rig.sessions->Get(billSession)->state),
             static_cast<int>(SessionState::Disconnected));
}

TEST(SessionSimulation, ReturningUserReconnectsToTheirExistingSession) {
    Rig rig;
    const uint32_t billSession = rig.sessions->CreateUserSession(L"WINBOX", L"Bill");
    rig.sessions->Get(billSession)->state = SessionState::Disconnected;
    rig.sessions->Get(billSession)->shellRunning = true;

    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    REQUIRE(SignIn(*controller, L"Bill", L"hunter2"));

    CHECK_EQ(controller->LogonSessionId(), billSession);
    CHECK_EQ(rig.sessions->connectCount, 1);
    CHECK_EQ(static_cast<int>(rig.sessions->Get(billSession)->state),
             static_cast<int>(SessionState::Active));
    // His programs are still running, and nothing of ours touched them.
    CHECK(rig.sessions->Get(billSession)->shellRunning);
}

TEST(SessionSimulation, ConnectingOneSessionDisconnectsTheActiveOne) {
    Rig rig;
    const uint32_t alice = rig.sessions->CreateUserSession(L"WINBOX", L"Alice");
    const uint32_t bill = rig.sessions->CreateUserSession(L"WINBOX", L"Bill");
    rig.sessions->Get(bill)->state = SessionState::Disconnected;

    CHECK(rig.sessions->ConnectSession(bill));

    CHECK_EQ(static_cast<int>(rig.sessions->Get(bill)->state),
             static_cast<int>(SessionState::Active));
    CHECK_EQ(static_cast<int>(rig.sessions->Get(alice)->state),
             static_cast<int>(SessionState::Disconnected));
}

TEST(SessionSimulation, FindsSessionsByNameAndIgnoresOthers) {
    Rig rig;
    const uint32_t bill = rig.sessions->CreateUserSession(L"WINBOX", L"Bill");
    rig.sessions->CreateUserSession(L"CONTOSO", L"alice");

    CHECK_EQ(rig.sessions->FindSessionForUser(L"WINBOX", L"Bill"), bill);
    CHECK_EQ(rig.sessions->FindSessionForUser(L"WINBOX", L"bill"), bill);
    CHECK_EQ(rig.sessions->FindSessionForUser(L"", L"Bill"), bill);
    CHECK_EQ(rig.sessions->FindSessionForUser(L"CONTOSO", L"Bill"), 0u);
    CHECK_EQ(rig.sessions->FindSessionForUser(L"WINBOX", L"Nobody"), 0u);
    // Session 0 is the services session and is never a logon target.
    CHECK_EQ(rig.sessions->FindSessionForUser(L"", L""), 0u);
}

TEST(SessionSimulation, LoggedOffSessionIsForgotten) {
    Rig rig;
    const uint32_t bill = rig.sessions->CreateUserSession(L"WINBOX", L"Bill");
    CHECK_EQ(rig.sessions->FindSessionForUser(L"WINBOX", L"Bill"), bill);

    rig.sessions->LogOff(bill);
    CHECK_EQ(rig.sessions->FindSessionForUser(L"WINBOX", L"Bill"), 0u);

    // Signing in again is a fresh logon rather than a reconnect.
    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    REQUIRE(SignIn(*controller, L"Bill", L"hunter2"));
    CHECK_EQ(rig.sessions->connectCount, 0);
}

TEST(SessionSimulation, ConnectingAnUnknownSessionFails) {
    Rig rig;
    CHECK_FALSE(rig.sessions->ConnectSession(9999));
    CHECK_FALSE(rig.sessions->LockSession(9999));
}

// ---------------------------------------------------------------------------
// Health bookkeeping tied to real logons
// ---------------------------------------------------------------------------

TEST(SessionSimulation, SuccessfulLogonClearsTheCrashStrikeCounter) {
    Rig rig;
    rig.store->WriteU64(healthkeys::kStartAttempts, 2);

    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    REQUIRE(SignIn(*controller, L"Bill", L"hunter2"));

    uint64_t attempts = 99;
    CHECK(rig.store->ReadU64(healthkeys::kStartAttempts, &attempts));
    CHECK_EQ(attempts, uint64_t(0));
}

TEST(SessionSimulation, FailedLogonLeavesTheStrikeCounterAlone) {
    Rig rig;
    rig.store->WriteU64(healthkeys::kStartAttempts, 2);

    auto controller = rig.Make();
    controller->Initialize(UsageScenario::Logon);
    controller->SelectUser(0);
    controller->SetPassword(L"wrong");
    controller->Submit();

    uint64_t attempts = 0;
    CHECK(rig.store->ReadU64(healthkeys::kStartAttempts, &attempts));
    CHECK_EQ(attempts, uint64_t(2));
}
