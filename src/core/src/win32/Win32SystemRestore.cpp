// XPLogin10 - System Restore checkpoint before an install.
//
// SRSetRestorePointW lives in SrClient.dll, which is absent on Server SKUs, so
// it is resolved at run time rather than linked. Two behaviours are worth
// knowing about:
//
//   * If System Protection is off for the system drive, the call fails. That is
//     a warning, not an error - the install still has three other ways back.
//   * Windows 10/11 silently skip a checkpoint if one was made in the last
//     24 hours (SystemRestorePointCreationFrequency, default 1440 minutes).
//     SuspendRateLimit() clears that for the duration of the install and
//     RestoreRateLimit() puts the original value back, including deleting the
//     value again if it did not exist before.
#include "Win32Common.h"

#include "xplogin/Interfaces.h"
#include "xplogin/Logging.h"
#include "xplogin/Win32Factories.h"

#include <objbase.h>

namespace xplogin {
namespace {

const wchar_t* const kSystemRestoreKey =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SystemRestore";
const wchar_t* const kFrequencyValue = L"SystemRestorePointCreationFrequency";

// From srrestoreptapi.h, declared here so the core does not need that header.
constexpr DWORD kBeginSystemChange = 100;
constexpr DWORD kEndSystemChange   = 101;
constexpr DWORD kApplicationInstall = 0;

#pragma pack(push, 8)
struct RestorePointInfoW {
    DWORD  dwEventType;
    DWORD  dwRestorePtType;
    INT64  llSequenceNumber;
    WCHAR  szDescription[256];
};
struct StateMgrStatus {
    DWORD  nStatus;
    INT64  llSequenceNumber;
};
#pragma pack(pop)

using SRSetRestorePointWFn = BOOL(WINAPI*)(RestorePointInfoW*, StateMgrStatus*);

// SRSetRestorePointW reaches the System Restore service through COM, so the
// calling thread needs an apartment. A caller should not have to know that:
// without one the call faults inside srclient.dll rather than returning an
// error, which takes the whole process with it.
class ScopedCom {
public:
    ScopedCom() {
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        // S_FALSE means this thread was already initialised - still ours to
        // balance. RPC_E_CHANGED_MODE means somebody else picked a different
        // apartment, which works fine here but must not be torn down by us.
        ours_ = SUCCEEDED(hr);
    }
    ~ScopedCom() {
        if (ours_) {
            ::CoUninitialize();
        }
    }
    ScopedCom(const ScopedCom&) = delete;
    ScopedCom& operator=(const ScopedCom&) = delete;

private:
    bool ours_ = false;
};

// The restore point is the one install step that is explicitly allowed to
// fail, so a fault inside a dynamically loaded third-party DLL must not be
// fatal. __try cannot share a function with anything that needs unwinding,
// which is why this call sits on its own with no C++ objects in scope.
BOOL CallSetRestorePoint(SRSetRestorePointWFn function, RestorePointInfoW* info,
                         StateMgrStatus* status, DWORD* exceptionCode) {
    *exceptionCode = 0;
    __try {
        return function(info, status);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *exceptionCode = GetExceptionCode();
        return FALSE;
    }
}

class Win32SystemRestore : public ISystemRestore {
public:
    ~Win32SystemRestore() override {
        RestoreRateLimit();
        if (module_) {
            ::FreeLibrary(module_);
        }
    }

    bool IsAvailable() override { return Resolve() != nullptr; }

