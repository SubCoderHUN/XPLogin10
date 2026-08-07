// Unit tests: the welcome screen geometry, theme and animation.
//
// The renderer draws whatever XpLayout hands it, so these tests are what stands
// between a code change and a logon screen with overlapping tiles or a go
// button you cannot click.
#include "xplogin/Config.h"
#include "xplogin/ui/XpLayout.h"
#include "xplogin/ui/XpTheme.h"
#include "xptest.h"

using namespace xplogin;
using namespace xplogin::ui;

namespace {

LayoutInput ListInput(int users = 3, int width = 1024, int height = 768) {
    LayoutInput input;
    input.screenWidth = width;
    input.screenHeight = height;
    input.state = UiState::UserList;
    input.userCount = users;
    return input;
}

LayoutInput PasswordInput(int selected = 1) {
    LayoutInput input = ListInput();
    input.state = UiState::PasswordEntry;
    input.selectedUser = selected;
    input.slideProgress = 1.0f;
    return input;
}

Point Center(const Rect& r) { return Point{r.x + r.width / 2, r.y + r.height / 2}; }

} // namespace

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

TEST(Theme, GradientReturnsEndpointsExactly) {
    Gradient gradient({{0.0f, MakeArgb(255, 0, 0, 0)}, {1.0f, MakeArgb(255, 255, 255, 255)}});
    CHECK_EQ(gradient.Sample(0.0f), MakeArgb(255, 0, 0, 0));
    CHECK_EQ(gradient.Sample(1.0f), MakeArgb(255, 255, 255, 255));
}

TEST(Theme, GradientInterpolatesLinearlyAtTheMidpoint) {
    Gradient gradient({{0.0f, MakeArgb(255, 0, 0, 0)}, {1.0f, MakeArgb(255, 200, 100, 50)}});
    const Argb mid = gradient.Sample(0.5f);
    CHECK_EQ(int(RedOf(mid)), 100);
    CHECK_EQ(int(GreenOf(mid)), 50);
    CHECK_EQ(int(BlueOf(mid)), 25);
    CHECK_EQ(int(AlphaOf(mid)), 255);
}

TEST(Theme, GradientClampsOutOfRangeSamples) {
    Gradient gradient({{0.2f, MakeArgb(255, 10, 20, 30)}, {0.8f, MakeArgb(255, 90, 80, 70)}});
    CHECK_EQ(gradient.Sample(-5.0f), MakeArgb(255, 10, 20, 30));
    CHECK_EQ(gradient.Sample(0.0f), MakeArgb(255, 10, 20, 30));
    CHECK_EQ(gradient.Sample(1.5f), MakeArgb(255, 90, 80, 70));
}

TEST(Theme, GradientHandlesThreeStops) {
    Gradient gradient({{0.0f, MakeArgb(255, 0, 0, 0)},
                       {0.5f, MakeArgb(255, 100, 100, 100)},
                       {1.0f, MakeArgb(255, 0, 0, 0)}});
    CHECK_EQ(int(RedOf(gradient.Sample(0.25f))), 50);
    CHECK_EQ(int(RedOf(gradient.Sample(0.5f))), 100);
    CHECK_EQ(int(RedOf(gradient.Sample(0.75f))), 50);
}

TEST(Theme, EmptyAndSingleStopGradientsAreSafe) {
    Gradient empty;
    CHECK_EQ(empty.Sample(0.5f), MakeArgb(255, 0, 0, 0));

    Gradient single({{0.3f, MakeArgb(255, 12, 34, 56)}});
    CHECK_EQ(single.Sample(0.0f), MakeArgb(255, 12, 34, 56));
    CHECK_EQ(single.Sample(1.0f), MakeArgb(255, 12, 34, 56));
}

TEST(Theme, AddStopKeepsStopsSorted) {
    Gradient gradient;
    gradient.AddStop(1.0f, MakeArgb(255, 255, 255, 255));
    gradient.AddStop(0.0f, MakeArgb(255, 0, 0, 0));
    gradient.AddStop(0.5f, MakeArgb(255, 128, 128, 128));

    CHECK_EQ(gradient.Stops().size(), size_t(3));
    CHECK_NEAR(gradient.Stops()[0].position, 0.0f, 1e-6);
    CHECK_NEAR(gradient.Stops()[1].position, 0.5f, 1e-6);
    CHECK_NEAR(gradient.Stops()[2].position, 1.0f, 1e-6);
}

TEST(Theme, DefaultPaletteMatchesTheDirectUiMarkup) {
    // These three are quoted from the UIFILE resource in XP SP3's logonui.exe:
    //   toppanel         rgb(0,48,156)
    //   contentcontainer rgb(90,126,220)
    //   bottompanel      gradient(rgb(57,52,173) -> rgb(0,48,156))
    const XpTheme theme = XpTheme::Default();

    // The centre panel is flat #5A7EDC, top to bottom.
    for (float t : {0.0f, 0.5f, 1.0f}) {
        const Argb centre = theme.colors.centerPanel.Sample(t);
        CHECK_EQ(int(RedOf(centre)), 0x5A);
        CHECK_EQ(int(GreenOf(centre)), 0x7E);
        CHECK_EQ(int(BlueOf(centre)), 0xDC);
    }

    // The header is flat too - it is not a ramp, which the first
    // reconstruction got wrong in a very visible way.
    CHECK_EQ(theme.colors.headerBand.Sample(0.0f),
             theme.colors.headerBand.Sample(1.0f));
    CHECK_EQ(theme.colors.headerBand.Sample(0.5f), MakeArgb(0xFF, 0x00, 0x30, 0x9C));

    // The footer really is a ramp, from the lighter indigo down to the header
    // colour.
    CHECK_EQ(theme.colors.footerBand.Sample(0.0f), MakeArgb(0xFF, 0x39, 0x34, 0xAD));
    CHECK_EQ(theme.colors.footerBand.Sample(1.0f), MakeArgb(0xFF, 0x00, 0x30, 0x9C));
}

