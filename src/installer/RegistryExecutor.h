// XPLogin10 - applies a validated RegistrationPlan to the real registry.
//
// The plan decides *what*; this decides *how*, and records a transcript so a
// failed install can be read after the fact from an elevated prompt.
#pragma once

#include "xplogin/RegistrationPlan.h"

#include <string>

namespace xplogin::installer {

struct ExecutionReport {
    bool         success = true;
    size_t       completed = 0;
    std::wstring transcript;
};

class RegistryExecutor {
public:
    // Stops at the first non-optional failure. Optional operations that fail
    // are logged and skipped.
    ExecutionReport Execute(const std::vector<RegOperation>& plan);

    static bool KeyExists(RegHive hive, const std::wstring& path);
};

// Registers XPLoginWatchdog.exe as an auto-start service and starts it.
bool InstallWatchdogService(const std::wstring& exePath);
bool RemoveWatchdogService();

// Stops the watchdog and waits for it to actually be gone. Needed before
// overwriting XPLoginWatchdog.exe: a running image is locked, and the copy
// fails with ERROR_SHARING_VIOLATION rather than anything that sounds like
// "the service is still running". Returns true when the service is not
// running afterwards, including when it was never installed.
bool StopWatchdogService();

} // namespace xplogin::installer
