// Unit tests: file deployment, the Programs and Features entry, and the order
// the setup does things in.
//
// The uninstall guard here is the most important assertion in the file: an
// installer that computes the wrong root and then removes a directory tree is
// not a bug anybody gets to apologise for afterwards.
#include "xplogin/DeploymentPlan.h"
#include "xplogin/RegistrationPlan.h"
#include "xplogin/StringUtil.h"
#include "Mocks.h"
#include "xptest.h"

#include <algorithm>

using namespace xplogin;
using namespace xplogin::testing;

namespace {

const wchar_t* kRoot = L"C:\\Program Files\\XPLogin10";

DeploymentOptions Options() {
    DeploymentOptions options;
    options.installRoot = kRoot;
    options.sourceRoot = L"D:\\build\\bin\\Release";
    return options;
}

InstallOptions FullInstallOptions() {
    InstallOptions options;
    options.installRoot = kRoot;
    options.dllPath = std::wstring(kRoot) + L"\\XPLoginProvider.dll";
    options.servicePath = std::wstring(kRoot) + L"\\XPLoginWatchdog.exe";
    options.configPath = std::wstring(kRoot) + L"\\XPLogin.ini";
    options.uninstallerPath = std::wstring(kRoot) + L"\\XPLogin10-Setup.exe";
    return options;
}

bool HasTarget(const std::vector<FileOperation>& plan, FileOpKind kind,
               const std::wstring& suffix) {
    for (const FileOperation& op : plan) {
        if (op.kind != kind || op.target.size() < suffix.size()) {
            continue;
        }
        if (EqualsNoCase(op.target.substr(op.target.size() - suffix.size()),
                         suffix)) {
            return true;
        }
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

int I(SetupStepKind kind) { return static_cast<int>(kind); }

} // namespace

// ---------------------------------------------------------------------------
// File deployment
// ---------------------------------------------------------------------------

TEST(Deployment, InstallCreatesTheDirectoryFirst) {
    const auto plan = DeploymentPlan::BuildInstall(Options());
    REQUIRE_FALSE(plan.empty());
    CHECK_EQ(static_cast<int>(plan.front().kind),
             static_cast<int>(FileOpKind::MakeDirectory));
    CHECK_EQ(plan.front().target, std::wstring(kRoot));
}

TEST(Deployment, InstallCopiesEveryPayloadFile) {
    const auto plan = DeploymentPlan::BuildInstall(Options());

    for (const PayloadFile& file : PayloadManifest()) {
        CHECK(HasTarget(plan, FileOpKind::Copy, file.name));
    }
    // And the uninstaller itself, so Programs and Features still has something
    // to run after the source folder is gone.
    CHECK(HasTarget(plan, FileOpKind::Copy, L"XPLogin10-Setup.exe"));
}

TEST(Deployment, RequiredAndOptionalPayloadFilesDiffer) {
    const auto plan = DeploymentPlan::BuildInstall(Options());

    for (const FileOperation& op : plan) {
        if (op.kind != FileOpKind::Copy) {
            continue;
        }
        if (op.target.find(L"XPLoginProvider.dll") != std::wstring::npos) {
            // Without the provider there is no product; this must be fatal.
            CHECK_FALSE(op.optional);
        }
        if (op.target.find(L"XPLogin.assets") != std::wstring::npos) {
            // The screen still comes up without the baked artwork, drawn from
            // primitives - so a build made with no XP files must still install.
            CHECK(op.optional);
        }
    }
}

TEST(Deployment, CopySourcesComeFromTheSourceRoot) {
    const auto plan = DeploymentPlan::BuildInstall(Options());
    for (const FileOperation& op : plan) {
        if (op.kind == FileOpKind::Copy) {
            CHECK(StartsWithNoCase(op.source, L"D:\\build\\bin\\Release"));
            CHECK(StartsWithNoCase(op.target, kRoot));
        }
    }
}

TEST(Deployment, UninstallRemovesEverythingInstallCreated) {
    const auto install = DeploymentPlan::BuildInstall(Options());
    const auto uninstall = DeploymentPlan::BuildUninstall(Options());

    for (const FileOperation& op : install) {
        if (op.kind != FileOpKind::Copy) {
            continue;
        }
        bool removed = false;
        for (const FileOperation& removal : uninstall) {
            if (removal.kind == FileOpKind::Delete &&
                EqualsNoCase(removal.target, op.target)) {
                removed = true;
            }
        }
        CHECK(removed);
    }
}

TEST(Deployment, EveryPayloadFileHasAUniqueNameAndResourceId) {
    // PayloadManifest() is the single list the setup embeds from, writes out
    // from, and uninstalls from. A duplicated id means one file silently
    // overwrites another inside XPLogin10-Setup.exe; a duplicated name means
    // one overwrites the other on disk. Neither would raise an error anywhere.
    const auto& manifest = PayloadManifest();
    REQUIRE_GT(manifest.size(), size_t(5));

    for (size_t i = 0; i < manifest.size(); ++i) {
        REQUIRE(manifest[i].name != nullptr);
        REQUIRE_FALSE(std::wstring(manifest[i].name).empty());
        // The ids are the contract with the generated payload.rc.
        REQUIRE_GE(manifest[i].resourceId, 101u);

        for (size_t j = i + 1; j < manifest.size(); ++j) {
            CHECK_FALSE(EqualsNoCase(manifest[i].name, manifest[j].name));
            CHECK(manifest[i].resourceId != manifest[j].resourceId);
        }
    }
}

TEST(Deployment, TheProviderAndItsConfigurationAreTheRequiredFiles) {
    // Required means "the install fails without it". Anything that only
    // changes how the screen looks or sounds must not be in that set, or a
    // build made without the reference material stops installing at all.
    for (const PayloadFile& file : PayloadManifest()) {
        const std::wstring name = file.name;
        const bool essential = name == L"XPLoginProvider.dll" ||
                               name == L"XPLoginWatchdog.exe" ||
                               name == L"xplogin-install.exe" ||
                               name == L"XPLogin.ini" ||
                               name == L"XPLogin.theme.ini";
        CHECK_EQ(file.required, essential);
    }
}

TEST(Deployment, TheXpArtworkAndSoundsShipButAreNotRequired) {
    // These are what make the product work out of the box, so they have to be
    // in the manifest. They are also all optional: a build made without the
    // reference material still installs, draws the screen from primitives and
    // stays silent, rather than refusing to install at all.
    const wchar_t* const extras[] = {
        L"XPLogin.assets",     L"XPLogin-logon.wav", L"XPLogin-logoff.wav",
        L"XPLogin-error.wav",  L"XPLogin-shutdown.wav",
    };

    for (const wchar_t* name : extras) {
        bool found = false;
        for (const PayloadFile& file : PayloadManifest()) {
            if (EqualsNoCase(file.name, name)) {
                found = true;
                CHECK_FALSE(file.required);
            }
        }
        REQUIRE(found);
    }

    // ...and every one of them is removed again on uninstall. Leaving 700 KB
    // of wav files behind would also stop the directory being removed.
    const auto uninstall = DeploymentPlan::BuildUninstall(Options());
    for (const wchar_t* name : extras) {
        CHECK(HasTarget(uninstall, FileOpKind::Delete, name));
    }
}

TEST(Deployment, UninstallAlsoRemovesRuntimeArtefacts) {
    const auto plan = DeploymentPlan::BuildUninstall(Options());

    // Logs and the recovery .reg are written after install; leaving them would
    // stop the directory being removed and make the uninstall look half done.
    CHECK(HasTarget(plan, FileOpKind::Delete, L"XPLogin.log"));
    CHECK(HasTarget(plan, FileOpKind::Delete, L"XPLoginWatchdog.log"));
    CHECK(HasTarget(plan, FileOpKind::Delete, L"XPLogin-Recovery.reg"));
}

TEST(Deployment, UninstallRemovesTheDirectoryLastAndOnlyOnce) {
    const auto plan = DeploymentPlan::BuildUninstall(Options());

    REQUIRE_FALSE(plan.empty());
    CHECK_EQ(static_cast<int>(plan.back().kind),
             static_cast<int>(FileOpKind::RemoveEmptyDirectory));
    CHECK_EQ(plan.back().target, std::wstring(kRoot));

    int directoryRemovals = 0;
    for (const FileOperation& op : plan) {
        if (op.kind == FileOpKind::RemoveEmptyDirectory) {
            ++directoryRemovals;
        }
    }
    CHECK_EQ(directoryRemovals, 1);
}

TEST(Deployment, EveryUninstallOperationIsOptional) {
    // A half-installed machine must still uninstall cleanly.
    const auto plan = DeploymentPlan::BuildUninstall(Options());
    for (const FileOperation& op : plan) {
        CHECK(op.optional);
    }
}

// ---------------------------------------------------------------------------
// The guard
// ---------------------------------------------------------------------------

TEST(Deployment, RejectsDangerousInstallRoots) {
    CHECK_FALSE(DeploymentPlan::IsInstallRootAllowed(L"C:\\Windows"));
    CHECK_FALSE(DeploymentPlan::IsInstallRootAllowed(L"C:\\Windows\\System32"));
    CHECK_FALSE(DeploymentPlan::IsInstallRootAllowed(L"C:\\Program Files"));
    CHECK_FALSE(DeploymentPlan::IsInstallRootAllowed(L"C:\\Program Files (x86)"));
    CHECK_FALSE(DeploymentPlan::IsInstallRootAllowed(L"C:\\Users"));
    CHECK_FALSE(DeploymentPlan::IsInstallRootAllowed(L"C:\\ProgramData"));
    CHECK_FALSE(DeploymentPlan::IsInstallRootAllowed(L"C:\\"));
    CHECK_FALSE(DeploymentPlan::IsInstallRootAllowed(L"C:"));
}

TEST(Deployment, RejectsRelativeAndTraversingRoots) {
    CHECK_FALSE(DeploymentPlan::IsInstallRootAllowed(L""));
    CHECK_FALSE(DeploymentPlan::IsInstallRootAllowed(L"XPLogin10"));
    CHECK_FALSE(DeploymentPlan::IsInstallRootAllowed(L"\\\\server\\share"));
    CHECK_FALSE(
        DeploymentPlan::IsInstallRootAllowed(L"C:\\Program Files\\..\\Windows"));
}

TEST(Deployment, AcceptsReasonableInstallRoots) {
    CHECK(DeploymentPlan::IsInstallRootAllowed(L"C:\\Program Files\\XPLogin10"));
    CHECK(DeploymentPlan::IsInstallRootAllowed(L"D:\\Tools\\XPLogin10"));
    CHECK(DeploymentPlan::IsInstallRootAllowed(L"C:\\XPLogin10"));
    // A trailing separator is a formatting difference, not a different path.
    CHECK(DeploymentPlan::IsInstallRootAllowed(L"C:\\Program Files\\XPLogin10\\"));
}

TEST(Deployment, IsUnderDoesNotMatchSiblingPrefixes) {
    CHECK(DeploymentPlan::IsUnder(L"C:\\XPLogin10\\a.dll", L"C:\\XPLogin10"));
    CHECK(DeploymentPlan::IsUnder(L"C:\\XPLogin10", L"C:\\XPLogin10"));
    // The classic prefix bug: "C:\XPLogin10Extra" is not inside "C:\XPLogin10".
    CHECK_FALSE(DeploymentPlan::IsUnder(L"C:\\XPLogin10Extra\\a.dll",
                                        L"C:\\XPLogin10"));
    CHECK_FALSE(DeploymentPlan::IsUnder(L"C:\\Windows\\a.dll", L"C:\\XPLogin10"));
}

TEST(Deployment, GeneratedPlansPassValidation) {
    std::wstring offender;
    CHECK(DeploymentPlan::Validate(DeploymentPlan::BuildInstall(Options()), kRoot,
                                   &offender));
    CHECK_EQ(offender, std::wstring(L""));
    CHECK(DeploymentPlan::Validate(DeploymentPlan::BuildUninstall(Options()), kRoot));
}

TEST(Deployment, ValidateRejectsATargetOutsideTheRoot) {
    auto plan = DeploymentPlan::BuildUninstall(Options());
    FileOperation escape;
    escape.kind = FileOpKind::Delete;
    escape.target = L"C:\\Windows\\System32\\kernel32.dll";
    plan.push_back(escape);

    std::wstring offender;
    CHECK_FALSE(DeploymentPlan::Validate(plan, kRoot, &offender));
    CHECK_EQ(offender, escape.target);
}

TEST(Deployment, ValidateRejectsRemovingAnyOtherDirectory) {
    auto plan = DeploymentPlan::BuildUninstall(Options());
    FileOperation subdirectory;
    subdirectory.kind = FileOpKind::RemoveEmptyDirectory;
    // Inside the root, so the "is it under the root" check passes - but only
    // the root itself may ever be removed.
    subdirectory.target = std::wstring(kRoot) + L"\\avatars";
    plan.push_back(subdirectory);

    CHECK_FALSE(DeploymentPlan::Validate(plan, kRoot));
}

TEST(Deployment, ValidateRejectsADangerousRootEvenWithAnEmptyPlan) {
    CHECK_FALSE(DeploymentPlan::Validate({}, L"C:\\Windows"));
    CHECK_FALSE(DeploymentPlan::Validate({}, L"C:\\"));
}

TEST(Deployment, ValidateRejectsACopyWithNoSource) {
    std::vector<FileOperation> plan;
    FileOperation op;
    op.kind = FileOpKind::Copy;
    op.target = std::wstring(kRoot) + L"\\XPLoginProvider.dll";
    plan.push_back(op); // source deliberately empty
    CHECK_FALSE(DeploymentPlan::Validate(plan, kRoot));
}

TEST(Deployment, DescribeCoversEveryOperation) {
    const auto plan = DeploymentPlan::BuildInstall(Options());
    const std::wstring text = DeploymentPlan::Describe(plan);

    size_t lines = 0;
    for (wchar_t c : text) {
        if (c == L'\n') {
            ++lines;
        }
    }
    CHECK_EQ(lines, plan.size());
    CHECK(text.find(L"mkdir") != std::wstring::npos);
    CHECK(text.find(L"copy") != std::wstring::npos);
}

// ---------------------------------------------------------------------------
// Programs and Features
// ---------------------------------------------------------------------------

TEST(ProgramsAndFeatures, InstallWritesTheUninstallEntry) {
    const auto plan = RegistrationPlan::BuildInstall(FullInstallOptions());

    const RegOperation* displayName =
        FindValue(plan, kArpKeyPath, L"DisplayName");
    REQUIRE(displayName != nullptr);
    CHECK_EQ(displayName->stringData, std::wstring(kProductName));

    CHECK(FindValue(plan, kArpKeyPath, L"DisplayVersion") != nullptr);
    CHECK(FindValue(plan, kArpKeyPath, L"Publisher") != nullptr);
    CHECK(FindValue(plan, kArpKeyPath, L"InstallLocation") != nullptr);
    CHECK(FindValue(plan, kArpKeyPath, L"DisplayIcon") != nullptr);
}

TEST(ProgramsAndFeatures, UninstallStringPointsAtTheInstalledSetup) {
    const auto plan = RegistrationPlan::BuildInstall(FullInstallOptions());

    const RegOperation* uninstall =
        FindValue(plan, kArpKeyPath, L"UninstallString");
    REQUIRE(uninstall != nullptr);
    // Quoted, because the default install path contains a space.
    CHECK_EQ(uninstall->stringData,
             std::wstring(L"\"C:\\Program Files\\XPLogin10\\XPLogin10-Setup.exe\" "
                          L"/uninstall"));

    const RegOperation* quiet =
        FindValue(plan, kArpKeyPath, L"QuietUninstallString");
    REQUIRE(quiet != nullptr);
    CHECK(quiet->stringData.find(L"/silent") != std::wstring::npos);
}

TEST(ProgramsAndFeatures, ThereIsNothingToModifyOrRepair) {
    const auto plan = RegistrationPlan::BuildInstall(FullInstallOptions());
    CHECK_EQ(FindValue(plan, kArpKeyPath, L"NoModify")->dwordData, 1u);
    CHECK_EQ(FindValue(plan, kArpKeyPath, L"NoRepair")->dwordData, 1u);
}

TEST(ProgramsAndFeatures, EntryIsRemovedOnUninstall) {
    const auto plan = RegistrationPlan::BuildUninstall(FullInstallOptions());

    bool removed = false;
    for (const RegOperation& op : plan) {
        if (op.kind == RegOpKind::DeleteKeyTree &&
            EqualsNoCase(op.path, kArpKeyPath)) {
            removed = true;
        }
    }
    CHECK(removed);
}

TEST(ProgramsAndFeatures, CanBeSuppressed) {
    InstallOptions options = FullInstallOptions();
    options.registerArp = false;
    const auto plan = RegistrationPlan::BuildInstall(options);

    for (const RegOperation& op : plan) {
        CHECK(op.path.find(L"Uninstall\\XPLogin10") == std::wstring::npos);
    }
}

TEST(ProgramsAndFeatures, TheUninstallKeyIsOnTheAllowList) {
    // Adding a new key to the plan without adding it to the guard would make
    // every install fail validation - this pins both halves together.
    CHECK(RegistrationPlan::IsPathAllowed(RegHive::LocalMachine, kArpKeyPath));
    CHECK(RegistrationPlan::Validate(
        RegistrationPlan::BuildInstall(FullInstallOptions())));
    CHECK(RegistrationPlan::Validate(
        RegistrationPlan::BuildUninstall(FullInstallOptions())));
}

TEST(ProgramsAndFeatures, FallsBackToTheDllDirectoryWhenNoRootIsGiven) {
    InstallOptions options;
    options.dllPath = L"D:\\Tools\\XPLogin10\\XPLoginProvider.dll";
    const auto plan = RegistrationPlan::BuildInstall(options);

    const RegOperation* location =
        FindValue(plan, kArpKeyPath, L"InstallLocation");
    REQUIRE(location != nullptr);
    CHECK_EQ(location->stringData, std::wstring(L"D:\\Tools\\XPLogin10"));
}

// ---------------------------------------------------------------------------
// Setup sequencing
// ---------------------------------------------------------------------------

TEST(SetupPlan, InstallStartsWithPrerequisites) {
    const auto plan = SetupPlan::BuildInstall(FullInstallOptions());
    REQUIRE_FALSE(plan.empty());
    CHECK_EQ(I(plan.front().kind), I(SetupStepKind::CheckPrerequisites));
    CHECK_FALSE(plan.front().mutatesSystem);
}

TEST(SetupPlan, RestorePointIsTakenBeforeAnythingChanges) {
    const auto plan = SetupPlan::BuildInstall(FullInstallOptions());
    CHECK(SetupPlan::RestorePointPrecedesAllChanges(plan));

    // And prove the invariant can actually fail, so the check is not vacuous.
    std::vector<SetupStep> broken;
    SetupStep deploy;
    deploy.kind = SetupStepKind::DeployFiles;
    deploy.mutatesSystem = true;
    SetupStep restore;
    restore.kind = SetupStepKind::CreateRestorePoint;
    broken.push_back(deploy);
    broken.push_back(restore);
    CHECK_FALSE(SetupPlan::RestorePointPrecedesAllChanges(broken));
}

TEST(SetupPlan, RecoveryFileIsWrittenBeforeTheRegistry) {
    const auto plan = SetupPlan::BuildInstall(FullInstallOptions());
    CHECK(SetupPlan::RecoveryFilePrecedesRegistry(plan));
}

TEST(SetupPlan, FilesAreCopiedBeforeTheyAreRegistered) {
    // Registering a DLL that is not on disk yet gives Windows a logon screen it
    // cannot load, which is precisely the failure this project must not have.
    const auto plan = SetupPlan::BuildInstall(FullInstallOptions());
    CHECK(SetupPlan::FilesPrecedeRegistry(plan));
}

TEST(SetupPlan, UninstallUnregistersBeforeDeleting) {
    const auto plan = SetupPlan::BuildUninstall(FullInstallOptions());
    CHECK(SetupPlan::RegistryRemovedBeforeFiles(plan));
}

TEST(SetupPlan, RestorePointFailureIsNotFatal) {
    // System Protection is off on plenty of machines. Refusing to install over
    // that would be worse than installing without a checkpoint - the other
    // recovery paths still exist.
    const auto plan = SetupPlan::BuildInstall(FullInstallOptions());
    for (const SetupStep& step : plan) {
        if (step.kind == SetupStepKind::CreateRestorePoint) {
            CHECK_FALSE(step.fatalOnFailure);
        }
        if (step.kind == SetupStepKind::ApplyRegistry ||
            step.kind == SetupStepKind::DeployFiles ||
            step.kind == SetupStepKind::WriteRecoveryFile) {
            CHECK(step.fatalOnFailure);
        }
    }
}

TEST(SetupPlan, RestorePointCanBeSkipped) {
    InstallOptions options = FullInstallOptions();
    options.createRestorePoint = false;
    const auto plan = SetupPlan::BuildInstall(options);

    for (const SetupStep& step : plan) {
        CHECK_NE(I(step.kind), I(SetupStepKind::CreateRestorePoint));
    }
    // The invariant holds vacuously when the step is absent.
    CHECK(SetupPlan::RestorePointPrecedesAllChanges(plan));
}

TEST(SetupPlan, ServiceStepsFollowTheServiceOption) {
    InstallOptions withService = FullInstallOptions();
    withService.installService = true;
    bool found = false;
    for (const SetupStep& step : SetupPlan::BuildInstall(withService)) {
        if (step.kind == SetupStepKind::InstallService) {
            found = true;
            CHECK_FALSE(step.fatalOnFailure);
        }
    }
    CHECK(found);

    InstallOptions without = FullInstallOptions();
    without.installService = false;
    for (const SetupStep& step : SetupPlan::BuildInstall(without)) {
        CHECK_NE(I(step.kind), I(SetupStepKind::InstallService));
    }
}

TEST(SetupPlan, StepNamesAreAllDistinct) {
    const SetupStepKind kinds[] = {
        SetupStepKind::CheckPrerequisites, SetupStepKind::CreateRestorePoint,
        SetupStepKind::WriteRecoveryFile,  SetupStepKind::DeployFiles,
        SetupStepKind::ApplyRegistry,      SetupStepKind::InstallService,
        SetupStepKind::RemoveService,      SetupStepKind::RemoveRegistry,
        SetupStepKind::RemoveFiles};
    const size_t count = sizeof(kinds) / sizeof(kinds[0]);
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            CHECK_NE(std::wstring(SetupPlan::StepName(kinds[i])),
                     std::wstring(SetupPlan::StepName(kinds[j])));
        }
    }
}

