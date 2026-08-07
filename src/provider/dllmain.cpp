// XPLogin10 - COM plumbing for XPLoginProvider.dll.
//
// This DLL is loaded into LogonUI.exe. Two consequences drive everything here:
//
//   * an exception that escapes into LogonUI can leave the machine with no way
//     to sign in, so every entry point is wrapped;
//   * the DLL must unload cleanly, so the object count is tracked properly and
//     DllCanUnloadNow tells the truth.
#include "Guids.h"
#include "XPFilter.h"
#include "XPProvider.h"

#include "xplogin/HealthMonitor.h"
#include "xplogin/Logging.h"
#include "xplogin/RegistrationPlan.h"
#include "xplogin/Win32Factories.h"

#include <windows.h>

#include <new>

namespace {

HINSTANCE g_moduleInstance = nullptr;
long g_objectCount = 0;

void AddObject() { ::InterlockedIncrement(&g_objectCount); }
void RemoveObject() { ::InterlockedDecrement(&g_objectCount); }

// Records the failure so the crash-loop protection can act on it, then lets
// the exception continue so Windows still gets its error report.
LONG RecordCrash(const wchar_t* where, DWORD exceptionCode) {
    auto store = xplogin::MakeWin32StateStore();
    auto clock = xplogin::MakeWin32Clock();
    xplogin::HealthMonitor health(store, clock, xplogin::HealthPolicy{});

    wchar_t reason[256] = {};
    ::swprintf_s(reason, L"%s raised 0x%08lx", where,
                 static_cast<unsigned long>(exceptionCode));
    health.ReportCrash(reason);

    XPLOG_ERROR("structured exception in the credential provider; strike recorded");
    return EXCEPTION_CONTINUE_SEARCH;
}

// Constructs the requested object. Kept separate from the SEH wrapper below
// because a function containing __try/__except may not also contain locals that
// need C++ unwinding, and these do.
HRESULT CreateUnguarded(const CLSID& clsid, REFIID riid, void** ppv) {
    if (::IsEqualCLSID(clsid, CLSID_XPLoginProvider)) {
        auto* provider =
            new (std::nothrow) xplogin::provider::XPProvider(g_moduleInstance);
        if (!provider) {
            return E_OUTOFMEMORY;
        }
        const HRESULT hr = provider->QueryInterface(riid, ppv);
        provider->Release();
        return hr;
    }
    if (::IsEqualCLSID(clsid, CLSID_XPLoginFilter)) {
        auto* filter =
            new (std::nothrow) xplogin::provider::XPFilter(g_moduleInstance);
        if (!filter) {
            return E_OUTOFMEMORY;
        }
        const HRESULT hr = filter->QueryInterface(riid, ppv);
        filter->Release();
        return hr;
    }
    return CLASS_E_CLASSNOTAVAILABLE;
}

// An access violation while constructing the provider would otherwise take
// LogonUI with it silently. Record the strike first so the crash-loop
// protection can act, then let the exception continue as normal.
HRESULT CreateGuarded(const CLSID& clsid, REFIID riid, void** ppv) {
    __try {
        return CreateUnguarded(clsid, riid, ppv);
    } __except (RecordCrash(L"CreateInstance", GetExceptionCode())) {
        // Unreachable: RecordCrash always continues the search. Present so the
        // __try block is well formed.
        return E_UNEXPECTED;
    }
}

// The classic COM class factory, one instance per CLSID we expose.
class ClassFactory : public IClassFactory {
public:
    explicit ClassFactory(const CLSID& clsid) : clsid_(clsid) { AddObject(); }
    virtual ~ClassFactory() { RemoveObject(); }

    IFACEMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(::InterlockedIncrement(&refCount_));
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        const long count = ::InterlockedDecrement(&refCount_);
        if (count == 0) {
            delete this;
        }
        return static_cast<ULONG>(count);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (::IsEqualIID(riid, IID_IUnknown) ||
            ::IsEqualIID(riid, IID_IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid,
                                  void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        *ppv = nullptr;
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }
        return CreateGuarded(clsid_, riid, ppv);
    }

    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) {
            AddObject();
        } else {
            RemoveObject();
        }
        return S_OK;
    }

private:
    long  refCount_ = 1;
    CLSID clsid_;
};

} // namespace

// Guids.cpp holds the definitions and the compile-time check that they agree
// with the CLSID strings the installer writes to the registry.

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_moduleInstance = static_cast<HINSTANCE>(module);
            // No thread notifications: LogonUI creates plenty and we do not
            // care about any of them.
            ::DisableThreadLibraryCalls(module);
            break;
        case DLL_PROCESS_DETACH:
            xplogin::Log::Flush();
            break;
        default:
            break;
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    *ppv = nullptr;

    if (!::IsEqualCLSID(rclsid, CLSID_XPLoginProvider) &&
        !::IsEqualCLSID(rclsid, CLSID_XPLoginFilter)) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    auto* factory = new (std::nothrow) ClassFactory(rclsid);
    if (!factory) {
        return E_OUTOFMEMORY;
    }
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllCanUnloadNow() {
    return ::InterlockedCompareExchange(&g_objectCount, 0, 0) == 0 ? S_OK : S_FALSE;
}
