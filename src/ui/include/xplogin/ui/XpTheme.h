// XPLogin10 - the Luna "Welcome" screen, as data.
//
// Every colour, gradient stop and metric that XP's logonui.exe used lives here
// rather than being scattered through the painting code. Two reasons:
//
//   * pixel accuracy is a tuning problem. Being able to nudge a stop in
//     XPLogin.theme.ini and restart LogonUI beats a 3 minute rebuild.
//   * the interpolation and scaling maths become unit testable.
//
// The metrics are expressed against XP's 1024x768 design resolution and scaled
// at runtime, because the welcome screen has to look right on a 4K panel too.
#pragma once

#include "xplogin/ShellStatus.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xplogin {
class IniFile;
}

namespace xplogin::ui {

// 0xAARRGGBB, matching GDI+'s ARGB layout.
using Argb = uint32_t;

constexpr Argb MakeArgb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return (static_cast<Argb>(a) << 24) | (static_cast<Argb>(r) << 16) |
           (static_cast<Argb>(g) << 8) | static_cast<Argb>(b);
}

constexpr uint8_t AlphaOf(Argb c) { return static_cast<uint8_t>((c >> 24) & 0xFF); }
constexpr uint8_t RedOf(Argb c)   { return static_cast<uint8_t>((c >> 16) & 0xFF); }
constexpr uint8_t GreenOf(Argb c) { return static_cast<uint8_t>((c >> 8) & 0xFF); }
constexpr uint8_t BlueOf(Argb c)  { return static_cast<uint8_t>(c & 0xFF); }

struct GradientStop {
    float position = 0.0f; // 0..1
    Argb  color = 0xFF000000;
};

// A vertical gradient with an arbitrary number of stops.
class Gradient {
public:
    Gradient() = default;
    explicit Gradient(std::vector<GradientStop> stops) : stops_(std::move(stops)) {}

    void AddStop(float position, Argb color);
    void Clear() { stops_.clear(); }

    // Linear interpolation in straight sRGB, which is what GDI's
    // LinearGradientBrush does - matching it matters more than being correct.
    Argb Sample(float t) const;

    const std::vector<GradientStop>& Stops() const { return stops_; }
    bool empty() const { return stops_.empty(); }

private:
    std::vector<GradientStop> stops_;
};

Argb LerpArgb(Argb a, Argb b, float t);

// XP's design canvas. Everything in XpMetrics is in these units.
constexpr int kDesignWidth  = 1024;
constexpr int kDesignHeight = 768;

struct XpMetrics {
    // NOTE ON PROVENANCE. Values marked [UIFILE] are Microsoft's own, read out
    // of the DirectUI markup in XP SP3's logonui.exe by tools/xp-extract. The
    // rest are still measurements from a screenshot and are the next things to
    // be replaced.
    //
    // XP lays the screen out with a borderlayout, not with absolute
    // coordinates: two 384rp columns centred as a 768rp group, a top panel of
    // a fixed height, and a bottom panel sized by its contents. The absolute
    // numbers below are that layout resolved against the 1024x768 canvas, which
    // is why some of them look oddly specific.

    // Horizontal bands.
    int topBandHeight      = 80;   // [UIFILE] toppanel height=80rp, flat navy
    // [UIFILE] The bottom panel has no fixed height - it is sized by what it
    // holds. Resolved for a desktop: 2rp divider + options padding top 20rp +
    // one 26rp button row + padding bottom 20rp = 68rp, so it starts at 700.
    // (A laptop also gets the 26rp "Undock computer" row plus its 2rp margin,
    // which is why XP's band is taller on portables. We do not show that row.)
    int bottomBandTop      = 700;
    int footerAccentHeight = 2;    // [UIFILE] divider height=2rp (a bitmap, res 126)
    int dividerThickness   = 2;    // [UIFILE] toppanel divider height=2rp

