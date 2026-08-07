// XPLogin10 - XPLogin10-Setup.exe
//
// The thing a user double-clicks. One file, everything inside it.
//
//   XPLogin10-Setup.exe                 install, with confirmation
//   XPLogin10-Setup.exe /silent         install with no prompts
//   XPLogin10-Setup.exe /uninstall      remove, with confirmation
//   XPLogin10-Setup.exe /uninstall /silent
//   XPLogin10-Setup.exe /dir <path>     install somewhere other than the default
//
// Options: /nofilter /nolockscreen /nonumlock /cad /noservice
//          /norestorepoint /forcerestorepoint
//
// The order of operations is not incidental and is pinned by tests in
// tests/unit/test_deployment_plan.cpp: restore point before any change,
// recovery file before the registry, files on disk before they are registered,
// and on the way out the registry before the files.
#include "Payload.h"

#include "FileExecutor.h"
#include "RegistryExecutor.h"

#include "xplogin/DeploymentPlan.h"
#include "xplogin/HealthMonitor.h"
#include "xplogin/Logging.h"
#include "xplogin/RegistrationPlan.h"
#include "xplogin/StringUtil.h"
#include "xplogin/Win32Factories.h"

#include <windows.h>
#include <objbase.h>
#include <shlobj.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

using namespace xplogin;
using namespace xplogin::installer;

namespace {

struct Args {
    bool uninstall = false;
    bool silent = false;
    bool help = false;
    std::wstring directory;
    InstallOptions options;
};

// ---------------------------------------------------------------------------
// Console helpers
// ---------------------------------------------------------------------------

void Print(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    ::vwprintf(format, args);
    va_end(args);
}

void Heading(const wchar_t* text) {
    Print(L"\n%s\n", text);
    Print(L"------------------------------------------------------------\n");
}

void Step(size_t index, size_t total, const wchar_t* description) {
    Print(L"[%zu/%zu] %s\n", index, total, description);
    XPLOG_INFO("[%d/%d] %s", static_cast<int>(index), static_cast<int>(total),
               WideToUtf8(description).c_str());
}

// Copies an executor transcript into the log, one line per operation. The log
// writer has a fixed message buffer, so a multi-line transcript handed to it
// whole loses its tail - which is the half that says what failed.
void LogTranscript(const wchar_t* what, const std::wstring& transcript) {
    size_t start = 0;
    while (start < transcript.size()) {
        size_t end = transcript.find(L'\n', start);
        std::wstring line = transcript.substr(
            start, end == std::wstring::npos ? std::wstring::npos : end - start);
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) {
            line.pop_back();
        }
        if (!line.empty()) {
            XPLOG_INFO("%s %s", WideToUtf8(what).c_str(), WideToUtf8(line).c_str());
        }
        if (end == std::wstring::npos) {
            break;
        }
        start = end + 1;
    }
}

// Three answers, not two.
//
// "Could not ask" is not the same as "said no", and collapsing them is a trap:
// Programs and Features launches the uninstaller without a usable stdin, so a
// failed read turned every uninstall started from there into a silent
// cancellation - a console window that appeared, answered its own question with
// no, and closed. The caller decides what an unanswerable question means,
// because it differs: an install that cannot ask must not proceed, an uninstall
// that Programs and Features already confirmed must.
enum class Answer { Yes, No, CannotAsk };

Answer Confirm(const wchar_t* question) {
    Print(L"\n%s [y/N] ", question);
    wchar_t line[16] = {};
    if (!::fgetws(line, 16, stdin)) {
        Print(L"(no console to answer from)\n");
        XPLOG_INFO("could not read a confirmation from stdin");
        return Answer::CannotAsk;
    }
    return (line[0] == L'y' || line[0] == L'Y') ? Answer::Yes : Answer::No;
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation = {};
    DWORD size = sizeof(elevation);
    const BOOL ok =
        ::GetTokenInformation(token, TokenElevation, &elevation, size, &size);
    ::CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

std::wstring ExecutablePath() {
    wchar_t path[MAX_PATH * 2] = {};
    ::GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return path;
}

std::wstring DirectoryOf(const std::wstring& filePath) {
    const size_t slash = filePath.find_last_of(L'\\');
    return slash == std::wstring::npos ? L"" : filePath.substr(0, slash);
}

std::wstring DefaultInstallRoot() {
    wchar_t* programFiles = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr,
                                         &programFiles)) &&
        programFiles) {
        std::wstring root = std::wstring(programFiles) + L"\\XPLogin10";
        ::CoTaskMemFree(programFiles);
        return root;
    }
    return L"C:\\Program Files\\XPLogin10";
}