    RestorePointResult CreateCheckpoint(const std::wstring& description,
                                        int64_t* sequenceNumber) override {
        SRSetRestorePointWFn setRestorePoint = Resolve();
        if (!setRestorePoint) {
            XPLOG_INFO("SrClient.dll not present; skipping the restore point");
            return RestorePointResult::NotSupported;
        }

        RestorePointInfoW info = {};
        info.dwEventType = kBeginSystemChange;
        info.dwRestorePtType = kApplicationInstall;
        info.llSequenceNumber = 0;
        ::wcsncpy_s(info.szDescription, description.c_str(), _TRUNCATE);

        StateMgrStatus status = {};
        ::SetLastError(ERROR_SUCCESS);

        ScopedCom com;
        DWORD exceptionCode = 0;
        const BOOL began =
            CallSetRestorePoint(setRestorePoint, &info, &status, &exceptionCode);

        if (exceptionCode != 0) {
            XPLOG_ERROR("SRSetRestorePoint(begin) raised 0x%08lx; carrying on "
                        "without a restore point",
                        static_cast<unsigned long>(exceptionCode));
            return RestorePointResult::Failed;
        }
        if (!began) {
            const DWORD error = status.nStatus ? status.nStatus : ::GetLastError();
            XPLOG_ERROR("SRSetRestorePoint(begin) failed: %lu",
                        static_cast<unsigned long>(error));
            // ERROR_SERVICE_DISABLED is what a machine with System Protection
            // switched off reports.
            if (error == ERROR_SERVICE_DISABLED) {
                return RestorePointResult::Disabled;
            }
            return RestorePointResult::Failed;
        }

        // A sequence number of zero means Windows accepted the call but decided
        // not to make a checkpoint - almost always the 24 hour rate limit.
        if (status.llSequenceNumber == 0) {
            XPLOG_INFO("Windows skipped the restore point (rate limited)");
            return RestorePointResult::RateLimited;
        }

        // Close the change window immediately: everything this installer does is
        // captured by the begin marker, and leaving it open would fold the
        // user's next hour of activity into our checkpoint.
        RestorePointInfoW end = info;
        end.dwEventType = kEndSystemChange;
        end.llSequenceNumber = status.llSequenceNumber;
        StateMgrStatus endStatus = {};
        DWORD endException = 0;
        CallSetRestorePoint(setRestorePoint, &end, &endStatus, &endException);

        if (sequenceNumber) {
            *sequenceNumber = status.llSequenceNumber;
        }
        XPLOG_INFO("restore point %lld created",
                   static_cast<long long>(status.llSequenceNumber));
        return RestorePointResult::Created;
    }

    bool SuspendRateLimit() override {
        if (rateLimitSuspended_) {
            return true;
        }
        DWORD existing = 0;
        hadFrequencyValue_ = win32::ReadRegistryDword(
            HKEY_LOCAL_MACHINE, kSystemRestoreKey, kFrequencyValue, &existing);
        previousFrequency_ = hadFrequencyValue_ ? existing : 0;

        if (!win32::WriteRegistryDword(HKEY_LOCAL_MACHINE, kSystemRestoreKey,
                                       kFrequencyValue, 0)) {
            return false;
        }
        rateLimitSuspended_ = true;
        return true;
    }

    void RestoreRateLimit() override {
        if (!rateLimitSuspended_) {
            return;
        }
        rateLimitSuspended_ = false;

        if (hadFrequencyValue_) {
            win32::WriteRegistryDword(HKEY_LOCAL_MACHINE, kSystemRestoreKey,
                                      kFrequencyValue, previousFrequency_);
            return;
        }
        // The value did not exist before us, so remove it rather than leaving a
        // policy behind that we invented.
        HKEY key = nullptr;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSystemRestoreKey, 0,
                            KEY_SET_VALUE | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
            ::RegDeleteValueW(key, kFrequencyValue);
            ::RegCloseKey(key);
        }
    }

private:
    SRSetRestorePointWFn Resolve() {
        if (resolved_) {
            return function_;
        }
        resolved_ = true;
        module_ = ::LoadLibraryExW(L"SrClient.dll", nullptr,
                                   LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) {
            return nullptr;
        }
        function_ = reinterpret_cast<SRSetRestorePointWFn>(
            ::GetProcAddress(module_, "SRSetRestorePointW"));
        return function_;
    }

    HMODULE              module_ = nullptr;
    SRSetRestorePointWFn function_ = nullptr;
    bool                 resolved_ = false;
    bool                 rateLimitSuspended_ = false;
    bool                 hadFrequencyValue_ = false;
    DWORD                previousFrequency_ = 0;
};

} // namespace

std::shared_ptr<ISystemRestore> MakeWin32SystemRestore() {
    return std::make_shared<Win32SystemRestore>();
}

} // namespace xplogin
