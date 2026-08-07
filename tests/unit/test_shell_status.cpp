// Which of XP's status lines belongs on the screen, and what it says.
//
// Every string these tests name is quoted from an XP binary, read with
// tools/xp-extract - "welcome" is rcstr(7) in logonui.exe and the rest are
// winlogon.exe's own status strings, ids 1682/1684/1685/1686/1687/1691.
// Winlogon owns the shutdown and logoff sequence and pushes a line to whatever
// host is on screen; on XP that host is the welcome screen itself, which is
// why logging off and shutting down are not separate screens there.
//
// The wording is pinned because it is evidence, not preference. Getting an
// ellipsis or a capital letter wrong here is the difference between a port and
// an impression, and nobody would ever notice it in a screenshot.
#include "xptest.h"

#include "xplogin/Config.h"
#include "xplogin/LogonStateMachine.h"
#include "xplogin/ShellStatus.h"
#include "xplogin/ui/XpTheme.h"

#include <string>

using namespace xplogin;

TEST(ShellStatus, EachPowerActionGetsWinlogonsOwnWording) {
    const ui::XpTheme theme = ui::XpTheme::Default();

    struct Case {
        PowerAction  action;
        ShellStatus  status;
        const wchar_t* text;
    };
    const Case cases[] = {
        // [winlogon 1684] There is no separate "restarting" string in XP: a
        // restart is a shutdown that comes back, and both say this.
        {PowerAction::TurnOff,   ShellStatus::ShuttingDown,
         L"Windows is shutting down..."},
        {PowerAction::Restart,   ShellStatus::ShuttingDown,
         L"Windows is shutting down..."},
        // [winlogon 1685] lower-case "stand by"...
        {PowerAction::StandBy,   ShellStatus::PreparingStandBy,
         L"Preparing to stand by..."},
        // ...and [winlogon 1686] capital "Hibernate". Microsoft's own
        // inconsistency, kept.
        {PowerAction::Hibernate, ShellStatus::PreparingHibernate,
         L"Preparing to Hibernate..."},
    };

    for (const Case& c : cases) {
        const ShellStatus status =
            ShellStatusFor(UiState::PowerActionPending, c.action);
        CHECK_EQ(static_cast<int>(status), static_cast<int>(c.status));
        CHECK_EQ(theme.StatusMessage(status), std::wstring(c.text));
    }
}

TEST(ShellStatus, TheLogonScreenHasNoStatusLine) {
    // A status line means the account list is hidden, which on a screen
    // somebody is trying to sign in on is the bug that produced an empty
    // screen once already. Only the two states where the machine is genuinely
    // busy get one.
    for (UiState state : {UiState::Hidden, UiState::UserList,
                          UiState::PasswordEntry, UiState::Authenticating,
                          UiState::AuthFailed, UiState::TurnOffDialog}) {
        CHECK_EQ(static_cast<int>(ShellStatusFor(state, PowerAction::None)),
                 static_cast<int>(ShellStatus::None));
        CHECK_FALSE(ShowsShellStatus(state));
    }
    CHECK(ShowsShellStatus(UiState::PowerActionPending));
    CHECK(ShowsShellStatus(UiState::LoggedOn));
}

TEST(ShellStatus, AnAcceptedLogonShowsWelcome) {
    // Between LSA accepting the credentials and the desktop appearing,
    // Winlogon is building the session - most of a minute on a slow machine.
    // Leaving the password view up for that looked like a hang: the box had
    // cleared itself and nothing answered a keypress. [logonui rcstr(7)]
    const ui::XpTheme theme = ui::XpTheme::Default();
    const ShellStatus status =
        ShellStatusFor(UiState::LoggedOn, PowerAction::None);
    CHECK_EQ(static_cast<int>(status), static_cast<int>(ShellStatus::Welcome));
    CHECK_EQ(theme.StatusMessage(status), std::wstring(L"welcome"));
}

TEST(ShellStatus, NoneHasNoWording) {
    const ui::XpTheme theme = ui::XpTheme::Default();
    CHECK(theme.StatusMessage(ShellStatus::None).empty());
}

TEST(ShellStatus, TheRestOfWinlogonsStatusLineIsCarriedToo) {
    // Nothing selects these: winlogon shows them during a sequence that has
    // already taken this screen down. They are here because they are XP's own
    // wording, a theme can reach them, and the day something can display them
    // it should not have to invent them.
    const ui::XpTheme theme = ui::XpTheme::Default();
    CHECK_EQ(theme.StatusMessage(ShellStatus::Welcome), std::wstring(L"welcome"));
    CHECK_EQ(theme.StatusMessage(ShellStatus::LoadingSettings),
             std::wstring(L"Loading your personal settings..."));
    CHECK_EQ(theme.StatusMessage(ShellStatus::LoggingOff),
             std::wstring(L"Logging off..."));
    CHECK_EQ(theme.StatusMessage(ShellStatus::SavingSettings),
             std::wstring(L"Saving your settings..."));
}

TEST(ShellStatus, AThemeCanTranslateEveryStatusLine) {
    IniFile ini;
    ini.ParseString(
        "[strings]\n"
        "shuttingdown = A szamitogep leall...\n"
        "preparingstandby = Keszules keszenletre...\n");

    const ui::XpTheme theme = ui::XpTheme::FromIni(ini);
    CHECK_EQ(theme.StatusMessage(ShellStatus::ShuttingDown),
             std::wstring(L"A szamitogep leall..."));
    CHECK_EQ(theme.StatusMessage(ShellStatus::PreparingStandBy),
             std::wstring(L"Keszules keszenletre..."));
    // Untouched keys keep XP's wording.
    CHECK_EQ(theme.StatusMessage(ShellStatus::PreparingHibernate),
             std::wstring(L"Preparing to Hibernate..."));
}