    // [UIFILE] The two columns are 384rp each and centred as a 768rp group, so
    // on a 1024 canvas the group starts at x=128 and the rule between them
    // lands exactly on the screen centre. The rule itself is 1rp.
    int columnWidth         = 384;
    int centerDividerX      = 512;
    int centerDividerWidth  = 1;
    int centerDividerTop    = 100;
    int centerDividerBottom = 640;

    // [UIFILE] An account row is 80rp tall with a 58x58rp picture frame, and
    // the list sits 26rp inside the right column.
    int tileWidth       = 357;
    int tileHeight      = 80;
    int tileSpacing     = 3;
    int tileColumnX     = 539;   // 128 + 384 + 1 divider + 26 margin
    int tileColumnTop   = 326;
    int avatarSize      = 58;
    // [UIFILE] userpane padding=rect(2rp,2rp,14rp,2rp) puts the frame 2rp in
    // from the tile's left edge, and pictureframe margin=rect(0,0,7rp,0) leaves
    // a 7rp gap before the name. They are different numbers, so they are two
    // fields rather than the single "avatarPadding" that used to stand in.
    int avatarPadding   = 2;
    int avatarTextGap   = 7;
    int avatarBorder    = 2;
    // [UIFILE] pictureframe is 58x58rp and holds `picture` under a
    // flowlayout(0,2,2) - centred both ways. Resource 113 confirms it: the
    // plate's flat interior is exactly x/y 5..52, a 48x48 well. The account
    // picture goes in that well, on top of the plate, not the other way round.
    int avatarPictureSize  = 48;
    int avatarPictureInset = 5;
    // [UIFILE] hotaccountlistss: logonaccount { alpha: 96 }, overridden to 255
    // for [mousewithin] and [selected]. XP swaps that sheet in once the pointer
    // is over the account list, so at rest every tile is solid and the moment
    // one is picked out the rest fade back. 96/255 = 0.376.
    int dimmedTileAlpha = 96;

    // Where the selected tile slides to.
    int selectedTileX   = 180;
    int selectedTileY   = 330;

    // Password box, to the right of the selected tile.
    int passwordBoxX      = 560;
    int passwordBoxY      = 352;
    int passwordBoxWidth  = 163;  // [UIFILE] edit id=password width=163rp
    int passwordBoxHeight = 22;
    // [UIFILE] go is rcbmp(103,...,26rp,26rp,...) with margin rect(5rp,0,0,0);
    // info is rcbmp(105,...,28rp,28rp,...) with the same margin. The two are
    // not the same size, which is why they were wrong when both said 26.
    int goButtonSize      = 26;
    int goButtonGap       = 5;
    int hintButtonSize    = 28;
    int hintButtonGap     = 5;

    // [UIFILE] options padding=rect(25rp,20rp,25rp,20rp). Everything in the
    // bottom band - the turn-off button on the left, the hint text on the
    // right - is laid out inside this padding box, which starts below the
    // band's own 2rp divider. XpLayout derives the rest from these three
    // numbers rather than repeating absolute Y values, because when
    // bottomBandTop moved the hand-written ones did not move with it and the
    // hint text ended up sitting on the orange rule.
    int footerPaddingX   = 25;
    int footerPaddingTop = 20;

    // Bottom left "Turn off computer".
    int powerButtonSize = 26;   // [UIFILE] content=rcbmp(107,...,26rp,26rp,...)
    int powerLabelGap   = 2;    // [UIFILE] label margin=rect(2rp,0,0,0)
    int powerLabelWidth = 220;
    int signinOptionsWidth = 240;

    // The logo lockup: the four-pane flag sits above and right of the
    // "Microsoft Windows xp" wordmark, and the whole block lives in the centre
    // column directly above the instruction text.
    // [UIFILE] product is contentalign=topright with padding rect(0,0,20rp,20rp)
    // inside the 384rp left column, so its right edge is 20rp short of the
    // centre rule: 128 + 384 - 20 = 492, and 492 - 137 = 355.
    int logoX      = 355;
    int logoY      = 300;
    int logoWidth  = 137;   // [UIFILE] product content=rcbmp(123,...,137,86,...)
    int logoHeight = 86;
    int logoFlagSize = 48;
    int logoFlagInset = 60; // the flag sits above-right of the wordmark

