// XPLogin10 - constructors for the Windows-backed implementations.
//
// Everything above the core talks to interfaces; this header is the one place
// that names the concrete Win32 classes, so a caller can build a fully wired
// LogonController in a few lines. Available only in a Windows build.
#pragma once

#ifdef XPLOGIN_WIN32

#include "xplogin/Config.h"
#include "xplogin/HealthMonitor.h"
#include "xplogin/Interfaces.h"
#include "xplogin/LogonController.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <memory>
#include <string>

namespace xplogin {

// Small Win32 helpers the provider, the service and the installer all need.
namespace win32 {

bool ReadRegistryString(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                        std::wstring* out);
bool ReadRegistryDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                       DWORD* out);
bool WriteRegistryDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                        DWORD value);
bool WriteRegistryString(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                         const std::wstring& value);

std::wstring SidToString(PSID sid);
std::wstring ModuleDirectory(HMODULE module);
bool IsOnSecureDesktop();
bool EnablePrivilege(const wchar_t* privilegeName);

} // namespace win32

std::shared_ptr<IClock>          MakeWin32Clock();
std::shared_ptr<IStateStore>     MakeWin32StateStore();
std::shared_ptr<IUserEnumerator> MakeWin32UserEnumerator();
std::shared_ptr<ISessionManager> MakeWin32SessionManager();
std::shared_ptr<IPowerController> MakeWin32PowerController();
std::shared_ptr<ISoundPlayer>    MakeWin32SoundPlayer(const SoundConfig& config);
std::shared_ptr<ISystemRestore>  MakeWin32SystemRestore();

// Calls LogonUserW directly. Used by the preview host and by the optional
// pre-flight check; the credential provider uses the deferred one below.
std::shared_ptr<IAuthenticator> MakeWin32Authenticator();

// Returns AuthStatus::Pending and lets LSA do the work. This is the correct
// path for a credential provider: it is what creates the session, loads the
// profile and starts the shell.
std::shared_ptr<IAuthenticator> MakeDeferredLsaAuthenticator();

// GetSystemMetrics(SM_CLEANBOOT), mapped to our enum.
BootMode DetectBootMode();

// Loads XPLogin.ini from the path recorded at install time, falling back to the
// file next to the DLL and then to built-in defaults.
AppConfig LoadInstalledConfig(const std::wstring& moduleDirectory);

// Builds every dependency at once.
ControllerDependencies MakeWin32Dependencies(const AppConfig& config,
                                             bool useDeferredAuthenticator);

} // namespace xplogin

#endif // XPLOGIN_WIN32
