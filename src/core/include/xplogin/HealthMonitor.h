// XPLogin10 - crash-loop protection.
//
// A credential provider that throws inside LogonUI can leave a machine with no
// way to sign in. This is the safety net, and it is deliberately paranoid:
//
//   1. Every time the provider is constructed it writes an "attempt" marker.
//   2. The marker is cleared only after a *successful interactive logon*.
//   3. If the provider starts N times without a single successful logon in
//      between, it stops filtering the stock Microsoft providers and then, one
//      strike later, refuses to instantiate at all - Windows falls back to its
//      own logon UI with no user intervention.
//   4. Safe Mode always bypasses us entirely.
//
// The logic is pure so the recovery path can be tested, which matters more here
// than anywhere else in the project: it is the code you cannot debug live.
#pragma once

#include "xplogin/Interfaces.h"

#include <cstdint>
#include <memory>
#include <string>

namespace xplogin {

enum class BootMode {
    Normal = 0,
    SafeMode,      // GetSystemMetrics(SM_CLEANBOOT) != 0
    SafeModeWithNetworking,
    RecoveryConsole,
};

enum class HealthAction {
    RunNormally = 0,   // draw the XP screen and filter the stock providers
    RunUnfiltered,     // draw the XP screen but leave the Windows tiles visible
    Bypass,            // do not instantiate; Windows shows its own logon UI
};

struct HealthPolicy {
    // Strikes before we stop hiding the Microsoft providers.
    uint32_t unfilterThreshold = 2;
    // Strikes before we disable ourselves completely.
    uint32_t bypassThreshold = 3;
    // A logon that completes within this window counts as "the provider works".
    uint64_t healthyLogonWindowMs = 15ULL * 60ULL * 1000ULL;
    // Master switch from config / group policy.
    bool enabled = true;
};

// Keys used in the state store, exposed so the installer and the watchdog
// service agree on the names.
namespace healthkeys {
constexpr const char* kStartAttempts   = "StartAttempts";
constexpr const char* kLastSuccessMs   = "LastSuccessfulLogonMs";
constexpr const char* kLastStartMs     = "LastStartMs";
constexpr const char* kDisabledFlag    = "Disabled";
constexpr const char* kTotalLogons     = "TotalSuccessfulLogons";
constexpr const char* kLastCrashReason = "LastCrashReason";
} // namespace healthkeys

class HealthMonitor {
public:
    HealthMonitor(std::shared_ptr<IStateStore> store,
                  std::shared_ptr<IClock> clock,
                  HealthPolicy policy = {});

    // Called from DllGetClassObject before anything else happens. Records the
    // attempt and returns what the provider is allowed to do this boot.
    HealthAction BeginProviderStart(BootMode bootMode);

    // Called from ICredentialProvider::GetSerialization once LSA has accepted
    // the credentials, and again from the watchdog service when it observes a
    // real interactive session. Clears the strike counter.
    void ReportSuccessfulLogon();

    // Called once the welcome screen is up and taking input. Clears the strike
    // counter too, and that is the point: the counter exists to catch a
    // provider that dies *while starting*, and once the window is painted that
    // danger has passed.
    //
    // Without this the counter only ever went down on a successful logon, so
    // every ordinary Win+L added a strike that nothing removed - two locks and
    // the filter stood down, three and the provider did. A screen that
    // switches itself off after being shown twice is not crash protection.
    void ReportUiReady();

    // Called from the SEH filter around the UI and from the watchdog when it
    // finds LogonUI has died. Adds a strike immediately instead of waiting for
    // the next boot.
    void ReportCrash(const std::wstring& reason);

    // Administrator escape hatches, used by the installer's /disable and
    // /enable verbs and by the offline recovery .reg file.
    void ForceDisable(const std::wstring& reason);
    void ForceEnable();

    uint32_t StartAttempts() const;
    bool IsDisabled() const;
    std::wstring LastCrashReason() const;

    // Pure decision function, split out so every branch is directly testable.
    static HealthAction Decide(BootMode bootMode,
                               bool disabledFlag,
                               uint32_t startAttempts,
                               const HealthPolicy& policy);

private:
    std::shared_ptr<IStateStore> store_;
    std::shared_ptr<IClock>      clock_;
    HealthPolicy                 policy_;
};

} // namespace xplogin