    // Instruction text, centred under the logo.
    // [UIFILE] help is contentalign=wrapright width=384rp with padding
    // rect(0rp,0rp,40rp,0rp): the left column's full width, text right-aligned,
    // ending 40rp short of the centre rule. It is not centred - XP hangs it off
    // the same right edge the logo sits on.
    int instructionX     = 128;   // the left column's left edge
    int instructionY     = 418;
    int instructionWidth = 344;   // 384rp column less the 40rp right padding

    // The big italic status line, in the box the logo would otherwise occupy.
    // [UIFILE] msgarea and logoarea are both children of leftpanel under a
    // filllayout, so they share one box and only one of them shows - which is
    // why XP's logoff screen has no logo on it. welcome is contentalign
    // topright with padding rect(0,0,22rp,0), so it hangs off a right edge
    // 22rp short of the centre rule: 128 + 384 - 22 = 490.
    int shellStatusX      = 128;
    int shellStatusY      = 300;   // the same top as the logo - one shared box
    int shellStatusWidth  = 362;   // 490 - 128
    int shellStatusHeight = 72;
    // [UIFILE] welcomeshadow padding rect(2rp,3rp,20rp,0) against welcome's
    // rect(0,0,22rp,0): 2rp further right and 3rp further down. That is the
    // whole drop shadow - the same string drawn twice in two colours.
    int shellStatusShadowDx = 2;
    int shellStatusShadowDy = 3;

    // Hint text bottom right. Its right edge and top come from footerPaddingX
    // and footerPaddingTop; only the width is its own.
    int hintTextWidth  = 325;   // [UIFILE] instruct width=325rp
    // The hint shares the options box's single content row with the turn-off
    // button - that is why the band comes out 26rp tall either way. Two lines
    // of the 10.5px small font fit in it; asking for more would push the text
    // out through the bottom of the band.
    int hintTextHeight = 26;

    // "Turn off computer" modal.
    //
    // [DIALOG] Not the welcome screen's own markup - this modal belongs to
    // msgina.dll, which draws it as a plain Win32 dialog with owner-drawn
    // buttons. Template #20100 is 208x122 *dialog units*, and its background
    // bitmap (#20142) is 313x198 pixels, which pins the conversion exactly:
    // 208 x 6/4 = 312 and 122 x 13/8 = 198.25, the standard 8pt MS Shell Dlg
    // base units. So x_px = du x 1.5 and y_px = du x 1.625, and every number
    // below is a control rectangle from that template put through it.
    //
    // The bands in the bitmap agree: navy down to y=42, the welcome-screen
    // blue from 45 to 154, navy again from 156 - which is where the title
    // sits, where the orbs and labels sit, and where Cancel sits.
    int dialogWidth  = 313;   // [DIALOG] the background bitmap, 1:1
    int dialogHeight = 198;
    int dialogButtonSize = 32;  // [DIALOG] 22x20du -> 33x33px; the orbs are 32x32
    int dialogButtonY    = 80;  // [DIALOG] y=49du
    // [DIALOG] Stand By x=36du, Turn Off x=93du, Restart x=150du - a 57du
    // pitch, which is 85.5px. Stored as the first x and the step rather than
    // as three numbers that could drift apart.
    int dialogButtonX    = 54;
    int dialogButtonStep = 86;
    int dialogLabelY      = 119;  // [DIALOG] statics at y=73du
    int dialogLabelWidth  = 75;   // [DIALOG] 50du
    int dialogLabelHeight = 13;   // [DIALOG] 8du
    int dialogTitleX      = 11;   // [DIALOG] static #20102 at 7,0du, 162x26du
    int dialogTitleWidth  = 243;
    int dialogTitleHeight = 42;
    int dialogFlagX       = 264;  // [DIALOG] static #20101 at 176,0du, 32x26du
    int dialogFlagWidth   = 48;   // the bitmap itself is 48x40
    int dialogFlagHeight  = 40;
    int dialogCancelX      = 242; // [DIALOG] button id=2 at 161,103du, 40x12du
    int dialogCancelY      = 167;
    int dialogCancelWidth  = 60;
    int dialogCancelHeight = 20;
};

