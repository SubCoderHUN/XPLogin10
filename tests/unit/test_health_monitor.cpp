// Unit tests: crash-loop protection.
//
// This is the code that decides whether a machine with a broken logon screen
// can still be logged into. Every branch is covered on purpose.
#include "xplogin/HealthMonitor.h"
#include "Mocks.h"
#include "xptest.h"

#include <memory>

using namespace xplogin;
using namespace xplogin::testing;

namespace {

int A(HealthAction action) { return static_cast<int>(action); }

struct Fixture {
    std::shared_ptr<FakeStateStore> store = std::make_shared<FakeStateStore>();
    std::shared_ptr<FakeClock> clock = std::make_shared<FakeClock>();
    HealthPolicy policy;

    HealthMonitor Make() { return HealthMonitor(store, clock, policy); }
};

} // namespace

// ---------------------------------------------------------------------------
// Decide() - the pure decision function
// ---------------------------------------------------------------------------

TEST(HealthMonitor, CleanMachineRunsNormally) {
    HealthPolicy policy;
    CHECK_EQ(A(HealthMonitor::Decide(BootMode::Normal, false, 0, policy)),
             A(HealthAction::RunNormally));
}

TEST(HealthMonitor, FirstStrikeStillRunsNormally) {
    HealthPolicy policy; // unfilter at 2, bypass at 3
    CHECK_EQ(A(HealthMonitor::Decide(BootMode::Normal, false, 1, policy)),
             A(HealthAction::RunNormally));
}

TEST(HealthMonitor, SecondStrikeStopsHidingTheWindowsTiles) {
    HealthPolicy policy;
    // The XP screen still comes up, but the stock provider is visible behind it
    // so there is always a way in.
    CHECK_EQ(A(HealthMonitor::Decide(BootMode::Normal, false, 2, policy)),
             A(HealthAction::RunUnfiltered));
}

TEST(HealthMonitor, ThirdStrikeDisablesTheProviderEntirely) {
    HealthPolicy policy;
    CHECK_EQ(A(HealthMonitor::Decide(BootMode::Normal, false, 3, policy)),
             A(HealthAction::Bypass));
    CHECK_EQ(A(HealthMonitor::Decide(BootMode::Normal, false, 99, policy)),
             A(HealthAction::Bypass));
}

TEST(HealthMonitor, SafeModeAlwaysBypasses) {
    HealthPolicy policy;
    // Safe Mode is the administrator's way back in; loading there could brick
    // the recovery path itself.
    CHECK_EQ(A(HealthMonitor::Decide(BootMode::SafeMode, false, 0, policy)),
             A(HealthAction::Bypass));
    CHECK_EQ(
        A(HealthMonitor::Decide(BootMode::SafeModeWithNetworking, false, 0, policy)),
        A(HealthAction::Bypass));
    CHECK_EQ(A(HealthMonitor::Decide(BootMode::RecoveryConsole, false, 0, policy)),
             A(HealthAction::Bypass));
}

TEST(HealthMonitor, DisabledFlagBypassesRegardlessOfStrikes) {
    HealthPolicy policy;
    CHECK_EQ(A(HealthMonitor::Decide(BootMode::Normal, true, 0, policy)),
             A(HealthAction::Bypass));
}

TEST(HealthMonitor, MasterSwitchOffBypasses) {
    HealthPolicy policy;
    policy.enabled = false;
    CHECK_EQ(A(HealthMonitor::Decide(BootMode::Normal, false, 0, policy)),
             A(HealthAction::Bypass));
}

TEST(HealthMonitor, ThresholdsAreConfigurable) {
    HealthPolicy strict;
    strict.unfilterThreshold = 1;
    strict.bypassThreshold = 2;
    CHECK_EQ(A(HealthMonitor::Decide(BootMode::Normal, false, 1, strict)),
             A(HealthAction::RunUnfiltered));
    CHECK_EQ(A(HealthMonitor::Decide(BootMode::Normal, false, 2, strict)),
             A(HealthAction::Bypass));

    HealthPolicy lenient;
    lenient.unfilterThreshold = 0; // disabled
    lenient.bypassThreshold = 10;
    CHECK_EQ(A(HealthMonitor::Decide(BootMode::Normal, false, 5, lenient)),
             A(HealthAction::RunNormally));
}

