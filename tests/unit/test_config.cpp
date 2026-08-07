// Unit tests: configuration parsing.
//
// The .ini is edited by hand on a machine that may not boot afterwards, so the
// parser has to be forgiving about formatting and absolutely unforgiving about
// silently misreading a value.
#include "xplogin/Config.h"
#include "xplogin/ui/XpTheme.h"
#include "xptest.h"

using namespace xplogin;

// ---------------------------------------------------------------------------
// IniFile
// ---------------------------------------------------------------------------

TEST(IniFile, ParsesSectionsAndKeys) {
    IniFile ini;
    REQUIRE(ini.ParseString(
        "[general]\n"
        "enabled = true\n"
        "loglevel = 3\n"
        "[ui]\n"
        "theme = C:\\XPLogin\\luna.ini\n"));

    CHECK(ini.GetBool("general", "enabled", false));
    CHECK_EQ(ini.GetInt("general", "loglevel", 0), 3);
    CHECK_EQ(ini.GetString("ui", "theme"), std::wstring(L"C:\\XPLogin\\luna.ini"));
}

TEST(IniFile, SectionAndKeyLookupIsCaseInsensitive) {
    IniFile ini;
    REQUIRE(ini.ParseString("[General]\nEnabled = yes\n"));

    CHECK(ini.GetBool("general", "enabled", false));
    CHECK(ini.GetBool("GENERAL", "ENABLED", false));
    CHECK(ini.Has("gEnErAl", "eNaBlEd"));
}

TEST(IniFile, IgnoresWholeLineCommentsAndBlankLines) {
    IniFile ini;
    REQUIRE(ini.ParseString(
        "; a comment\n"
        "\n"
        "# another comment\n"
        "[general]\n"
        "   \n"
        "enabled = false\n"));

    CHECK_FALSE(ini.GetBool("general", "enabled", true));
    CHECK_EQ(ini.Sections().size(), size_t(1));
}

TEST(IniFile, DoesNotStripTrailingComments) {
    // Win32's GetPrivateProfileString treats ';' as a comment only at the start
    // of a line, and a path like "C:\a;b" is legal. We match that, so a value
    // with a trailing ';' is the whole rest of the line - and an unparseable
    // one falls back rather than being silently half-read.
    IniFile ini;
    REQUIRE(ini.ParseString(
        "[general]\n"
        "enabled = false ; turn this on later\n"
        "logpath = C:\\Logs\\xplogin.log\n"));

    CHECK_EQ(ini.GetString("general", "enabled"),
             std::wstring(L"false ; turn this on later"));
    CHECK(ini.GetBool("general", "enabled", true));   // unparseable -> fallback
    CHECK_FALSE(ini.GetBool("general", "enabled", false));
    CHECK_EQ(ini.GetString("general", "logpath"),
             std::wstring(L"C:\\Logs\\xplogin.log"));
}

TEST(IniFile, TrimsWhitespaceAndStripsQuotes) {
    IniFile ini;
    REQUIRE(ini.ParseString(
        "[ui]\n"
        "   avatardirectory   =    \"C:\\Program Files\\XPLogin10\\avatars\"   \n"));
    CHECK_EQ(ini.GetString("ui", "avatardirectory"),
             std::wstring(L"C:\\Program Files\\XPLogin10\\avatars"));
}

TEST(IniFile, AcceptsAllTheBooleanSpellings) {
    IniFile ini;
    REQUIRE(ini.ParseString(
        "[b]\n"
        "a = 1\nb = true\nc = yes\nd = on\n"
        "e = 0\nf = false\ng = no\nh = off\n"
        "i = TRUE\nj = Off\n"));

    CHECK(ini.GetBool("b", "a", false));
    CHECK(ini.GetBool("b", "b", false));
    CHECK(ini.GetBool("b", "c", false));
    CHECK(ini.GetBool("b", "d", false));
    CHECK_FALSE(ini.GetBool("b", "e", true));
    CHECK_FALSE(ini.GetBool("b", "f", true));
    CHECK_FALSE(ini.GetBool("b", "g", true));
    CHECK_FALSE(ini.GetBool("b", "h", true));
    CHECK(ini.GetBool("b", "i", false));
    CHECK_FALSE(ini.GetBool("b", "j", true));
}

