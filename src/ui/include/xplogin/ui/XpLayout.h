// XPLogin10 - where everything goes on screen.
//
// The renderer does no arithmetic of its own: it asks XpLayout for rectangles
// and draws into them. That keeps the geometry (including the tile slide
// animation and hit testing) in portable code the tests can drive at any
// resolution without opening a window.
#pragma once

#include "xplogin/LogonStateMachine.h"
#include "xplogin/Types.h"
#include "xplogin/ui/XpTheme.h"

#include <vector>

namespace xplogin::ui {

struct Point {
    int x = 0;
    int y = 0;
};

struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    int Right() const { return x + width; }
    int Bottom() const { return y + height; }
    bool IsEmpty() const { return width <= 0 || height <= 0; }

    bool Contains(Point p) const {
        return p.x >= x && p.x < Right() && p.y >= y && p.y < Bottom();
    }
    bool Intersects(const Rect& other) const {
        return !(other.x >= Right() || other.Right() <= x || other.y >= Bottom() ||
                 other.Bottom() <= y);
    }
};

enum class HitTarget {
    None = 0,
    UserTile,      // index carries which one
    PasswordBox,
    GoButton,
    HintButton,
    BackButton,
    TurnOffButton,
    SigninOptions,
    DialogStandBy,
    DialogTurnOff,
    DialogRestart,
    DialogCancel,
    DialogBackdrop,
    ScrollUp,
    ScrollDown,
};

struct HitResult {
    HitTarget target = HitTarget::None;
    int index = -1;
};

// One tile's geometry, already animated.
struct TileLayout {
    int   userIndex = -1;
    Rect  bounds;
    Rect  avatar;         // the 58x58 picture frame
    Rect  avatarPicture;  // the 48x48 well inside it, where the photo goes
    Rect  nameText;
    Rect  statusText;
    float opacity = 1.0f;
    bool  selected = false;
};

struct FrameLayout {
    Rect screen;
    Rect headerBand;
    Rect centerPanel;
    Rect footerBand;
    Rect headerDivider;
    Rect footerDivider;
    Rect footerAccent;    // the orange rule along the top of the footer
    Rect centerDivider;   // empty in the password view, XP hid it
    Rect logo;      // the whole logo lockup, in the centre column
    Rect logoFlag;  // the four-pane mark, above and right of the wordmark
    Rect logoWordmark;
    Rect instruction;
    Rect hintFooter;
    Rect powerButton;
    Rect powerLabel;
    Rect signinOptions;  // "other ways to sign in", empty when not offered

    std::vector<TileLayout> tiles;

    Rect passwordBox;
    Rect goButton;
    Rect hintButton;
    Rect backButton;
    Rect errorText;

    // The big italic line in the left column while the machine is busy, and
    // its drop shadow. Non-empty exactly when the account list is not drawn:
    // [UIFILE] msgarea and logoarea share one box under a filllayout, so XP's
    // logoff and shutdown screens have no logo and no tiles on them at all.
    Rect shellStatus;
    Rect shellStatusShadow;

    // Only populated in UiState::TurnOffDialog.
    Rect dialogBackdrop;
    Rect dialog;
    Rect dialogTitle;
    Rect dialogFlag;   // the Windows flag at the right of the title band
    Rect dialogStandBy;
    Rect dialogTurnOff;
    Rect dialogRestart;
    Rect dialogCancel;
    Rect dialogStandByLabel;
    Rect dialogTurnOffLabel;
    Rect dialogRestartLabel;

    float scale = 1.0f;
    bool  passwordViewActive = false;
};

// Inputs the layout needs beyond the theme.
struct LayoutInput {
    int   screenWidth = 1024;
    int   screenHeight = 768;
    UiState state = UiState::UserList;
    int   userCount = 0;
    int   selectedUser = -1;
    int   hoveredTile = -1;      // drives the dimming of everything else
    int   scrollOffset = 0;      // first visible tile in the list
    float slideProgress = 0.0f;  // 0 = list position, 1 = selected position
    bool  showHintButton = true;
    bool  showBackButton = true;
    // Whether to offer Windows' own sign-in tiles (PIN, Hello, smartcard).
    bool  showSigninOptions = false;
    bool  hibernateInsteadOfStandBy = false;
    // One account, no password: the machine signs itself in and there is
    // nothing to click. The screen is the busy one from the first frame, so
    // the account list never appears at all - see SignsInWithoutAsking.
    bool  signingInAutomatically = false;
};

class XpLayout {
public:
    explicit XpLayout(XpTheme theme);

    const XpTheme& Theme() const { return theme_; }
    void SetTheme(XpTheme theme) { theme_ = std::move(theme); }

    FrameLayout Compute(const LayoutInput& input) const;

    // Hit testing runs against a computed frame so it can never disagree with
    // what was painted.
    static HitResult HitTest(const FrameLayout& frame, Point point);

    // How many tiles fit in the column at this height.
    int VisibleTileCount(const LayoutInput& input) const;

    // Where the block of tiles starts. XP centres the account list vertically
    // in the right column, so this depends on how many accounts there are.
    int TileColumnTop(const LayoutInput& input) const;

    // Clamps a scroll offset so the selected tile stays on screen.
    int ClampScroll(const LayoutInput& input, int desiredOffset) const;

    // XP's tile slide used a smooth ease-out. Exposed for testing so the
    // animation cannot silently become linear.
    static float EaseOutCubic(float t);

    // Progress of the slide at time `elapsedMs`, clamped to [0,1].
    static float SlideProgress(uint64_t elapsedMs, uint64_t durationMs);

private:
    Rect Scale(int x, int y, int w, int h, float scale, int offsetX, int offsetY) const;

    XpTheme theme_;
};

} // namespace xplogin::ui