// ---------------------------------------------------------------------------
// Stateful behaviour
// ---------------------------------------------------------------------------

TEST(HealthMonitor, StartRecordsAStrikeBeforePainting) {
    Fixture fixture;
    HealthMonitor monitor = fixture.Make();

    CHECK_EQ(A(monitor.BeginProviderStart(BootMode::Normal)),
             A(HealthAction::RunNormally));
    // The counter goes up immediately, so a hang during painting still counts.
    CHECK_EQ(monitor.StartAttempts(), 1u);
}

TEST(HealthMonitor, SuccessfulLogonClearsTheStrikeCounter) {
    Fixture fixture;
    HealthMonitor monitor = fixture.Make();

    monitor.BeginProviderStart(BootMode::Normal);
    monitor.BeginProviderStart(BootMode::Normal);
    CHECK_EQ(monitor.StartAttempts(), 2u);

    monitor.ReportSuccessfulLogon();
    CHECK_EQ(monitor.StartAttempts(), 0u);

    uint64_t total = 0;
    CHECK(fixture.store->ReadU64(healthkeys::kTotalLogons, &total));
    CHECK_EQ(total, uint64_t(1));
}

TEST(HealthMonitor, ShowingTheScreenClearsTheStrikeCounter) {
    // Regression. The counter came down only on a successful logon, so every
    // ordinary lock added a strike that nothing removed: two Win+L cycles and
    // the filter stood down, three and the provider did. A logon screen that
    // switches itself off after being shown twice is not crash protection, it
    // is a countdown.
    Fixture fixture;
    HealthMonitor monitor = fixture.Make();

    monitor.BeginProviderStart(BootMode::Normal);
    monitor.BeginProviderStart(BootMode::Normal);
    CHECK_EQ(monitor.StartAttempts(), 2u);

    monitor.ReportUiReady();
    CHECK_EQ(monitor.StartAttempts(), 0u);

    // The next start is back to a clean verdict rather than one strike from
    // standing down.
    CHECK_EQ(A(monitor.BeginProviderStart(BootMode::Normal)),
             A(HealthAction::RunNormally));
}

TEST(HealthMonitor, ShowingTheScreenIsNotCountedAsALogon) {
    // ReportUiReady clears the strikes but must not touch the logon tally or
    // the last-success timestamp: the screen being drawn is not somebody
    // signing in, and the watchdog reads those to judge whether this machine
    // is actually usable.
    Fixture fixture;
    HealthMonitor monitor = fixture.Make();

    monitor.BeginProviderStart(BootMode::Normal);
    monitor.ReportUiReady();

    uint64_t total = 0;
    fixture.store->ReadU64(healthkeys::kTotalLogons, &total);
    CHECK_EQ(total, uint64_t(0));

    uint64_t lastSuccess = 0;
    fixture.store->ReadU64(healthkeys::kLastSuccessMs, &lastSuccess);
    CHECK_EQ(lastSuccess, uint64_t(0));

    // A real logon still records both.
    monitor.ReportSuccessfulLogon();
    fixture.store->ReadU64(healthkeys::kTotalLogons, &total);
    CHECK_EQ(total, uint64_t(1));
}

TEST(HealthMonitor, ThreeStartsWithoutALogonEndInBypass) {
    Fixture fixture;
    HealthMonitor monitor = fixture.Make();

    CHECK_EQ(A(monitor.BeginProviderStart(BootMode::Normal)),
             A(HealthAction::RunNormally));   // strike 0 -> 1
    CHECK_EQ(A(monitor.BeginProviderStart(BootMode::Normal)),
             A(HealthAction::RunNormally));   // strike 1 -> 2
    CHECK_EQ(A(monitor.BeginProviderStart(BootMode::Normal)),
             A(HealthAction::RunUnfiltered)); // strike 2 -> 3
    CHECK_EQ(A(monitor.BeginProviderStart(BootMode::Normal)),
             A(HealthAction::Bypass));        // and now we stand down

    // Bypass must not keep incrementing; the machine has already recovered
    // control and there is nothing left to count.
    CHECK_EQ(monitor.StartAttempts(), 3u);
}

