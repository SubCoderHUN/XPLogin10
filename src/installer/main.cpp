// XPLogin10 - xplogin-install.exe
//
// Registers and unregisters the credential provider. Every write goes through
// RegistrationPlan, which is validated before a single key is touched, and an
// emergency recovery .reg is written next to the binaries before the first
// change so there is always a way back even if the machine will not boot.
//
//   xplogin-install /install [/nofilter] [/nolockscreen] [/nocad] [/noservice]
//   xplogin-install /uninstall
//   xplogin-install /disable          - keep the files, restore Windows logon
//   xplogin-install /enable
//   xplogin-install /status
//   xplogin-install /preview          - print the plan, change nothing
#include "RegistryExecutor.h"

#include "xplogin/HealthMonitor.h"
#include "xplogin/RegistrationPlan.h"
#include "xplogin/StringUtil.h"
#include "xplogin/Win32Factories.h"

#include <windows.h>
#include <shlwapi.h>

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace xplogin;
using namespace xplogin::installer;

namespace {

void Print(const wchar_t* format, ...) {
    va_list args;
    va_start(args, format);
    ::vwprintf(format, args);
    va_end(args);
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

std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH * 2] = {};
    ::GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    std::wstring full = path;
    const size_t slash = full.find_last_of(L'\\');
    return slash == std::wstring::npos ? L"" : full.substr(0, slash + 1);
}

InstallOptions BuildOptions(const std::vector<std::wstring>& args) {
    const std::wstring directory = ExecutableDirectory();

    InstallOptions options;
    // This tool registers what is already on disk; the install root is simply
    // wherever it is being run from.
    options.installRoot = directory.empty() ? directory
                                            : directory.substr(0, directory.size() - 1);
    options.dllPath = directory + L"XPLoginProvider.dll";
    options.servicePath = directory + L"XPLoginWatchdog.exe";
    options.configPath = directory + L"XPLogin.ini";
    options.uninstallerPath = directory + L"XPLogin10-Setup.exe";

    for (const std::wstring& arg : args) {
        if (EqualsNoCase(arg, L"/nofilter")) {
            options.registerFilter = false;
        } else if (EqualsNoCase(arg, L"/nonumlock")) {
            options.numLockAtLogon = false;
        } else if (EqualsNoCase(arg, L"/nolockscreen")) {
            options.disableLockScreenImage = false;
        } else if (EqualsNoCase(arg, L"/nocad")) {
            options.disableCtrlAltDel = false;
        } else if (EqualsNoCase(arg, L"/cad")) {
            options.disableCtrlAltDel = true;
        } else if (EqualsNoCase(arg, L"/noservice")) {
            options.installService = false;
        } else if (EqualsNoCase(arg, L"/noarp")) {
            options.registerArp = false;
        }
    }
    return options;
}

