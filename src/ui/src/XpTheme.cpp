#include "xplogin/ui/XpTheme.h"

#include "xplogin/Config.h"

#include <algorithm>

namespace xplogin::ui {
namespace {

uint8_t LerpChannel(uint8_t a, uint8_t b, float t) {
    const float value = static_cast<float>(a) + (static_cast<float>(b) - a) * t;
    if (value <= 0.0f) return 0;
    if (value >= 255.0f) return 255;
    return static_cast<uint8_t>(value + 0.5f);
}

void ReadGradient(const IniFile& ini, const std::string& section, Gradient* gradient) {
    // A gradient section looks like:
    //   [header]
    //   stop0 = 0.0, #003399
    //   stop1 = 1.0, #4A80D8
    std::vector<GradientStop> stops;
    for (int i = 0; i < 16; ++i) {
        const std::string key = "stop" + std::to_string(i);
        if (!ini.Has(section, key)) {
            continue;
        }
        // Split "position, color" by hand: GetColor wants just the colour part.
        const std::wstring raw = ini.GetString(section, key);
        const size_t comma = raw.find(L',');
        if (comma == std::wstring::npos) {
            continue;
        }
        const std::wstring positionText = raw.substr(0, comma);
        const std::wstring colorText = raw.substr(comma + 1);

        IniFile scratch;
        scratch.Set("_", "p", positionText);
        scratch.Set("_", "c", colorText);
        const double position = scratch.GetDouble("_", "p", -1.0);
        if (position < 0.0) {
            continue;
        }
        const Argb color = scratch.GetColor("_", "c", 0);
        if (color == 0) {
            continue;
        }
        stops.push_back(GradientStop{static_cast<float>(position), color});
    }
    if (!stops.empty()) {
        *gradient = Gradient(std::move(stops));
    }
}

} // namespace

void Gradient::AddStop(float position, Argb color) {
    stops_.push_back(GradientStop{position, color});
    std::stable_sort(stops_.begin(), stops_.end(),
                     [](const GradientStop& a, const GradientStop& b) {
                         return a.position < b.position;
                     });
}

Argb Gradient::Sample(float t) const {
    if (stops_.empty()) {
        return 0xFF000000;
    }
    if (stops_.size() == 1) {
        return stops_.front().color;
    }
    if (t <= stops_.front().position) {
        return stops_.front().color;
    }
    if (t >= stops_.back().position) {
        return stops_.back().color;
    }
    for (size_t i = 1; i < stops_.size(); ++i) {
        const GradientStop& lo = stops_[i - 1];
        const GradientStop& hi = stops_[i];
        if (t <= hi.position) {
            const float span = hi.position - lo.position;
            const float local = span <= 0.0f ? 0.0f : (t - lo.position) / span;
            return LerpArgb(lo.color, hi.color, local);
        }
    }
    return stops_.back().color;
}

Argb LerpArgb(Argb a, Argb b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return MakeArgb(LerpChannel(AlphaOf(a), AlphaOf(b), t),
                    LerpChannel(RedOf(a), RedOf(b), t),
                    LerpChannel(GreenOf(a), GreenOf(b), t),
                    LerpChannel(BlueOf(a), BlueOf(b), t));
}

XpTheme XpTheme::Default() {
    XpTheme theme;

    // Read out of the DirectUI markup in XP SP3's logonui.exe:
    //
    //   element [id=atom(toppanel)]        background: rgb(0,48,156)
    //   element [id=atom(contentcontainer)] background: rgb(90,126,220)
    //   element [id=atom(bottompanel)]     background: gradient(argb(0,57,52,173),
    //                                                  argb(0,0,48,156), 0)
    //
    // Two of these settle old arguments. The centre panel really is #5A7EDC,
    // which the reconstruction had right. The top panel is a *flat* #00309C -
    // not the tall dark-to-light ramp the reconstruction used, and not the
    // #0A246A the screenshot measurement suggested.
    theme.colors.headerBand = Gradient({
        {0.00f, MakeArgb(0xFF, 0x00, 0x30, 0x9C)},
    });
    theme.colors.centerPanel = Gradient({
        {0.00f, MakeArgb(0xFF, 0x5A, 0x7E, 0xDC)},
    });
    theme.colors.footerBand = Gradient({
        {0.00f, MakeArgb(0xFF, 0x39, 0x34, 0xAD)},
        {1.00f, MakeArgb(0xFF, 0x00, 0x30, 0x9C)},
    });
    theme.colors.dialogBody = Gradient({
        {0.00f, MakeArgb(0xFF, 0x4E, 0x77, 0xD6)},
        {1.00f, MakeArgb(0xFF, 0x2C, 0x53, 0xAE)},
    });

    return theme;
}

float XpTheme::ScaleFor(int screenWidth, int screenHeight) {
    if (screenWidth <= 0 || screenHeight <= 0) {
        return 1.0f;
    }
    const float sx = static_cast<float>(screenWidth) / kDesignWidth;
    const float sy = static_cast<float>(screenHeight) / kDesignHeight;
    // Uniform scale keeps the tiles square; XP never stretched them.
    return std::min(sx, sy);
}

XpTheme XpTheme::FromIni(const IniFile& ini) {
    XpTheme theme = Default();
    XpMetrics& m = theme.metrics;
    XpColors& c = theme.colors;
    XpFonts& f = theme.fonts;

    m.topBandHeight = ini.GetInt("metrics", "topbandheight", m.topBandHeight);
    m.bottomBandTop = ini.GetInt("metrics", "bottombandtop", m.bottomBandTop);
    m.dividerThickness =
        ini.GetInt("metrics", "dividerthickness", m.dividerThickness);
    m.centerDividerX = ini.GetInt("metrics", "centerdividerx", m.centerDividerX);
    m.centerDividerWidth =
        ini.GetInt("metrics", "centerdividerwidth", m.centerDividerWidth);
    m.columnWidth = ini.GetInt("metrics", "columnwidth", m.columnWidth);
    m.centerDividerTop = ini.GetInt("metrics", "centerdividertop", m.centerDividerTop);
    m.centerDividerBottom =
        ini.GetInt("metrics", "centerdividerbottom", m.centerDividerBottom);
    m.tileWidth = ini.GetInt("metrics", "tilewidth", m.tileWidth);
    m.tileHeight = ini.GetInt("metrics", "tileheight", m.tileHeight);
    m.tileSpacing = ini.GetInt("metrics", "tilespacing", m.tileSpacing);
    m.tileColumnX = ini.GetInt("metrics", "tilecolumnx", m.tileColumnX);
    m.tileColumnTop = ini.GetInt("metrics", "tilecolumntop", m.tileColumnTop);
    m.avatarSize = ini.GetInt("metrics", "avatarsize", m.avatarSize);
    m.selectedTileX = ini.GetInt("metrics", "selectedtilex", m.selectedTileX);
    m.selectedTileY = ini.GetInt("metrics", "selectedtiley", m.selectedTileY);
    m.passwordBoxX = ini.GetInt("metrics", "passwordboxx", m.passwordBoxX);
    m.passwordBoxY = ini.GetInt("metrics", "passwordboxy", m.passwordBoxY);
    m.passwordBoxWidth = ini.GetInt("metrics", "passwordboxwidth", m.passwordBoxWidth);
    m.passwordBoxHeight =
        ini.GetInt("metrics", "passwordboxheight", m.passwordBoxHeight);
    m.footerPaddingX = ini.GetInt("metrics", "footerpaddingx", m.footerPaddingX);
    m.footerPaddingTop =
        ini.GetInt("metrics", "footerpaddingtop", m.footerPaddingTop);
    m.powerButtonSize = ini.GetInt("metrics", "powerbuttonsize", m.powerButtonSize);
    m.footerAccentHeight =
        ini.GetInt("metrics", "footeraccentheight", m.footerAccentHeight);
    m.logoX = ini.GetInt("metrics", "logox", m.logoX);
    m.logoY = ini.GetInt("metrics", "logoy", m.logoY);
    m.logoWidth = ini.GetInt("metrics", "logowidth", m.logoWidth);
    m.logoHeight = ini.GetInt("metrics", "logoheight", m.logoHeight);
    m.logoFlagSize = ini.GetInt("metrics", "logoflagsize", m.logoFlagSize);
    m.logoFlagInset = ini.GetInt("metrics", "logoflaginset", m.logoFlagInset);
    m.instructionWidth =
        ini.GetInt("metrics", "instructionwidth", m.instructionWidth);
    m.instructionX = ini.GetInt("metrics", "instructionx", m.instructionX);
    m.instructionY = ini.GetInt("metrics", "instructiony", m.instructionY);

    c.dividerLine = ini.GetColor("colors", "dividerline", c.dividerLine);
    c.centerDivider = ini.GetColor("colors", "centerdivider", c.centerDivider);
    c.primaryText = ini.GetColor("colors", "primarytext", c.primaryText);
    c.secondaryText = ini.GetColor("colors", "secondarytext", c.secondaryText);
    c.errorText = ini.GetColor("colors", "errortext", c.errorText);
    c.tileHoverFill = ini.GetColor("colors", "tilehover", c.tileHoverFill);
    c.tileSelectedFill = ini.GetColor("colors", "tileselected", c.tileSelectedFill);
    c.goButtonOuter = ini.GetColor("colors", "gobuttonouter", c.goButtonOuter);
    c.goButtonInner = ini.GetColor("colors", "gobuttoninner", c.goButtonInner);
    c.powerButtonOuter = ini.GetColor("colors", "powerbuttonouter", c.powerButtonOuter);
    c.powerButtonInner = ini.GetColor("colors", "powerbuttoninner", c.powerButtonInner);
    c.dialogShade = ini.GetColor("colors", "dialogshade", c.dialogShade);
    c.footerAccent = ini.GetColor("colors", "footeraccent", c.footerAccent);
    c.footerAccentGlow =
        ini.GetColor("colors", "footeraccentglow", c.footerAccentGlow);
    c.centerGlow = ini.GetColor("colors", "centerglow", c.centerGlow);
    c.centerGlowX =
        static_cast<float>(ini.GetDouble("colors", "centerglowx", c.centerGlowX));
    c.centerGlowY =
        static_cast<float>(ini.GetDouble("colors", "centerglowy", c.centerGlowY));
    c.centerGlowRadius = static_cast<float>(
        ini.GetDouble("colors", "centerglowradius", c.centerGlowRadius));

    ReadGradient(ini, "gradient.header", &c.headerBand);
    ReadGradient(ini, "gradient.center", &c.centerPanel);
    ReadGradient(ini, "gradient.footer", &c.footerBand);
    ReadGradient(ini, "gradient.dialog", &c.dialogBody);

    f.headingFamily = ini.GetString("fonts", "heading", f.headingFamily);
    f.bodyFamily = ini.GetString("fonts", "body", f.bodyFamily);
    f.fallbackFamily = ini.GetString("fonts", "fallback", f.fallbackFamily);
    f.headingSize =
        static_cast<float>(ini.GetDouble("fonts", "headingsize", f.headingSize));
    f.userNameSize =
        static_cast<float>(ini.GetDouble("fonts", "usernamesize", f.userNameSize));
    f.statusSize =
        static_cast<float>(ini.GetDouble("fonts", "statussize", f.statusSize));
    f.logoSize = static_cast<float>(ini.GetDouble("fonts", "logosize", f.logoSize));

    theme.textClickUserName =
        ini.GetString("strings", "clickusername", theme.textClickUserName);
    theme.textTypePassword =
        ini.GetString("strings", "typepassword", theme.textTypePassword);
    theme.textTurnOff = ini.GetString("strings", "turnoff", theme.textTurnOff);
    theme.textSigninOptions =
        ini.GetString("strings", "signinoptions", theme.textSigninOptions);
    theme.textLoggingOn = ini.GetString("strings", "loggingon", theme.textLoggingOn);
    theme.textHintFooter = ini.GetString("strings", "hintfooter", theme.textHintFooter);
    theme.textStandBy = ini.GetString("strings", "standby", theme.textStandBy);
    theme.textTurnOffButton =
        ini.GetString("strings", "turnoffbutton", theme.textTurnOffButton);
    theme.textRestart = ini.GetString("strings", "restart", theme.textRestart);
    theme.textCancel = ini.GetString("strings", "cancel", theme.textCancel);
    theme.textHibernate = ini.GetString("strings", "hibernate", theme.textHibernate);

    theme.textWelcome = ini.GetString("strings", "welcome", theme.textWelcome);
    theme.textLoadingSettings =
        ini.GetString("strings", "loadingsettings", theme.textLoadingSettings);
    theme.textLoggingOff =
        ini.GetString("strings", "loggingoff", theme.textLoggingOff);
    theme.textSavingSettings =
        ini.GetString("strings", "savingsettings", theme.textSavingSettings);
    theme.textShuttingDown =
        ini.GetString("strings", "shuttingdown", theme.textShuttingDown);
    theme.textPreparingStandBy =
        ini.GetString("strings", "preparingstandby", theme.textPreparingStandBy);
    theme.textPreparingHibernate =
        ini.GetString("strings", "preparinghibernate", theme.textPreparingHibernate);

    return theme;
}

const std::wstring& XpTheme::StatusMessage(ShellStatus status) const {
    static const std::wstring kNone;
    switch (status) {
        case ShellStatus::Welcome:            return textWelcome;
        case ShellStatus::LoadingSettings:    return textLoadingSettings;
        case ShellStatus::LoggingOff:         return textLoggingOff;
        case ShellStatus::SavingSettings:     return textSavingSettings;
        case ShellStatus::ShuttingDown:       return textShuttingDown;
        case ShellStatus::PreparingStandBy:   return textPreparingStandBy;
        case ShellStatus::PreparingHibernate: return textPreparingHibernate;
        case ShellStatus::None:               break;
    }
    return kNone;
}

} // namespace xplogin::ui