TEST(Theme, GeometryMatchesTheDirectUiMarkup) {
    // Every value here is quoted from logonui.exe's UIFILE resource. They are
    // the reason this screen is a port rather than an impression, so a change
    // to any of them should have to argue with this test first.
    const XpMetrics m = XpTheme::Default().metrics;

    CHECK_EQ(m.topBandHeight, 80);      // toppanel height=80rp
    CHECK_EQ(m.dividerThickness, 2);    // divider height=2rp
    CHECK_EQ(m.columnWidth, 384);       // rightpanel width=384rp
    CHECK_EQ(m.centerDividerWidth, 1);  // divider width=1rp
    CHECK_EQ(m.tileHeight, 80);         // userpanelayer height=80rp
    CHECK_EQ(m.avatarSize, 58);         // pictureframe 58x58rp
    CHECK_EQ(m.passwordBoxWidth, 163);  // edit id=password width=163rp
    CHECK_EQ(m.powerButtonSize, 26);    // rcbmp(107,...,26rp,26rp,...)
    CHECK_EQ(m.hintTextWidth, 325);     // instruct width=325rp
    CHECK_EQ(m.logoWidth, 137);         // rcbmp(123,...,137,86,...)
    CHECK_EQ(m.logoHeight, 86);

    // The go and info buttons are NOT the same size, however much they look it:
    // go is 26rp, info is 28rp, and both carry margin rect(5rp,0,0,0).
    CHECK_EQ(m.goButtonSize, 26);       // rcbmp(103,...,26rp,26rp,...)
    CHECK_EQ(m.hintButtonSize, 28);     // rcbmp(105,...,28rp,28rp,...)
    CHECK_EQ(m.goButtonGap, 5);         // button[go] margin=rect(5rp,0,0,0)
    CHECK_EQ(m.hintButtonGap, 5);       // button[info] margin=rect(5rp,0,0,0)

    // Inside an account tile: userpane padding=rect(2rp,2rp,14rp,2rp) and
    // pictureframe margin=rect(0,0,7rp,0).
    CHECK_EQ(m.avatarPadding, 2);
    CHECK_EQ(m.avatarTextGap, 7);

    // pictureframe borderthickness=rect(5,5,5,5) over a 58rp frame leaves a
    // 48x48 well, which is also exactly the size of the default picture (114).
    CHECK_EQ(m.avatarPictureInset, 5);
    CHECK_EQ(m.avatarPictureSize, 48);
    CHECK_EQ(m.avatarSize - 2 * m.avatarPictureInset, m.avatarPictureSize);

    // The two columns are centred as a group, which puts the rule between them
    // on the screen centre line.
    CHECK_EQ((kDesignWidth - 2 * m.columnWidth) / 2 + m.columnWidth,
             m.centerDividerX);

    // The logo is right-aligned in the left column with 20rp of padding, so its
    // right edge lands 20rp short of the centre rule.
    const int leftColumnRight = m.centerDividerX;
    CHECK_EQ(m.logoX + m.logoWidth, leftColumnRight - 20);
}

TEST(Theme, BottomBandIsSizedByItsContents) {
    // XP gives the bottom panel no height - it is whatever fits. Resolved for a
    // desktop, that is the 2rp divider, options padding of 20rp top and bottom,
    // and one 26rp button row. Getting this wrong moves the whole footer.
    const XpMetrics m = XpTheme::Default().metrics;

    const int bandHeight = kDesignHeight - m.bottomBandTop;
    CHECK_EQ(bandHeight, m.dividerThickness + 20 + m.powerButtonSize + 20);

    // ...and everything in it hangs off the one padding box.
    CHECK_EQ(m.footerPaddingX, 25);     // options padding=rect(25rp,...,25rp,...)
    CHECK_EQ(m.footerPaddingTop, 20);   // options padding=rect(...,20rp,...,20rp)
}

TEST(Theme, ScaleIsOneAtTheDesignResolution) {
    CHECK_NEAR(XpTheme::ScaleFor(1024, 768), 1.0f, 1e-6);
}

TEST(Theme, ScaleIsUniformAndLetterboxes) {
    // 1920x1080 is wider than 4:3, so the height is the limiting axis.
    CHECK_NEAR(XpTheme::ScaleFor(1920, 1080), 1080.0f / 768.0f, 1e-5);
    // A 5:4 panel is limited by width.
    CHECK_NEAR(XpTheme::ScaleFor(1280, 1024), 1280.0f / 1024.0f, 1e-5);
    CHECK_NEAR(XpTheme::ScaleFor(2048, 1536), 2.0f, 1e-5);
}

TEST(Theme, ScaleRejectsDegenerateScreens) {
    CHECK_NEAR(XpTheme::ScaleFor(0, 0), 1.0f, 1e-6);
    CHECK_NEAR(XpTheme::ScaleFor(-100, 768), 1.0f, 1e-6);
}

TEST(Theme, IniOverridesMetricsAndColors) {
    IniFile ini;
    REQUIRE(ini.ParseString(
        "[metrics]\n"
        "tilewidth = 300\n"
        "avatarsize = 64\n"
        "[colors]\n"
        "primarytext = #FF00FF\n"
        "[fonts]\n"
        "heading = Verdana\n"
        "headingsize = 30.5\n"
        "[strings]\n"
        "clickusername = Kattintson a felhasznalonevere\n"));

    const XpTheme theme = XpTheme::FromIni(ini);
    CHECK_EQ(theme.metrics.tileWidth, 300);
    CHECK_EQ(theme.metrics.avatarSize, 64);
    CHECK_EQ(theme.colors.primaryText, MakeArgb(0xFF, 0xFF, 0x00, 0xFF));
    CHECK_EQ(theme.fonts.headingFamily, std::wstring(L"Verdana"));
    CHECK_NEAR(theme.fonts.headingSize, 30.5f, 1e-4);
    CHECK_EQ(theme.textClickUserName,
             std::wstring(L"Kattintson a felhasznalonevere"));

    // Anything not mentioned keeps the default.
    CHECK_EQ(theme.metrics.tileHeight, XpTheme::Default().metrics.tileHeight);
}

TEST(Theme, IniCanReplaceAWholeGradient) {
    IniFile ini;
    REQUIRE(ini.ParseString(
        "[gradient.center]\n"
        "stop0 = 0.0, #102030\n"
        "stop1 = 1.0, #405060\n"));

    const XpTheme theme = XpTheme::FromIni(ini);
    CHECK_EQ(theme.colors.centerPanel.Stops().size(), size_t(2));
    CHECK_EQ(theme.colors.centerPanel.Sample(0.0f), MakeArgb(0xFF, 0x10, 0x20, 0x30));
    CHECK_EQ(theme.colors.centerPanel.Sample(1.0f), MakeArgb(0xFF, 0x40, 0x50, 0x60));
}

TEST(Theme, MalformedThemeFallsBackInsteadOfBreaking) {
    // A broken theme file must never stop the logon screen coming up.
    IniFile ini;
    ini.ParseString(
        "[metrics]\n"
        "tilewidth = not-a-number\n"
        "[colors]\n"
        "primarytext = #ZZZZZZ\n"
        "[gradient.center]\n"
        "stop0 = garbage\n");

    const XpTheme theme = XpTheme::FromIni(ini);
    CHECK_EQ(theme.metrics.tileWidth, XpTheme::Default().metrics.tileWidth);
    CHECK_EQ(theme.colors.primaryText, XpTheme::Default().colors.primaryText);
    CHECK_FALSE(theme.colors.centerPanel.empty());
}

// ---------------------------------------------------------------------------
// Layout: bands
// ---------------------------------------------------------------------------

