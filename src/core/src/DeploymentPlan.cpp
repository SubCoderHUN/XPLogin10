#include "xplogin/DeploymentPlan.h"

#include "xplogin/StringUtil.h"

#include <sstream>

namespace xplogin {
namespace {

// Directories no installer has any business owning or deleting.
const wchar_t* const kForbiddenRoots[] = {
    L"C:\\Windows",
    L"C:\\Program Files",
    L"C:\\Program Files (x86)",
    L"C:\\ProgramData",
    L"C:\\Users",
    L"C:\\",
};

std::wstring StripTrailingSeparator(const std::wstring& path) {
    std::wstring value = path;
    while (value.size() > 1 && value.back() == L'\\') {
        value.pop_back();
    }
    return value;
}

FileOperation MakeOp(FileOpKind kind, std::wstring target, std::wstring source = L"",
                     bool optional = false) {
    FileOperation op;
    op.kind = kind;
    op.target = std::move(target);
    op.source = std::move(source);
    op.optional = optional;
    return op;
}

// Files the provider and the watchdog create at run time. They are not part of
// the payload, but leaving them behind makes an uninstall look half done and
// stops the directory being removed.
const wchar_t* const kRuntimeArtifacts[] = {
    L"XPLogin.log",
    L"XPLoginWatchdog.log",
    L"XPLogin-Recovery.reg",
};

} // namespace

const std::vector<PayloadFile>& PayloadManifest() {
    // The resource ids are the contract with src/setup/CMakeLists.txt, which
    // generates payload.rc from them. Changing one means changing both.
    static const std::vector<PayloadFile> manifest = {
        {L"XPLoginProvider.dll", true,  101},
        {L"XPLoginWatchdog.exe", true,  102},
        {L"xplogin-install.exe", true,  103},
        {L"XPLogin.ini",         true,  105},
        {L"XPLogin.theme.ini",   true,  106},
        // The baked XP artwork. Optional in the sense that the screen still
        // comes up without it - just drawn from primitives rather than from
        // Microsoft's own bitmaps.
        {L"XPLogin.assets",      false, 107},
        // XP's own sounds, deployed flat rather than in a subdirectory so the
        // uninstall never has to remove a folder it did not create.
        {L"XPLogin-logon.wav",    false, 108},
        {L"XPLogin-logoff.wav",   false, 109},
        {L"XPLogin-error.wav",    false, 110},
        {L"XPLogin-shutdown.wav", false, 111},
    };
    return manifest;
}

bool DeploymentPlan::IsUnder(const std::wstring& path, const std::wstring& root) {
    if (path.empty() || root.empty()) {
        return false;
    }
    const std::wstring normalizedRoot = StripTrailingSeparator(root);
    if (!StartsWithNoCase(path, normalizedRoot)) {
        return false;
    }
    if (path.size() == normalizedRoot.size()) {
        return true; // the root itself
    }
    // "C:\XPLogin10Extra" must not count as being inside "C:\XPLogin10".
    return path[normalizedRoot.size()] == L'\\';
}

bool DeploymentPlan::IsInstallRootAllowed(const std::wstring& root) {
    if (root.empty()) {
        return false;
    }
    if (root.find(L"..") != std::wstring::npos) {
        return false;
    }
    // Must be absolute: "X:\something".
    if (root.size() < 4 || root[1] != L':' || root[2] != L'\\') {
        return false;
    }

    const std::wstring normalized = StripTrailingSeparator(root);
    for (const wchar_t* forbidden : kForbiddenRoots) {
        if (EqualsNoCase(normalized, StripTrailingSeparator(forbidden))) {
            return false;
        }
    }
    // Directly inside Windows is out too - a subfolder of System32 is still a
    // system directory.
    if (IsUnder(normalized, L"C:\\Windows")) {
        return false;
    }
    // A root has to have at least one path component beyond the drive.
    return normalized.size() > 3;
}

std::vector<FileOperation> DeploymentPlan::BuildInstall(
    const DeploymentOptions& options) {
    std::vector<FileOperation> plan;
    const std::wstring root = StripTrailingSeparator(options.installRoot);
    const std::wstring source = StripTrailingSeparator(options.sourceRoot);

    plan.push_back(MakeOp(FileOpKind::MakeDirectory, root));

    for (const PayloadFile& file : PayloadManifest()) {
        plan.push_back(MakeOp(FileOpKind::Copy, root + L"\\" + file.name,
                              source + L"\\" + file.name,
                              /*optional=*/!file.required));
    }

    // The uninstaller has to live in the install directory: Programs and
    // Features runs it long after whatever folder the setup was launched from
    // has been deleted.
    //
    // From uninstallerSource, not from source. See DeploymentOptions: the
    // setup is the one file that cannot be in its own payload, so the staging
    // directory never has a copy of it. And not optional either - an install
    // that registers an UninstallString pointing at a file it failed to place
    // is worse than one that stops and says so.
    if (!options.setupFileName.empty()) {
        const std::wstring setupFrom = StripTrailingSeparator(
            options.uninstallerSource.empty() ? options.sourceRoot
                                              : options.uninstallerSource);
        plan.push_back(MakeOp(FileOpKind::Copy,
                              root + L"\\" + options.setupFileName,
                              setupFrom + L"\\" + options.setupFileName,
                              /*optional=*/false));
    }
    return plan;
}

std::vector<FileOperation> DeploymentPlan::BuildUninstall(
    const DeploymentOptions& options) {
    std::vector<FileOperation> plan;
    const std::wstring root = StripTrailingSeparator(options.installRoot);

    for (const PayloadFile& file : PayloadManifest()) {
        plan.push_back(MakeOp(FileOpKind::Delete, root + L"\\" + file.name, L"",
                              /*optional=*/true));
    }
    for (const wchar_t* artifact : kRuntimeArtifacts) {
        plan.push_back(
            MakeOp(FileOpKind::Delete, root + L"\\" + artifact, L"", true));
    }
    if (!options.setupFileName.empty()) {
        plan.push_back(MakeOp(FileOpKind::Delete,
                              root + L"\\" + options.setupFileName, L"", true));
    }

    // The directory goes last and only if it is empty by then. A recursive
    // delete here would take anything the user happened to put in the folder
    // with it, which is not the uninstaller's call to make.
    plan.push_back(MakeOp(FileOpKind::RemoveEmptyDirectory, root, L"", true));
    return plan;
}

bool DeploymentPlan::Validate(const std::vector<FileOperation>& plan,
                              const std::wstring& installRoot,
                              std::wstring* offendingPath) {
    if (!IsInstallRootAllowed(installRoot)) {
        if (offendingPath) {
            *offendingPath = installRoot;
        }
        return false;
    }

    for (const FileOperation& op : plan) {
        if (op.target.empty() || !IsUnder(op.target, installRoot)) {
            if (offendingPath) {
                *offendingPath = op.target;
            }
            return false;
        }
        if (op.kind == FileOpKind::Copy && op.source.empty()) {
            if (offendingPath) {
                *offendingPath = op.target;
            }
            return false;
        }
        // Only the install root itself may ever be removed as a directory.
        if (op.kind == FileOpKind::RemoveEmptyDirectory &&
            !EqualsNoCase(StripTrailingSeparator(op.target),
                          StripTrailingSeparator(installRoot))) {
            if (offendingPath) {
                *offendingPath = op.target;
            }
            return false;
        }
    }
    return true;
}

std::wstring DeploymentPlan::Describe(const std::vector<FileOperation>& plan) {
    std::wostringstream out;
    for (const FileOperation& op : plan) {
        switch (op.kind) {
            case FileOpKind::MakeDirectory:
                out << L"  mkdir    " << op.target << L"\r\n";
                break;
            case FileOpKind::Copy:
                out << L"  copy     " << op.target << L"\r\n";
                break;
            case FileOpKind::Delete:
                out << L"  delete   " << op.target << L"\r\n";
                break;
            case FileOpKind::RemoveEmptyDirectory:
                out << L"  rmdir    " << op.target << L"\r\n";
                break;
        }
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// SetupPlan
// ---------------------------------------------------------------------------

namespace {

SetupStep Step(SetupStepKind kind, const wchar_t* description, bool fatal,
               bool mutates) {
    SetupStep step;
    step.kind = kind;
    step.description = description;
    step.fatalOnFailure = fatal;
    step.mutatesSystem = mutates;
    return step;
}

int IndexOf(const std::vector<SetupStep>& plan, SetupStepKind kind) {
    for (size_t i = 0; i < plan.size(); ++i) {
        if (plan[i].kind == kind) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

} // namespace

std::vector<SetupStep> SetupPlan::BuildInstall(const InstallOptions& options) {
    std::vector<SetupStep> plan;

    plan.push_back(Step(SetupStepKind::CheckPrerequisites,
                        L"Checking prerequisites", true, false));

    if (options.createRestorePoint) {
        // Not fatal: System Protection is off by default on some machines, and
        // refusing to install over that would be worse than installing without
        // a checkpoint - the other three recovery paths still exist.
        plan.push_back(Step(SetupStepKind::CreateRestorePoint,
                            L"Creating a System Restore point", false, false));
    }

    plan.push_back(Step(SetupStepKind::WriteRecoveryFile,
                        L"Writing the offline recovery file", true, false));
    plan.push_back(
        Step(SetupStepKind::DeployFiles, L"Copying files", true, true));

    plan.push_back(Step(SetupStepKind::ApplyRegistry,
                        L"Registering the credential provider", true, true));

    if (options.installService) {
        // The provider's own crash counter still protects the machine without
        // the watchdog, so a service that will not install is a warning.
        plan.push_back(Step(SetupStepKind::InstallService,
                            L"Installing the watchdog service", false, true));
    }
    return plan;
}

std::vector<SetupStep> SetupPlan::BuildUninstall(const InstallOptions& options) {
    std::vector<SetupStep> plan;

    plan.push_back(Step(SetupStepKind::CheckPrerequisites,
                        L"Checking prerequisites", true, false));
    if (options.installService) {
        plan.push_back(Step(SetupStepKind::RemoveService,
                            L"Removing the watchdog service", false, true));
    }
    plan.push_back(Step(SetupStepKind::RemoveRegistry,
                        L"Unregistering the credential provider", true, true));
    plan.push_back(Step(SetupStepKind::RemoveFiles, L"Removing files", false, true));
    return plan;
}

bool SetupPlan::RestorePointPrecedesAllChanges(const std::vector<SetupStep>& plan) {
    const int restore = IndexOf(plan, SetupStepKind::CreateRestorePoint);
    if (restore < 0) {
        return true; // not requested
    }
    for (size_t i = 0; i < plan.size(); ++i) {
        if (plan[i].mutatesSystem && static_cast<int>(i) < restore) {
            return false;
        }
    }
    return true;
}

bool SetupPlan::RecoveryFilePrecedesRegistry(const std::vector<SetupStep>& plan) {
    const int recovery = IndexOf(plan, SetupStepKind::WriteRecoveryFile);
    const int registry = IndexOf(plan, SetupStepKind::ApplyRegistry);
    if (registry < 0) {
        return true;
    }
    return recovery >= 0 && recovery < registry;
}

bool SetupPlan::FilesPrecedeRegistry(const std::vector<SetupStep>& plan) {
    const int files = IndexOf(plan, SetupStepKind::DeployFiles);
    const int registry = IndexOf(plan, SetupStepKind::ApplyRegistry);
    if (registry < 0) {
        return true;
    }
    return files >= 0 && files < registry;
}

bool SetupPlan::RegistryRemovedBeforeFiles(const std::vector<SetupStep>& plan) {
    const int registry = IndexOf(plan, SetupStepKind::RemoveRegistry);
    const int files = IndexOf(plan, SetupStepKind::RemoveFiles);
    if (files < 0) {
        return true;
    }
    return registry >= 0 && registry < files;
}

const wchar_t* SetupPlan::StepName(SetupStepKind kind) {
    switch (kind) {
        case SetupStepKind::CheckPrerequisites: return L"CheckPrerequisites";
        case SetupStepKind::CreateRestorePoint: return L"CreateRestorePoint";
        case SetupStepKind::WriteRecoveryFile:  return L"WriteRecoveryFile";
        case SetupStepKind::DeployFiles:        return L"DeployFiles";
        case SetupStepKind::ApplyRegistry:      return L"ApplyRegistry";
        case SetupStepKind::InstallService:     return L"InstallService";
        case SetupStepKind::RemoveService:      return L"RemoveService";
        case SetupStepKind::RemoveRegistry:     return L"RemoveRegistry";
        case SetupStepKind::RemoveFiles:        return L"RemoveFiles";
    }
    return L"?";
}

} // namespace xplogin