bool WriteRecoveryFile(const std::wstring& directory) {
    const std::wstring path = directory + L"XPLogin-Recovery.reg";
    const std::string content = RegistrationPlan::BuildRecoveryRegFile();

    std::ofstream file(WideToUtf8(path), std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    // regedit wants UTF-16LE with a BOM for a "Version 5.00" file, but it also
    // accepts ANSI. UTF-8 without a BOM is the safest common denominator for a
    // file that may be read from WinRE.
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    return file.good();
}

bool VerifyPayload(const InstallOptions& options) {
    if (!::PathFileExistsW(options.dllPath.c_str())) {
        Print(L"error: %s not found. Build the solution first.\n",
              options.dllPath.c_str());
        return false;
    }
    // A provider DLL on a path LogonUI cannot read is the single most common
    // way an installation silently does nothing.
    if (options.dllPath.find(L' ') != std::wstring::npos &&
        options.dllPath.front() != L'"') {
        // Not an error - just worth knowing when reading the log later.
        Print(L"note: install path contains spaces; this is supported.\n");
    }
    return true;
}

int DoInstall(const std::vector<std::wstring>& args) {
    const InstallOptions options = BuildOptions(args);
    if (!VerifyPayload(options)) {
        return 2;
    }

    const auto plan = RegistrationPlan::BuildInstall(options);
    std::wstring offender;
    if (!RegistrationPlan::Validate(plan, &offender)) {
        Print(L"error: refusing to run a plan that touches %s\n", offender.c_str());
        return 3;
    }

    if (!WriteRecoveryFile(ExecutableDirectory())) {
        Print(L"error: could not write XPLogin-Recovery.reg; aborting so there is "
              L"always a way back.\n");
        return 4;
    }
    Print(L"Recovery file written to %sXPLogin-Recovery.reg\n",
          ExecutableDirectory().c_str());

    RegistryExecutor executor;
    const ExecutionReport report = executor.Execute(plan);
    Print(L"%s", report.transcript.c_str());

    if (!report.success) {
        Print(L"\nInstallation failed after %u of %u operations. Rolling back.\n",
              static_cast<unsigned>(report.completed),
              static_cast<unsigned>(plan.size()));
        RegistryExecutor rollback;
        rollback.Execute(RegistrationPlan::BuildUninstall(options));
        return 5;
    }

    if (options.installService) {
        if (InstallWatchdogService(options.servicePath)) {
            Print(L"Watchdog service installed and started.\n");
        } else {
            // Not fatal: the provider's own crash counter still protects the
            // machine; the service only adds the out-of-band recovery.
            Print(L"warning: could not install the watchdog service (%lu).\n",
                  static_cast<unsigned long>(::GetLastError()));
        }
    }

    Print(L"\nXPLogin10 installed.\n"
          L"Sign out (or press Win+L) to see the XP welcome screen.\n"
          L"If anything goes wrong: boot into Safe Mode - the provider disables "
          L"itself there - and run  xplogin-install /uninstall\n");
    return 0;
}

int DoUninstall() {
    const InstallOptions options = BuildOptions({});
    // Always pass the flags that own shared Windows values so their cleanup
    // steps are generated; they are optional operations and will no-op if the
    // values were never written.
    InstallOptions full = options;
    full.disableLockScreenImage = true;
    full.disableCtrlAltDel = true;
    full.installService = true;
    full.registerArp = true;

    RemoveWatchdogService();

    const auto plan = RegistrationPlan::BuildUninstall(full);
    std::wstring offender;
    if (!RegistrationPlan::Validate(plan, &offender)) {
        Print(L"error: refusing to run a plan that touches %s\n", offender.c_str());
        return 3;
    }

    RegistryExecutor executor;
    const ExecutionReport report = executor.Execute(plan);
    Print(L"%s", report.transcript.c_str());
    Print(report.success ? L"\nXPLogin10 removed. The Windows logon screen is back.\n"
                         : L"\nUninstall completed with warnings; see above.\n");
    return report.success ? 0 : 1;
}

int DoSetEnabled(bool enabled) {
    const auto plan =
        enabled ? RegistrationPlan::BuildEnable() : RegistrationPlan::BuildDisable();
    std::wstring offender;
    if (!RegistrationPlan::Validate(plan, &offender)) {
        Print(L"error: refusing to run a plan that touches %s\n", offender.c_str());
        return 3;
    }

    RegistryExecutor executor;
    const ExecutionReport report = executor.Execute(plan);
    Print(L"%s", report.transcript.c_str());
    Print(enabled ? L"\nXPLogin10 enabled.\n"
                  : L"\nXPLogin10 disabled; Windows will show its own logon screen.\n");
    return report.success ? 0 : 1;
}

int DoStatus() {
    auto store = MakeWin32StateStore();
    auto clock = MakeWin32Clock();
    HealthMonitor health(store, clock, HealthPolicy{});

    std::wstring dllPath;
    const bool registered = RegistryExecutor::KeyExists(
        RegHive::LocalMachine,
        std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication"
                     L"\\Credential Providers\\") +
            kProviderClsid);
    const bool filtered = RegistryExecutor::KeyExists(
        RegHive::LocalMachine,
        std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Authentication"
                     L"\\Credential Provider Filters\\") +
            kFilterClsid);

    Print(L"XPLogin10 status\n");
    Print(L"  provider registered : %s\n", registered ? L"yes" : L"no");
    Print(L"  filter registered   : %s\n", filtered ? L"yes" : L"no");
    Print(L"  disabled flag       : %s\n", health.IsDisabled() ? L"yes" : L"no");
    Print(L"  start attempts      : %u\n", health.StartAttempts());

    const std::wstring crash = health.LastCrashReason();
    if (!crash.empty()) {
        Print(L"  last crash          : %s\n", crash.c_str());
    }

    const HealthAction action = HealthMonitor::Decide(
        DetectBootMode(), health.IsDisabled(), health.StartAttempts(), HealthPolicy{});
    const wchar_t* actionText = action == HealthAction::RunNormally  ? L"run normally"
                                : action == HealthAction::RunUnfiltered
                                    ? L"run, Windows tiles also visible"
                                    : L"stand down (Windows logon screen)";
    Print(L"  next logon will     : %s\n", actionText);
    return 0;
}