struct XpColors {
    Gradient headerBand;     // top navy band - nearly flat
    Gradient centerPanel;    // main area
    Gradient footerBand;     // bottom band, below the orange rule
    Gradient dialogBody;

    // The orange rule along the top of the footer. In XP this is a bitmap
    // (resource 126 in logonui.exe), not a solid fill; these two stops are a
    // stand-in until the renderer composites the real image.
    Argb footerAccent      = MakeArgb(0xFF, 0xF3, 0x9A, 0x1C);
    Argb footerAccentGlow  = MakeArgb(0xFF, 0xC8, 0x6A, 0x14);

    // A soft light source in the upper left of the centre panel.
    Argb centerGlow        = MakeArgb(0x5C, 0xC8, 0xDA, 0xFF);
    float centerGlowX      = 0.11f;  // fraction of the design canvas
    float centerGlowY      = 0.19f;
    float centerGlowRadius = 0.34f;

    Argb dividerLine       = MakeArgb(0xFF, 0xFF, 0xFF, 0xFF);
    Argb dividerGlow       = MakeArgb(0x66, 0xFF, 0xFF, 0xFF);
    Argb centerDivider     = MakeArgb(0x80, 0xFF, 0xFF, 0xFF);

    Argb primaryText       = MakeArgb(0xFF, 0xFF, 0xFF, 0xFF);
    // [UIFILE] leftpanelss: element[id=atom(leftpanel)] foreground
    // rgb(239,247,255), and welcomeshadow overrides it with rgb(49,81,181).
    // The status line is drawn twice, the shadow first.
    Argb shellStatusText        = MakeArgb(0xFF, 0xEF, 0xF7, 0xFF);
    Argb shellStatusShadow  = MakeArgb(0xFF, 0x31, 0x51, 0xB5);
    Argb secondaryText     = MakeArgb(0xFF, 0xC7, 0xD9, 0xF5);
    Argb errorText         = MakeArgb(0xFF, 0xFF, 0xE4, 0x8A);
    Argb tileHoverFill     = MakeArgb(0x33, 0xFF, 0xFF, 0xFF);
    Argb tileSelectedFill  = MakeArgb(0x59, 0xFF, 0xFF, 0xFF);
    Argb avatarBorder      = MakeArgb(0xFF, 0xFF, 0xFF, 0xFF);
    Argb avatarShadow      = MakeArgb(0x55, 0x00, 0x00, 0x00);

    Argb passwordBoxFill   = MakeArgb(0xFF, 0xFF, 0xFF, 0xFF);
    Argb passwordBoxBorder = MakeArgb(0xFF, 0x7F, 0x9D, 0xB9);
    Argb passwordText      = MakeArgb(0xFF, 0x00, 0x00, 0x00);

    Argb goButtonOuter     = MakeArgb(0xFF, 0x4C, 0xA8, 0x2E);
    Argb goButtonInner     = MakeArgb(0xFF, 0x8A, 0xD6, 0x5E);
    Argb hintButtonOuter   = MakeArgb(0xFF, 0x2A, 0x5F, 0xBE);
    Argb hintButtonInner   = MakeArgb(0xFF, 0x74, 0xA6, 0xEE);
    Argb powerButtonOuter  = MakeArgb(0xFF, 0xA8, 0x1F, 0x1F);
    Argb powerButtonInner  = MakeArgb(0xFF, 0xE2, 0x5B, 0x4B);

    Argb dialogShade       = MakeArgb(0x9A, 0x00, 0x1A, 0x50);
    Argb dialogBorder      = MakeArgb(0xFF, 0xFF, 0xFF, 0xFF);
};

