// XPLogin10 - XPLoginWatchdog.exe
//
// A small session-0 service whose entire job is making sure a broken logon
// screen cannot lock anybody out permanently.
//
// It does two things:
//   1. On start it looks at the strike counter. The provider bumps that counter
//      every time it is constructed and clears it on a successful logon, so a
//      high value at service start means the last few logon screens never
//      produced a logon. Past the bypass threshold the service disables the
//      provider outright, and the next boot shows the Windows logon screen.
//   2. It subscribes to session change notifications and clears the counter the
//      moment a real interactive session appears - the out-of-band confirmation
//      that signing in still works, even if the provider itself never got as
//      far as reporting success.
#include "xplogin/HealthMonitor.h"
#include "xplogin/Logging.h"
#include "xplogin/RegistrationPlan.h"
#include "xplogin/StringUtil.h"
#include "xplogin/Win32Factories.h"

#include <windows.h>
#include <wtsapi32.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace xplogin;

namespace {

SERVICE_STATUS g_status = {};
SERVICE_STATUS_HANDLE g_statusHandle = nullptr;
HANDLE g_stopEvent = nullptr;

std::shared_ptr<IStateStore> g_store;
std::shared_ptr<IClock> g_clock;

AppConfig g_config;

void SetState(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exitCode;
    g_status.dwWaitHint = waitHint;
    g_status.dwControlsAccepted =
        (state == SERVICE_START_PENDING)
            ? 0
            : (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN |
               SERVICE_ACCEPT_SESSIONCHANGE);
    if (state == SERVICE_RUNNING || state == SERVICE_STOPPED) {
        g_status.dwCheckPoint = 0;
    } else {
        ++g_status.dwCheckPoint;
    }
    if (g_statusHandle) {
        ::SetServiceStatus(g_statusHandle, &g_status);
    }
}

// A real interactive session appeared: signing in works, so clear the strikes.
void ConfirmLogonWorks(DWORD sessionId) {
    if (!g_store) {
        return;
    }
    HealthMonitor health(g_store, g_clock, HealthPolicy{});
    if (health.StartAttempts() > 0) {
        XPLOG_INFO("session %lu logged on; clearing %u strike(s)",
                   static_cast<unsigned long>(sessionId), health.StartAttempts());
    }
    health.ReportSuccessfulLogon();

    // And nothing else. This used to start the shell "in case Winlogon had
    // not", which is the one thing a service must not do here: the session
    // has only just logged on, so explorer.exe is not running *yet* rather
    // than not running at all. The check passed every time, we won the race
    // every time, and a second explorer.exe with no arguments is a My Computer
    // window opening over the desktop at every sign-in.
}

// Runs once at service start, before anybody has had a chance to log on.
void EvaluateRecoveryOnBoot() {
    if (!g_store) {
        return;
    }
    HealthMonitor health(g_store, g_clock, HealthPolicy{});
    const HealthPolicy policy;

    const uint32_t attempts = health.StartAttempts();
    if (attempts == 0 || health.IsDisabled()) {
        return;
    }

    const HealthAction action =
        HealthMonitor::Decide(DetectBootMode(), false, attempts, policy);

    if (action == HealthAction::Bypass) {
        XPLOG_ERROR("%u logon screens with no successful sign-in; disabling the "
                    "XPLogin provider so Windows takes the logon screen back",
                    attempts);
        health.ForceDisable(L"watchdog: crash loop detected");
        // Removing the filter registration is what actually brings the Windows
        // tiles back, so do it here rather than trusting the provider to read
        // its own configuration on a boot where it may not even load.
        const std::wstring filterKey =
            std::wstring(
                L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication"
                L"\\Credential Provider Filters\\") +
            kFilterClsid;
        ::RegDeleteTreeW(HKEY_LOCAL_MACHINE, filterKey.c_str());
        ::RegDeleteKeyExW(HKEY_LOCAL_MACHINE, filterKey.c_str(), KEY_WOW64_64KEY, 0);
    } else if (action == HealthAction::RunUnfiltered) {
        XPLOG_INFO("%u strike(s) recorded; the provider will run unfiltered",
                   attempts);
    }
}

DWORD WINAPI ServiceControlHandler(DWORD control, DWORD eventType, LPVOID eventData,
                                   LPVOID /*context*/) {
    switch (control) {
        case SERVICE_CONTROL_SHUTDOWN:
        case SERVICE_CONTROL_STOP:
            SetState(SERVICE_STOP_PENDING, NO_ERROR, 3000);
            if (g_stopEvent) {
                ::SetEvent(g_stopEvent);
            }
            return NO_ERROR;

        case SERVICE_CONTROL_SESSIONCHANGE: {
            auto* notification =
                static_cast<WTSSESSION_NOTIFICATION*>(eventData);
            if (!notification) {
                return NO_ERROR;
            }
            if (eventType == WTS_SESSION_LOGON ||
                eventType == WTS_SESSION_UNLOCK) {
                ConfirmLogonWorks(notification->dwSessionId);
            }
            return NO_ERROR;
        }

        case SERVICE_CONTROL_INTERROGATE:
            SetState(g_status.dwCurrentState);
            return NO_ERROR;

        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI ServiceMain(DWORD /*argc*/, LPWSTR* /*argv*/) {
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_statusHandle =
        ::RegisterServiceCtrlHandlerExW(kServiceName, ServiceControlHandler, nullptr);
    if (!g_statusHandle) {
        return;
    }

    SetState(SERVICE_START_PENDING, NO_ERROR, 5000);

    g_store = MakeWin32StateStore();
    g_clock = MakeWin32Clock();

    std::wstring installPath;
    win32::ReadRegistryString(HKEY_LOCAL_MACHINE, kConfigKeyPath, L"InstallPath",
                              &installPath);
    const size_t slash = installPath.find_last_of(L'\\');
    const std::wstring directory =
        slash == std::wstring::npos ? L"" : installPath.substr(0, slash + 1);
    Log::Configure(WideToUtf8(directory + L"XPLoginWatchdog.log"), LogLevel::Info);

    g_config = LoadInstalledConfig(directory);

    XPLOG_INFO("watchdog starting");
    EvaluateRecoveryOnBoot();

    g_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetState(SERVICE_RUNNING);

    // Session notifications arrive through the control handler; this thread
    // just waits. A periodic wake-up also re-checks the counter in case the
    // provider crashed without any session change at all.
    for (;;) {
        const DWORD wait = ::WaitForSingleObject(g_stopEvent, 5 * 60 * 1000);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
        if (wait == WAIT_TIMEOUT) {
            EvaluateRecoveryOnBoot();
        }
    }

    XPLOG_INFO("watchdog stopping");
    if (g_stopEvent) {
        ::CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    SetState(SERVICE_STOPPED);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // `/console` runs the same logic in the foreground, which is how you debug
    // the recovery path without waiting for a reboot.
    if (argc > 1 && EqualsNoCase(argv[1], L"/console")) {
        g_store = MakeWin32StateStore();
        g_clock = MakeWin32Clock();
        Log::Configure("", LogLevel::Debug);
        EvaluateRecoveryOnBoot();
        ::wprintf(L"Recovery evaluation complete.\n");
        return 0;
    }

    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), ServiceMain},
        {nullptr, nullptr},
    };
    if (!::StartServiceCtrlDispatcherW(table)) {
        return static_cast<int>(::GetLastError());
    }
    return 0;
}