TEST(Layout, BandsTileTheScreenWithoutGaps) {
    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(ListInput());

    CHECK_EQ(frame.headerBand.y, 0);
    CHECK_EQ(frame.headerBand.Bottom(), frame.centerPanel.y);
    CHECK_EQ(frame.centerPanel.Bottom(), frame.footerBand.y);
    CHECK_EQ(frame.footerBand.Bottom(), 768);

    // Bands run edge to edge so nothing shows through on a widescreen panel.
    CHECK_EQ(frame.headerBand.width, 1024);
    CHECK_EQ(frame.footerBand.width, 1024);
}

TEST(Layout, BandsStillCoverTheScreenAt1920x1080) {
    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(ListInput(3, 1920, 1080));

    CHECK_EQ(frame.headerBand.width, 1920);
    CHECK_EQ(frame.footerBand.Bottom(), 1080);
    CHECK_EQ(frame.headerBand.Bottom(), frame.centerPanel.y);
    CHECK_EQ(frame.centerPanel.Bottom(), frame.footerBand.y);
    CHECK_GT(frame.centerPanel.height, 0);
}

TEST(Layout, FooterContentsSitInsideTheBandBelowTheRule) {
    // Regression. The turn-off button and the hint text used to carry absolute
    // Y values tuned against an earlier, taller bottom band. When the band was
    // corrected to the height the markup implies, those numbers did not follow
    // it and the hint text ended up drawn on the orange rule, above the band it
    // belongs to. Everything in the footer is now derived from the band, and
    // this is what says so.
    for (int height : {768, 1080, 800, 1200}) {
        XpLayout layout(XpTheme::Default());
        const FrameLayout frame =
            layout.Compute(ListInput(3, height * 4 / 3, height));

        const Rect band = frame.footerBand;
        const int contentTop = frame.footerAccent.Bottom();

        for (const Rect& r : {frame.powerButton, frame.powerLabel,
                              frame.hintFooter}) {
            REQUIRE_FALSE(r.IsEmpty());
            // Below the rule, and fully inside the band.
            CHECK_GE(r.y, contentTop);
            CHECK_GE(r.y, band.y);
            CHECK_LE(r.Bottom(), band.Bottom());
            CHECK_FALSE(r.Intersects(frame.footerAccent));
            CHECK_GE(r.x, 0);
            CHECK_LE(r.Right(), frame.screen.width);
        }

        // The icon and its label share a baseline box, which is what keeps the
        // text optically centred against the icon.
        CHECK_EQ(frame.powerLabel.y, frame.powerButton.y);
        CHECK_EQ(frame.powerLabel.height, frame.powerButton.height);
        CHECK_GE(frame.powerLabel.x, frame.powerButton.Right());

        // The hint text is over on the right, clear of the label.
        CHECK_GT(frame.hintFooter.x, frame.powerLabel.x);
    }
}

TEST(Layout, SigninOptionsIsAbsentUnlessOffered) {
    // On a machine with nothing behind the XP screen the footer must look
    // exactly like XP's - no extra link, nothing to explain.
    XpLayout layout(XpTheme::Default());
    LayoutInput input = ListInput();
    input.showSigninOptions = false;

    const FrameLayout frame = layout.Compute(input);
    CHECK(frame.signinOptions.IsEmpty());

    // Nothing to click where the link would have been. Taken from the offered
    // layout rather than written as a literal, so the two cannot drift apart.
    LayoutInput offered = input;
    offered.showSigninOptions = true;
    const Rect wouldBe = layout.Compute(offered).signinOptions;
    REQUIRE_FALSE(wouldBe.IsEmpty());
    CHECK_EQ(static_cast<int>(XpLayout::HitTest(frame, Center(wouldBe)).target),
             static_cast<int>(HitTarget::None));
}

TEST(Layout, SigninOptionsSitsInTheFooterAndIsClickable) {
    XpLayout layout(XpTheme::Default());
    LayoutInput input = ListInput();
    input.showSigninOptions = true;

    const FrameLayout frame = layout.Compute(input);
    REQUIRE_FALSE(frame.signinOptions.IsEmpty());

    // Inside the band, below the rule, clear of the turn-off label.
    CHECK_GE(frame.signinOptions.y, frame.footerAccent.Bottom());
    CHECK_LE(frame.signinOptions.Bottom(), frame.footerBand.Bottom());
    CHECK_FALSE(frame.signinOptions.Intersects(frame.powerLabel));
    CHECK_FALSE(frame.signinOptions.Intersects(frame.powerButton));
    CHECK_GE(frame.signinOptions.x, frame.powerLabel.Right());

    // ...and clear of the hint text on the right, at the design resolution.
    CHECK_FALSE(frame.signinOptions.Intersects(frame.hintFooter));

    CHECK_EQ(static_cast<int>(
                 XpLayout::HitTest(frame, Center(frame.signinOptions)).target),
             static_cast<int>(HitTarget::SigninOptions));
}

TEST(Layout, SigninOptionsStaysClickableInThePasswordView) {
    // The moment somebody most needs it is after their password was refused.
    LayoutInput input = PasswordInput(0);
    input.showSigninOptions = true;

    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(input);
    REQUIRE_FALSE(frame.signinOptions.IsEmpty());
    CHECK_EQ(static_cast<int>(
                 XpLayout::HitTest(frame, Center(frame.signinOptions)).target),
             static_cast<int>(HitTarget::SigninOptions));
}

TEST(Layout, DividerSitsOnTheBandBoundary) {
    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(ListInput());

    CHECK_EQ(frame.headerDivider.Bottom(), frame.headerBand.Bottom());
    CHECK_EQ(frame.footerDivider.y, frame.footerBand.y);
    CHECK_GE(frame.headerDivider.height, 1);
}

// ---------------------------------------------------------------------------
// Layout: tiles
// ---------------------------------------------------------------------------

TEST(Layout, ListViewShowsEveryUserTile) {
    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(ListInput(4));
    CHECK_EQ(frame.tiles.size(), size_t(4));

    for (size_t i = 0; i < frame.tiles.size(); ++i) {
        CHECK_EQ(frame.tiles[i].userIndex, static_cast<int>(i));
        CHECK_FALSE(frame.tiles[i].bounds.IsEmpty());
    }
}

TEST(Layout, TilesDoNotOverlapAndAreEvenlySpaced) {
    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(ListInput(5));
    REQUIRE_EQ(frame.tiles.size(), size_t(5));

    int expectedStep = frame.tiles[1].bounds.y - frame.tiles[0].bounds.y;
    for (size_t i = 1; i < frame.tiles.size(); ++i) {
        const Rect& previous = frame.tiles[i - 1].bounds;
        const Rect& current = frame.tiles[i].bounds;
        CHECK_FALSE(previous.Intersects(current));
        CHECK_GE(current.y, previous.Bottom());
        CHECK_EQ(current.y - previous.y, expectedStep);
        CHECK_EQ(current.x, previous.x);
    }
}

