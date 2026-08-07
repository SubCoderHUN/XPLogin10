// XPLogin10 - what the installer is about to do to the registry, as data.
//
// Registering a credential provider means writing to keys that decide whether
// anybody can log in to this machine ever again. Building the operation list
// first, and only then executing it, buys three things:
//
//   * the plan can be unit tested without touching a live registry,
//   * `xplogin-install /preview` can print exactly what will change,
//   * an uninstall plan is generated from the same source of truth, so nothing
//     is left behind.
//
// A guard rail (`IsPathAllowed`) rejects any plan that would touch a key
// outside the small set this project owns.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xplogin {

// Our CLSID. Generated once for this project; changing it orphans existing
// installations, so it is a constant, not a build option.
constexpr const wchar_t* kProviderClsid = L"{6E2B1FA0-71D4-4C57-9E4A-3B2D5F8C1A77}";
constexpr const wchar_t* kFilterClsid   = L"{9C1D3E82-40B6-4F1D-8A57-2E9C6B04D311}";
constexpr const wchar_t* kProviderName  = L"XPLogin10 Credential Provider";
constexpr const wchar_t* kFilterName    = L"XPLogin10 Credential Provider Filter";
constexpr const wchar_t* kConfigKeyPath = L"SOFTWARE\\XPLogin10";
constexpr const wchar_t* kServiceName   = L"XPLoginWatchdog";

enum class RegHive {
    LocalMachine = 0,
    ClassesRoot,
    // HKEY_USERS. Only ever used for the .DEFAULT profile, which is the hive
    // Winlogon reads for the sign-in desktop - there is no logged-on user to
    // have preferences yet, so that is where the keyboard state lives.
    Users,
};

enum class RegValueType {
    String = 0,
    ExpandString,
    Dword,
};

enum class RegOpKind {
    CreateKey = 0,
    SetValue,
    DeleteValue,
    DeleteKeyTree,
};

struct RegOperation {
    RegOpKind    kind = RegOpKind::CreateKey;
    RegHive      hive = RegHive::LocalMachine;
    std::wstring path;          // no leading backslash
    std::wstring valueName;     // empty means the key's default value
    RegValueType valueType = RegValueType::String;
    std::wstring stringData;
    uint32_t     dwordData = 0;

    // Safe to fail: used for cleanup steps that may legitimately not exist.
    bool optional = false;
};

struct InstallOptions {
    std::wstring installRoot;           // C:\Program Files\XPLogin10
    std::wstring dllPath;               // absolute path to XPLoginProvider.dll
    std::wstring servicePath;           // absolute path to XPLoginWatchdog.exe
    std::wstring configPath;            // absolute path to XPLogin.ini
    std::wstring uninstallerPath;       // what Programs and Features will run
    bool         registerFilter = true; // hide the stock Windows tiles
    bool         installService = true;
    bool         disableLockScreenImage = true; // go straight to the XP screen on Win+L
    bool         disableCtrlAltDel = false;     // XP welcome screen had no CAD prompt
    // Num Lock on at the sign-in screen, so the number pad types digits.
    // Windows' own mechanism: InitialKeyboardIndicators under the default user
    // profile, which is the hive Winlogon reads before anybody has signed in.
    // Off by default in Windows, and there is no way to reach it from the
    // sign-in screen itself - which is why a PIN or a password typed on the
    // number pad silently does nothing.
    bool         numLockAtLogon = true;

    // Take a System Restore checkpoint before touching anything. On by default:
    // this is the one-click way back for somebody who did not read the README.
    bool         createRestorePoint = true;
    // Windows silently skips a checkpoint if one was made in the last 24 hours.
    // Setting this clears that rate limit for the duration of the install and
    // puts the original value back afterwards.
    bool         forceRestorePoint = false;
    // Add the Programs and Features entry.
    bool         registerArp = true;
};

// Values shown in Programs and Features (Apps and features on Windows 10/11).
constexpr const wchar_t* kArpKeyPath =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\XPLogin10";
constexpr const wchar_t* kProductName    = L"XPLogin10 - Windows XP logon screen";
constexpr const wchar_t* kProductVersion = L"1.0.0";
constexpr const wchar_t* kPublisher      = L"XPLogin10 project";

class RegistrationPlan {
public:
    // Everything needed to make Windows load the provider.
    static std::vector<RegOperation> BuildInstall(const InstallOptions& options);

    // The exact inverse. Every key BuildInstall creates is removed, and every
    // Windows-owned value it changed is restored.
    static std::vector<RegOperation> BuildUninstall(const InstallOptions& options);

    // Disables the provider without uninstalling it - the recovery path the
    // watchdog uses after a crash loop, and what `/disable` writes.
    static std::vector<RegOperation> BuildDisable();
    static std::vector<RegOperation> BuildEnable();

    // Safety gate. Returns false for any path this project has no business
    // writing to. Every operation is checked before execution.
    static bool IsPathAllowed(RegHive hive, const std::wstring& path);

    // True when every operation in the plan passes IsPathAllowed.
    static bool Validate(const std::vector<RegOperation>& plan,
                         std::wstring* offendingPath = nullptr);

    // A .reg file that undoes an installation from WinRE, for the case where
    // the machine will not boot far enough to run the uninstaller.
    static std::string BuildRecoveryRegFile();

    // Human-readable dump for `/preview`.
    static std::wstring Describe(const std::vector<RegOperation>& plan);
};

} // namespace xplogin