struct XpFonts {
    // XP used Franklin Gothic Medium for the large text and Tahoma elsewhere.
    // The fallback chain keeps things sane on a machine that has neither.
    std::wstring headingFamily  = L"Franklin Gothic Medium";
    // [UIFILE] leftpanelss sets fontface rcstr(1) for the whole left column,
    // and logonui string 1 is "arial". The big status line is the one place
    // that is quoted rather than inherited from the heading family, because
    // its italic-bold form is what makes it recognisable.
    std::wstring statusFamily   = L"Arial";
    std::wstring bodyFamily     = L"Tahoma";
    std::wstring fallbackFamily = L"Segoe UI";

    float headingSize   = 24.0f; // "To begin, click your user name"
    // [UIFILE] welcome: fontsize rcint(44) pt, and logonui string 44 is "36".
    // 36pt at 96dpi is 48px. fontface is rcstr(1) - "arial" - with fontstyle
    // italic and fontweight bold, which is what makes XP's "welcome" and its
    // shutdown messages look the way they do.
    float shellStatusSize = 48.0f;
    float userNameSize  = 15.0f;
    float statusSize    = 11.0f; // "2 programs running"
    float bodySize      = 12.0f;
    float logoSize      = 22.0f;
    float smallSize     = 10.5f;
};

struct XpTheme {
    XpMetrics metrics;
    XpColors  colors;
    XpFonts   fonts;

    // Localizable strings, so the screen can be translated without a rebuild.
    //
    // The instruction line belongs to the theme rather than to the state
    // machine: [UIFILE] `help` has no content= in the markup, because logonui
    // fills it from its string table at run time. Ours comes from here, so it
    // can be translated or cleared from XPLogin.theme.ini.
    std::wstring textClickUserName = L"To begin, click your user name";
    std::wstring textTypePassword  = L"Type your password";
    std::wstring textTurnOff       = L"Turn off computer";
    // Not an XP string - XP had one way in. Worded to sound like the rest of
    // the screen rather than like Windows 10.
    std::wstring textSigninOptions = L"Other ways to log on";
    std::wstring textLoggingOn     = L"Loading your personal settings...";
    std::wstring textHintFooter =
        L"After you log on, you can add or change accounts.\r\n"
        L"Just go to Control Panel and click User Accounts.";
    std::wstring textDialogTitle   = L"Turn off computer";
    std::wstring textStandBy       = L"Stand By";
    std::wstring textTurnOffButton = L"Turn Off";
    std::wstring textRestart       = L"Restart";
    std::wstring textCancel        = L"Cancel";
    std::wstring textHibernate     = L"Hibernate";

    // The big italic line in the left column while the machine is busy.
    //
    // Every one of these is quoted, not written: "welcome" is rcstr(7) in
    // logonui.exe, and the rest are winlogon.exe's own status strings, ids
    // 1682/1684/1685/1686/1687/1691, read with tools/xp-extract. Winlogon owns
    // the shutdown and logoff sequence and pushes these to whichever host is
    // on screen, which on XP is the welcome screen itself.
    //
    // Lower case "welcome" is not a mistake. That is how the string is stored.
    std::wstring textWelcome            = L"welcome";
    std::wstring textLoadingSettings    = L"Loading your personal settings...";
    std::wstring textLoggingOff         = L"Logging off...";
    std::wstring textSavingSettings     = L"Saving your settings...";
    std::wstring textShuttingDown       = L"Windows is shutting down...";
    std::wstring textPreparingStandBy   = L"Preparing to stand by...";
    std::wstring textPreparingHibernate = L"Preparing to Hibernate...";

    // The wording for a status, or an empty string for ShellStatus::None.
    const std::wstring& StatusMessage(ShellStatus status) const;

    // The stock Luna palette. This is what ships and what the tests pin.
    static XpTheme Default();

    // Overrides anything present in the .ini, leaves the rest at the default.
    static XpTheme FromIni(const IniFile& ini);

    // Uniform scale factor from the 1024x768 design canvas to the real screen.
    // XP letterboxed rather than stretched, so the smaller axis wins.
    static float ScaleFor(int screenWidth, int screenHeight);
};

} // namespace xplogin::ui
