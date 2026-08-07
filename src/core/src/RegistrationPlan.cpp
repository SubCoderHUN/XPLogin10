#include "xplogin/RegistrationPlan.h"

#include "xplogin/StringUtil.h"

#include <sstream>

namespace xplogin {
namespace {

const wchar_t* const kCredentialProvidersKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential "
    L"Providers";
const wchar_t* const kCredentialProviderFiltersKey =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication\\Credential "
    L"Provider Filters";
const wchar_t* const kPersonalizationKey =
    L"SOFTWARE\\Policies\\Microsoft\\Windows\\Personalization";
const wchar_t* const kWinlogonKey =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
const wchar_t* const kServicesKey = L"SYSTEM\\CurrentControlSet\\Services";
// The keyboard state Winlogon applies to the sign-in desktop. .DEFAULT is the
// profile it reads when nobody has signed in yet.
const wchar_t* const kDefaultKeyboardKey =
    L".DEFAULT\\Control Panel\\Keyboard";
// InitialKeyboardIndicators is a REG_SZ bitmask: 1 Caps Lock, 2 Num Lock,
// 4 Scroll Lock. "2" is Num Lock on and nothing else, which is what a machine
// with a number pad wants and what Windows does not do by default.
const wchar_t* const kNumLockOn = L"2";

RegOperation CreateKey(RegHive hive, std::wstring path) {
    RegOperation op;
    op.kind = RegOpKind::CreateKey;
    op.hive = hive;
    op.path = std::move(path);
    return op;
}

RegOperation SetString(RegHive hive, std::wstring path, std::wstring valueName,
                       std::wstring data) {
    RegOperation op;
    op.kind = RegOpKind::SetValue;
    op.hive = hive;
    op.path = std::move(path);
    op.valueName = std::move(valueName);
    op.valueType = RegValueType::String;
    op.stringData = std::move(data);
    return op;
}

RegOperation SetDword(RegHive hive, std::wstring path, std::wstring valueName,
                      uint32_t data) {
    RegOperation op;
    op.kind = RegOpKind::SetValue;
    op.hive = hive;
    op.path = std::move(path);
    op.valueName = std::move(valueName);
    op.valueType = RegValueType::Dword;
    op.dwordData = data;
    return op;
}

RegOperation DeleteTree(RegHive hive, std::wstring path, bool optional = true) {
    RegOperation op;
    op.kind = RegOpKind::DeleteKeyTree;
    op.hive = hive;
    op.path = std::move(path);
    op.optional = optional;
    return op;
}

RegOperation DeleteValue(RegHive hive, std::wstring path, std::wstring valueName) {
    RegOperation op;
    op.kind = RegOpKind::DeleteValue;
    op.hive = hive;
    op.path = std::move(path);
    op.valueName = std::move(valueName);
    op.optional = true;
    return op;
}

std::wstring ProviderKeyPath() {
    return std::wstring(kCredentialProvidersKey) + L"\\" + kProviderClsid;
}

std::wstring FilterKeyPath() {
    return std::wstring(kCredentialProviderFiltersKey) + L"\\" + kFilterClsid;
}

std::wstring ClsidKeyPath(const wchar_t* clsid) {
    return std::wstring(L"CLSID\\") + clsid;
}

// Reported to Programs and Features so the size column is not blank. The
// payload is a little under a megabyte; the value is in KB.
constexpr uint32_t kEstimatedSizeKb = 900;

std::wstring DirectoryOf(const std::wstring& filePath) {
    const size_t slash = filePath.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : filePath.substr(0, slash);
}

std::wstring EscapeRegString(const std::wstring& in) {
    std::wstring out;
    for (wchar_t c : in) {
        if (c == L'\\' || c == L'"') {
            out.push_back(L'\\');
        }
        out.push_back(c);
    }
    return out;
}

} // namespace

std::vector<RegOperation> RegistrationPlan::BuildInstall(
    const InstallOptions& options) {
    std::vector<RegOperation> plan;

    // --- COM registration for the provider ---------------------------------
    plan.push_back(CreateKey(RegHive::ClassesRoot, ClsidKeyPath(kProviderClsid)));
    plan.push_back(SetString(RegHive::ClassesRoot, ClsidKeyPath(kProviderClsid), L"",
                             kProviderName));
    plan.push_back(CreateKey(RegHive::ClassesRoot,
                             ClsidKeyPath(kProviderClsid) + L"\\InprocServer32"));
    plan.push_back(SetString(RegHive::ClassesRoot,
                             ClsidKeyPath(kProviderClsid) + L"\\InprocServer32", L"",
                             options.dllPath));
    // LogonUI is single threaded apartment; anything else and CoCreateInstance
    // marshals us onto another thread, which breaks the UI window.
    plan.push_back(SetString(RegHive::ClassesRoot,
                             ClsidKeyPath(kProviderClsid) + L"\\InprocServer32",
                             L"ThreadingModel", L"Apartment"));

    // --- tell Windows this CLSID is a credential provider -------------------
    plan.push_back(CreateKey(RegHive::LocalMachine, ProviderKeyPath()));
    plan.push_back(
        SetString(RegHive::LocalMachine, ProviderKeyPath(), L"", kProviderName));

    // --- the filter that hides the stock Windows tiles ----------------------
    if (options.registerFilter) {
        plan.push_back(CreateKey(RegHive::ClassesRoot, ClsidKeyPath(kFilterClsid)));
        plan.push_back(SetString(RegHive::ClassesRoot, ClsidKeyPath(kFilterClsid), L"",
                                 kFilterName));
        plan.push_back(CreateKey(RegHive::ClassesRoot,
                                 ClsidKeyPath(kFilterClsid) + L"\\InprocServer32"));
        plan.push_back(SetString(RegHive::ClassesRoot,
                                 ClsidKeyPath(kFilterClsid) + L"\\InprocServer32", L"",
                                 options.dllPath));
        plan.push_back(SetString(RegHive::ClassesRoot,
                                 ClsidKeyPath(kFilterClsid) + L"\\InprocServer32",
                                 L"ThreadingModel", L"Apartment"));
        plan.push_back(CreateKey(RegHive::LocalMachine, FilterKeyPath()));
        plan.push_back(
            SetString(RegHive::LocalMachine, FilterKeyPath(), L"", kFilterName));
    }

    // --- our own configuration ---------------------------------------------
    plan.push_back(CreateKey(RegHive::LocalMachine, kConfigKeyPath));
    plan.push_back(
        SetString(RegHive::LocalMachine, kConfigKeyPath, L"ConfigPath",
                  options.configPath));
    plan.push_back(SetString(RegHive::LocalMachine, kConfigKeyPath, L"InstallPath",
                             options.dllPath));
    plan.push_back(SetDword(RegHive::LocalMachine, kConfigKeyPath, L"Enabled", 1));
    plan.push_back(CreateKey(RegHive::LocalMachine,
                             std::wstring(kConfigKeyPath) + L"\\Health"));
    plan.push_back(SetDword(RegHive::LocalMachine,
                            std::wstring(kConfigKeyPath) + L"\\Health",
                            L"StartAttempts", 0));
    plan.push_back(SetDword(RegHive::LocalMachine,
                            std::wstring(kConfigKeyPath) + L"\\Health", L"Disabled",
                            0));

    // Remember what the machine looked like so uninstall can put it back.
    if (options.disableLockScreenImage) {
        plan.push_back(CreateKey(RegHive::LocalMachine, kPersonalizationKey));
        plan.push_back(
            SetDword(RegHive::LocalMachine, kPersonalizationKey, L"NoLockScreen", 1));
    }
    if (options.disableCtrlAltDel) {
        plan.push_back(
            SetDword(RegHive::LocalMachine, kWinlogonKey, L"DisableCAD", 1));
    }
    if (options.numLockAtLogon) {
        plan.push_back(CreateKey(RegHive::Users, kDefaultKeyboardKey));
        plan.push_back(SetString(RegHive::Users, kDefaultKeyboardKey,
                                 L"InitialKeyboardIndicators", kNumLockOn));
    }

    // --- Programs and Features -------------------------------------------
    // Without this the only way to remove the software is a command line,
    // which is not a reasonable thing to ask of somebody who installed it by
    // double-clicking.
    if (options.registerArp) {
        const std::wstring root =
            options.installRoot.empty() ? DirectoryOf(options.dllPath)
                                        : options.installRoot;
        const std::wstring uninstaller =
            options.uninstallerPath.empty()
                ? (root + L"\\XPLogin10-Setup.exe")
                : options.uninstallerPath;

        plan.push_back(CreateKey(RegHive::LocalMachine, kArpKeyPath));
        plan.push_back(
            SetString(RegHive::LocalMachine, kArpKeyPath, L"DisplayName",
                      kProductName));
        plan.push_back(SetString(RegHive::LocalMachine, kArpKeyPath,
                                 L"DisplayVersion", kProductVersion));
        plan.push_back(
            SetString(RegHive::LocalMachine, kArpKeyPath, L"Publisher", kPublisher));
        plan.push_back(SetString(RegHive::LocalMachine, kArpKeyPath,
                                 L"InstallLocation", root));
        plan.push_back(SetString(RegHive::LocalMachine, kArpKeyPath,
                                 L"UninstallString",
                                 L"\"" + uninstaller + L"\" /uninstall"));
        plan.push_back(SetString(RegHive::LocalMachine, kArpKeyPath,
                                 L"QuietUninstallString",
                                 L"\"" + uninstaller + L"\" /uninstall /silent"));
        plan.push_back(SetString(RegHive::LocalMachine, kArpKeyPath,
                                 L"DisplayIcon", options.dllPath + L",0"));
        // There is nothing to modify or repair, so Windows should not offer it.
        plan.push_back(SetDword(RegHive::LocalMachine, kArpKeyPath, L"NoModify", 1));
        plan.push_back(SetDword(RegHive::LocalMachine, kArpKeyPath, L"NoRepair", 1));
        plan.push_back(SetDword(RegHive::LocalMachine, kArpKeyPath, L"EstimatedSize",
                                kEstimatedSizeKb));
    }

    return plan;
}

std::vector<RegOperation> RegistrationPlan::BuildUninstall(
    const InstallOptions& options) {
    std::vector<RegOperation> plan;

    // Unregister in the reverse order of installation: stop Windows loading us
    // before removing the code it would load.
    plan.push_back(DeleteTree(RegHive::LocalMachine, FilterKeyPath()));
    plan.push_back(DeleteTree(RegHive::LocalMachine, ProviderKeyPath()));
    plan.push_back(DeleteTree(RegHive::ClassesRoot, ClsidKeyPath(kFilterClsid)));
    plan.push_back(DeleteTree(RegHive::ClassesRoot, ClsidKeyPath(kProviderClsid)));

    if (options.disableLockScreenImage) {
        plan.push_back(
            DeleteValue(RegHive::LocalMachine, kPersonalizationKey, L"NoLockScreen"));
    }
    if (options.disableCtrlAltDel) {
        plan.push_back(DeleteValue(RegHive::LocalMachine, kWinlogonKey, L"DisableCAD"));
    }
    if (options.numLockAtLogon) {
        // Deleting the value rather than writing "0" puts the key back to the
        // state Windows ships with, which is what an uninstall owes.
        plan.push_back(DeleteValue(RegHive::Users, kDefaultKeyboardKey,
                                   L"InitialKeyboardIndicators"));
    }

    if (options.installService) {
        plan.push_back(DeleteTree(RegHive::LocalMachine,
                                  std::wstring(kServicesKey) + L"\\" + kServiceName));
    }

    if (options.registerArp) {
        plan.push_back(DeleteTree(RegHive::LocalMachine, kArpKeyPath));
    }

    plan.push_back(DeleteTree(RegHive::LocalMachine, kConfigKeyPath));
    return plan;
}

std::vector<RegOperation> RegistrationPlan::BuildDisable() {
    std::vector<RegOperation> plan;
    plan.push_back(SetDword(RegHive::LocalMachine, kConfigKeyPath, L"Enabled", 0));
    plan.push_back(SetDword(RegHive::LocalMachine,
                            std::wstring(kConfigKeyPath) + L"\\Health", L"Disabled",
                            1));
    // Losing the filter is what actually brings the Windows tiles back, so do
    // it here too rather than relying on the provider reading its config.
    plan.push_back(DeleteTree(RegHive::LocalMachine, FilterKeyPath()));
    return plan;
}

std::vector<RegOperation> RegistrationPlan::BuildEnable() {
    std::vector<RegOperation> plan;
    plan.push_back(SetDword(RegHive::LocalMachine, kConfigKeyPath, L"Enabled", 1));
    plan.push_back(SetDword(RegHive::LocalMachine,
                            std::wstring(kConfigKeyPath) + L"\\Health", L"Disabled",
                            0));
    plan.push_back(SetDword(RegHive::LocalMachine,
                            std::wstring(kConfigKeyPath) + L"\\Health",
                            L"StartAttempts", 0));
    plan.push_back(CreateKey(RegHive::LocalMachine, FilterKeyPath()));
    plan.push_back(SetString(RegHive::LocalMachine, FilterKeyPath(), L"", kFilterName));
    return plan;
}

bool RegistrationPlan::IsPathAllowed(RegHive hive, const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    // No leading or trailing separators, no relative segments.
    if (path.front() == L'\\' || path.back() == L'\\') {
        return false;
    }
    if (path.find(L"..") != std::wstring::npos) {
        return false;
    }

    if (hive == RegHive::Users) {
        // Exactly one key, and only the default profile's - never a real
        // user's hive.
        return StartsWithNoCase(path, kDefaultKeyboardKey);
    }

    if (hive == RegHive::ClassesRoot) {
        // Only our two CLSIDs, never the whole CLSID hive.
        return StartsWithNoCase(path, ClsidKeyPath(kProviderClsid)) ||
               StartsWithNoCase(path, ClsidKeyPath(kFilterClsid));
    }

    // HKLM: a short, explicit allow list.
    const std::wstring allowed[] = {
        ProviderKeyPath(),
        FilterKeyPath(),
        kConfigKeyPath,
        kArpKeyPath,
        std::wstring(kServicesKey) + L"\\" + kServiceName,
        kPersonalizationKey,
        kWinlogonKey,
    };
    for (const std::wstring& prefix : allowed) {
        if (StartsWithNoCase(path, prefix)) {
            return true;
        }
    }
    return false;
}

bool RegistrationPlan::Validate(const std::vector<RegOperation>& plan,
                                std::wstring* offendingPath) {
    for (const RegOperation& op : plan) {
        if (!IsPathAllowed(op.hive, op.path)) {
            if (offendingPath) {
                *offendingPath = op.path;
            }
            return false;
        }
        // Deleting a whole tree is only ever allowed for keys we created. The
        // two shared Windows keys may have single values removed, never more.
        if (op.kind == RegOpKind::DeleteKeyTree && op.hive == RegHive::LocalMachine) {
            if (StartsWithNoCase(op.path, kPersonalizationKey) ||
                StartsWithNoCase(op.path, kWinlogonKey)) {
                if (offendingPath) {
                    *offendingPath = op.path;
                }
                return false;
            }
        }
    }
    return true;
}

std::string RegistrationPlan::BuildRecoveryRegFile() {
    std::wostringstream out;
    out << L"Windows Registry Editor Version 5.00\r\n\r\n";
    out << L"; XPLogin10 emergency recovery.\r\n"
        << L"; Import this from WinRE after loading the offline SOFTWARE hive as\r\n"
        << L"; HKLM\\OFFLINE, or run it from Safe Mode, to restore the stock\r\n"
        << L"; Windows logon screen. See README.md -> Recovery.\r\n\r\n";

    // Removing the filter is enough to get the Windows tiles back; removing the
    // provider registration stops the DLL being loaded at all.
    out << L"[-HKEY_LOCAL_MACHINE\\" << kCredentialProviderFiltersKey << L"\\"
        << kFilterClsid << L"]\r\n\r\n";
    out << L"[-HKEY_LOCAL_MACHINE\\" << kCredentialProvidersKey << L"\\"
        << kProviderClsid << L"]\r\n\r\n";
    out << L"[HKEY_LOCAL_MACHINE\\" << kConfigKeyPath << L"]\r\n";
    out << L"\"Enabled\"=dword:00000000\r\n\r\n";
    out << L"[HKEY_LOCAL_MACHINE\\" << kConfigKeyPath << L"\\Health]\r\n";
    out << L"\"Disabled\"=dword:00000001\r\n";
    out << L"\"StartAttempts\"=dword:00000000\r\n\r\n";
    out << L"[HKEY_LOCAL_MACHINE\\" << kPersonalizationKey << L"]\r\n";
    out << L"\"NoLockScreen\"=-\r\n\r\n";

    return WideToUtf8(out.str());
}

std::wstring RegistrationPlan::Describe(const std::vector<RegOperation>& plan) {
    std::wostringstream out;
    for (const RegOperation& op : plan) {
        const wchar_t* hive =
            (op.hive == RegHive::ClassesRoot) ? L"HKCR"
            : (op.hive == RegHive::Users)      ? L"HKU"
                                               : L"HKLM";
        switch (op.kind) {
            case RegOpKind::CreateKey:
                out << L"  create   " << hive << L"\\" << op.path << L"\r\n";
                break;
            case RegOpKind::SetValue:
                out << L"  set      " << hive << L"\\" << op.path << L" ["
                    << (op.valueName.empty() ? L"(default)" : op.valueName) << L"] = ";
                if (op.valueType == RegValueType::Dword) {
                    out << op.dwordData;
                } else {
                    out << L"\"" << EscapeRegString(op.stringData) << L"\"";
                }
                out << L"\r\n";
                break;
            case RegOpKind::DeleteValue:
                out << L"  delval   " << hive << L"\\" << op.path << L" ["
                    << op.valueName << L"]\r\n";
                break;
            case RegOpKind::DeleteKeyTree:
                out << L"  delkey   " << hive << L"\\" << op.path << L"\r\n";
                break;
        }
    }
    return out.str();
}

} // namespace xplogin
