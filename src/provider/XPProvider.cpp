#include "XPProvider.h"

// propkey.h must come before anything that could pull in <initguid.h>, so the
// PROPERTYKEYs stay plain declarations resolved from propsys.lib rather than
// being redefined here.
#include <propkey.h>

#include "Guids.h"
#include "xplogin/HealthMonitor.h"
#include "xplogin/Logging.h"
#include "xplogin/SigninOptions.h"
#include "xplogin/StringUtil.h"
#include "xplogin/UserDirectory.h"
#include "xplogin/Win32Factories.h"

#define SECURITY_WIN32
#include <sddl.h>
#include <security.h>
#include <secext.h>

#include <new>
#include <vector>

namespace xplogin::provider {
namespace {

// Field descriptors. All hidden - see XPCredential - but LogonUI insists on a
// well formed set, including exactly one submit button.
CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR g_fieldDescriptors[] = {
    {kFieldTileImage, CPFT_TILE_IMAGE, const_cast<PWSTR>(L"Image")},
    {kFieldLabel, CPFT_LARGE_TEXT, const_cast<PWSTR>(L"XPLogin10")},
    {kFieldPassword, CPFT_PASSWORD_TEXT, const_cast<PWSTR>(L"Password")},
    {kFieldSubmit, CPFT_SUBMIT_BUTTON, const_cast<PWSTR>(L"Submit")},
};

static_assert(ARRAYSIZE(g_fieldDescriptors) == kFieldCount,
              "field descriptor table is out of step with FieldId");

// An IUserEnumerator backed by the ICredentialProviderUserArray Windows gives
// us. This is authoritative on a domain-joined or AzureAD machine, where
// enumerating the SAM would miss most of the accounts that can actually sign in.
class UserArrayEnumerator : public IUserEnumerator {
public:
    explicit UserArrayEnumerator(ICredentialProviderUserArray* array)
        : array_(array) {
        if (array_) {
            array_->AddRef();
        }
    }
    ~UserArrayEnumerator() override {
        if (array_) {
            array_->Release();
        }
    }

    std::vector<UserAccount> EnumerateLocalUsers() override {
        return Collect(/*wantDomain=*/false);
    }
    std::vector<UserAccount> EnumerateDomainUsers() override {
        return Collect(/*wantDomain=*/true);
    }
    std::vector<std::wstring> HiddenAccountNames() override { return {}; }

    std::wstring MachineName() override {
        wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD size = ARRAYSIZE(buffer);
        if (!::GetComputerNameW(buffer, &size)) {
            return std::wstring();
        }
        return std::wstring(buffer, size);
    }

    bool IsDomainJoined() override { return false; }

    bool Usable() const {
        if (!array_) {
            return false;
        }
        DWORD count = 0;
        return SUCCEEDED(array_->GetCount(&count)) && count > 0;
    }

private:
    static std::wstring TakeString(PWSTR value) {
        if (!value) {
            return std::wstring();
        }
        std::wstring result = value;
        ::CoTaskMemFree(value);
        return result;
    }

    std::vector<UserAccount> Collect(bool wantDomain) {
        std::vector<UserAccount> accounts;
        if (!array_) {
            return accounts;
        }

        DWORD count = 0;
        if (FAILED(array_->GetCount(&count))) {
            return accounts;
        }
        const std::wstring machine = MachineName();

        for (DWORD i = 0; i < count; ++i) {
            ICredentialProviderUser* user = nullptr;
            if (FAILED(array_->GetAt(i, &user)) || !user) {
                continue;
            }

            UserAccount account;
            PWSTR value = nullptr;

            if (SUCCEEDED(user->GetStringValue(PKEY_Identity_QualifiedUserName,
                                               &value))) {
                const std::wstring qualified = TakeString(value);
                std::wstring domain;
                std::wstring name;
                if (SplitQualifiedName(qualified, &domain, &name)) {
                    account.domain = domain.empty() ? machine : domain;
                    account.username = name;
                }
            }
            if (SUCCEEDED(user->GetStringValue(PKEY_Identity_DisplayName, &value))) {
                account.displayName = TakeString(value);
            }
            if (SUCCEEDED(user->GetSid(&value))) {
                account.sid = TakeString(value);
            }

            user->Release();

            if (account.username.empty()) {
                continue;
            }
            account.source = ClassifyAccountDomain(account.domain, machine);

            // Only a real Active Directory account belongs in the domain
            // bucket. A Microsoft account comes back from EnumerateLocalUsers
            // because that is what it is underneath - a local profile with a
            // local SID - and because the alternative hides the only account on
            // a machine that was set up with one and nothing else the moment
            // somebody turns domain accounts off.
            const bool isDomain = account.source == AccountSource::Domain;
            if (isDomain != wantDomain) {
                continue;
            }
            if (account.displayName.empty()) {
                account.displayName = account.username;
            }
            accounts.push_back(std::move(account));
        }
        return accounts;
    }

