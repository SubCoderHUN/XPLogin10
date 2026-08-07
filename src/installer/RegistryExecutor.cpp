#include "RegistryExecutor.h"

#include <windows.h>

#include <sstream>

namespace xplogin::installer {
namespace {

HKEY RootFor(RegHive hive) {
    switch (hive) {
        case RegHive::ClassesRoot: return HKEY_CLASSES_ROOT;
        case RegHive::Users:       return HKEY_USERS;
        case RegHive::LocalMachine: break;
    }
    return HKEY_LOCAL_MACHINE;
}

const wchar_t* HiveName(RegHive hive) {
    switch (hive) {
        case RegHive::ClassesRoot: return L"HKCR";
        case RegHive::Users:       return L"HKU";
        case RegHive::LocalMachine: break;
    }
    return L"HKLM";
}

// RegDeleteTree needs the parent key open; this opens it and deletes the leaf.
LONG DeleteTree(HKEY root, const std::wstring& path) {
    const size_t slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        return ::RegDeleteTreeW(root, path.c_str());
    }

    const std::wstring parent = path.substr(0, slash);
    const std::wstring leaf = path.substr(slash + 1);

    HKEY parentKey = nullptr;
    LONG status = ::RegOpenKeyExW(root, parent.c_str(), 0,
                                  KEY_ALL_ACCESS | KEY_WOW64_64KEY, &parentKey);
    if (status != ERROR_SUCCESS) {
        return status;
    }
    status = ::RegDeleteTreeW(parentKey, leaf.c_str());
    if (status == ERROR_SUCCESS) {
        ::RegDeleteKeyExW(parentKey, leaf.c_str(), KEY_WOW64_64KEY, 0);
    }
    ::RegCloseKey(parentKey);
    return status;
}

LONG ApplyOperation(const RegOperation& op) {
    HKEY root = RootFor(op.hive);

    switch (op.kind) {
        case RegOpKind::CreateKey: {
            HKEY key = nullptr;
            DWORD disposition = 0;
            const LONG status = ::RegCreateKeyExW(
                root, op.path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &key, &disposition);
            if (status == ERROR_SUCCESS) {
                ::RegCloseKey(key);
            }
            return status;
        }

        case RegOpKind::SetValue: {
            HKEY key = nullptr;
            DWORD disposition = 0;
            LONG status = ::RegCreateKeyExW(
                root, op.path.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, &disposition);
            if (status != ERROR_SUCCESS) {
                return status;
            }

            const wchar_t* valueName =
                op.valueName.empty() ? nullptr : op.valueName.c_str();
            if (op.valueType == RegValueType::Dword) {
                const DWORD data = op.dwordData;
                status = ::RegSetValueExW(key, valueName, 0, REG_DWORD,
                                          reinterpret_cast<const BYTE*>(&data),
                                          sizeof(data));
            } else {
                const DWORD type = op.valueType == RegValueType::ExpandString
                                       ? REG_EXPAND_SZ
                                       : REG_SZ;
                const DWORD bytes = static_cast<DWORD>(
                    (op.stringData.size() + 1) * sizeof(wchar_t));
                status = ::RegSetValueExW(
                    key, valueName, 0, type,
                    reinterpret_cast<const BYTE*>(op.stringData.c_str()), bytes);
            }
            ::RegCloseKey(key);
            return status;
        }

        case RegOpKind::DeleteValue: {
            HKEY key = nullptr;
            LONG status = ::RegOpenKeyExW(root, op.path.c_str(), 0,
                                          KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
            if (status != ERROR_SUCCESS) {
                return status;
            }
            status = ::RegDeleteValueW(
                key, op.valueName.empty() ? nullptr : op.valueName.c_str());
            ::RegCloseKey(key);
            return status;
        }

        case RegOpKind::DeleteKeyTree:
            return DeleteTree(root, op.path);
    }
    return ERROR_INVALID_FUNCTION;
}

} // namespace

bool RegistryExecutor::KeyExists(RegHive hive, const std::wstring& path) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(RootFor(hive), path.c_str(), 0,
                        KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return false;
    }
    ::RegCloseKey(key);
    return true;
}