int DoPreview(const std::vector<std::wstring>& args) {
    const InstallOptions options = BuildOptions(args);

    Print(L"Install plan:\n%s\n",
          RegistrationPlan::Describe(RegistrationPlan::BuildInstall(options)).c_str());
    Print(L"Uninstall plan:\n%s\n",
          RegistrationPlan::Describe(RegistrationPlan::BuildUninstall(options))
              .c_str());

    std::wstring offender;
    const bool valid =
        RegistrationPlan::Validate(RegistrationPlan::BuildInstall(options), &offender);
    Print(L"Plan validation: %s\n", valid ? L"passed" : offender.c_str());
    return valid ? 0 : 3;
}

void PrintUsage() {
    Print(
        L"XPLogin10 installer\n\n"
        L"  xplogin-install /install [/nofilter] [/nolockscreen] [/cad] [/noservice]\n"
        L"  xplogin-install /uninstall\n"
        L"  xplogin-install /disable      keep the files, restore Windows logon\n"
        L"  xplogin-install /enable\n"
        L"  xplogin-install /status\n"
        L"  xplogin-install /preview      print the registry plan, change nothing\n\n"
        L"Options\n"
        L"  /nofilter      leave the Windows credential tiles visible alongside\n"
        L"  /nolockscreen  do not suppress the Windows 10 lock screen image\n"
        L"  /nonumlock     leave Num Lock alone at the sign-in screen\n"
        L"  /cad           require Ctrl+Alt+Del before the welcome screen\n"
        L"  /noservice     do not install the watchdog service\n"
        L"  /noarp         do not add a Programs and Features entry\n\n"
        L"For a normal installation use XPLogin10-Setup.exe instead: it deploys\n"
        L"the files, takes a System Restore point first and can be removed from\n"
        L"Settings > Apps. This tool only registers what is already on disk.\n");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    if (args.empty() || EqualsNoCase(args[0], L"/?") ||
        EqualsNoCase(args[0], L"/help")) {
        PrintUsage();
        return 0;
    }

    const std::wstring& verb = args[0];

    // /preview and /status only read.
    if (EqualsNoCase(verb, L"/preview")) {
        return DoPreview(args);
    }
    if (EqualsNoCase(verb, L"/status")) {
        return DoStatus();
    }

    if (!IsElevated()) {
        Print(L"error: this must be run from an elevated command prompt.\n");
        return 1;
    }

    if (EqualsNoCase(verb, L"/install")) {
        return DoInstall(args);
    }
    if (EqualsNoCase(verb, L"/uninstall")) {
        return DoUninstall();
    }
    if (EqualsNoCase(verb, L"/disable")) {
        return DoSetEnabled(false);
    }
    if (EqualsNoCase(verb, L"/enable")) {
        return DoSetEnabled(true);
    }

    Print(L"error: unknown verb %s\n\n", verb.c_str());
    PrintUsage();
    return 1;
}