// ---------------------------------------------------------------------------
// System Restore behaviour
// ---------------------------------------------------------------------------

TEST(SystemRestore, CheckpointIsCreatedOnAHealthyMachine) {
    FakeSystemRestore restore;
    int64_t sequence = 0;
    CHECK_EQ(static_cast<int>(
                 restore.CreateCheckpoint(L"Before installing XPLogin10", &sequence)),
             static_cast<int>(RestorePointResult::Created));
    CHECK_EQ(sequence, int64_t(42));
    CHECK_EQ(restore.lastDescription,
             std::wstring(L"Before installing XPLogin10"));
}

TEST(SystemRestore, ReportsSystemProtectionBeingOff) {
    FakeSystemRestore restore;
    restore.protectionEnabled = false;
    CHECK_EQ(static_cast<int>(restore.CreateCheckpoint(L"x", nullptr)),
             static_cast<int>(RestorePointResult::Disabled));
    CHECK(restore.created.empty());
}

TEST(SystemRestore, ReportsMissingSupport) {
    FakeSystemRestore restore;
    restore.available = false;
    CHECK_FALSE(restore.IsAvailable());
    CHECK_EQ(static_cast<int>(restore.CreateCheckpoint(L"x", nullptr)),
             static_cast<int>(RestorePointResult::NotSupported));
}

