// XPLogin10 - ICredentialProviderFilter.
//
// The filter is what makes the Windows tiles disappear so only the XP screen is
// left. It is also the single most dangerous object in this project: a filter
// that hides everything and then fails to show its own provider leaves a
// machine nobody can sign in to.
//
// Two safeguards, both checked on every call:
//   * the health monitor's crash-loop verdict - after two bad starts the filter
//     becomes a no-op and the Windows tiles come back on their own;
//   * an explicit whitelist - our own CLSID is never filtered out.
//
// Which of Windows' tiles to replace is decided by BuildFilterPlan in
// src/core/src/SigninOptions.cpp rather than here, so the rule can be unit
// tested off Windows. The short version: the XP screen replaces the *password*
// tiles, and leaves PIN, Windows Hello, smartcard and anything unrecognised
// alone. Hiding those would lock out every account that signs in with a PIN.
#include "XPFilter.h"

#include "Guids.h"
#include "xplogin/HealthMonitor.h"
#include "xplogin/Logging.h"
#include "xplogin/SigninOptions.h"
#include "xplogin/StringUtil.h"
#include "xplogin/Win32Factories.h"

#include <iterator>
#include <string>
#include <vector>

namespace xplogin::provider {
namespace {

// The braced, upper-case form BuildFilterPlan compares against.
std::wstring ClsidToString(const GUID& guid) {
    wchar_t buffer[64] = {};
    if (::StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer))) == 0) {
        return std::wstring();
    }
    return buffer;
}

} // namespace

XPFilter::XPFilter(HINSTANCE moduleInstance) {
    config_ = LoadInstalledConfig(win32::ModuleDirectory(moduleInstance));
    // The filter is often the first of our objects LogonUI creates, and it used
    // to leave the log unconfigured - so its decisions, which are exactly what
    // you want to read when the screen comes up wrong, went nowhere.
    Log::Configure(WideToUtf8(config_.logPath),
                   static_cast<LogLevel>(config_.logLevel));
    XPLOG_INFO("filter created");
}

IFACEMETHODIMP_(ULONG) XPFilter::AddRef() {
    return static_cast<ULONG>(::InterlockedIncrement(&refCount_));
}

IFACEMETHODIMP_(ULONG) XPFilter::Release() {
    const long count = ::InterlockedDecrement(&refCount_);
    if (count == 0) {
        delete this;
    }
    return static_cast<ULONG>(count);
}

IFACEMETHODIMP XPFilter::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    if (::IsEqualIID(riid, IID_IUnknown) ||
        ::IsEqualIID(riid, IID_ICredentialProviderFilter)) {
        *ppv = static_cast<ICredentialProviderFilter*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

IFACEMETHODIMP XPFilter::Filter(CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario,
                                DWORD /*flags*/, GUID* providerClsids,
                                BOOL* allow, DWORD providerCount) {
    if (!providerClsids || !allow) {
        return E_INVALIDARG;
    }

    // Only the logon and unlock screens are ours. Leave CredUI and password
    // changes to Windows.
    if (scenario != CPUS_LOGON && scenario != CPUS_UNLOCK_WORKSTATION) {
        return S_OK;
    }
    if (!config_.enabled || !config_.filterOtherProviders) {
        return S_OK;
    }

    auto stateStore = MakeWin32StateStore();
    auto clock = MakeWin32Clock();
    HealthMonitor health(stateStore, clock, config_.health);

    // Note the deliberate asymmetry with XPProvider: the filter *reads* the
    // strike counter but never increments it. Only the provider records an
    // attempt, so the two objects cannot double-count a single boot.
    const HealthAction action =
        HealthMonitor::Decide(DetectBootMode(), health.IsDisabled(),
                              health.StartAttempts(), config_.health);

    if (action != HealthAction::RunNormally) {
        XPLOG_INFO("filter standing down (action=%d): leaving the Windows tiles "
                   "visible so this machine stays usable",
                   static_cast<int>(action));
        return S_OK;
    }

    std::vector<std::wstring> clsids;
    clsids.reserve(providerCount);
    for (DWORD i = 0; i < providerCount; ++i) {
        clsids.push_back(ClsidToString(providerClsids[i]));
    }

    FilterPolicy policy;
    policy.mode = config_.signinOptions;
    policy.ownClsid = ClsidToString(CLSID_XPLoginProvider);
    policy.alsoAllow = config_.alwaysAllowProviders;

    std::vector<bool> plan;
    std::string explanation;
    const bool applied = BuildFilterPlan(clsids, policy, &plan, &explanation);

    // Worth logging in full: when somebody cannot sign in, the first question
    // is always which tiles were hidden and why. One line per provider rather
    // than one long string, because the log writer has a fixed message buffer
    // and would quietly truncate the interesting end of it.
    XPLOG_INFO("credential provider filter (mode=%ls, %u providers)",
               SigninOptionsModeName(policy.mode),
               static_cast<unsigned>(providerCount));
    for (size_t start = 0; start < explanation.size();) {
        const size_t end = explanation.find('\n', start);
        const std::string line = explanation.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty()) {
            XPLOG_INFO("%s", line.c_str());
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    if (!applied) {
        XPLOG_ERROR("filter plan would have left no way to sign in; every "
                    "Windows credential provider has been restored");
    }

    for (DWORD i = 0; i < providerCount && i < plan.size(); ++i) {
        allow[i] = plan[i] ? TRUE : FALSE;
    }
    return S_OK;
}

IFACEMETHODIMP XPFilter::UpdateRemoteCredential(
    const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* /*in*/,
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* /*out*/) {
    return E_NOTIMPL;
}

} // namespace xplogin::provider
