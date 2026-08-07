// XPLogin10 - the "Turn off computer" actions.
//
// Shutting down from the logon desktop needs SeShutdownPrivilege, which
// LogonUI's token has, plus the ShutdownWithoutLogon policy to be enabled -
// exactly the two conditions XP checked before drawing the red button.
#include "Win32Common.h"

#include "xplogin/Interfaces.h"
#include "xplogin/Logging.h"
#include "xplogin/Win32Factories.h"

#include <powrprof.h>

namespace xplogin {
namespace {

bool ShutdownWithoutLogonAllowed() {
    DWORD value = 1;
    if (win32::ReadRegistryDword(
            HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
            L"ShutdownWithoutLogon", &value)) {
        return value != 0;
    }
    return true; // absent means allowed on a workstation SKU
}

class Win32PowerController : public IPowerController {
public:
    bool IsActionAvailable(PowerAction action) override {
        switch (action) {
            case PowerAction::None:
                return true;
            case PowerAction::StandBy:
                return ::IsPwrSuspendAllowed() != FALSE;
            case PowerAction::Hibernate:
                return ::IsPwrHibernateAllowed() != FALSE;
            case PowerAction::TurnOff:
            case PowerAction::Restart:
                return ShutdownWithoutLogonAllowed();
        }
        return false;
    }

    bool Execute(PowerAction action) override {
        if (!IsActionAvailable(action)) {
            return false;
        }

        switch (action) {
            case PowerAction::None:
                return true;

            case PowerAction::StandBy:
                return ::SetSuspendState(FALSE, FALSE, FALSE) != FALSE;

            case PowerAction::Hibernate:
                return ::SetSuspendState(TRUE, FALSE, FALSE) != FALSE;

            case PowerAction::TurnOff:
            case PowerAction::Restart: {
                if (!win32::EnablePrivilege(SE_SHUTDOWN_NAME)) {
                    XPLOG_ERROR("SeShutdownPrivilege not held; cannot power off");
                    return false;
                }
                const UINT flags =
                    (action == PowerAction::Restart ? EWX_REBOOT : EWX_SHUTDOWN) |
                    EWX_FORCEIFHUNG;
                const DWORD reason = SHTDN_REASON_MAJOR_OTHER |
                                     SHTDN_REASON_MINOR_OTHER |
                                     SHTDN_REASON_FLAG_PLANNED;
                if (!::ExitWindowsEx(flags, reason)) {
                    XPLOG_ERROR("ExitWindowsEx failed: %lu",
                                static_cast<unsigned long>(::GetLastError()));
                    return false;
                }
                return true;
            }
        }
        return false;
    }
};

} // namespace

std::shared_ptr<IPowerController> MakeWin32PowerController() {
    return std::make_shared<Win32PowerController>();
}

} // namespace xplogin