std::wstring TempStagingDirectory() {
    wchar_t buffer[MAX_PATH] = {};
    ::GetTempPathW(ARRAYSIZE(buffer), buffer);
    return std::wstring(buffer) + L"XPLogin10-setup-" +
           std::to_wstring(::GetCurrentProcessId());
}

void RemoveDirectoryTree(const std::wstring& directory) {
    WIN32_FIND_DATAW found = {};
    HANDLE search = ::FindFirstFileW((directory + L"\\*").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (::wcscmp(found.cFileName, L".") == 0 ||
                ::wcscmp(found.cFileName, L"..") == 0) {
                continue;
            }
            const std::wstring child = directory + L"\\" + found.cFileName;
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryTree(child);
            } else {
                ::SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                ::DeleteFileW(child.c_str());
            }
        } while (::FindNextFileW(search, &found));
        ::FindClose(search);
    }
    ::RemoveDirectoryW(directory.c_str());
}

bool WriteRecoveryFile(const std::wstring& directory) {
    const std::string content = RegistrationPlan::BuildRecoveryRegFile();
    std::ofstream file(WideToUtf8(directory + L"\\XPLogin-Recovery.reg"),
                       std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    return file.good();
}

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------

Args ParseArgs(int argc, wchar_t** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (EqualsNoCase(arg, L"/uninstall") || EqualsNoCase(arg, L"/u")) {
            args.uninstall = true;
        } else if (EqualsNoCase(arg, L"/silent") || EqualsNoCase(arg, L"/s") ||
                   EqualsNoCase(arg, L"/quiet")) {
            args.silent = true;
        } else if (EqualsNoCase(arg, L"/dir") && i + 1 < argc) {
            args.directory = argv[++i];
        } else if (EqualsNoCase(arg, L"/nofilter")) {
            args.options.registerFilter = false;
        } else if (EqualsNoCase(arg, L"/nolockscreen")) {
            args.options.disableLockScreenImage = false;
        } else if (EqualsNoCase(arg, L"/cad")) {
            args.options.disableCtrlAltDel = true;
        } else if (EqualsNoCase(arg, L"/nonumlock")) {
            args.options.numLockAtLogon = false;
        } else if (EqualsNoCase(arg, L"/noservice")) {
            args.options.installService = false;
        } else if (EqualsNoCase(arg, L"/norestorepoint")) {
            args.options.createRestorePoint = false;
        } else if (EqualsNoCase(arg, L"/forcerestorepoint")) {
            args.options.forceRestorePoint = true;
        } else if (EqualsNoCase(arg, L"/?") || EqualsNoCase(arg, L"/help")) {
            args.help = true;
        }
    }
    return args;
}

void FillPaths(Args& args) {
    const std::wstring root =
        args.directory.empty() ? DefaultInstallRoot() : args.directory;

    args.options.installRoot = root;
    args.options.dllPath = root + L"\\XPLoginProvider.dll";
    args.options.servicePath = root + L"\\XPLoginWatchdog.exe";
    args.options.configPath = root + L"\\XPLogin.ini";
    args.options.uninstallerPath = root + L"\\XPLogin10-Setup.exe";
}

void PrintUsage() {
    Print(
        L"\nXPLogin10 %s - Windows XP logon screen for Windows 10 and 11\n\n"
        L"  XPLogin10-Setup.exe                install (asks first)\n"
        L"  XPLogin10-Setup.exe /silent        install with no prompts\n"
        L"  XPLogin10-Setup.exe /uninstall     remove\n"
        L"  XPLogin10-Setup.exe /dir <path>    install somewhere else\n\n"
        L"Options\n"
        L"  /nofilter           leave the Windows logon tiles visible too\n"
        L"                      (recommended for a first install)\n"
        L"  /nolockscreen       keep the Windows lock screen image\n"
        L"  /nonumlock          leave Num Lock alone at the sign-in screen\n"
        L"  /cad                require Ctrl+Alt+Del first\n"
        L"  /noservice          skip the recovery watchdog service\n"
        L"  /norestorepoint     do not create a System Restore point\n"
        L"  /forcerestorepoint  create one even if Windows made one today\n\n",
        kProductVersion);
}