TEST(Layout, AvatarAndTextSitInsideTheTile) {
    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(ListInput(2));

    for (const TileLayout& tile : frame.tiles) {
        CHECK_GE(tile.avatar.x, tile.bounds.x);
        CHECK_GE(tile.avatar.y, tile.bounds.y);
        CHECK_LE(tile.avatar.Right(), tile.bounds.Right());
        CHECK_LE(tile.avatar.Bottom(), tile.bounds.Bottom());

        CHECK_GE(tile.nameText.x, tile.avatar.Right());
        CHECK_LE(tile.nameText.Right(), tile.bounds.Right());
        CHECK_FALSE(tile.nameText.Intersects(tile.avatar));
        CHECK_FALSE(tile.nameText.Intersects(tile.statusText));
    }
}

TEST(Layout, AvatarPictureSitsInTheWellOfTheFrame) {
    // The 48x48 photo is centred in the 58x58 plate. The renderer draws the
    // plate first and the photo into this rect on top of it, so if the two ever
    // stop agreeing the account pictures land on the plate's border.
    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(ListInput(3));
    REQUIRE_FALSE(frame.tiles.empty());

    for (const TileLayout& tile : frame.tiles) {
        CHECK_FALSE(tile.avatarPicture.IsEmpty());
        CHECK_GT(tile.avatarPicture.x, tile.avatar.x);
        CHECK_GT(tile.avatarPicture.y, tile.avatar.y);
        CHECK_LT(tile.avatarPicture.Right(), tile.avatar.Right());
        CHECK_LT(tile.avatarPicture.Bottom(), tile.avatar.Bottom());

        // Centred: the inset is the same on both axes and both sides.
        CHECK_EQ(tile.avatarPicture.x - tile.avatar.x,
                 tile.avatar.Right() - tile.avatarPicture.Right());
        CHECK_EQ(tile.avatarPicture.y - tile.avatar.y,
                 tile.avatar.Bottom() - tile.avatarPicture.Bottom());
        CHECK_EQ(tile.avatarPicture.width, tile.avatarPicture.height);
    }
}

// ---------------------------------------------------------------------------
// Layout: the hot/rest dimming rule
// ---------------------------------------------------------------------------

TEST(Layout, EveryTileIsSolidWhenNothingIsSingledOut) {
    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(ListInput(4));
    REQUIRE_EQ(frame.tiles.size(), size_t(4));
    for (const TileLayout& tile : frame.tiles) {
        CHECK_NEAR(tile.opacity, 1.0f, 1e-6);
    }
}

TEST(Layout, HoveringOneTileDimsTheOthers) {
    // [UIFILE] hotaccountlistss puts logonaccount at alpha 96 and everything
    // [mousewithin] back at 255.
    LayoutInput input = ListInput(4);
    input.hoveredTile = 2;

    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(input);
    REQUIRE_EQ(frame.tiles.size(), size_t(4));

    const float dimmed = XpTheme::Default().metrics.dimmedTileAlpha / 255.0f;
    for (const TileLayout& tile : frame.tiles) {
        if (tile.userIndex == 2) {
            CHECK_NEAR(tile.opacity, 1.0f, 1e-6);
        } else {
            CHECK_NEAR(tile.opacity, dimmed, 1e-6);
            // Dimmed, not invisible - XP fades them back, it does not hide them.
            CHECK_GT(tile.opacity, 0.0f);
            CHECK_LT(tile.opacity, 1.0f);
        }
    }
}

TEST(Layout, SelectingOneTileDimsTheOthers) {
    // Same rule via [selected] rather than [mousewithin]: picking an account
    // with the keyboard has to dim the rest too, or the highlight only works
    // for the mouse.
    LayoutInput input = ListInput(3);
    input.selectedUser = 0;

    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(input);

    for (const TileLayout& tile : frame.tiles) {
        if (tile.selected) {
            CHECK_NEAR(tile.opacity, 1.0f, 1e-6);
        } else {
            CHECK_LT(tile.opacity, 1.0f);
        }
    }
}

TEST(Layout, TheSelectedTileStaysSolidThroughTheSlide) {
    // The one tile that survives into the password view must never be drawn
    // faded, at any point in the animation.
    for (float progress : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        LayoutInput input = PasswordInput(1);
        input.slideProgress = progress;

        XpLayout layout(XpTheme::Default());
        const FrameLayout frame = layout.Compute(input);
        REQUIRE_EQ(frame.tiles.size(), size_t(1));
        CHECK_NEAR(frame.tiles[0].opacity, 1.0f, 1e-6);
    }
}

TEST(Layout, DimmingIsThemeable) {
    XpTheme theme = XpTheme::Default();
    theme.metrics.dimmedTileAlpha = 0; // fully transparent
    LayoutInput input = ListInput(3);
    input.hoveredTile = 0;

    XpLayout layout(theme);
    const FrameLayout frame = layout.Compute(input);
    CHECK_NEAR(frame.tiles[1].opacity, 0.0f, 1e-6);

    // ...and out-of-range values are clamped rather than trusted.
    theme.metrics.dimmedTileAlpha = 4000;
    XpLayout wild(theme);
    const FrameLayout wildFrame = wild.Compute(input);
    CHECK_NEAR(wildFrame.tiles[1].opacity, 1.0f, 1e-6);
}

TEST(Layout, AvatarIsSquare) {
    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(ListInput(1, 1920, 1080));
    REQUIRE_EQ(frame.tiles.size(), size_t(1));
    CHECK_EQ(frame.tiles[0].avatar.width, frame.tiles[0].avatar.height);
}

TEST(Layout, PasswordViewKeepsOnlyTheSelectedTile) {
    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(PasswordInput(1));

    REQUIRE_EQ(frame.tiles.size(), size_t(1));
    CHECK_EQ(frame.tiles[0].userIndex, 1);
    CHECK(frame.tiles[0].selected);
}

TEST(Layout, SelectedTileSlidesToTheLeftColumn) {
    XpLayout layout(XpTheme::Default());

    LayoutInput start = PasswordInput(0);
    start.slideProgress = 0.0f;
    const FrameLayout atStart = layout.Compute(start);

    LayoutInput end = PasswordInput(0);
    end.slideProgress = 1.0f;
    const FrameLayout atEnd = layout.Compute(end);

    REQUIRE_EQ(atStart.tiles.size(), size_t(1));
    REQUIRE_EQ(atEnd.tiles.size(), size_t(1));
    // It ends up well to the left of where it started.
    CHECK_LT(atEnd.tiles[0].bounds.x, atStart.tiles[0].bounds.x);
    CHECK_GT(atEnd.tiles[0].bounds.y, atStart.tiles[0].bounds.y);
}