TEST(SystemRestore, WindowsSkipsASecondCheckpointWithinTheRateLimit) {
    FakeSystemRestore restore;
    CHECK_EQ(static_cast<int>(restore.CreateCheckpoint(L"first", nullptr)),
             static_cast<int>(RestorePointResult::Created));
    CHECK_EQ(static_cast<int>(restore.CreateCheckpoint(L"second", nullptr)),
             static_cast<int>(RestorePointResult::RateLimited));
}

TEST(SystemRestore, SuspendingTheRateLimitForcesACheckpoint) {
    FakeSystemRestore restore;
    restore.recentCheckpointExists = true;

    CHECK_EQ(static_cast<int>(restore.CreateCheckpoint(L"blocked", nullptr)),
             static_cast<int>(RestorePointResult::RateLimited));

    CHECK(restore.SuspendRateLimit());
    CHECK_EQ(static_cast<int>(restore.CreateCheckpoint(L"forced", nullptr)),
             static_cast<int>(RestorePointResult::Created));

    restore.RestoreRateLimit();
    CHECK_EQ(restore.suspendCalls, 1);
    CHECK_EQ(restore.restoreCalls, 1);
    CHECK_FALSE(restore.rateLimitSuspended);
}

TEST(SystemRestore, RestoringTheRateLimitTwiceIsHarmless) {
    FakeSystemRestore restore;
    restore.SuspendRateLimit();
    restore.RestoreRateLimit();
    restore.RestoreRateLimit();
    CHECK_EQ(restore.restoreCalls, 1);
}