TEST(HealthMonitor, RecoveryAfterASingleGoodLogon) {
    Fixture fixture;
    HealthMonitor monitor = fixture.Make();

    monitor.BeginProviderStart(BootMode::Normal);
    monitor.BeginProviderStart(BootMode::Normal);
    CHECK_EQ(A(monitor.BeginProviderStart(BootMode::Normal)),
             A(HealthAction::RunUnfiltered));

    // The user got in through the unfiltered Windows tile.
    monitor.ReportSuccessfulLogon();
    CHECK_EQ(A(monitor.BeginProviderStart(BootMode::Normal)),
             A(HealthAction::RunNormally));
}

TEST(HealthMonitor, ReportedCrashCountsAsAStrikeImmediately) {
    Fixture fixture;
    HealthMonitor monitor = fixture.Make();

    monitor.BeginProviderStart(BootMode::Normal);
    monitor.ReportCrash(L"unhandled exception in XpRenderer::Paint");
    CHECK_EQ(monitor.StartAttempts(), 2u);
    CHECK_EQ(monitor.LastCrashReason(),
             std::wstring(L"unhandled exception in XpRenderer::Paint"));
}

TEST(HealthMonitor, SuccessfulLogonClearsTheCrashReason) {
    Fixture fixture;
    HealthMonitor monitor = fixture.Make();

    monitor.ReportCrash(L"boom");
    CHECK_FALSE(monitor.LastCrashReason().empty());
    monitor.ReportSuccessfulLogon();
    CHECK(monitor.LastCrashReason().empty());
}

TEST(HealthMonitor, ForceDisableAndForceEnable) {
    Fixture fixture;
    HealthMonitor monitor = fixture.Make();

    CHECK_FALSE(monitor.IsDisabled());
    monitor.ForceDisable(L"administrator ran xplogin-install /disable");
    CHECK(monitor.IsDisabled());
    CHECK_EQ(A(monitor.BeginProviderStart(BootMode::Normal)), A(HealthAction::Bypass));

    monitor.ForceEnable();
    CHECK_FALSE(monitor.IsDisabled());
    CHECK_EQ(monitor.StartAttempts(), 0u);
    CHECK_EQ(A(monitor.BeginProviderStart(BootMode::Normal)),
             A(HealthAction::RunNormally));
}

TEST(HealthMonitor, TimestampsAreRecorded) {
    Fixture fixture;
    HealthMonitor monitor = fixture.Make();

    fixture.clock->SetNow(5'000'000);
    monitor.BeginProviderStart(BootMode::Normal);
    uint64_t startedAt = 0;
    CHECK(fixture.store->ReadU64(healthkeys::kLastStartMs, &startedAt));
    CHECK_EQ(startedAt, uint64_t(5'000'000));

    fixture.clock->Advance(4'200);
    monitor.ReportSuccessfulLogon();
    uint64_t succeededAt = 0;
    CHECK(fixture.store->ReadU64(healthkeys::kLastSuccessMs, &succeededAt));
    CHECK_EQ(succeededAt, uint64_t(5'004'200));
}

TEST(HealthMonitor, SurvivesAMissingStateStore) {
    // If the registry is unreachable we must still return a usable decision
    // rather than crashing inside LogonUI.
    HealthMonitor monitor(nullptr, nullptr, HealthPolicy{});
    CHECK_EQ(A(monitor.BeginProviderStart(BootMode::Normal)),
             A(HealthAction::RunNormally));
    monitor.ReportSuccessfulLogon();
    monitor.ReportCrash(L"no store");
    CHECK_EQ(monitor.StartAttempts(), 0u);
    CHECK_FALSE(monitor.IsDisabled());
}

TEST(HealthMonitor, BypassDoesNotWriteToTheStore) {
    Fixture fixture;
    fixture.policy.enabled = false;
    HealthMonitor monitor = fixture.Make();

    const int writesBefore = fixture.store->writes;
    CHECK_EQ(A(monitor.BeginProviderStart(BootMode::Normal)), A(HealthAction::Bypass));
    CHECK_EQ(fixture.store->writes, writesBefore);
}