TEST(Layout, SlideIsMonotonicAcrossTheAnimation) {
    XpLayout layout(XpTheme::Default());
    int previousX = 1 << 20;
    for (int step = 0; step <= 10; ++step) {
        LayoutInput input = PasswordInput(0);
        input.slideProgress = static_cast<float>(step) / 10.0f;
        const FrameLayout frame = layout.Compute(input);
        REQUIRE_EQ(frame.tiles.size(), size_t(1));
        CHECK_LE(frame.tiles[0].bounds.x, previousX);
        previousX = frame.tiles[0].bounds.x;
    }
}

// ---------------------------------------------------------------------------
// Layout: password controls
// ---------------------------------------------------------------------------

TEST(Layout, PasswordControlsAppearOnlyInThePasswordView) {
    XpLayout layout(XpTheme::Default());

    const FrameLayout list = layout.Compute(ListInput());
    CHECK(list.passwordBox.IsEmpty());
    CHECK(list.goButton.IsEmpty());
    CHECK_FALSE(list.passwordViewActive);
    // The vertical rule and the instruction text only exist in the list view.
    CHECK_FALSE(list.centerDivider.IsEmpty());
    CHECK_FALSE(list.instruction.IsEmpty());

    const FrameLayout password = layout.Compute(PasswordInput());
    CHECK_FALSE(password.passwordBox.IsEmpty());
    CHECK_FALSE(password.goButton.IsEmpty());
    CHECK(password.passwordViewActive);
    CHECK(password.centerDivider.IsEmpty());
    CHECK(password.instruction.IsEmpty());
}

TEST(Layout, GoButtonSitsToTheRightOfThePasswordBoxWithoutOverlap) {
    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(PasswordInput());

    CHECK_GT(frame.goButton.x, frame.passwordBox.Right());
    CHECK_FALSE(frame.goButton.Intersects(frame.passwordBox));
    CHECK_FALSE(frame.goButton.Intersects(frame.hintButton));
    CHECK_GT(frame.hintButton.x, frame.goButton.Right());
}

TEST(Layout, HintAndBackButtonsAreOptional) {
    XpLayout layout(XpTheme::Default());

    LayoutInput noExtras = PasswordInput();
    noExtras.showHintButton = false;
    noExtras.showBackButton = false;
    const FrameLayout frame = layout.Compute(noExtras);

    CHECK(frame.hintButton.IsEmpty());
    CHECK(frame.backButton.IsEmpty());
    CHECK_FALSE(frame.goButton.IsEmpty());
}

// ---------------------------------------------------------------------------
// Layout: turn off dialog
// ---------------------------------------------------------------------------

TEST(Layout, TurnOffDialogIsCentredWithThreeButtons) {
    XpLayout layout(XpTheme::Default());
    LayoutInput input = ListInput();
    input.state = UiState::TurnOffDialog;
    const FrameLayout frame = layout.Compute(input);

    REQUIRE_FALSE(frame.dialog.IsEmpty());
    const int dialogCentre = frame.dialog.x + frame.dialog.width / 2;
    CHECK_NEAR(dialogCentre, 512, 2);

    // Stand By, Turn Off, Restart - in that order, same size, evenly spaced.
    CHECK_LT(frame.dialogStandBy.x, frame.dialogTurnOff.x);
    CHECK_LT(frame.dialogTurnOff.x, frame.dialogRestart.x);
    CHECK_EQ(frame.dialogStandBy.width, frame.dialogTurnOff.width);
    CHECK_EQ(frame.dialogTurnOff.width, frame.dialogRestart.width);
    CHECK_EQ(frame.dialogTurnOff.x - frame.dialogStandBy.x,
             frame.dialogRestart.x - frame.dialogTurnOff.x);

    // The button group is itself centred in the dialog.
    const int groupCentre =
        (frame.dialogStandBy.x + frame.dialogRestart.Right()) / 2;
    CHECK_NEAR(groupCentre, dialogCentre, 2);

    CHECK_FALSE(frame.dialogCancel.IsEmpty());
    CHECK_FALSE(frame.dialogBackdrop.IsEmpty());
}

TEST(Layout, DialogButtonsDoNotOverlapTheirLabels) {
    XpLayout layout(XpTheme::Default());
    LayoutInput input = ListInput();
    input.state = UiState::TurnOffDialog;
    const FrameLayout frame = layout.Compute(input);

    CHECK_FALSE(frame.dialogStandBy.Intersects(frame.dialogStandByLabel));
    CHECK_FALSE(frame.dialogTurnOff.Intersects(frame.dialogTurnOffLabel));
    CHECK_GE(frame.dialogStandByLabel.y, frame.dialogStandBy.Bottom());
}

// ---------------------------------------------------------------------------
// Hit testing
// ---------------------------------------------------------------------------

TEST(HitTest, ClickingATilePicksTheRightUser) {
    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(ListInput(4));

    for (const TileLayout& tile : frame.tiles) {
        const HitResult hit = XpLayout::HitTest(frame, Center(tile.bounds));
        CHECK_EQ(static_cast<int>(hit.target), static_cast<int>(HitTarget::UserTile));
        CHECK_EQ(hit.index, tile.userIndex);
    }
}

TEST(HitTest, ClickingEmptySpaceHitsNothing) {
    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(ListInput(2));

    const HitResult hit = XpLayout::HitTest(frame, Point{5, 400});
    CHECK_EQ(static_cast<int>(hit.target), static_cast<int>(HitTarget::None));
    CHECK_EQ(hit.index, -1);
}

TEST(HitTest, TurnOffButtonAndItsLabelBothRespond) {
    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(ListInput());

    CHECK_EQ(static_cast<int>(XpLayout::HitTest(frame, Center(frame.powerButton)).target),
             static_cast<int>(HitTarget::TurnOffButton));
    CHECK_EQ(static_cast<int>(XpLayout::HitTest(frame, Center(frame.powerLabel)).target),
             static_cast<int>(HitTarget::TurnOffButton));
}

TEST(HitTest, PasswordControlsAreDistinguishable) {
    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(PasswordInput());

    CHECK_EQ(static_cast<int>(XpLayout::HitTest(frame, Center(frame.passwordBox)).target),
             static_cast<int>(HitTarget::PasswordBox));
    CHECK_EQ(static_cast<int>(XpLayout::HitTest(frame, Center(frame.goButton)).target),
             static_cast<int>(HitTarget::GoButton));
    CHECK_EQ(static_cast<int>(XpLayout::HitTest(frame, Center(frame.hintButton)).target),
             static_cast<int>(HitTarget::HintButton));
    CHECK_EQ(static_cast<int>(XpLayout::HitTest(frame, Center(frame.backButton)).target),
             static_cast<int>(HitTarget::BackButton));
}