ExecutionReport RegistryExecutor::Execute(const std::vector<RegOperation>& plan) {
    ExecutionReport report;
    std::wostringstream transcript;

    for (const RegOperation& op : plan) {
        const LONG status = ApplyOperation(op);
        const bool ok = status == ERROR_SUCCESS ||
                        (op.optional && (status == ERROR_FILE_NOT_FOUND ||
                                         status == ERROR_PATH_NOT_FOUND));

        transcript << (ok ? L"  ok   " : L"  FAIL ") << HiveName(op.hive) << L"\\"
                   << op.path;
        if (!op.valueName.empty()) {
            transcript << L" [" << op.valueName << L"]";
        }
        if (!ok) {
            transcript << L"  (error " << status << L")";
        }
        transcript << L"\r\n";

        if (!ok) {
            report.success = false;
            report.transcript = transcript.str();
            return report;
        }
        ++report.completed;
    }

    report.transcript = transcript.str();
    return report;
}

// ---------------------------------------------------------------------------
// The watchdog service
// ---------------------------------------------------------------------------

bool InstallWatchdogService(const std::wstring& exePath) {
    SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!manager) {
        return false;
    }

    const std::wstring commandLine = L"\"" + exePath + L"\" /service";

    SC_HANDLE service = ::CreateServiceW(
        manager, kServiceName, L"XPLogin10 Logon Watchdog", SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        commandLine.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);

    if (!service && ::GetLastError() == ERROR_SERVICE_EXISTS) {
        service = ::OpenServiceW(manager, kServiceName, SERVICE_ALL_ACCESS);
    }
    if (!service) {
        ::CloseServiceHandle(manager);
        return false;
    }

    SERVICE_DESCRIPTIONW description = {};
    wchar_t text[] = L"Watches the XPLogin10 credential provider and restores the "
                     L"Windows logon screen if it fails to start.";
    description.lpDescription = text;
    ::ChangeServiceConfig2W(service, SERVICE_CONFIG_DESCRIPTION, &description);

    // Restart on failure, but give up after three tries rather than looping.
    SC_ACTION actions[3] = {};
    for (SC_ACTION& action : actions) {
        action.Type = SC_ACTION_RESTART;
        action.Delay = 60000;
    }
    SERVICE_FAILURE_ACTIONSW failure = {};
    failure.dwResetPeriod = 86400;
    failure.cActions = ARRAYSIZE(actions);
    failure.lpsaActions = actions;
    ::ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);

    const BOOL started = ::StartServiceW(service, 0, nullptr);
    const DWORD error = ::GetLastError();

    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    return started || error == ERROR_SERVICE_ALREADY_RUNNING;
}

// ControlService only *asks* the service to stop; the process keeps its image
// locked until it actually exits. Everything that overwrites or deletes the
// binary has to wait for that, which is what this does.
bool WaitForServiceToStop(SC_HANDLE service, DWORD timeoutMs) {
    const DWORD deadline = ::GetTickCount() + timeoutMs;
    SERVICE_STATUS status = {};
    while (::QueryServiceStatus(service, &status)) {
        if (status.dwCurrentState == SERVICE_STOPPED) {
            return true;
        }
        if (::GetTickCount() >= deadline) {
            return false;
        }
        ::Sleep(200);
    }
    return false;
}

bool StopWatchdogService() {
    SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        return false;
    }
    SC_HANDLE service = ::OpenServiceW(
        manager, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!service) {
        // Not installed. That is the state the caller wanted.
        ::CloseServiceHandle(manager);
        return true;
    }

    SERVICE_STATUS status = {};
    ::ControlService(service, SERVICE_CONTROL_STOP, &status);
    const bool stopped = WaitForServiceToStop(service, 15000);

    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    return stopped;
}

bool RemoveWatchdogService() {
    SC_HANDLE manager = ::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        return false;
    }
    SC_HANDLE service =
        ::OpenServiceW(manager, kServiceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
    if (!service) {
        ::CloseServiceHandle(manager);
        return true; // not installed is the desired end state
    }

    SERVICE_STATUS status = {};
    ::ControlService(service, SERVICE_CONTROL_STOP, &status);
    // Wait rather than deleting a service that is still running: DeleteService
    // only marks it, and the exe stays locked until the process exits - which
    // is what then makes removing the install directory fail.
    WaitForServiceToStop(service, 15000);
    const BOOL deleted = ::DeleteService(service);

    ::CloseServiceHandle(service);
    ::CloseServiceHandle(manager);
    return deleted != FALSE;
}

} // namespace xplogin::installer
