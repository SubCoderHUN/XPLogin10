#include "Win32Common.h"

#include "xplogin/Config.h"
#include "xplogin/HealthMonitor.h"
#include "xplogin/Interfaces.h"
#include "xplogin/RegistrationPlan.h"
#include "xplogin/StringUtil.h"
#include "xplogin/Win32Factories.h"

#include <vector>

namespace xplogin::win32 {

bool ReadRegistryString(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                        std::wstring* out) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) !=
        ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD bytes = 0;
    LONG status = ::RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        ::RegCloseKey(key);
        return false;
    }

    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    status = ::RegQueryValueExW(key, valueName, nullptr, &type,
                                reinterpret_cast<LPBYTE>(buffer.data()), &bytes);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        return false;
    }

    if (out) {
        *out = buffer.data();
    }
    return true;
}

bool ReadRegistryDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                       DWORD* out) {
    HKEY key = nullptr;
    if (::RegOpenKeyExW(root, subKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) !=
        ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    const LONG status = ::RegQueryValueExW(key, valueName, nullptr, &type,
                                           reinterpret_cast<LPBYTE>(&value), &bytes);
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_DWORD) {
        return false;
    }
    if (out) {
        *out = value;
    }
    return true;
}

bool WriteRegistryDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                        DWORD value) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (::RegCreateKeyExW(root, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                          KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key,
                          &disposition) != ERROR_SUCCESS) {
        return false;
    }
    const LONG status =
        ::RegSetValueExW(key, valueName, 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&value), sizeof(value));
    ::RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

bool WriteRegistryString(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                         const std::wstring& value) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    if (::RegCreateKeyExW(root, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                          KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key,
                          &disposition) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD bytes =
        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    const LONG status =
        ::RegSetValueExW(key, valueName, 0, REG_SZ,
                         reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    ::RegCloseKey(key);
    return status == ERROR_SUCCESS;
}

std::wstring SidToString(PSID sid) {
    if (!sid || !::IsValidSid(sid)) {
        return std::wstring();
    }
    LPWSTR text = nullptr;
    if (!::ConvertSidToStringSidW(sid, &text)) {
        return std::wstring();
    }
    std::wstring result = text;
    ::LocalFree(text);
    return result;
}

std::wstring ModuleDirectory(HMODULE module) {
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD length = ::GetModuleFileNameW(module, path, ARRAYSIZE(path));
    if (length == 0 || length >= ARRAYSIZE(path)) {
        return std::wstring();
    }
    std::wstring full(path, length);
    const size_t slash = full.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        return std::wstring();
    }
    return full.substr(0, slash + 1);
}

bool IsOnSecureDesktop() {
    HDESK desktop = ::GetThreadDesktop(::GetCurrentThreadId());
    if (!desktop) {
        return false;
    }
    wchar_t name[256] = {};
    DWORD needed = 0;
    if (!::GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &needed)) {
        return false;
    }
    return ::_wcsicmp(name, L"Winlogon") == 0;
}

bool EnablePrivilege(const wchar_t* privilegeName) {
    ScopedHandle token;
    if (!::OpenProcessToken(::GetCurrentProcess(),
                            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, token.Receive())) {
        return false;
    }

    LUID luid = {};
    if (!::LookupPrivilegeValueW(nullptr, privilegeName, &luid)) {
        return false;
    }

    TOKEN_PRIVILEGES privileges = {};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!::AdjustTokenPrivileges(token.Get(), FALSE, &privileges, sizeof(privileges),
                                 nullptr, nullptr)) {
        return false;
    }
    // AdjustTokenPrivileges reports success even when the privilege is not held.
    return ::GetLastError() == ERROR_SUCCESS;
}

} // namespace xplogin::win32

namespace xplogin {

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

class Win32Clock : public IClock {
public:
    uint64_t NowMs() const override {
        FILETIME fileTime = {};
        ::GetSystemTimeAsFileTime(&fileTime);
        ULARGE_INTEGER value;
        value.LowPart = fileTime.dwLowDateTime;
        value.HighPart = fileTime.dwHighDateTime;
        return value.QuadPart / 10000ULL; // 100ns ticks -> ms
    }
};

std::shared_ptr<IClock> MakeWin32Clock() { return std::make_shared<Win32Clock>(); }

// ---------------------------------------------------------------------------
// State store, backed by HKLM\SOFTWARE\XPLogin10\Health
// ---------------------------------------------------------------------------

class Win32StateStore : public IStateStore {
public:
    explicit Win32StateStore(std::wstring subKey) : subKey_(std::move(subKey)) {}

    bool ReadU64(const std::string& key, uint64_t* value) const override {
        DWORD dword = 0;
        if (!win32::ReadRegistryDword(HKEY_LOCAL_MACHINE, subKey_.c_str(),
                                      Widen(key).c_str(), &dword)) {
            return false;
        }
        if (value) {
            *value = dword;
        }
        return true;
    }

    bool WriteU64(const std::string& key, uint64_t value) override {
        // The counters are small; a DWORD keeps the values readable in regedit
        // and keeps the recovery .reg file simple.
        const DWORD clamped =
            value > 0xFFFFFFFFULL ? 0xFFFFFFFFu : static_cast<DWORD>(value);
        return win32::WriteRegistryDword(HKEY_LOCAL_MACHINE, subKey_.c_str(),
                                         Widen(key).c_str(), clamped);
    }

    bool ReadString(const std::string& key, std::wstring* value) const override {
        return win32::ReadRegistryString(HKEY_LOCAL_MACHINE, subKey_.c_str(),
                                         Widen(key).c_str(), value);
    }