TEST(HitTest, DialogIsModalAndSwallowsBackgroundClicks) {
    XpLayout layout(XpTheme::Default());
    LayoutInput input = ListInput();
    input.state = UiState::TurnOffDialog;
    const FrameLayout frame = layout.Compute(input);

    CHECK_EQ(
        static_cast<int>(XpLayout::HitTest(frame, Center(frame.dialogTurnOff)).target),
        static_cast<int>(HitTarget::DialogTurnOff));
    CHECK_EQ(
        static_cast<int>(XpLayout::HitTest(frame, Center(frame.dialogRestart)).target),
        static_cast<int>(HitTarget::DialogRestart));
    CHECK_EQ(
        static_cast<int>(XpLayout::HitTest(frame, Center(frame.dialogCancel)).target),
        static_cast<int>(HitTarget::DialogCancel));

    // A click far from the dialog lands on the backdrop, never on a tile.
    const HitResult stray = XpLayout::HitTest(frame, Point{10, 10});
    CHECK_EQ(static_cast<int>(stray.target), static_cast<int>(HitTarget::DialogBackdrop));
}

TEST(HitTest, EdgesBelongToTheControlAndOneOutsideDoesNot) {
    XpLayout layout(XpTheme::Default());
    const FrameLayout frame = layout.Compute(PasswordInput());
    const Rect& box = frame.passwordBox;

    CHECK_EQ(static_cast<int>(XpLayout::HitTest(frame, Point{box.x, box.y}).target),
             static_cast<int>(HitTarget::PasswordBox));
    CHECK_EQ(static_cast<int>(
                 XpLayout::HitTest(frame, Point{box.Right() - 1, box.Bottom() - 1})
                     .target),
             static_cast<int>(HitTarget::PasswordBox));
    CHECK_NE(static_cast<int>(
                 XpLayout::HitTest(frame, Point{box.Right(), box.Bottom()}).target),
             static_cast<int>(HitTarget::PasswordBox));
}

// ---------------------------------------------------------------------------
// Scrolling
// ---------------------------------------------------------------------------

TEST(Layout, AccountListIsCentredVertically) {
    // XP uses a verticalflowlayout for the account list, so a machine with one
    // account shows its tile halfway down the screen rather than tucked under
    // the header. Getting this wrong was very visible against the reference
    // screenshot.
    XpLayout layout(XpTheme::Default());
    const XpMetrics& m = XpTheme::Default().metrics;

    LayoutInput one = ListInput(1);
    const FrameLayout single = layout.Compute(one);
    REQUIRE_EQ(single.tiles.size(), size_t(1));

    const int contentTop = m.topBandHeight;
    const int contentBottom = m.bottomBandTop;
    const int tileCentre =
        single.tiles[0].bounds.y + single.tiles[0].bounds.height / 2;
    CHECK_NEAR(tileCentre, (contentTop + contentBottom) / 2, 3);

    // More accounts grow the block symmetrically about the same centre line.
    const FrameLayout three = layout.Compute(ListInput(3));
    REQUIRE_EQ(three.tiles.size(), size_t(3));
    const int blockCentre =
        (three.tiles.front().bounds.y + three.tiles.back().bounds.Bottom()) / 2;
    CHECK_NEAR(blockCentre, (contentTop + contentBottom) / 2, 3);
}

TEST(Layout, LongUserListScrolls) {
    XpLayout layout(XpTheme::Default());
    LayoutInput input = ListInput(30);

    const int visible = layout.VisibleTileCount(input);
    CHECK_GT(visible, 0);
    CHECK_LT(visible, 30);

    const FrameLayout frame = layout.Compute(input);
    CHECK_EQ(frame.tiles.size(), static_cast<size_t>(visible));
}

TEST(Layout, ScrollOffsetIsClampedToTheList) {
    XpLayout layout(XpTheme::Default());
    LayoutInput input = ListInput(30);
    const int visible = layout.VisibleTileCount(input);

    CHECK_EQ(layout.ClampScroll(input, -10), 0);
    CHECK_EQ(layout.ClampScroll(input, 1000), 30 - visible);
}

TEST(Layout, ScrollFollowsTheSelectedTile) {
    XpLayout layout(XpTheme::Default());
    LayoutInput input = ListInput(30);
    const int visible = layout.VisibleTileCount(input);

    input.selectedUser = 25;
    const int offset = layout.ClampScroll(input, 0);
    CHECK_LE(offset, 25);
    CHECK_GT(offset + visible, 25);

    input.selectedUser = 0;
    CHECK_EQ(layout.ClampScroll(input, 20), 0);
}

TEST(Layout, ScrolledListShowsTheRightWindowOfTiles) {
    XpLayout layout(XpTheme::Default());
    LayoutInput input = ListInput(30);
    input.scrollOffset = 5;
    const FrameLayout frame = layout.Compute(input);

    REQUIRE_FALSE(frame.tiles.empty());
    CHECK_EQ(frame.tiles.front().userIndex, 5);
    CHECK_EQ(frame.tiles.back().userIndex,
             5 + static_cast<int>(frame.tiles.size()) - 1);
}

TEST(Layout, ShortListNeedsNoScrolling) {
    XpLayout layout(XpTheme::Default());
    LayoutInput input = ListInput(3);
    CHECK_EQ(layout.ClampScroll(input, 5), 0);
}

// ---------------------------------------------------------------------------
// Animation
// ---------------------------------------------------------------------------

TEST(Animation, EaseOutCubicHitsBothEndpoints) {
    CHECK_NEAR(XpLayout::EaseOutCubic(0.0f), 0.0f, 1e-6);
    CHECK_NEAR(XpLayout::EaseOutCubic(1.0f), 1.0f, 1e-6);
    CHECK_NEAR(XpLayout::EaseOutCubic(-1.0f), 0.0f, 1e-6);
    CHECK_NEAR(XpLayout::EaseOutCubic(2.0f), 1.0f, 1e-6);
}

TEST(Animation, EaseOutCubicIsFastFirstThenSlow) {
    // The defining property: at the halfway point it is already 7/8 done.
    CHECK_NEAR(XpLayout::EaseOutCubic(0.5f), 0.875f, 1e-5);
    CHECK_GT(XpLayout::EaseOutCubic(0.25f), 0.25f);
    // Monotonic throughout.
    float previous = -1.0f;
    for (int i = 0; i <= 20; ++i) {
        const float value = XpLayout::EaseOutCubic(static_cast<float>(i) / 20.0f);
        CHECK_GT(value, previous);
        previous = value;
    }
}

TEST(Animation, SlideProgressIsLinearInTimeAndClamps) {
    CHECK_NEAR(XpLayout::SlideProgress(0, 220), 0.0f, 1e-6);
    CHECK_NEAR(XpLayout::SlideProgress(110, 220), 0.5f, 1e-5);
    CHECK_NEAR(XpLayout::SlideProgress(220, 220), 1.0f, 1e-6);
    CHECK_NEAR(XpLayout::SlideProgress(9999, 220), 1.0f, 1e-6);
}

TEST(Animation, ZeroDurationSnapsStraightToTheEnd) {
    // Animations disabled in config must not divide by zero.
    CHECK_NEAR(XpLayout::SlideProgress(0, 0), 1.0f, 1e-6);
}