    ICredentialProviderUserArray* array_ = nullptr;
};

} // namespace

XPProvider::XPProvider(HINSTANCE moduleInstance)
    : moduleInstance_(moduleInstance) {
    config_ = LoadInstalledConfig(win32::ModuleDirectory(moduleInstance));
    Log::Configure(WideToUtf8(config_.logPath),
                   static_cast<LogLevel>(config_.logLevel));

    IniFile themeIni;
    if (!config_.ui.themePath.empty()) {
        themeIni.LoadFromFile(WideToUtf8(config_.ui.themePath));
    }
    theme_ = ui::XpTheme::FromIni(themeIni);
}

XPProvider::~XPProvider() {
    ReleaseCredentials();
    if (events_) {
        events_->Release();
        events_ = nullptr;
    }
    if (userArray_) {
        userArray_->Release();
        userArray_ = nullptr;
    }
}

void XPProvider::ReleaseCredentials() {
    for (XPCredential* credential : credentials_) {
        if (credential) {
            credential->Retire();
            credential->Release();
        }
    }
    credentials_.clear();
}

// One credential per account. BuildTileSids in src/core/src/SigninOptions.cpp
// decides the set and carries the explanation of why; it lives there so the
// rule is unit tested off Windows, like the filter plan next to it.
void XPProvider::RebuildCredentials() {
    ReleaseCredentials();
    credentialsStale_ = false;
    if (!active_ || !controller_) {
        return;
    }

    const std::vector<UserAccount>& users = controller_->Users();
    for (const std::wstring& sid : BuildTileSids(users)) {
        auto* credential = new (std::nothrow) XPCredential();
        if (!credential) {
            continue;
        }
        const HRESULT hr = credential->Initialize(scenario_, controller_.get(),
                                                  this, moduleInstance_, theme_,
                                                  sid);
        if (FAILED(hr)) {
            XPLOG_ERROR("credential for %s failed to initialise: hr=0x%08lx",
                        WideToUtf8(sid).c_str(),
                        static_cast<unsigned long>(hr));
            credential->Release();
            continue;
        }
        credentials_.push_back(credential);
    }

    if (credentials_.empty()) {
        // No account had a SID - a machine we could not enumerate at all. One
        // unattached credential is still better than none: it will not be on a
        // user tile, but it keeps the provider alive and the log honest.
        XPLOG_ERROR("no account produced a SID (%d account(s) known); falling "
                    "back to a single unattached tile",
                    static_cast<int>(users.size()));
        auto* credential = new (std::nothrow) XPCredential();
        if (credential) {
            if (SUCCEEDED(credential->Initialize(scenario_, controller_.get(),
                                                 this, moduleInstance_, theme_,
                                                 std::wstring()))) {
                credentials_.push_back(credential);
            } else {
                credential->Release();
            }
        }
    }

    XPLOG_INFO("%d tile(s) built from %d account(s)",
               static_cast<int>(credentials_.size()),
               static_cast<int>(users.size()));
}

void XPProvider::EnsureCredentials() {
    if (!active_ || !controller_) {
        return;
    }
    // Never rebuild while a submission is in flight: the credential holding the
    // pending flag - and the XP window - is the one LogonUI is about to call
    // GetSerialization on.
    if (SubmitPendingIndex() >= 0) {
        return;
    }
    if (credentials_.empty() || credentialsStale_) {
        RebuildCredentials();
    }
}

int XPProvider::SubmitPendingIndex() const {
    for (size_t i = 0; i < credentials_.size(); ++i) {
        if (credentials_[i] && credentials_[i]->SubmitPending()) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// LogonUI normally picks the tile itself in the user-centric world, but it
// still asks, and CPUS_UNLOCK in particular expects the locked account.
DWORD XPProvider::DefaultCredentialIndex() const {
    if (credentials_.empty()) {
        return CREDENTIAL_PROVIDER_NO_DEFAULT;
    }
    if (controller_) {
        const UserAccount* selected = controller_->SelectedUser();
        if (selected && !selected->sid.empty()) {
            for (size_t i = 0; i < credentials_.size(); ++i) {
                if (credentials_[i] &&
                    EqualsNoCase(credentials_[i]->TileSid(), selected->sid)) {
                    return static_cast<DWORD>(i);
                }
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------

IFACEMETHODIMP_(ULONG) XPProvider::AddRef() {
    return static_cast<ULONG>(::InterlockedIncrement(&refCount_));
}

IFACEMETHODIMP_(ULONG) XPProvider::Release() {
    const long count = ::InterlockedDecrement(&refCount_);
    if (count == 0) {
        delete this;
    }
    return static_cast<ULONG>(count);
}

IFACEMETHODIMP XPProvider::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    if (::IsEqualIID(riid, IID_IUnknown) ||
        ::IsEqualIID(riid, IID_ICredentialProvider)) {
        *ppv = static_cast<ICredentialProvider*>(this);
        AddRef();
        return S_OK;
    }
    if (::IsEqualIID(riid, IID_ICredentialProviderSetUserArray)) {
        *ppv = static_cast<ICredentialProviderSetUserArray*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

// ---------------------------------------------------------------------------
// ICredentialProvider
// ---------------------------------------------------------------------------

UsageScenario XPProvider::TranslateScenario(
    CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario) const {
    switch (scenario) {
        case CPUS_LOGON: return UsageScenario::Logon;
        case CPUS_UNLOCK_WORKSTATION: return UsageScenario::UnlockWorkstation;
        case CPUS_CHANGE_PASSWORD: return UsageScenario::ChangePassword;
        case CPUS_CREDUI: return UsageScenario::CredUI;
        default: return UsageScenario::Invalid;
    }
}

void XPProvider::CaptureLockedUser() {
    if (scenario_ != CPUS_UNLOCK_WORKSTATION || !controller_) {
        return;
    }

    // The account that locked the workstation owns the current logon session.
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return;
    }

    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    if (size > 0 &&
        ::GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        auto* tokenUser = reinterpret_cast<TOKEN_USER*>(buffer.data());
        controller_->SetLockedUserSid(win32::SidToString(tokenUser->User.Sid));
    }
    ::CloseHandle(token);

    // A name is a useful fallback when the SID is not in our list (a profile
    // that has been renamed, for instance).
    wchar_t name[512] = {};
    DWORD nameSize = ARRAYSIZE(name);
    if (::GetUserNameExW(NameSamCompatible, name, &nameSize)) {
        controller_->SetLockedUserName(name);
    }
}

IFACEMETHODIMP XPProvider::SetUsageScenario(
    CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario, DWORD /*flags*/) {
    scenario_ = scenario;
    // Built-on stamp: the quickest way to tell whether the DLL on this machine
    // is the one that was just built, which cost a round trip to work out once.
    XPLOG_INFO("SetUsageScenario(%d)  [XPLoginProvider built " __DATE__ " " __TIME__ "]",
               static_cast<int>(scenario));

    // CPUS_CREDUI is an in-session UAC-style prompt on the user's own desktop.
    // Replacing it with a full-screen XP window would be wrong and jarring, so
    // we decline and Windows uses its normal prompt.
    if (scenario != CPUS_LOGON && scenario != CPUS_UNLOCK_WORKSTATION) {
        XPLOG_INFO("scenario %d is not ours; leaving it to Windows",
                   static_cast<int>(scenario));
        return E_NOTIMPL;
    }

    // Crash-loop protection. This is checked on every single instantiation, so
    // a provider that dies while painting can only take three boots with it.
    auto stateStore = MakeWin32StateStore();
    auto clock = MakeWin32Clock();
    HealthMonitor health(stateStore, clock, config_.health);
    const HealthAction action = health.BeginProviderStart(DetectBootMode());

    if (!config_.enabled || action == HealthAction::Bypass) {
        XPLOG_INFO("standing down (enabled=%d, health action=%d)",
                   config_.enabled ? 1 : 0, static_cast<int>(action));
        active_ = false;
        return E_NOTIMPL;
    }
    active_ = true;
    XPLOG_INFO("health action=%d, proceeding", static_cast<int>(action));

    ControllerDependencies deps = MakeWin32Dependencies(config_, true);

    // Prefer the account list Windows itself intends to display; fall back to
    // enumerating the SAM when LogonUI did not give us one.
    auto arrayEnumerator = std::make_shared<UserArrayEnumerator>(userArray_);
    if (arrayEnumerator->Usable()) {
        deps.userEnumerator = arrayEnumerator;
    }

    controller_ = std::make_unique<LogonController>(deps, config_);
    controller_->SetObserver(this);

    CaptureLockedUser();
    controller_->Initialize(TranslateScenario(scenario));
    XPLOG_INFO("controller initialised with %d account(s)",
               static_cast<int>(controller_->Users().size()));

    // The tiles are built in GetCredentialCount, not here: SetUserArray still
    // has to arrive, and it is the list we would rather build from.
    ReleaseCredentials();
    credentialsStale_ = true;
    return S_OK;
}

IFACEMETHODIMP XPProvider::SetSerialization(
    const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* /*serialization*/) {
    // Pre-filled credentials (remote assistance, "run as") are not something a
    // welcome screen replacement should consume.
    return E_NOTIMPL;
}

IFACEMETHODIMP XPProvider::Advise(ICredentialProviderEvents* events,
                                  UINT_PTR adviseContext) {
    if (events_) {
        events_->Release();
        events_ = nullptr;
    }
    if (events) {
        events_ = events;
        events_->AddRef();
        adviseContext_ = adviseContext;
    }
    return S_OK;
}

IFACEMETHODIMP XPProvider::UnAdvise() {
    if (events_) {
        events_->Release();
        events_ = nullptr;
    }
    adviseContext_ = 0;
    return S_OK;
}

IFACEMETHODIMP XPProvider::GetFieldDescriptorCount(DWORD* count) {
    if (!count) {
        return E_POINTER;
    }
    *count = kFieldCount;
    return S_OK;
}

IFACEMETHODIMP XPProvider::GetFieldDescriptorAt(
    DWORD index, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR** descriptor) {
    if (!descriptor) {
        return E_POINTER;
    }
    if (index >= kFieldCount) {
        return E_INVALIDARG;
    }

    auto* copy = static_cast<CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR*>(
        ::CoTaskMemAlloc(sizeof(CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR)));
    if (!copy) {
        return E_OUTOFMEMORY;
    }
    *copy = g_fieldDescriptors[index];

    const size_t bytes =
        (::wcslen(g_fieldDescriptors[index].pszLabel) + 1) * sizeof(wchar_t);
    auto* label = static_cast<PWSTR>(::CoTaskMemAlloc(bytes));
    if (!label) {
        ::CoTaskMemFree(copy);
        return E_OUTOFMEMORY;
    }
    ::memcpy(label, g_fieldDescriptors[index].pszLabel, bytes);
    copy->pszLabel = label;

    *descriptor = copy;
    return S_OK;
}

IFACEMETHODIMP XPProvider::GetCredentialCount(DWORD* count,
                                              DWORD* defaultCredential,
                                              BOOL* autoLogonWithDefault) {
    if (!count || !defaultCredential || !autoLogonWithDefault) {
        return E_POINTER;
    }

    EnsureCredentials();

    if (!active_ || credentials_.empty()) {
        // The screen will have no tile on it at all. Worth an error line
        // rather than a debug one: this is what an empty logon screen looks
        // like from the inside.
        XPLOG_ERROR("GetCredentialCount: reporting 0 tiles (active=%d) - the "
                    "logon screen will be empty",
                    active_ ? 1 : 0);
        *count = 0;
        *defaultCredential = CREDENTIAL_PROVIDER_NO_DEFAULT;
        *autoLogonWithDefault = FALSE;
        return S_OK;
    }

    // TRUE here is how a credential provider says "the credentials are ready,
    // come and take them": LogonUI selects the default tile and calls
    // GetSerialization on it. RequestSubmit() sets the flag and triggers the
    // re-enumeration that lands here. The default must then be the tile that
    // holds the typed password, not whichever one LogonUI opened on.
    const int pending = SubmitPendingIndex();

    // The other way this is TRUE: one account with no password. Windows signs
    // such a machine in without asking, and so did XP - so the screen agrees
    // with it rather than putting up a one-tile list that is clicked past a
    // moment later by something the user never saw. The window draws XP's
    // "welcome" for the whole of it; see LogonController::SignsInAutomatically.
    //
    // Safe to be wrong about: LSA is what decides. If the account turns out to
    // have a password after all, the blank one is rejected and the screen comes
    // back with the error, exactly as if it had been typed.
    const bool automatic =
        pending < 0 && controller_ && controller_->SignsInAutomatically();
    if (automatic) {
        XPLOG_INFO("one account and no password: signing in without asking");
    }

    *count = static_cast<DWORD>(credentials_.size());
    *defaultCredential = pending >= 0 ? static_cast<DWORD>(pending)
                                      : DefaultCredentialIndex();
    *autoLogonWithDefault = (pending >= 0 || automatic) ? TRUE : FALSE;

    XPLOG_DEBUG("GetCredentialCount: %lu tile(s), default=%lu, autologon=%d",
               static_cast<unsigned long>(*count),
               static_cast<unsigned long>(*defaultCredential),
               *autoLogonWithDefault ? 1 : 0);
    return S_OK;
}

void XPProvider::RequestSubmit() {
    if (events_) {
        events_->CredentialsChanged(adviseContext_);
    }
}

IFACEMETHODIMP XPProvider::GetCredentialAt(
    DWORD index, ICredentialProviderCredential** credential) {
    if (!credential) {
        return E_POINTER;
    }
    if (index >= credentials_.size() || !credentials_[index]) {
        XPLOG_ERROR("GetCredentialAt(%lu): nothing to hand over (%d tile(s))",
                    static_cast<unsigned long>(index),
                    static_cast<int>(credentials_.size()));
        return E_INVALIDARG;
    }
    XPLOG_DEBUG("GetCredentialAt(%lu) -> %s", static_cast<unsigned long>(index),
               WideToUtf8(credentials_[index]->TileSid()).c_str());
    return credentials_[index]->QueryInterface(
        IID_ICredentialProviderCredential,
        reinterpret_cast<void**>(credential));
}

IFACEMETHODIMP XPProvider::SetUserArray(ICredentialProviderUserArray* users) {
    if (userArray_) {
        userArray_->Release();
        userArray_ = nullptr;
    }
    if (users) {
        userArray_ = users;
        userArray_->AddRef();
    }

    DWORD count = 0;
    if (userArray_) {
        userArray_->GetCount(&count);
    }
    XPLOG_INFO("SetUserArray: %lu account(s) from LogonUI",
               static_cast<unsigned long>(count));

    // LogonUI calls this *after* SetUsageScenario, so the controller built
    // there has already fallen back to enumerating the SAM. Re-read the
    // accounts now that the authoritative list has arrived - on a domain or
    // Entra-joined machine it is the only one that includes the accounts that
    // can actually sign in.
    if (active_ && controller_ && userArray_) {
        auto enumerator = std::make_shared<UserArrayEnumerator>(userArray_);
        if (enumerator->Usable()) {
            controller_->SetUserEnumerator(enumerator);
            controller_->RefreshUsers();
            XPLOG_INFO("account list rebuilt from LogonUI: %d account(s)",
                       static_cast<int>(controller_->Users().size()));
        }
    }
    // Different accounts mean different tiles; GetCredentialCount rebuilds.
    credentialsStale_ = true;
    return S_OK;
}

// ---------------------------------------------------------------------------
// IControllerObserver
// ---------------------------------------------------------------------------

void XPProvider::OnStateChanged(UiState from, UiState to) {
    XPLOG_DEBUG("state %s -> %s", LogonStateMachine::StateName(from),
                LogonStateMachine::StateName(to));
}

void XPProvider::OnUsersChanged() {
    // A different set of accounts is a different set of tiles.
    credentialsStale_ = true;
    if (events_) {
        events_->CredentialsChanged(adviseContext_);
    }
}

void XPProvider::OnAuthError(const AuthResult& result) {
    XPLOG_INFO("auth error surfaced to the UI: status=%d",
               static_cast<int>(result.status));
}

} // namespace xplogin::provider