// ---------------------------------------------------------------------------
// The uninstaller has to be put somewhere it can be found again.
//
// Programs and Features stores an UninstallString and runs it much later, so
// the setup copies itself into the install directory. It was copying itself
// from the staging directory the payload is unpacked to - and the setup is the
// one file that cannot be inside its own payload, so nothing was ever there.
// The copy was optional, so it was skipped without a word, and the registry
// still got an UninstallString pointing at a file that did not exist: a
// console window that flashed and vanished, and an entry that could be
// "removed" for ever without going away.
// ---------------------------------------------------------------------------

TEST(Deployment, TheUninstallerIsCopiedFromWhereTheSetupActuallyIs) {
    DeploymentOptions options;
    options.installRoot = L"C:\\Program Files\\XPLogin10";
    options.sourceRoot = L"C:\\Users\\Bill\\AppData\\Local\\Temp\\XPLogin10-setup-42";
    options.uninstallerSource = L"D:\\Downloads";

    bool found = false;
    for (const FileOperation& op : DeploymentPlan::BuildInstall(options)) {
        if (op.kind != FileOpKind::Copy ||
            op.target != options.installRoot + L"\\XPLogin10-Setup.exe") {
            continue;
        }
        found = true;
        CHECK_EQ(op.source, std::wstring(L"D:\\Downloads\\XPLogin10-Setup.exe"));
        // Never from the staging directory: the payload cannot contain the
        // program that carries it.
        CHECK_FALSE(DeploymentPlan::IsUnder(op.source, options.sourceRoot));
        // And not optional. An install that registers an UninstallString it
        // could not place is worse than one that stops and says so.
        CHECK_FALSE(op.optional);
    }
    CHECK(found);
}