// ---------------------------------------------------------------------------
// The account picture sits in its plate's well.
//
// The plate (#113/#119) is 58x58 with a 5px border and a 48x48 opaque
// interior, drawn 9-sliced. Scaling the photo's rect independently of the
// plate's is what put a lopsided frame around every avatar on an 800x600
// screen: two roundings of the same number that do not have to agree.
// ---------------------------------------------------------------------------

TEST(Layout, TheAccountPictureLandsInsideItsPlateAtEveryResolution) {
    const struct { int w, h; } screens[] = {
        {800, 600}, {1024, 768}, {1280, 1024}, {1366, 768}, {1920, 1080},
    };

    for (const auto& screen : screens) {
        XpLayout layout(XpTheme::Default());
        LayoutInput input;
        input.screenWidth = screen.w;
        input.screenHeight = screen.h;
        input.userCount = 2;
        input.state = UiState::UserList;

        const FrameLayout frame = layout.Compute(input);
        REQUIRE(!frame.tiles.empty());

        for (const TileLayout& tile : frame.tiles) {
            const Rect& plate = tile.avatar;
            const Rect& picture = tile.avatarPicture;

            CHECK(picture.x >= plate.x);
            CHECK(picture.y >= plate.y);
            CHECK(picture.Right() <= plate.Right());
            CHECK(picture.Bottom() <= plate.Bottom());

            // Centred: the border is the same width on both sides, which is
            // what "lopsided" meant when it was not.
            CHECK_EQ(picture.x - plate.x, plate.Right() - picture.Right());
            CHECK_EQ(picture.y - plate.y, plate.Bottom() - picture.Bottom());

            // And there is still a picture left to draw.
            CHECK(picture.width > 0);
            CHECK(picture.height > 0);
        }
    }
}

TEST(Layout, SigningInKeepsTheChosenTileRatherThanTheList) {
    // XP does not snap back to the account list while a password is being
    // typed or checked: the chosen tile stays and the rest go.
    //
    // LoggedOn is deliberately not in this list. Leaving the password view up
    // after LSA accepted the credentials made the screen look frozen - the box
    // had cleared itself, nothing responded to a key, and Winlogon can take
    // most of a minute to build the session. That state is the welcome screen
    // now; see TheWelcomeScreenReplacesTheTileOnceTheLogonIsAccepted.
    for (UiState state : {UiState::PasswordEntry, UiState::Authenticating,
                          UiState::AuthFailed}) {
        XpLayout layout(XpTheme::Default());
        LayoutInput input;
        input.screenWidth = 1024;
        input.screenHeight = 768;
        input.userCount = 3;
        input.selectedUser = 1;
        input.state = state;

        const FrameLayout frame = layout.Compute(input);
        CHECK(frame.passwordViewActive);
        CHECK(frame.instruction.IsEmpty());
    }
}

// ---------------------------------------------------------------------------
// The "Turn off computer" modal.
//
// Not the welcome screen's geometry: this dialog belongs to msgina.dll, which
// lays it out with a Win32 dialog template (#20100, 208x122 dialog units) and
// draws it on a 313x198 background bitmap. Those two numbers together fix the
// conversion - 208 x 6/4 = 312 and 122 x 13/8 = 198.25, the standard 8pt
// MS Shell Dlg base units - and every rectangle here is a control from that
// template put through it. Before this the dialog was invented: a 480x190
// panel with 48px orbs 60px apart, which is why it did not look like XP's.
// ---------------------------------------------------------------------------

TEST(Layout, TheTurnOffDialogMatchesMsginasTemplate) {
    XpLayout layout(XpTheme::Default());
    LayoutInput input;
    input.screenWidth = 1024;
    input.screenHeight = 768;
    input.userCount = 2;
    input.state = UiState::TurnOffDialog;

    const FrameLayout frame = layout.Compute(input);
    REQUIRE(!frame.dialog.IsEmpty());

    // At 1:1 the dialog is exactly its background bitmap.
    CHECK_EQ(frame.dialog.width, 313);
    CHECK_EQ(frame.dialog.height, 198);
    CHECK_EQ(frame.dialog.x, (1024 - 313) / 2);
    CHECK_EQ(frame.dialog.y, (768 - 198) / 2);

    const int ox = frame.dialog.x;
    const int oy = frame.dialog.y;

    // [DIALOG] the three owner-drawn buttons at 36, 93 and 150du, y=49du.
    // Left to right on screen: Stand By, Turn Off, Restart.
    CHECK_EQ(frame.dialogStandBy.x, ox + 54);
    CHECK_EQ(frame.dialogTurnOff.x, ox + 140);
    CHECK_EQ(frame.dialogRestart.x, ox + 226);
    CHECK_EQ(frame.dialogStandBy.y, oy + 80);
    CHECK_EQ(frame.dialogStandBy.width, 32);   // the orb frames are 32x32
    CHECK_EQ(frame.dialogStandBy.height, 32);

    // Evenly pitched, and in the right order.
    CHECK_EQ(frame.dialogTurnOff.x - frame.dialogStandBy.x,
             frame.dialogRestart.x - frame.dialogTurnOff.x);
    CHECK_LT(frame.dialogStandBy.x, frame.dialogTurnOff.x);
    CHECK_LT(frame.dialogTurnOff.x, frame.dialogRestart.x);

    // The orbs and their labels sit in the blue body of the bitmap (y 45..154),
    // the title in the navy band above it (0..42), Cancel in the one below
    // (156..197). Getting any of these outside its band is immediately visible.
    CHECK_GE(frame.dialogStandBy.y - oy, 45);
    CHECK_LE(frame.dialogStandBy.Bottom() - oy, 154);
    CHECK_GE(frame.dialogStandByLabel.y - oy, 45);
    CHECK_LE(frame.dialogStandByLabel.Bottom() - oy, 154);
    CHECK_LE(frame.dialogTitle.Bottom() - oy, 43);
    CHECK_GE(frame.dialogFlag.y - oy, 0);
    CHECK_LE(frame.dialogFlag.Bottom() - oy, 43);
    CHECK_GE(frame.dialogCancel.y - oy, 156);
    CHECK_LE(frame.dialogCancel.Bottom() - oy, 198);

    // Each label is centred under its orb.
    struct Pair { const Rect& orb; const Rect& label; };
    const Pair pairs[] = {
        {frame.dialogStandBy, frame.dialogStandByLabel},
        {frame.dialogTurnOff, frame.dialogTurnOffLabel},
        {frame.dialogRestart, frame.dialogRestartLabel},
    };
    for (const Pair& pair : pairs) {
        CHECK_EQ(pair.orb.x + pair.orb.width / 2,
                 pair.label.x + pair.label.width / 2);
    }
}

