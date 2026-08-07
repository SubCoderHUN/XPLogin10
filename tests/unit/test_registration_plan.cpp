// Unit tests: the installer's registry plan.
//
// A wrong key here is the difference between "the XP screen appears" and "this
// machine no longer boots to a logon prompt", so the plan is asserted key by
// key and the safety gate is attacked deliberately.
#include "xplogin/RegistrationPlan.h"
#include "xplogin/StringUtil.h"
#include "xptest.h"

#include <algorithm>

using namespace xplogin;

namespace {

InstallOptions DefaultOptions() {
    InstallOptions options;
    options.dllPath = L"C:\\Program Files\\XPLogin10\\XPLoginProvider.dll";
    options.servicePath = L"C:\\Program Files\\XPLogin10\\XPLoginWatchdog.exe";
    options.configPath = L"C:\\Program Files\\XPLogin10\\XPLogin.ini";
    return options;
}

bool HasOperation(const std::vector<RegOperation>& plan, RegOpKind kind, RegHive hive,
                  const std::wstring& pathSuffix, const std::wstring& valueName = L"") {
    for (const RegOperation& op : plan) {
        if (op.kind != kind || op.hive != hive) {
            continue;
        }
        if (op.path.size() < pathSuffix.size()) {
            continue;
        }
        const std::wstring tail = op.path.substr(op.path.size() - pathSuffix.size());
        if (!EqualsNoCase(tail, pathSuffix)) {
            continue;
        }
        if (!valueName.empty() && !EqualsNoCase(op.valueName, valueName)) {
            continue;
        }
        return true;
    }
    return false;
}

const RegOperation* FindValue(const std::vector<RegOperation>& plan,
                              const std::wstring& pathContains,
                              const std::wstring& valueName) {
    for (const RegOperation& op : plan) {
        if (op.kind != RegOpKind::SetValue) {
            continue;
        }
        if (op.path.find(pathContains) == std::wstring::npos) {
            continue;
        }
        if (EqualsNoCase(op.valueName, valueName)) {
            return &op;
        }
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

TEST(RegistrationPlan, InstallRegistersTheComServer) {
    const auto plan = RegistrationPlan::BuildInstall(DefaultOptions());

    CHECK(HasOperation(plan, RegOpKind::CreateKey, RegHive::ClassesRoot,
                       std::wstring(kProviderClsid)));
    CHECK(HasOperation(plan, RegOpKind::CreateKey, RegHive::ClassesRoot,
                       L"InprocServer32"));

    const RegOperation* server = FindValue(plan, kProviderClsid, L"");
    REQUIRE(server != nullptr);

    const RegOperation* dll = nullptr;
    for (const RegOperation& op : plan) {
        if (op.kind == RegOpKind::SetValue && op.valueName.empty() &&
            op.path.find(L"InprocServer32") != std::wstring::npos &&
            op.path.find(kProviderClsid) != std::wstring::npos) {
            dll = &op;
        }
    }
    REQUIRE(dll != nullptr);
    CHECK_EQ(dll->stringData, DefaultOptions().dllPath);
}

TEST(RegistrationPlan, InstallUsesApartmentThreading) {
    // LogonUI is an STA host. Anything else and CoCreateInstance marshals the
    // provider onto a different thread, which breaks the window we create.
    const auto plan = RegistrationPlan::BuildInstall(DefaultOptions());
    const RegOperation* threading = FindValue(plan, kProviderClsid, L"ThreadingModel");
    REQUIRE(threading != nullptr);
    CHECK_EQ(threading->stringData, std::wstring(L"Apartment"));
}

TEST(RegistrationPlan, InstallAnnouncesTheProviderToWindows) {
    const auto plan = RegistrationPlan::BuildInstall(DefaultOptions());
    bool found = false;
    for (const RegOperation& op : plan) {
        if (op.hive == RegHive::LocalMachine &&
            op.path.find(L"Authentication\\Credential Providers") !=
                std::wstring::npos &&
            op.path.find(kProviderClsid) != std::wstring::npos) {
            found = true;
        }
    }
    CHECK(found);
}

TEST(RegistrationPlan, InstallRegistersTheFilterWhenAsked) {
    InstallOptions options = DefaultOptions();
    options.registerFilter = true;
    const auto plan = RegistrationPlan::BuildInstall(options);

    bool found = false;
    for (const RegOperation& op : plan) {
        if (op.path.find(L"Credential Provider Filters") != std::wstring::npos &&
            op.path.find(kFilterClsid) != std::wstring::npos) {
            found = true;
        }
    }
    CHECK(found);
}

TEST(RegistrationPlan, InstallSkipsTheFilterWhenNotAsked) {
    InstallOptions options = DefaultOptions();
    options.registerFilter = false;
    const auto plan = RegistrationPlan::BuildInstall(options);

    for (const RegOperation& op : plan) {
        CHECK(op.path.find(L"Credential Provider Filters") == std::wstring::npos);
    }
}

TEST(RegistrationPlan, InstallSeedsTheHealthCountersAtZero) {
    const auto plan = RegistrationPlan::BuildInstall(DefaultOptions());

    const RegOperation* attempts = FindValue(plan, L"Health", L"StartAttempts");
    REQUIRE(attempts != nullptr);
    CHECK_EQ(static_cast<int>(attempts->valueType),
             static_cast<int>(RegValueType::Dword));
    CHECK_EQ(attempts->dwordData, 0u);

    const RegOperation* disabled = FindValue(plan, L"Health", L"Disabled");
    REQUIRE(disabled != nullptr);
    CHECK_EQ(disabled->dwordData, 0u);
}

TEST(RegistrationPlan, InstallStoresTheConfigPath) {
    const auto plan = RegistrationPlan::BuildInstall(DefaultOptions());
    const RegOperation* configPath = FindValue(plan, kConfigKeyPath, L"ConfigPath");
    REQUIRE(configPath != nullptr);
    CHECK_EQ(configPath->stringData, DefaultOptions().configPath);
}

TEST(RegistrationPlan, LockScreenSuppressionIsOptional) {
    InstallOptions on = DefaultOptions();
    on.disableLockScreenImage = true;
    CHECK(FindValue(RegistrationPlan::BuildInstall(on), L"Personalization",
                    L"NoLockScreen") != nullptr);

    InstallOptions off = DefaultOptions();
    off.disableLockScreenImage = false;
    CHECK(FindValue(RegistrationPlan::BuildInstall(off), L"Personalization",
                    L"NoLockScreen") == nullptr);
}

TEST(RegistrationPlan, CtrlAltDeleteSuppressionIsOffByDefault) {
    const auto plan = RegistrationPlan::BuildInstall(DefaultOptions());
    CHECK(FindValue(plan, L"Winlogon", L"DisableCAD") == nullptr);

    InstallOptions options = DefaultOptions();
    options.disableCtrlAltDel = true;
    const RegOperation* cad =
        FindValue(RegistrationPlan::BuildInstall(options), L"Winlogon", L"DisableCAD");
    REQUIRE(cad != nullptr);
    CHECK_EQ(cad->dwordData, 1u);
}

// ---------------------------------------------------------------------------
// Uninstall
// ---------------------------------------------------------------------------

TEST(RegistrationPlan, UninstallRemovesEverythingInstallCreated) {
    const InstallOptions options = DefaultOptions();
    const auto uninstall = RegistrationPlan::BuildUninstall(options);

    CHECK(HasOperation(uninstall, RegOpKind::DeleteKeyTree, RegHive::ClassesRoot,
                       std::wstring(kProviderClsid)));
    CHECK(HasOperation(uninstall, RegOpKind::DeleteKeyTree, RegHive::ClassesRoot,
                       std::wstring(kFilterClsid)));
    CHECK(HasOperation(uninstall, RegOpKind::DeleteKeyTree, RegHive::LocalMachine,
                       std::wstring(kProviderClsid)));
    CHECK(HasOperation(uninstall, RegOpKind::DeleteKeyTree, RegHive::LocalMachine,
                       std::wstring(kConfigKeyPath)));
}

TEST(RegistrationPlan, UninstallRemovesTheFilterBeforeTheProvider) {
    // Order matters: stop Windows from loading us before deleting the code it
    // would load, otherwise a logon in between finds a dangling registration.
    const auto plan = RegistrationPlan::BuildUninstall(DefaultOptions());

    size_t filterIndex = plan.size();
    size_t providerIndex = plan.size();
    for (size_t i = 0; i < plan.size(); ++i) {
        if (plan[i].path.find(L"Credential Provider Filters") != std::wstring::npos) {
            filterIndex = std::min(filterIndex, i);
        }
        if (plan[i].hive == RegHive::ClassesRoot &&
            plan[i].path.find(kProviderClsid) != std::wstring::npos) {
            providerIndex = std::min(providerIndex, i);
        }
    }
    REQUIRE(filterIndex < plan.size());
    REQUIRE(providerIndex < plan.size());
    CHECK_LT(filterIndex, providerIndex);
}

TEST(RegistrationPlan, UninstallOnlyRemovesSingleValuesFromWindowsOwnedKeys) {
    InstallOptions options = DefaultOptions();
    options.disableLockScreenImage = true;
    options.disableCtrlAltDel = true;
    const auto plan = RegistrationPlan::BuildUninstall(options);

    for (const RegOperation& op : plan) {
        const bool sharedKey =
            op.path.find(L"Personalization") != std::wstring::npos ||
            op.path.find(L"Winlogon") != std::wstring::npos;
        if (sharedKey) {
            // Deleting the whole Winlogon key would be catastrophic.
            CHECK_NE(static_cast<int>(op.kind),
                     static_cast<int>(RegOpKind::DeleteKeyTree));
            CHECK_EQ(static_cast<int>(op.kind),
                     static_cast<int>(RegOpKind::DeleteValue));
        }
    }
}

TEST(RegistrationPlan, CleanupOperationsAreOptional) {
    // An uninstall must succeed even on a half-installed machine.
    const auto plan = RegistrationPlan::BuildUninstall(DefaultOptions());
    for (const RegOperation& op : plan) {
        if (op.kind == RegOpKind::DeleteKeyTree || op.kind == RegOpKind::DeleteValue) {
            CHECK(op.optional);
        }
    }
}

// ---------------------------------------------------------------------------
// Disable / enable
// ---------------------------------------------------------------------------

TEST(RegistrationPlan, DisableClearsTheFilterSoWindowsTilesComeBack) {
    const auto plan = RegistrationPlan::BuildDisable();

    CHECK(FindValue(plan, kConfigKeyPath, L"Enabled") != nullptr);
    CHECK_EQ(FindValue(plan, kConfigKeyPath, L"Enabled")->dwordData, 0u);
    CHECK(HasOperation(plan, RegOpKind::DeleteKeyTree, RegHive::LocalMachine,
                       std::wstring(kFilterClsid)));
}

TEST(RegistrationPlan, EnableRestoresTheFilterAndResetsStrikes) {
    const auto plan = RegistrationPlan::BuildEnable();

    CHECK_EQ(FindValue(plan, kConfigKeyPath, L"Enabled")->dwordData, 1u);
    CHECK_EQ(FindValue(plan, L"Health", L"StartAttempts")->dwordData, 0u);
    CHECK_EQ(FindValue(plan, L"Health", L"Disabled")->dwordData, 0u);
    CHECK(HasOperation(plan, RegOpKind::CreateKey, RegHive::LocalMachine,
                       std::wstring(kFilterClsid)));
}

// ---------------------------------------------------------------------------
// The safety gate
// ---------------------------------------------------------------------------

TEST(RegistrationPlan, EveryGeneratedPlanPassesValidation) {
    InstallOptions options = DefaultOptions();
    options.disableCtrlAltDel = true;
    std::wstring offender;

    CHECK(RegistrationPlan::Validate(RegistrationPlan::BuildInstall(options),
                                     &offender));
    CHECK_EQ(offender, std::wstring(L""));
    CHECK(RegistrationPlan::Validate(RegistrationPlan::BuildUninstall(options)));
    CHECK(RegistrationPlan::Validate(RegistrationPlan::BuildDisable()));
    CHECK(RegistrationPlan::Validate(RegistrationPlan::BuildEnable()));
}

TEST(RegistrationPlan, RejectsWritesOutsideOurKeys) {
    CHECK_FALSE(RegistrationPlan::IsPathAllowed(RegHive::LocalMachine,
                                                L"SOFTWARE\\Microsoft\\Windows"));
    CHECK_FALSE(RegistrationPlan::IsPathAllowed(RegHive::LocalMachine,
                                                L"SYSTEM\\CurrentControlSet\\Services"));
    CHECK_FALSE(
        RegistrationPlan::IsPathAllowed(RegHive::LocalMachine, L"SAM\\SAM\\Domains"));
    CHECK_FALSE(RegistrationPlan::IsPathAllowed(RegHive::ClassesRoot, L"CLSID"));
    CHECK_FALSE(RegistrationPlan::IsPathAllowed(
        RegHive::ClassesRoot, L"CLSID\\{00000000-0000-0000-0000-000000000000}"));
}

TEST(RegistrationPlan, RejectsMalformedPaths) {
    CHECK_FALSE(RegistrationPlan::IsPathAllowed(RegHive::LocalMachine, L""));
    CHECK_FALSE(
        RegistrationPlan::IsPathAllowed(RegHive::LocalMachine, L"\\SOFTWARE\\XPLogin10"));
    CHECK_FALSE(
        RegistrationPlan::IsPathAllowed(RegHive::LocalMachine, L"SOFTWARE\\XPLogin10\\"));
    CHECK_FALSE(RegistrationPlan::IsPathAllowed(RegHive::LocalMachine,
                                                L"SOFTWARE\\XPLogin10\\..\\Microsoft"));
}

TEST(RegistrationPlan, ValidateRejectsATamperedPlan) {
    auto plan = RegistrationPlan::BuildInstall(DefaultOptions());
    RegOperation malicious;
    malicious.kind = RegOpKind::DeleteKeyTree;
    malicious.hive = RegHive::LocalMachine;
    malicious.path = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    plan.push_back(malicious);

    std::wstring offender;
    CHECK_FALSE(RegistrationPlan::Validate(plan, &offender));
    CHECK_EQ(offender, malicious.path);
}

TEST(RegistrationPlan, ValidateRejectsDeletingTheWholeWinlogonKey) {
    std::vector<RegOperation> plan;
    RegOperation op;
    op.kind = RegOpKind::DeleteKeyTree;
    op.hive = RegHive::LocalMachine;
    op.path = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
    plan.push_back(op);

    // The path is on the allow list (we set DisableCAD there) but removing the
    // whole tree would take Shell, Userinit and everything else with it.
    CHECK(RegistrationPlan::IsPathAllowed(op.hive, op.path));
    CHECK_FALSE(RegistrationPlan::Validate(plan));
}

// ---------------------------------------------------------------------------
// Recovery artefacts
// ---------------------------------------------------------------------------

TEST(RegistrationPlan, RecoveryRegFileRemovesTheRegistrations) {
    const std::string reg = RegistrationPlan::BuildRecoveryRegFile();

    CHECK(reg.find("Windows Registry Editor Version 5.00") == 0);
    // A leading '-' on the key name is what makes regedit delete it.
    CHECK(reg.find("[-HKEY_LOCAL_MACHINE") != std::string::npos);
    CHECK(reg.find("Credential Provider Filters") != std::string::npos);
    CHECK(reg.find("\"Disabled\"=dword:00000001") != std::string::npos);
    CHECK(reg.find("\"NoLockScreen\"=-") != std::string::npos);
}

TEST(RegistrationPlan, DescribeMentionsEveryOperation) {
    const auto plan = RegistrationPlan::BuildInstall(DefaultOptions());
    const std::wstring text = RegistrationPlan::Describe(plan);

    CHECK_FALSE(text.empty());
    size_t lines = 0;
    for (wchar_t c : text) {
        if (c == L'\n') {
            ++lines;
        }
    }
    CHECK_EQ(lines, plan.size());
    CHECK(text.find(L"Apartment") != std::wstring::npos);
}

TEST(RegistrationPlan, ClsidsAreWellFormedAndDistinct) {
    const std::wstring provider = kProviderClsid;
    const std::wstring filter = kFilterClsid;

    CHECK_EQ(provider.size(), size_t(38)); // {8-4-4-4-12}
    CHECK_EQ(filter.size(), size_t(38));
    CHECK_EQ(provider.front(), L'{');
    CHECK_EQ(provider.back(), L'}');
    CHECK_NE(provider, filter);
}

// ---------------------------------------------------------------------------
// Num Lock at the sign-in screen.
//
// Windows leaves Num Lock off there, and there is no way to reach the setting
// from the sign-in screen itself - so a password typed on the number pad
// silently produces nothing at all. The fix is Windows' own mechanism:
// InitialKeyboardIndicators under the .DEFAULT profile, which is the hive
// Winlogon reads when nobody has signed in yet.
// ---------------------------------------------------------------------------

TEST(RegistrationPlan, NumLockIsTurnedOnForTheSignInScreen) {
    InstallOptions options = DefaultOptions();
    const auto plan = RegistrationPlan::BuildInstall(options);

    bool found = false;
    for (const RegOperation& op : plan) {
        if (op.hive == RegHive::Users && op.kind == RegOpKind::SetValue &&
            EqualsNoCase(op.valueName, L"InitialKeyboardIndicators")) {
            found = true;
            // A REG_SZ bitmask: 1 Caps Lock, 2 Num Lock, 4 Scroll Lock. Just
            // Num Lock, so an install does not start switching Caps Lock on.
            CHECK_EQ(static_cast<int>(op.valueType),
                     static_cast<int>(RegValueType::String));
            CHECK_EQ(op.stringData, std::wstring(L"2"));
            CHECK(StartsWithNoCase(op.path, L".DEFAULT\\"));
        }
    }
    CHECK(found);
    CHECK(RegistrationPlan::Validate(plan));
}

TEST(RegistrationPlan, UninstallPutsTheKeyboardBackAsWindowsShipsIt) {
    InstallOptions options = DefaultOptions();
    const auto plan = RegistrationPlan::BuildUninstall(options);

    bool found = false;
    for (const RegOperation& op : plan) {
        if (op.hive == RegHive::Users &&
            EqualsNoCase(op.valueName, L"InitialKeyboardIndicators")) {
            found = true;
            // Deleted, not set to "0": Windows ships with no value there, and
            // an uninstall owes the machine the state it found.
            CHECK_EQ(static_cast<int>(op.kind),
                     static_cast<int>(RegOpKind::DeleteValue));
        }
    }
    CHECK(found);
    CHECK(RegistrationPlan::Validate(plan));
}

TEST(RegistrationPlan, NumLockCanBeDeclined) {
    InstallOptions options = DefaultOptions();
    options.numLockAtLogon = false;
    for (const RegOperation& op : RegistrationPlan::BuildInstall(options)) {
        CHECK(op.hive != RegHive::Users);
    }
}

TEST(RegistrationPlan, TheUsersHiveIsOnlyEverTheDefaultProfilesKeyboard) {
    // HKEY_USERS holds every signed-in account's whole registry. The guard has
    // to be as narrow as the one use: a plan that could write anywhere else
    // under HKU is a plan that could rewrite somebody's profile.
    CHECK(RegistrationPlan::IsPathAllowed(RegHive::Users,
                                          L".DEFAULT\\Control Panel\\Keyboard"));
    CHECK_FALSE(RegistrationPlan::IsPathAllowed(RegHive::Users, L".DEFAULT"));
    CHECK_FALSE(RegistrationPlan::IsPathAllowed(
        RegHive::Users, L"S-1-5-21-1-2-3-1001\\Control Panel\\Keyboard"));
    CHECK_FALSE(RegistrationPlan::IsPathAllowed(RegHive::Users,
                                                L".DEFAULT\\Software"));
    CHECK_FALSE(RegistrationPlan::IsPathAllowed(RegHive::Users, L""));
}