TEST(IniFile, UnparseableValuesFallBackInsteadOfGuessing) {
    IniFile ini;
    REQUIRE(ini.ParseString("[x]\nn = banana\nb = maybe\n"));
    CHECK_EQ(ini.GetInt("x", "n", 42), 42);
    CHECK(ini.GetBool("x", "b", true));
    CHECK_FALSE(ini.GetBool("x", "b", false));
}

TEST(IniFile, MissingKeysReturnTheFallback) {
    IniFile ini;
    REQUIRE(ini.ParseString("[x]\na = 1\n"));
    CHECK_EQ(ini.GetInt("x", "missing", 7), 7);
    CHECK_EQ(ini.GetInt("missing", "a", 7), 7);
    CHECK_EQ(ini.GetString("x", "missing", L"default"), std::wstring(L"default"));
    CHECK_FALSE(ini.Has("x", "missing"));
}

TEST(IniFile, ParsesHexAndDecimalColors) {
    IniFile ini;
    REQUIRE(ini.ParseString(
        "[c]\n"
        "rgb = #5A7EDC\n"
        "argb = #805A7EDC\n"
        "triplet = 90, 126, 220\n"));

    CHECK_EQ(ini.GetColor("c", "rgb", 0), 0xFF5A7EDCu);
    CHECK_EQ(ini.GetColor("c", "argb", 0), 0x805A7EDCu);
    CHECK_EQ(ini.GetColor("c", "triplet", 0), 0xFF5A7EDCu);
}

TEST(IniFile, RejectsMalformedColors) {
    IniFile ini;
    REQUIRE(ini.ParseString(
        "[c]\n"
        "short = #ABC\n"
        "bad = #GGGGGG\n"
        "outofrange = 300, 0, 0\n"
        "missing = 1, 2\n"));

    CHECK_EQ(ini.GetColor("c", "short", 0xDEADBEEF), 0xDEADBEEFu);
    CHECK_EQ(ini.GetColor("c", "bad", 0xDEADBEEF), 0xDEADBEEFu);
    CHECK_EQ(ini.GetColor("c", "outofrange", 0xDEADBEEF), 0xDEADBEEFu);
    CHECK_EQ(ini.GetColor("c", "missing", 0xDEADBEEF), 0xDEADBEEFu);
}

TEST(IniFile, ReportsSyntaxErrorsWithoutThrowingThemAway) {
    IniFile ini;
    // Parsing "fails" but everything valid is still available - a typo in one
    // line must not blank out the whole configuration.
    CHECK_FALSE(ini.ParseString(
        "[general\n"
        "enabled = true\n"
        "this line has no equals sign\n"
        "= emptykey\n"));

    CHECK_GE(ini.Errors().size(), size_t(3));
    CHECK(ini.GetBool("", "enabled", false));
}

TEST(IniFile, LaterKeysWin) {
    IniFile ini;
    REQUIRE(ini.ParseString("[x]\na = 1\na = 2\n"));
    CHECK_EQ(ini.GetInt("x", "a", 0), 2);
}

TEST(IniFile, HandlesCrLfLineEndings) {
    IniFile ini;
    REQUIRE(ini.ParseString("[general]\r\nenabled = true\r\nloglevel = 1\r\n"));
    CHECK(ini.GetBool("general", "enabled", false));
    CHECK_EQ(ini.GetInt("general", "loglevel", 0), 1);
}

TEST(IniFile, HandlesUtf8Values) {
    IniFile ini;
    REQUIRE(ini.ParseString("[strings]\nclickusername = Válasszon felhasználót\n"));
    CHECK_EQ(ini.GetString("strings", "clickusername"),
             std::wstring(L"Válasszon felhasználót"));
}

