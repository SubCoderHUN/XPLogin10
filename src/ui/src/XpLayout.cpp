#include "xplogin/ui/XpLayout.h"

#include <algorithm>
#include <cmath>

namespace xplogin::ui {
namespace {

int Lerp(int a, int b, float t) {
    return static_cast<int>(std::lround(a + (b - a) * static_cast<double>(t)));
}

} // namespace

XpLayout::XpLayout(XpTheme theme) : theme_(std::move(theme)) {}

float XpLayout::EaseOutCubic(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    const float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

float XpLayout::SlideProgress(uint64_t elapsedMs, uint64_t durationMs) {
    if (durationMs == 0) {
        return 1.0f;
    }
    if (elapsedMs >= durationMs) {
        return 1.0f;
    }
    return static_cast<float>(static_cast<double>(elapsedMs) /
                              static_cast<double>(durationMs));
}

Rect XpLayout::Scale(int x, int y, int w, int h, float scale, int offsetX,
                     int offsetY) const {
    Rect r;
    r.x = offsetX + static_cast<int>(std::lround(x * static_cast<double>(scale)));
    r.y = offsetY + static_cast<int>(std::lround(y * static_cast<double>(scale)));
    r.width = static_cast<int>(std::lround(w * static_cast<double>(scale)));
    r.height = static_cast<int>(std::lround(h * static_cast<double>(scale)));
    return r;
}

int XpLayout::VisibleTileCount(const LayoutInput& input) const {
    const XpMetrics& m = theme_.metrics;
    // The account list is a scrollviewer filling the right column between the
    // two bands, so the whole centre panel is available to it - not some
    // arbitrary slice starting part way down.
    const int available = m.bottomBandTop - m.topBandHeight;
    const int step = m.tileHeight + m.tileSpacing;
    if (step <= 0) {
        return 1;
    }
    (void)input;
    return std::max(1, available / step);
}

int XpLayout::TileColumnTop(const LayoutInput& input) const {
    // XP lays the accounts out with a verticalflowlayout that centres them in
    // the right column, which is why a machine with one account shows its tile
    // halfway down the screen rather than tucked under the header.
    const XpMetrics& m = theme_.metrics;
    const int available = m.bottomBandTop - m.topBandHeight;
    const int step = m.tileHeight + m.tileSpacing;
    const int shown =
        std::max(1, std::min(input.userCount, VisibleTileCount(input)));
    const int blockHeight = shown * step - m.tileSpacing;
    return m.topBandHeight + std::max(0, (available - blockHeight) / 2);
}

int XpLayout::ClampScroll(const LayoutInput& input, int desiredOffset) const {
    const int visible = VisibleTileCount(input);
    const int maxOffset = std::max(0, input.userCount - visible);
    int offset = std::max(0, std::min(desiredOffset, maxOffset));

    if (input.selectedUser >= 0) {
        if (input.selectedUser < offset) {
            offset = input.selectedUser;
        } else if (input.selectedUser >= offset + visible) {
            offset = input.selectedUser - visible + 1;
        }
    }
    return std::max(0, std::min(offset, maxOffset));
}

FrameLayout XpLayout::Compute(const LayoutInput& input) const {
    const XpMetrics& m = theme_.metrics;
    FrameLayout frame;

    frame.scale = XpTheme::ScaleFor(input.screenWidth, input.screenHeight);
    frame.screen = Rect{0, 0, input.screenWidth, input.screenHeight};

    // XP's canvas is 4:3. On a wider screen the bands still span the full
    // width, but the content block is centred on the design canvas.
    const float scale = frame.scale;
    const int contentWidth = static_cast<int>(std::lround(kDesignWidth * scale));
    const int contentHeight = static_cast<int>(std::lround(kDesignHeight * scale));
    const int offsetX = (input.screenWidth - contentWidth) / 2;
    const int offsetY = (input.screenHeight - contentHeight) / 2;

    auto S = [&](int x, int y, int w, int h) {
        return Scale(x, y, w, h, scale, offsetX, offsetY);
    };

    // Bands run edge to edge so nothing shows through on a 16:9 panel.
    const int headerBottom = S(0, m.topBandHeight, 0, 0).y;
    const int footerTop = S(0, m.bottomBandTop, 0, 0).y;

    frame.headerBand = Rect{0, 0, input.screenWidth, headerBottom};
    frame.centerPanel = Rect{0, headerBottom, input.screenWidth, footerTop - headerBottom};
    frame.footerBand =
        Rect{0, footerTop, input.screenWidth, input.screenHeight - footerTop};

    const int dividerThickness =
        std::max(1, static_cast<int>(std::lround(m.dividerThickness * scale)));
    frame.headerDivider =
        Rect{0, headerBottom - dividerThickness, input.screenWidth, dividerThickness};

    // XP's most recognisable single line: an orange rule along the top edge of
    // the footer, with the dark band underneath it.
    const int accentHeight =
        std::max(1, static_cast<int>(std::lround(m.footerAccentHeight * scale)));
    frame.footerAccent = Rect{0, footerTop, input.screenWidth, accentHeight};
    frame.footerDivider = frame.footerAccent;

    // Everything in the bottom band hangs off one padding box, so the contents
    // follow the band wherever it goes instead of being pinned to absolute Y
    // values that quietly stop matching it.
    const int optionsTop =
        m.bottomBandTop + m.dividerThickness + m.footerPaddingTop;

    frame.powerButton = S(m.footerPaddingX, optionsTop, m.powerButtonSize,
                          m.powerButtonSize);
    // The label box is exactly as tall as the icon; the renderer centres the
    // text in it, which is what lines the two up.
    frame.powerLabel = S(m.footerPaddingX + m.powerButtonSize + m.powerLabelGap,
                         optionsTop, m.powerLabelWidth, m.powerButtonSize);

    frame.hintFooter = S(kDesignWidth - m.footerPaddingX - m.hintTextWidth,
                         optionsTop, m.hintTextWidth, m.hintTextHeight);

    // "Other ways to sign in", on the same row as the turn-off label and well
    // clear of it. Only present when the machine actually has another provider
    // to fall through to, so on a plain local-password machine the footer looks
    // exactly like XP's. XP stacked a second row here for "Undock computer",
    // which would be the more authentic place - but that grows the band, and a
    // taller band would move every other measurement on the screen.
    if (input.showSigninOptions) {
        frame.signinOptions =
            S(m.footerPaddingX + m.powerButtonSize + m.powerLabelGap +
                  m.powerLabelWidth + 20,
              optionsTop, m.signinOptionsWidth, m.powerButtonSize);
    }

    frame.passwordViewActive =
        (input.state == UiState::PasswordEntry ||
         input.state == UiState::Authenticating ||
         input.state == UiState::AuthFailed);

    // ---- the busy screen ---------------------------------------------------
    //
    // Logging off and shutting down are not separate screens in XP: they are
    // this one with the account list gone and a single large italic line where
    // the logo would be. [UIFILE] msgarea and logoarea are both children of
    // leftpanel under a filllayout - one box, one of them visible - so when
    // the message is up the logo is not, and neither are the tiles.
    // Or a machine that signs itself in: there is no list to show, and a
    // one-tile list that flashes past before anybody could click it is worse
    // than no list. XP showed the message for the whole of it.
    const bool busy =
        ShowsShellStatus(input.state) || input.signingInAutomatically;
    if (busy) {
        frame.shellStatus = S(m.shellStatusX, m.shellStatusY, m.shellStatusWidth,
                              m.shellStatusHeight);
        frame.shellStatusShadow =
            S(m.shellStatusX + m.shellStatusShadowDx,
              m.shellStatusY + m.shellStatusShadowDy, m.shellStatusWidth,
              m.shellStatusHeight);
    }

    // The vertical rule only exists in the list view; selecting a user made XP
    // fade it out along with the instruction text.
    if (!frame.passwordViewActive && !busy) {
        frame.centerDivider =
            S(m.centerDividerX, m.centerDividerTop, m.centerDividerWidth,
              m.centerDividerBottom - m.centerDividerTop);
        frame.instruction =
            S(m.instructionX, m.instructionY, m.instructionWidth, 60);

        // The logo lives in the left column directly above the instruction
        // text, not in the header band. The flag sits above and to the right
        // of the wordmark, which is the XP lockup.
        frame.logo = S(m.logoX, m.logoY, m.logoWidth, m.logoHeight);
        frame.logoFlag = S(m.logoX + m.logoFlagInset, m.logoY, m.logoFlagSize,
                           m.logoFlagSize);
        frame.logoWordmark =
            S(m.logoX, m.logoY + m.logoFlagSize - 4, m.logoWidth,
              m.logoHeight - m.logoFlagSize + 4);
    }

    // ---- tiles -------------------------------------------------------------
    const int visible = VisibleTileCount(input);
    const int scrollOffset = ClampScroll(input, input.scrollOffset);
    const int columnTop = TileColumnTop(input);
    const float progress = frame.passwordViewActive
                               ? EaseOutCubic(std::clamp(input.slideProgress, 0.0f, 1.0f))
                               : 0.0f;

    // [UIFILE] The account list has two style sheets. At rest it uses
    // accountlistss, where every tile is solid. As soon as the pointer is over
    // the list - or an account has been picked - XP swaps in hotaccountlistss,
    // whose base alpha is 96 with 255 for [mousewithin] and [selected]. The
    // effect is that singling out one account visibly pushes the others back.
    const bool listIsHot = input.hoveredTile >= 0 || input.selectedUser >= 0;
    const float dimmedOpacity =
        std::clamp(m.dimmedTileAlpha / 255.0f, 0.0f, 1.0f);

    // No accounts at all while the status line is up: XP's logoff and shutdown
    // screens are the bands, the background and one line of text.
    const int tileCount = busy ? 0 : input.userCount;
    for (int i = 0; i < tileCount; ++i) {
        const bool isSelected = (i == input.selectedUser);

        // In the password view only the selected tile remains.
        if (frame.passwordViewActive && !isSelected) {
            continue;
        }
        if (!frame.passwordViewActive) {
            if (i < scrollOffset || i >= scrollOffset + visible) {
                continue;
            }
        }

        const int slot = i - scrollOffset;
        const int listX = m.tileColumnX;
        const int listY = columnTop + slot * (m.tileHeight + m.tileSpacing);

        int designX = listX;
        int designY = listY;
        if (isSelected && progress > 0.0f) {
            designX = Lerp(listX, m.selectedTileX, progress);
            designY = Lerp(listY, m.selectedTileY, progress);
        }

        TileLayout tile;
        tile.userIndex = i;
        tile.selected = isSelected;
        const bool standsOut = isSelected || i == input.hoveredTile;
        tile.opacity = (!listIsHot || standsOut) ? 1.0f : dimmedOpacity;
        tile.bounds = S(designX, designY, m.tileWidth, m.tileHeight);
        const int avatarX = designX + m.avatarPadding;
        const int avatarY = designY + (m.tileHeight - m.avatarSize) / 2;
        tile.avatar = S(avatarX, avatarY, m.avatarSize, m.avatarSize);
        // The photo goes in the plate's well, and the well is derived from the
        // *already scaled* plate rather than scaled on its own.
        //
        // Scaling it independently is what put a lopsided frame around every
        // account picture. At 800x600 the scale is 0.78125, and the two
        // roundings disagree: the plate becomes lround(58*0.78125) = 45 with
        // 9-sliced borders of lround(5*0.78125) = 4, leaving a 37px well,
        // while the photo becomes lround(48*0.78125) = 38 - one pixel wider,
        // and its origin rounds independently again. The photo ends up over
        // the frame on the right and bottom and short of it on the left and
        // top, which reads as a border on two sides only.
        //
        // Deriving it makes the two agree by construction at any scale.
        const int inset =
            static_cast<int>(std::lround(m.avatarPictureInset * static_cast<double>(scale)));
        tile.avatarPicture = Rect{tile.avatar.x + inset, tile.avatar.y + inset,
                                  tile.avatar.width - 2 * inset,
                                  tile.avatar.height - 2 * inset};

        const int textX = designX + m.avatarPadding + m.avatarSize + m.avatarTextGap;
        // [UIFILE] userpane padding right is 14rp.
        const int textWidth = m.tileWidth - (textX - designX) - 14;
        tile.nameText = S(textX, designY + 12, textWidth, 22);
        tile.statusText = S(textX, designY + 34, textWidth, 18);

        frame.tiles.push_back(tile);
    }

    // Scroll affordances, only when the list is longer than the column.
    if (!frame.passwordViewActive && input.userCount > visible) {
        // Rendered as the small XP chevrons above and below the column.
        frame.dialogBackdrop = Rect{}; // unused here, keeps the struct honest
    }

    // ---- password view -----------------------------------------------------
    if (frame.passwordViewActive) {
        frame.passwordBox = S(m.passwordBoxX, m.passwordBoxY, m.passwordBoxWidth,
                              m.passwordBoxHeight);
        frame.goButton = S(m.passwordBoxX + m.passwordBoxWidth + m.goButtonGap,
                           m.passwordBoxY - 2, m.goButtonSize, m.goButtonSize);
        if (input.showHintButton) {
            frame.hintButton =
                S(m.passwordBoxX + m.passwordBoxWidth + m.goButtonGap +
                      m.goButtonSize + m.hintButtonGap,
                  m.passwordBoxY - 2, m.hintButtonSize, m.hintButtonSize);
        }
        if (input.showBackButton) {
            frame.backButton = S(m.passwordBoxX - 34, m.passwordBoxY - 2, 26, 26);
        }
        frame.errorText = S(m.passwordBoxX - 40, m.passwordBoxY + 40, 400, 60);
    }

    // ---- turn off dialog ---------------------------------------------------
    //
    // TurnOffDialog only. PowerActionPending used to be here too, and that one
    // line is what every report of this screen has actually been describing:
    //
    //   "our screen with the shutdown menu open on it, and the logging off
    //    text behind it"
    //
    // PowerActionPending means the choice has been made and the machine is on
    // its way down. XP took the dialog away at that moment and put "Windows is
    // shutting down..." in its place; leaving it up means the last thing on the
    // screen is a modal offering three buttons that no longer do anything. It
    // was drawn by the sign-in window after the click, and by the shutdown host
    // - which renders this state and nothing else - for the whole of every
    // sign-out and shutdown.
    if (input.state == UiState::TurnOffDialog) {
        frame.dialogBackdrop = frame.screen;

        // Everything here is msgina's dialog template resolved into the
        // background bitmap's own 313x198 pixel grid, then offset to wherever
        // the dialog is centred. Laid out relative to the dialog rather than
        // to the design canvas, because the dialog is a fixed-size bitmap and
        // its contents have to keep their places inside it.
        const int dialogX = (kDesignWidth - m.dialogWidth) / 2;
        const int dialogY = (kDesignHeight - m.dialogHeight) / 2;
        frame.dialog = S(dialogX, dialogY, m.dialogWidth, m.dialogHeight);

        auto D = [&](int x, int y, int w, int h) {
            return S(dialogX + x, dialogY + y, w, h);
        };

        frame.dialogTitle =
            D(m.dialogTitleX, 0, m.dialogTitleWidth, m.dialogTitleHeight);
        frame.dialogFlag =
            D(m.dialogFlagX, 0, m.dialogFlagWidth, m.dialogFlagHeight);

        // [DIALOG] Stand By, Turn Off, Restart - in that order left to right,
        // which is not the order the orb strip stores them in.
        const int size = m.dialogButtonSize;
        frame.dialogStandBy = D(m.dialogButtonX, m.dialogButtonY, size, size);
        frame.dialogTurnOff =
            D(m.dialogButtonX + m.dialogButtonStep, m.dialogButtonY, size, size);
        frame.dialogRestart =
            D(m.dialogButtonX + 2 * m.dialogButtonStep, m.dialogButtonY, size, size);

        // The labels are wider than the orbs and centred under them.
        const int labelInset = (m.dialogLabelWidth - size) / 2;
        frame.dialogStandByLabel =
            D(m.dialogButtonX - labelInset, m.dialogLabelY, m.dialogLabelWidth,
              m.dialogLabelHeight);
        frame.dialogTurnOffLabel =
            D(m.dialogButtonX + m.dialogButtonStep - labelInset, m.dialogLabelY,
              m.dialogLabelWidth, m.dialogLabelHeight);
        frame.dialogRestartLabel =
            D(m.dialogButtonX + 2 * m.dialogButtonStep - labelInset, m.dialogLabelY,
              m.dialogLabelWidth, m.dialogLabelHeight);

        frame.dialogCancel = D(m.dialogCancelX, m.dialogCancelY,
                               m.dialogCancelWidth, m.dialogCancelHeight);
    }

    return frame;
}

HitResult XpLayout::HitTest(const FrameLayout& frame, Point point) {
    HitResult result;

    // The modal eats every click that is not on one of its controls.
    if (!frame.dialog.IsEmpty()) {
        if (frame.dialogStandBy.Contains(point)) {
            result.target = HitTarget::DialogStandBy;
            return result;
        }
        if (frame.dialogTurnOff.Contains(point)) {
            result.target = HitTarget::DialogTurnOff;
            return result;
        }
        if (frame.dialogRestart.Contains(point)) {
            result.target = HitTarget::DialogRestart;
            return result;
        }
        if (frame.dialogCancel.Contains(point)) {
            result.target = HitTarget::DialogCancel;
            return result;
        }
        result.target = HitTarget::DialogBackdrop;
        return result;
    }

    if (!frame.goButton.IsEmpty() && frame.goButton.Contains(point)) {
        result.target = HitTarget::GoButton;
        return result;
    }
    if (!frame.hintButton.IsEmpty() && frame.hintButton.Contains(point)) {
        result.target = HitTarget::HintButton;
        return result;
    }
    if (!frame.backButton.IsEmpty() && frame.backButton.Contains(point)) {
        result.target = HitTarget::BackButton;
        return result;
    }
    if (!frame.passwordBox.IsEmpty() && frame.passwordBox.Contains(point)) {
        result.target = HitTarget::PasswordBox;
        return result;
    }
    if (!frame.signinOptions.IsEmpty() && frame.signinOptions.Contains(point)) {
        return {HitTarget::SigninOptions, -1};
    }
    if (!frame.powerButton.IsEmpty() &&
        (frame.powerButton.Contains(point) || frame.powerLabel.Contains(point))) {
        result.target = HitTarget::TurnOffButton;
        return result;
    }

    for (const TileLayout& tile : frame.tiles) {
        if (tile.bounds.Contains(point)) {
            result.target = HitTarget::UserTile;
            result.index = tile.userIndex;
            return result;
        }
    }

    return result;
}

} // namespace xplogin::ui