// ---------------------------------------------------------------------------
// The restore point
// ---------------------------------------------------------------------------

bool TakeRestorePoint(const InstallOptions& options) {
    auto restore = MakeWin32SystemRestore();

    if (options.forceRestorePoint) {
        // Windows skips a checkpoint if it made one in the last 24 hours.
        restore->SuspendRateLimit();
    }

    int64_t sequence = 0;
    const RestorePointResult result =
        restore->CreateCheckpoint(L"Before installing XPLogin10", &sequence);

    if (options.forceRestorePoint) {
        restore->RestoreRateLimit();
    }

    switch (result) {
        case RestorePointResult::Created:
            Print(L"       restore point #%lld created\n",
                  static_cast<long long>(sequence));
            return true;

        case RestorePointResult::RateLimited:
            Print(L"       Windows already made a restore point today and "
                  L"skipped this one.\n"
                  L"       Re-run with /forcerestorepoint to insist.\n");
            return false;

        case RestorePointResult::Disabled:
            Print(L"       System Protection is turned off for this drive, so no\n"
                  L"       restore point was made. Turn it on in System "
                  L"Properties\n       > System Protection if you want one.\n");
            return false;

        case RestorePointResult::NotSupported:
            Print(L"       System Restore is not available on this edition of "
                  L"Windows.\n");
            return false;

        case RestorePointResult::Failed:
        default:
            Print(L"       Could not create a restore point. Carrying on - the\n"
                  L"       recovery file, Safe Mode and the crash counter all\n"
                  L"       still apply. See the log for why.\n");
            return false;
    }
}

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

