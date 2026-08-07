// XPLogin10 - what the setup is about to do to the file system, as data.
//
// Same pattern as RegistrationPlan: build the operation list, validate it,
// then execute. The guard here matters even more than the registry one - an
// uninstaller that computes the wrong root and then removes a directory tree
// is not a bug you get to apologise for.
#pragma once

#include "xplogin/RegistrationPlan.h"

#include <string>
#include <vector>

namespace xplogin {

// NOTE ON THE NAMES. The obvious spellings - CreateDirectory, CopyFile,
// DeleteFile, RemoveDirectory - are all preprocessor macros in windows.h,
// which rewrites them to their ...W forms before the compiler sees the enum.
// Being scoped inside `enum class` does not help: the preprocessor does not
// know what a scope is. So `case FileOpKind::CopyFile:` became
// `case FileOpKind::CopyFileW:` in any translation unit that had included
// windows.h, and MSVC reported it as "illegal qualified name in member
// declaration", which points nowhere near the real cause. These names avoid
// the collision instead of #undef-ing four macros in every Win32 file.
enum class FileOpKind {
    MakeDirectory = 0,
    Copy,
    Delete,
    // Only ever removes an empty directory - the name says so because that is
    // the guarantee the uninstaller depends on.
    RemoveEmptyDirectory,
};

struct FileOperation {
    FileOpKind   kind = FileOpKind::MakeDirectory;
    std::wstring source;   // empty for anything but Copy
    std::wstring target;
    bool         optional = false; // cleanup steps may legitimately find nothing
};

// One file the product is made of. `resourceId` is the RCDATA id the setup
// embeds it under - it lives here rather than in src/setup so the embedding
// side and the deployment side cannot disagree about which files exist. They
// did, once: a file added to one list and not the other is embedded and never
// written out, or written out and never removed by the uninstall.
struct PayloadFile {
    const wchar_t* name;
    bool           required;
    unsigned       resourceId;
};

const std::vector<PayloadFile>& PayloadManifest();

struct DeploymentOptions {
    std::wstring installRoot;  // C:\Program Files\XPLogin10
    std::wstring sourceRoot;   // where the payload files are copied from

    // Where the setup executable itself is, which is NOT sourceRoot.
    //
    // The payload is unpacked to a staging directory and everything is copied
    // from there - everything except the setup, which cannot be inside its own
    // payload. Copying it from sourceRoot therefore looked for a file that was
    // never going to be there, and because the operation is optional it was
    // skipped in silence. The result was an installation with no
    // XPLogin10-Setup.exe in it and a Programs and Features entry whose
    // UninstallString pointed at that missing file: a console window that
    // flashed and vanished, and an entry that could be "removed" for ever.
    //
    // Empty means "same as sourceRoot", which is what a plan built without a
    // staging directory wants.
    std::wstring uninstallerSource;

    // The uninstaller is copied in so it still exists after the folder the
    // setup ran from is gone - Programs and Features runs it much later.
    std::wstring setupFileName = L"XPLogin10-Setup.exe";
};

class DeploymentPlan {
public:
    static std::vector<FileOperation> BuildInstall(const DeploymentOptions& options);

    // Removes exactly what BuildInstall created: every payload file, the log
    // files the provider writes at runtime, then the directory itself.
    static std::vector<FileOperation> BuildUninstall(const DeploymentOptions& options);

    // Refuses roots that are not safe to own outright: drive roots, the Windows
    // directory, the bare Program Files folders, anything relative, anything
    // containing "..".
    static bool IsInstallRootAllowed(const std::wstring& root);

    // Every target must sit inside `installRoot`. This is what stops a
    // mis-computed root from turning into a recursive delete somewhere else.
    static bool Validate(const std::vector<FileOperation>& plan,
                         const std::wstring& installRoot,
                         std::wstring* offendingPath = nullptr);

    static std::wstring Describe(const std::vector<FileOperation>& plan);

    // True when `path` is inside `root` (case insensitive, separator aware).
    static bool IsUnder(const std::wstring& path, const std::wstring& root);
};

// ---------------------------------------------------------------------------
// The ordered steps a setup runs. Expressed as data so the ordering rules that
// actually keep a machine bootable are unit tested rather than hoped for.
// ---------------------------------------------------------------------------

enum class SetupStepKind {
    CheckPrerequisites = 0, // elevation, architecture, existing install
    CreateRestorePoint,
    WriteRecoveryFile,
    DeployFiles,
    ApplyRegistry,
    InstallService,
    RemoveService,
    RemoveRegistry,
    RemoveFiles,
};

struct SetupStep {
    SetupStepKind kind = SetupStepKind::CheckPrerequisites;
    const wchar_t* description = L"";
    bool fatalOnFailure = true;
    // True when the step changes something a restore point would capture.
    bool mutatesSystem = false;
};

class SetupPlan {
public:
    static std::vector<SetupStep> BuildInstall(const InstallOptions& options);
    static std::vector<SetupStep> BuildUninstall(const InstallOptions& options);

    // ---- invariants the tests pin -----------------------------------------

    // A restore point taken after the first change is worth nothing.
    static bool RestorePointPrecedesAllChanges(const std::vector<SetupStep>& plan);

    // The offline recovery .reg has to exist before the registry is touched,
    // because after that point the machine may not boot far enough to write it.
    static bool RecoveryFilePrecedesRegistry(const std::vector<SetupStep>& plan);

    // Registering a DLL that is not on disk yet gives Windows a logon screen it
    // cannot load - the classic way to make this exact kind of software fail.
    static bool FilesPrecedeRegistry(const std::vector<SetupStep>& plan);

    // Uninstall must unregister before deleting, for the same reason in reverse.
    static bool RegistryRemovedBeforeFiles(const std::vector<SetupStep>& plan);

    static const wchar_t* StepName(SetupStepKind kind);
};

} // namespace xplogin
