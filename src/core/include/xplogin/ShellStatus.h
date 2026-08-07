// XPLogin10 - the big italic message XP shows while it is busy.
//
// XP's "logging off" and "shutting down" screens are not separate screens.
// They are the welcome screen with its account list gone and one large italic
// line in the left column - the `msgarea` element of logonui.exe's markup,
// which sits in the same box as the Windows logo and replaces it:
//
//   [UIFILE] <element id=atom(msgarea) layout=verticalflowlayout(0,0,0,2)>
//              <element id=atom(welcomeshadow) content=rcstr(7)/>
//              <element id=atom(welcome) content=rcstr(7)/>
//
// Two copies of the same string, the first one drawn as a drop shadow. Its
// default content is rcstr(7), which in logonui.exe's string table is the
// lower-case word "welcome".
//
// The other messages are not logonui's at all: winlogon.exe owns the shutdown
// and logoff sequence and pushes a status line to whichever host is showing.
// They are quoted below from winlogon.exe's own string table, ids 1675-1691,
// read with tools/xp-extract - so the wording, the capitalisation and the
// ellipses are Microsoft's rather than a reconstruction from memory.
#pragma once

#include "xplogin/Types.h"

namespace xplogin {

enum class UiState;

// Which of XP's status lines belongs on the screen. `None` means the left
// column shows the logo and the instruction instead, which is the ordinary
// case.
enum class ShellStatus {
    None = 0,
    Welcome,            // [logonui rcstr(7)]  "welcome"
    LoadingSettings,    // [winlogon 1682]     "Loading your personal settings..."
    LoggingOff,         // [winlogon 1691]     "Logging off..."
    SavingSettings,     // [winlogon 1687]     "Saving your settings..."
    ShuttingDown,       // [winlogon 1684]     "Windows is shutting down..."
    PreparingStandBy,   // [winlogon 1685]     "Preparing to stand by..."
    PreparingHibernate, // [winlogon 1686]     "Preparing to Hibernate..."
};

// The status for a given state. `action` only matters while a power action is
// pending, and is what tells a shutdown from a stand-by.
ShellStatus ShellStatusFor(UiState state, PowerAction action);

// True when the left column shows a status line instead of the logo, which is
// also when the account list is not drawn at all: XP's logoff screen is bare.
bool ShowsShellStatus(UiState state);

const char* ShellStatusName(ShellStatus status);

} // namespace xplogin