int RunInstall(Args& args) {
    FillPaths(args);
    const std::wstring root = args.options.installRoot;

    Heading(L"XPLogin10 setup");
    Print(L"Install to      : %s\n", root.c_str());
    Print(L"Windows tiles   : %s\n",
          args.options.registerFilter ? L"password tiles replaced by the XP screen"
                                      : L"still visible alongside");
    // The single most useful thing somebody can know before they commit: a PIN
    // still works afterwards. Saying it here saves the support question.
    if (args.options.registerFilter) {
        Print(L"PIN and Hello   : still available - \"Other ways to log on\" in "
              L"the bottom left,\n                  or press Escape at the "
              L"account list\n");
    }
    Print(L"Restore point   : %s\n",
          args.options.createRestorePoint ? L"yes" : L"no");
    Print(L"Watchdog service: %s\n", args.options.installService ? L"yes" : L"no");

    if (!DeploymentPlan::IsInstallRootAllowed(root)) {
        Print(L"\nerror: %s is not a safe place to install to.\n", root.c_str());
        return 2;
    }

    if (!args.silent) {
        Print(L"\nThis replaces the Windows sign-in screen. If something goes "
              L"wrong,\nSafe Mode always shows the normal Windows screen, and "
              L"the installer\nleaves an XPLogin-Recovery.reg next to the "
              L"program files.\n");
        // An install that cannot ask does not go ahead: nobody has said yes to
        // replacing the sign-in screen.
        if (Confirm(L"Install XPLogin10?") != Answer::Yes) {
            Print(L"Cancelled. Nothing was changed.\n");
            XPLOG_INFO("install cancelled at the confirmation");
            return 1;
        }
    }

    // Where the files are copied from: either the embedded payload unpacked to
    // a temporary folder, or the folder this setup is sitting in.
    std::wstring sourceRoot = DirectoryOf(ExecutablePath());
    std::wstring staging;
    if (setup::HasEmbeddedPayload()) {
        staging = TempStagingDirectory();
        std::wstring error;
        if (!setup::ExtractAll(staging, &error)) {
            Print(L"\nerror: %s\n", error.c_str());
            RemoveDirectoryTree(staging);
            return 3;
        }
        sourceRoot = staging;
    }

    DeploymentOptions deployment;
    deployment.installRoot = root;
    deployment.sourceRoot = sourceRoot;
    // The setup copies itself from where it actually is. sourceRoot is the
    // staging directory by this point, and the setup is never in there.
    deployment.uninstallerSource = DirectoryOf(ExecutablePath());

    const auto filePlan = DeploymentPlan::BuildInstall(deployment);
    const auto registryPlan = RegistrationPlan::BuildInstall(args.options);

    std::wstring offender;
    if (!DeploymentPlan::Validate(filePlan, root, &offender)) {
        Print(L"\nerror: refusing a file plan that touches %s\n", offender.c_str());
        RemoveDirectoryTree(staging);
        return 4;
    }
    if (!RegistrationPlan::Validate(registryPlan, &offender)) {
        Print(L"\nerror: refusing a registry plan that touches %s\n",
              offender.c_str());
        RemoveDirectoryTree(staging);
        return 4;
    }

    const auto steps = SetupPlan::BuildInstall(args.options);
    const size_t total = steps.size();
    size_t index = 0;
    bool rebootRequired = false;

    Heading(L"Installing");

    for (const SetupStep& step : steps) {
        Step(++index, total, step.description);
        bool ok = true;

        switch (step.kind) {
            case SetupStepKind::CheckPrerequisites:
                ok = true; // elevation was verified before we got here
                break;

            case SetupStepKind::CreateRestorePoint:
                // Never fatal: the other recovery paths still exist, and
                // refusing to install because System Protection is off would
                // help nobody.
                TakeRestorePoint(args.options);
                ok = true;
                break;

            case SetupStepKind::DeployFiles: {
                // Re-installing over a working install would otherwise fail
                // here: XPLoginWatchdog.exe is running, so its image is locked
                // and CopyFile returns ERROR_SHARING_VIOLATION. Stopping it
                // first is what an upgrade is supposed to do anyway - the
                // InstallService step below starts it again.
                if (args.options.installService && StopWatchdogService()) {
                    XPLOG_INFO("watchdog stopped before copying files");
                }
                FileExecutor executor;
                const FileExecutionReport report = executor.Execute(filePlan);
                Print(L"%s", report.transcript.c_str());
                LogTranscript(L"files", report.transcript);
                rebootRequired = rebootRequired || report.rebootRequired;
                ok = report.success;
                break;
            }

            case SetupStepKind::WriteRecoveryFile:
                // Goes in first so it exists even if the registry step is what
                // breaks the machine.
                FileExecutor::EnsureDirectory(root);
                ok = WriteRecoveryFile(root);
                if (ok) {
                    Print(L"       %s\\XPLogin-Recovery.reg\n", root.c_str());
                }
                break;

            case SetupStepKind::ApplyRegistry: {
                RegistryExecutor executor;
                const ExecutionReport report = executor.Execute(registryPlan);
                Print(L"%s", report.transcript.c_str());
                LogTranscript(L"registry", report.transcript);
                ok = report.success;
                if (!ok) {
                    Print(L"\nRolling back the registry changes.\n");
                    RegistryExecutor rollback;
                    rollback.Execute(RegistrationPlan::BuildUninstall(args.options));
                }
                break;
            }

            case SetupStepKind::InstallService:
                ok = InstallWatchdogService(args.options.servicePath);
                if (!ok) {
                    Print(L"       warning: the watchdog service could not be "
                          L"installed (%lu).\n"
                          L"       The provider's own crash counter still "
                          L"protects this machine.\n",
                          static_cast<unsigned long>(::GetLastError()));
                }
                break;

            default:
                break;
        }

        if (!ok && step.fatalOnFailure) {
            Print(L"\nInstallation failed at: %s\n", step.description);
            RemoveDirectoryTree(staging);
            return 5;
        }
    }

    RemoveDirectoryTree(staging);

    Heading(L"Done");
    Print(L"XPLogin10 is installed in %s\n", root.c_str());
    Print(L"It appears in Settings > Apps as \"%s\".\n\n", kProductName);
    if (rebootRequired) {
        Print(L"Some files were in use and will be replaced on the next "
              L"restart.\n\n");
    }
    Print(L"Press Win+L to see the XP welcome screen.\n\n");
    Print(L"If it ever misbehaves:\n"
          L"  - it disables itself after three sign-in screens with no "
          L"successful logon\n"
          L"  - Safe Mode always shows the normal Windows screen\n"
          L"  - \"%s\\XPLogin10-Setup.exe\" /uninstall removes it\n",
          root.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// Uninstall
// ---------------------------------------------------------------------------

int RunUninstall(Args& args) {
    // Prefer the recorded install location over guessing from our own path:
    // Programs and Features runs the copy inside the install directory, but a
    // user may also run the original download.
    std::wstring root;
    if (!win32::ReadRegistryString(HKEY_LOCAL_MACHINE, kArpKeyPath,
                                   L"InstallLocation", &root) ||
        root.empty()) {
        root = DirectoryOf(ExecutablePath());
    }
    args.directory = root;
    FillPaths(args);

    Heading(L"XPLogin10 uninstall");
    Print(L"Remove from: %s\n", root.c_str());
    // Into the log as well as onto the console. An uninstall started from
    // Programs and Features runs in a window that closes on its own, so the
    // console is not where anybody reads what happened - and "it did nothing
    // and said nothing" is not a report anyone can act on.
    XPLOG_INFO("uninstall: root=%s (from %s)", WideToUtf8(root).c_str(),
               root.empty() ? "nowhere" : "ARP InstallLocation or our own path");

    if (!DeploymentPlan::IsInstallRootAllowed(root)) {
        Print(L"\nerror: %s does not look like an XPLogin10 installation; "
              L"refusing to delete anything.\n",
              root.c_str());
        XPLOG_ERROR("uninstall refused: %s is not an allowed install root",
                    WideToUtf8(root).c_str());
        return 2;
    }

    if (!args.silent) {
        // Only an explicit "no" stops an uninstall. Programs and Features runs
        // UninstallString after asking the user itself, in a console they
        // cannot type into, so treating an unanswerable question as a refusal
        // meant the entry could be "removed" indefinitely and never went away.
        const Answer answer = Confirm(L"Remove XPLogin10 and restore the "
                                      L"Windows sign-in screen?");
        if (answer == Answer::No) {
            Print(L"Cancelled. Nothing was changed.\n");
            XPLOG_INFO("uninstall cancelled at the confirmation");
            return 1;
        }
        if (answer == Answer::CannotAsk) {
            Print(L"Proceeding: this looks like Programs and Features, which "
                  L"has already asked.\n");
        }
    }

    // Every flag on, so the cleanup steps for values we may have written are
    // generated. They are all optional and no-op when the value is absent.
    args.options.disableLockScreenImage = true;
    args.options.disableCtrlAltDel = true;
    args.options.installService = true;
    args.options.registerArp = true;

    DeploymentOptions deployment;
    deployment.installRoot = root;

    const auto registryPlan = RegistrationPlan::BuildUninstall(args.options);
    const auto filePlan = DeploymentPlan::BuildUninstall(deployment);

    std::wstring offender;
    if (!RegistrationPlan::Validate(registryPlan, &offender) ||
        !DeploymentPlan::Validate(filePlan, root, &offender)) {
        Print(L"\nerror: refusing a plan that touches %s\n", offender.c_str());
        return 4;
    }

    const auto steps = SetupPlan::BuildUninstall(args.options);
    const size_t total = steps.size();
    size_t index = 0;
    bool rebootRequired = false;

    Heading(L"Removing");

    for (const SetupStep& step : steps) {
        Step(++index, total, step.description);

        switch (step.kind) {
            case SetupStepKind::RemoveService:
                RemoveWatchdogService();
                break;

            case SetupStepKind::RemoveRegistry: {
                // Unregister before deleting: a registration pointing at a
                // missing DLL is exactly the state that breaks logon.
                RegistryExecutor executor;
                const ExecutionReport report = executor.Execute(registryPlan);
                Print(L"%s", report.transcript.c_str());
                LogTranscript(L"registry", report.transcript);
                if (!report.success && step.fatalOnFailure) {
                    Print(L"\nUninstall failed while unregistering. The files "
                          L"were left in place.\n");
                    XPLOG_ERROR("uninstall stopped: the registry plan failed");
                    return 5;
                }
                break;
            }

            case SetupStepKind::RemoveFiles: {
                FileExecutor executor;
                const FileExecutionReport report = executor.Execute(filePlan);
                Print(L"%s", report.transcript.c_str());
                LogTranscript(L"files", report.transcript);
                rebootRequired = rebootRequired || report.rebootRequired;
                if (!report.success) {
                    // Not fatal - the registration is already gone, so the
                    // machine signs in normally either way - but it must not
                    // be silent, because "uninstalled" and "the folder is
                    // still full" is what the user is looking at.
                    Print(L"\nSome files could not be removed. %s\n",
                          L"They are listed above; anything still loaded will "
                          L"go on the next restart.");
                    XPLOG_ERROR("uninstall: %d of %d file operations completed",
                                static_cast<int>(report.completed),
                                static_cast<int>(filePlan.size()));
                }
                break;
            }

            default:
                break;
        }
    }

    Heading(L"Done");
    Print(L"XPLogin10 has been removed. The Windows sign-in screen is back.\n");
    if (rebootRequired) {
        Print(L"\nThe setup program is running from the folder it just emptied, "
              L"so the\nlast few files will disappear on the next restart.\n");
    }
    return 0;
}

// %TEMP% rather than next to the binary: the setup may be run from a
// read-only location, and this has to work on the run that fails.
std::wstring SetupLogPath() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = ::GetTempPathW(ARRAYSIZE(buffer), buffer);
    if (length == 0 || length >= ARRAYSIZE(buffer)) {
        return L"XPLogin10-Setup.log";
    }
    return std::wstring(buffer) + L"XPLogin10-Setup.log";
}

// True when this process is the only one attached to the console, which means
// Windows made the window for us and it disappears the instant we return.
// That is what happens on a double-click, and also when the exe is started
// from a *non-elevated* prompt: the manifest asks for administrator, cmd
// cannot grant it in place, so UAC re-launches us in a fresh console and the
// original prompt comes straight back with nothing printed in it.
bool OwnsConsole() {
    DWORD processes[4] = {};
    const DWORD count = ::GetConsoleProcessList(processes, ARRAYSIZE(processes));
    return count <= 1;
}

// Set by ParseArgs so the exit path can see it even after a fault.
bool g_silent = false;

void Trace(const wchar_t* what) {
    Print(L"  . %s\n", what);
    XPLOG_DEBUG("%s", WideToUtf8(what).c_str());
}

// What the SEH filter manages to record. A plain struct with no destructor,
// because it is filled from inside an exception filter.
struct FaultInfo {
    DWORD code = 0;
    void* address = nullptr;
};

int RecordFault(EXCEPTION_POINTERS* pointers, FaultInfo* out) {
    if (pointers && pointers->ExceptionRecord) {
        out->code = pointers->ExceptionRecord->ExceptionCode;
        out->address = pointers->ExceptionRecord->ExceptionAddress;
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// Turns a faulting address into "module+offset". Which binary faulted is the
// single most useful fact about a crash, and an address on its own is not
// portable between runs because of ASLR.
std::wstring DescribeAddress(void* address) {
    if (!address) {
        return L"unknown";
    }
    HMODULE module = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(address), &module) ||
        !module) {
        wchar_t bare[32] = {};
        ::swprintf_s(bare, L"0x%p", address);
        return bare;
    }

    wchar_t path[MAX_PATH] = {};
    ::GetModuleFileNameW(module, path, ARRAYSIZE(path));
    const wchar_t* name = ::wcsrchr(path, L'\\');
    name = name ? name + 1 : path;

    const uintptr_t offset = reinterpret_cast<uintptr_t>(address) -
                             reinterpret_cast<uintptr_t>(module);
    wchar_t described[MAX_PATH + 64] = {};
    ::swprintf_s(described, L"%s+0x%llx", name,
                 static_cast<unsigned long long>(offset));
    return described;
}

void HoldConsole() {
    if (g_silent || !OwnsConsole()) {
        return;
    }
    Print(L"\nPress Enter to close.");
    wchar_t line[8] = {};
    ::fgetws(line, 8, stdin);
}

// Everything that uses C++ objects. Kept out of wmain because the SEH frame
// there cannot share a function with anything that unwinds.
int RunSetupBody(int argc, wchar_t** argv) {
    // The startup steps are traced one by one because they run before the log
    // exists: if one of them faults, the last marker on screen is the only
    // thing that says which. They are cheap and there are five of them.
    Trace(L"reading the command line");
    Args args = ParseArgs(argc, argv);
    g_silent = args.silent;

    if (args.help) {
        PrintUsage();
        return 0;
    }

    Trace(L"resolving the log path");
    const std::wstring logPath = SetupLogPath();

    // Inside the guard, not before it: if opening the log is what fails, that
    // has to be a reported error rather than a window that closes.
    Trace(L"opening the log");
    Log::Configure(WideToUtf8(logPath), LogLevel::Debug);
    XPLOG_INFO("XPLogin10 setup starting");
    Print(L"Logging this run to:\n  %s\n", logPath.c_str());

    Trace(L"checking elevation");
    if (!IsElevated()) {
        // The manifest asks for elevation, so reaching here means the prompt
        // was declined or the manifest did not make it into the binary.
        Print(L"\nXPLogin10 setup needs to run as an administrator.\n"
              L"Right-click XPLogin10-Setup.exe and choose \"Run as "
              L"administrator\".\n");
        return 1;
    }

    Trace(args.uninstall ? L"starting the uninstall" : L"starting the install");
    const int result = args.uninstall ? RunUninstall(args) : RunInstall(args);
    XPLOG_INFO("setup finished with code %d", result);
    Print(L"\nA log of this run is at:\n  %s\n", logPath.c_str());
    return result;
}

// The SEH frame. A fault anywhere in the setup used to close the window with
// no message and nothing written down; now it is caught, logged, named on
// screen, and the console is still held open afterwards.
//
// Note the unqualified GetExceptionInformation. It looks like a Win32 call
// but it is a macro, and <excpt.h> expands it to
// (struct _EXCEPTION_POINTERS *)_exception_info - so a :: in front of it
// produces the token sequence "::(" and a diagnostic that describes a syntax
// error rather than the cause. GetExceptionCode is the same kind of thing and
// survives :: only because its expansion happens to start with an identifier.
int RunGuarded(int argc, wchar_t** argv, FaultInfo* fault) {
    __try {
        return RunSetupBody(argc, argv);
    } __except (RecordFault(GetExceptionInformation(), fault)) {
        return 9;
    }
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // Before anything else that could fail. Unbuffered so a fault later cannot
    // swallow the last thing printed, and a first line on screen so that a
    // black window means "did not start" rather than "started and said
    // nothing" - those need different fixes and used to look identical.
    ::setvbuf(stdout, nullptr, _IONBF, 0);
    Print(L"\nXPLogin10 setup %s\n", kProductVersion);

    // SHGetKnownFolderPath and CoTaskMemFree - both used to work out where
    // Program Files is - are shell/COM calls, and this process had no
    // apartment at all. Done here rather than deeper down so every path
    // through the setup has one, including the ones that only free memory
    // COM allocated.
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    FaultInfo fault;
    const int result = RunGuarded(argc, argv, &fault);

    if (fault.code != 0) {
        const std::wstring where = DescribeAddress(fault.address);
        Print(L"\nXPLogin10 setup stopped unexpectedly.\n"
              L"  code : 0x%08lX\n"
              L"  at   : %s\n",
              static_cast<unsigned long>(fault.code), where.c_str());
        Print(L"Nothing further was changed. If a restore point was made it is\n"
              L"still there, and the machine is untouched otherwise.\n");
        XPLOG_ERROR("setup faulted with 0x%08lx at %s",
                    static_cast<unsigned long>(fault.code),
                    WideToUtf8(where).c_str());
    }

    if (SUCCEEDED(com)) {
        ::CoUninitialize();
    }

    HoldConsole();
    return result;
}