    bool WriteString(const std::string& key, const std::wstring& value) override {
        return win32::WriteRegistryString(HKEY_LOCAL_MACHINE, subKey_.c_str(),
                                          Widen(key).c_str(), value);
    }

    bool Remove(const std::string& key) override {
        HKEY handle = nullptr;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKey_.c_str(), 0,
                            KEY_SET_VALUE | KEY_WOW64_64KEY, &handle) !=
            ERROR_SUCCESS) {
            return false;
        }
        const LONG status = ::RegDeleteValueW(handle, Widen(key).c_str());
        ::RegCloseKey(handle);
        return status == ERROR_SUCCESS;
    }

private:
    static std::wstring Widen(const std::string& value) {
        return std::wstring(value.begin(), value.end()); // keys are ASCII
    }

    std::wstring subKey_;
};

std::shared_ptr<IStateStore> MakeWin32StateStore() {
    return std::make_shared<Win32StateStore>(std::wstring(kConfigKeyPath) +
                                             L"\\Health");
}

// ---------------------------------------------------------------------------
// Boot mode
// ---------------------------------------------------------------------------

BootMode DetectBootMode() {
    // SM_CLEANBOOT: 0 normal, 1 fail-safe, 2 fail-safe with network.
    switch (::GetSystemMetrics(SM_CLEANBOOT)) {
        case 0: return BootMode::Normal;
        case 1: return BootMode::SafeMode;
        case 2: return BootMode::SafeModeWithNetworking;
        default: return BootMode::SafeMode; // unknown: assume the safe answer
    }
}

// ---------------------------------------------------------------------------
// Configuration loading
// ---------------------------------------------------------------------------

AppConfig LoadInstalledConfig(const std::wstring& moduleDirectory) {
    std::wstring configPath;
    if (!win32::ReadRegistryString(HKEY_LOCAL_MACHINE, kConfigKeyPath, L"ConfigPath",
                                   &configPath) ||
        configPath.empty()) {
        configPath = moduleDirectory + L"XPLogin.ini";
    }

    IniFile ini;
    // A missing or malformed file is not fatal: the built-in defaults are a
    // working configuration, and refusing to start would strand the machine.
    ini.LoadFromFile(WideToUtf8(configPath));
    AppConfig config = AppConfig::FromIni(ini);

    // The registry overrides the file, so group policy wins.
    DWORD enabled = 1;
    if (win32::ReadRegistryDword(HKEY_LOCAL_MACHINE, kConfigKeyPath, L"Enabled",
                                 &enabled)) {
        config.enabled = enabled != 0;
    }

    if (config.ui.themePath.empty()) {
        config.ui.themePath = moduleDirectory + L"XPLogin.theme.ini";
    }
    if (config.ui.avatarDirectory.empty()) {
        config.ui.avatarDirectory = moduleDirectory + L"avatars\\";
    }
    if (config.logPath.empty()) {
        config.logPath = moduleDirectory + L"XPLogin.log";
    }

    // The sounds the setup deployed, unless XPLogin.ini named others. A path
    // that does not exist is skipped silently at play time, so a build made
    // without assets/Media simply stays quiet.
    struct SoundDefault {
        std::wstring* path;
        const wchar_t* fileName;
    };
    const SoundDefault sounds[] = {
        {&config.sounds.logonPath,    L"XPLogin-logon.wav"},
        {&config.sounds.logoffPath,   L"XPLogin-logoff.wav"},
        {&config.sounds.errorPath,    L"XPLogin-error.wav"},
        {&config.sounds.shutdownPath, L"XPLogin-shutdown.wav"},
    };
    for (const SoundDefault& sound : sounds) {
        if (sound.path->empty()) {
            *sound.path = moduleDirectory + sound.fileName;
        }
    }
    return config;
}

ControllerDependencies MakeWin32Dependencies(const AppConfig& config,
                                             bool useDeferredAuthenticator) {
    ControllerDependencies deps;
    deps.authenticator = useDeferredAuthenticator ? MakeDeferredLsaAuthenticator()
                                                  : MakeWin32Authenticator();
    // The deferred authenticator cannot answer a question - it only defers to
    // LSA - so the blank-password probe gets its own direct LogonUserW path.
    // This is the "optional pre-flight check" MakeWin32Authenticator was left
    // for: before the screen decides to sign itself in, it confirms with LSA
    // that the sole account really does take an empty password.
    deps.blankPasswordProber = MakeWin32Authenticator();
    deps.userEnumerator = MakeWin32UserEnumerator();
    // Deliberately no session manager.
    //
    // These dependencies are for the credential provider, and a credential
    // provider does not run the logon - Winlogon does. Its whole job is to
    // hand LSA a blob and then get out of the way. Connecting sessions and
    // starting the shell from inside LogonUI means doing Winlogon's work
    // underneath it while it is doing the same work itself, and both of those
    // were visible on the machine:
    //
    //   * WTSConnectSession, called from ReportResult for an account that
    //     already had a session, aborted the sign-in Winlogon was completing.
    //     The password was accepted, nothing happened, and half a minute later
    //     the logon screen came back.
    //   * EnsureShellRunning raced Winlogon to start explorer.exe. It checks
    //     first, but it checks at the moment the credential is reported, which
    //     is before Winlogon has started the shell - so it wins the race, and
    //     the user gets the desktop plus a second Explorer window.
    //
    // MakeWin32SessionManager still exists for the watchdog service, which
    // runs in session 0 and genuinely does need to look at sessions.
    deps.powerController = MakeWin32PowerController();
    deps.soundPlayer = MakeWin32SoundPlayer(config.sounds);
    deps.clock = MakeWin32Clock();
    deps.stateStore = MakeWin32StateStore();
    return deps;
}

} // namespace xplogin