TEST(Deployment, AnAbsentUninstallerSourceFallsBackToTheSourceRoot) {
    // A plan built without a staging directory - the two are the same place.
    DeploymentOptions options;
    options.installRoot = L"C:\\Program Files\\XPLogin10";
    options.sourceRoot = L"D:\\Downloads";

    for (const FileOperation& op : DeploymentPlan::BuildInstall(options)) {
        if (op.kind == FileOpKind::Copy &&
            op.target == options.installRoot + L"\\XPLogin10-Setup.exe") {
            CHECK_EQ(op.source, std::wstring(L"D:\\Downloads\\XPLogin10-Setup.exe"));
        }
    }
}

TEST(Deployment, EveryPayloadFileStillComesFromTheStagingDirectory) {
    // The uninstaller is the exception, and it has to stay the only one.
    DeploymentOptions options;
    options.installRoot = L"C:\\Program Files\\XPLogin10";
    options.sourceRoot = L"C:\\staging";
    options.uninstallerSource = L"D:\\Downloads";

    for (const FileOperation& op : DeploymentPlan::BuildInstall(options)) {
        if (op.kind != FileOpKind::Copy) {
            continue;
        }
        if (op.target == options.installRoot + L"\\XPLogin10-Setup.exe") {
            continue;
        }
        CHECK(DeploymentPlan::IsUnder(op.source, options.sourceRoot));
    }
}