// ---------------------------------------------------------------------------
// AppConfig
// ---------------------------------------------------------------------------

TEST(AppConfig, EmptyIniYieldsWorkingDefaults) {
    IniFile ini;
    const AppConfig config = AppConfig::FromIni(ini);

    CHECK(config.enabled);
    CHECK(config.filterOtherProviders);
    CHECK(config.allowShutdown);
    CHECK(config.sounds.enabled);
    CHECK(config.health.enabled);
    CHECK_EQ(config.health.unfilterThreshold, 2u);
    CHECK_EQ(config.health.bypassThreshold, 3u);
    CHECK_FALSE(config.users.showGuestAccount);
    CHECK_FALSE(config.users.showBuiltInAdministrator);
    CHECK(config.users.showDomainAccounts);
}

TEST(AppConfig, ReadsEverySection) {
    IniFile ini;
    REQUIRE(ini.ParseString(
        "[general]\n"
        "enabled = true\n"
        "filterotherproviders = false\n"
        "allowshutdown = false\n"
        "maxfailedattempts = 5\n"
        "[users]\n"
        "showdisabled = true\n"
        "showguest = true\n"
        "maxaccounts = 12\n"
        "hidden = kiosk, svc_backup , Guest\n"
        "[ui]\n"
        "animations = false\n"
        "tileslidems = 400\n"
        "[sounds]\n"
        "enabled = true\n"
        "logon = C:\\Windows\\Media\\XP\\logon.wav\n"
        "volume = 80\n"
        "[safety]\n"
        "unfilterthreshold = 1\n"
        "bypassthreshold = 2\n"));

    const AppConfig config = AppConfig::FromIni(ini);

    CHECK_FALSE(config.filterOtherProviders);
    CHECK_FALSE(config.allowShutdown);
    CHECK_EQ(config.maxFailedAttempts, 5);

    CHECK(config.users.showDisabledAccounts);
    CHECK(config.users.showGuestAccount);
    CHECK_EQ(config.users.maxAccounts, size_t(12));
    REQUIRE_EQ(config.users.extraHiddenNames.size(), size_t(3));
    CHECK_EQ(config.users.extraHiddenNames[0], std::wstring(L"kiosk"));
    CHECK_EQ(config.users.extraHiddenNames[1], std::wstring(L"svc_backup"));
    CHECK_EQ(config.users.extraHiddenNames[2], std::wstring(L"Guest"));

    CHECK_FALSE(config.ui.animationsEnabled);
    CHECK_EQ(config.ui.tileSlideDurationMs, 400);

    CHECK_EQ(config.sounds.logonPath,
             std::wstring(L"C:\\Windows\\Media\\XP\\logon.wav"));
    CHECK_EQ(config.sounds.volumePercent, 80);

    CHECK_EQ(config.health.unfilterThreshold, 1u);
    CHECK_EQ(config.health.bypassThreshold, 2u);
}

TEST(AppConfig, ClampsVolumeToARealRange) {
    IniFile loud;
    REQUIRE(loud.ParseString("[sounds]\nvolume = 500\n"));
    CHECK_EQ(AppConfig::FromIni(loud).sounds.volumePercent, 100);

    IniFile negative;
    REQUIRE(negative.ParseString("[sounds]\nvolume = -20\n"));
    CHECK_EQ(AppConfig::FromIni(negative).sounds.volumePercent, 0);
}

TEST(AppConfig, EmptyHiddenListProducesNoEntries) {
    IniFile ini;
    REQUIRE(ini.ParseString("[users]\nhidden = \n"));
    CHECK(AppConfig::FromIni(ini).users.extraHiddenNames.empty());

    IniFile commas;
    REQUIRE(commas.ParseString("[users]\nhidden = , , ,\n"));
    CHECK(AppConfig::FromIni(commas).users.extraHiddenNames.empty());
}

