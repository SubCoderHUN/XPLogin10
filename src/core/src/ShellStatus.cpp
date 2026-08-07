#include "xplogin/ShellStatus.h"

#include "xplogin/LogonStateMachine.h"

namespace xplogin {

ShellStatus ShellStatusFor(UiState state, PowerAction action) {
    switch (state) {
        case UiState::PowerActionPending:
            switch (action) {
                case PowerAction::StandBy:
                    return ShellStatus::PreparingStandBy;
                case PowerAction::Hibernate:
                    return ShellStatus::PreparingHibernate;
                case PowerAction::TurnOff:
                case PowerAction::Restart:
                    // XP shows the same line for both. There is no separate
                    // "restarting" string in winlogon's table - a restart is a
                    // shutdown that comes back, and it says so.
                    return ShellStatus::ShuttingDown;
                case PowerAction::None:
                    break;
            }
            return ShellStatus::ShuttingDown;

        case UiState::LoggedOn:
            // LSA has accepted the credentials and Winlogon is building the
            // session - which takes long enough to be visible, and on this
            // machine took the best part of a minute.
            //
            // Leaving the password view up for that made the screen look
            // frozen: the box had cleared itself, nothing responded to a key,
            // and there was no sign anything was happening until the desktop
            // appeared. XP does not do that. It puts up the word rcstr(7) -
            // "welcome" - and that is the whole point of msgarea existing.
            return ShellStatus::Welcome;

        default:
            return ShellStatus::None;
    }
}

bool ShowsShellStatus(UiState state) {
    return ShellStatusFor(state, PowerAction::None) != ShellStatus::None;
}

const char* ShellStatusName(ShellStatus status) {
    switch (status) {
        case ShellStatus::None:               return "none";
        case ShellStatus::Welcome:            return "welcome";
        case ShellStatus::LoadingSettings:    return "loading settings";
        case ShellStatus::LoggingOff:         return "logging off";
        case ShellStatus::SavingSettings:     return "saving settings";
        case ShellStatus::ShuttingDown:       return "shutting down";
        case ShellStatus::PreparingStandBy:   return "preparing to stand by";
        case ShellStatus::PreparingHibernate: return "preparing to hibernate";
    }
    return "?";
}

} // namespace xplogin