TEST(Layout, EveryTurnOffControlStaysInsideTheDialog) {
    // The dialog is a fixed-size bitmap and its contents are placed relative to
    // it, so a control escaping it means the two have come apart.
    for (const auto& screen : {std::pair<int,int>{800,600}, {1024,768}, {1920,1080}}) {
        XpLayout layout(XpTheme::Default());
        LayoutInput input;
        input.screenWidth = screen.first;
        input.screenHeight = screen.second;
        input.userCount = 1;
        input.state = UiState::TurnOffDialog;

        const FrameLayout frame = layout.Compute(input);
        const Rect& d = frame.dialog;
        for (const Rect* r : {&frame.dialogTitle, &frame.dialogFlag,
                              &frame.dialogStandBy, &frame.dialogTurnOff,
                              &frame.dialogRestart, &frame.dialogStandByLabel,
                              &frame.dialogTurnOffLabel, &frame.dialogRestartLabel,
                              &frame.dialogCancel}) {
            CHECK_GE(r->x, d.x);
            CHECK_GE(r->y, d.y);
            CHECK_LE(r->Right(), d.Right());
            CHECK_LE(r->Bottom(), d.Bottom());
        }
    }
}

// ---------------------------------------------------------------------------
// Logging off and shutting down.
//
// Not separate screens in XP: this one, with the account list gone and a
// single large italic line where the Windows logo would be. [UIFILE] msgarea
// and logoarea are both children of leftpanel under a filllayout, so they
// share one box and only one of them is ever visible.
// ---------------------------------------------------------------------------

TEST(Layout, TheShutdownScreenIsBareExceptForItsMessage) {
    XpLayout layout(XpTheme::Default());
    LayoutInput input;
    input.screenWidth = 1024;
    input.screenHeight = 768;
    input.userCount = 3;
    input.selectedUser = 1;
    input.state = UiState::PowerActionPending;

    const FrameLayout frame = layout.Compute(input);

    CHECK(!frame.shellStatus.IsEmpty());
    CHECK(!frame.shellStatusShadow.IsEmpty());

    // Nothing else in the middle of the screen.
    CHECK(frame.tiles.empty());
    CHECK(frame.logo.IsEmpty());
    CHECK(frame.instruction.IsEmpty());
    CHECK(frame.centerDivider.IsEmpty());
    CHECK(frame.passwordBox.IsEmpty());

    // Including the dialog the choice was made in. This test claimed the screen
    // was bare and never checked the one thing on it that was not: the turn-off
    // modal was laid out for this state too, so every sign-out and every
    // shutdown ended on the XP screen with three buttons offering choices that
    // had already been made and could no longer be taken.
    CHECK(frame.dialog.IsEmpty());
    CHECK(frame.dialogBackdrop.IsEmpty());
    CHECK(frame.dialogTurnOff.IsEmpty());
    CHECK(frame.dialogStandBy.IsEmpty());
    CHECK(frame.dialogRestart.IsEmpty());
    CHECK(frame.dialogCancel.IsEmpty());

    // The bands stay - XP's shutdown screen keeps its chrome.
    CHECK(!frame.headerBand.IsEmpty());
    CHECK(!frame.footerBand.IsEmpty());
    CHECK(!frame.footerAccent.IsEmpty());

    // The message occupies the box the logo would have: same top, and hanging
    // off a right edge 22rp short of the centre rule where the logo's is 20rp.
    CHECK_EQ(frame.shellStatus.y, frame.screen.y + 300);
    CHECK_EQ(frame.shellStatus.Right(), 490);

    // [UIFILE] welcomeshadow is padded 2rp right and 3rp down of welcome, and
    // that offset is the entire drop shadow.
    CHECK_EQ(frame.shellStatusShadow.x - frame.shellStatus.x, 2);
    CHECK_EQ(frame.shellStatusShadow.y - frame.shellStatus.y, 3);
}

TEST(Layout, TheWelcomeScreenReplacesTheTileOnceTheLogonIsAccepted) {
    // The gap between "LSA said yes" and "the desktop appears" is Winlogon
    // building the session, and it is long enough to look like a hang. XP
    // fills it with rcstr(7) - "welcome" - in the box the logo was in, and
    // that is what msgarea exists for.
    XpLayout layout(XpTheme::Default());
    LayoutInput input;
    input.screenWidth = 1024;
    input.screenHeight = 768;
    input.userCount = 3;
    input.selectedUser = 1;
    input.state = UiState::LoggedOn;

    const FrameLayout frame = layout.Compute(input);
    CHECK(!frame.shellStatus.IsEmpty());
    CHECK(frame.tiles.empty());
    CHECK(frame.passwordBox.IsEmpty());
    CHECK_FALSE(frame.passwordViewActive);
}

TEST(Layout, TheLogonScreenNeverShowsTheShutdownMessage) {
    for (UiState state : {UiState::UserList, UiState::PasswordEntry,
                          UiState::Authenticating, UiState::AuthFailed,
                          UiState::TurnOffDialog}) {
        XpLayout layout(XpTheme::Default());
        LayoutInput input;
        input.screenWidth = 1024;
        input.screenHeight = 768;
        input.userCount = 2;
        input.state = state;

        const FrameLayout frame = layout.Compute(input);
        CHECK(frame.shellStatus.IsEmpty());
        CHECK(frame.shellStatusShadow.IsEmpty());
    }
}

// A machine that signs itself in gets the busy screen from the first frame:
// XP's welcome message where the logo would be, and no account list at all.
// A one-tile list that flashes past before anybody could click it is worse
// than no list.
TEST(Layout, AMachineThatSignsItselfInShowsTheMessageNotTheList) {
    XpLayout layout(XpTheme::Default());
    LayoutInput input;
    input.screenWidth = 1024;
    input.screenHeight = 768;
    input.userCount = 1;
    input.selectedUser = -1;
    input.state = UiState::UserList;   // nothing has been submitted yet
    input.signingInAutomatically = true;

    const FrameLayout frame = layout.Compute(input);

    CHECK(!frame.shellStatus.IsEmpty());
    CHECK(frame.tiles.empty());
    CHECK(frame.logo.IsEmpty());
    CHECK(frame.instruction.IsEmpty());
    CHECK(frame.passwordBox.IsEmpty());
    // The chrome stays: it is still the XP screen.
    CHECK(!frame.headerBand.IsEmpty());
    CHECK(!frame.footerBand.IsEmpty());
}

// And the same screen without the flag is the ordinary one-account list, so
// the test above is not passing for some other reason.
TEST(Layout, TheSameScreenWithoutTheFlagIsAnOrdinaryList) {
    XpLayout layout(XpTheme::Default());
    LayoutInput input;
    input.screenWidth = 1024;
    input.screenHeight = 768;
    input.userCount = 1;
    input.selectedUser = -1;
    input.state = UiState::UserList;

    const FrameLayout frame = layout.Compute(input);

    CHECK(frame.shellStatus.IsEmpty());
    CHECK_EQ(frame.tiles.size(), size_t(1));
    CHECK(!frame.logo.IsEmpty());
}