// ---------------------------------------------------------------------------
// The files the setup actually ships.
//
// XPLogin.theme.ini is deployed next to the DLL and read at every sign-in, and
// what it says wins over the built-in defaults. That makes it a second, quieter
// copy of the screen's geometry - and for a while it was a *stale* copy: a set
// of numbers measured off a screenshot before Microsoft's own were recovered
// from the DirectUI markup in logonui.exe. The previews and every test used
// the real values; the installed product used the guesses. The screen was laid
// out with a top band twice as tall as XP's and the instruction line above the
// Windows logo instead of below it, and nothing failed.
// ---------------------------------------------------------------------------

namespace {

IniFile ShippedTheme() {
    IniFile ini;
    const bool loaded =
        ini.LoadFromFile(std::string(XPLOGIN_SOURCE_DIR) +
                         "/resources/XPLogin.theme.ini");
    CHECK(loaded);
    return ini;
}

} // namespace

TEST(AppConfig, TheShippedThemeDoesNotRelayoutTheScreen) {
    const ui::XpMetrics shipped = ui::XpTheme::FromIni(ShippedTheme()).metrics;
    const ui::XpMetrics expected = ui::XpTheme::Default().metrics;

    // One CHECK_EQ per field, so a failure names the field that drifted and
    // prints both numbers.
#define SAME_METRIC(field) CHECK_EQ(shipped.field, expected.field)

    SAME_METRIC(topBandHeight);
    SAME_METRIC(bottomBandTop);
    SAME_METRIC(dividerThickness);
    SAME_METRIC(footerAccentHeight);
    SAME_METRIC(centerDividerX);
    SAME_METRIC(centerDividerTop);
    SAME_METRIC(centerDividerBottom);
    SAME_METRIC(columnWidth);
    SAME_METRIC(tileWidth);
    SAME_METRIC(tileHeight);
    SAME_METRIC(tileSpacing);
    SAME_METRIC(tileColumnX);
    SAME_METRIC(tileColumnTop);
    SAME_METRIC(avatarSize);
    SAME_METRIC(avatarPadding);
    SAME_METRIC(avatarTextGap);
    SAME_METRIC(avatarPictureSize);
    SAME_METRIC(avatarPictureInset);
    SAME_METRIC(selectedTileX);
    SAME_METRIC(selectedTileY);
    SAME_METRIC(passwordBoxX);
    SAME_METRIC(passwordBoxY);
    SAME_METRIC(passwordBoxWidth);
    SAME_METRIC(passwordBoxHeight);
    SAME_METRIC(goButtonSize);
    SAME_METRIC(hintButtonSize);
    SAME_METRIC(footerPaddingX);
    SAME_METRIC(footerPaddingTop);
    SAME_METRIC(powerButtonSize);
    SAME_METRIC(logoX);
    SAME_METRIC(logoY);
    SAME_METRIC(logoWidth);
    SAME_METRIC(logoHeight);
    SAME_METRIC(instructionX);
    SAME_METRIC(instructionY);
    SAME_METRIC(instructionWidth);
    SAME_METRIC(hintTextWidth);
#undef SAME_METRIC
}

TEST(AppConfig, TheInstructionSitsUnderTheWindowsLogo) {
    // [UIFILE] logoarea is a verticalflowlayout holding `product` (the 137x86
    // logo) and then `help` - in that order, so the logo is on top and the
    // instruction under it. Having them the other way round is the single most
    // visible way to get XP's left column wrong.
    const ui::XpMetrics m = ui::XpTheme::FromIni(ShippedTheme()).metrics;
    CHECK(m.instructionY >= m.logoY + m.logoHeight);

    // Both hang off the same right edge - [UIFILE] product padding
    // rect(0,0,20rp,20rp), help padding rect(0,0,40rp,0) inside a 384rp column.
    CHECK_EQ(m.logoX + m.logoWidth, 492);
    CHECK_EQ(m.instructionX + m.instructionWidth, 472);
}
