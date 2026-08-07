// XPLogin10 - GDI+ painting of the XP welcome screen.
//
// The renderer owns no state beyond its resources: it is handed a FrameLayout
// and a description of what to draw, and it draws it. All geometry decisions
// were already made by XpLayout, which is what makes the screen testable.
//
// Everything is drawn with primitives rather than shipped bitmaps. That is a
// deliberate licensing decision (see NOTICE.md) and it also means the screen
// scales cleanly to any resolution. A deployment that owns the original assets
// can drop them into resources/assets/local and they will be used instead.
#pragma once

#include "xplogin/LogonStateMachine.h"
#include "xplogin/ui/win32/XpAssets.h"
#include "xplogin/Types.h"
#include "xplogin/ui/XpLayout.h"
#include "xplogin/ui/XpTheme.h"

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace Gdiplus {
class Graphics;
class Bitmap;
class Font;
class FontFamily;
} // namespace Gdiplus

namespace xplogin::ui::win32 {

// Everything that changes between frames but is not geometry.
struct RenderState {
    UiState      state = UiState::UserList;
    int          selectedUser = -1;
    int          hoveredTile = -1;
    HitTarget    hoveredTarget = HitTarget::None;
    HitTarget    pressedTarget = HitTarget::None;
    std::wstring statusText;
    std::wstring errorText;
    // The big italic line for the logoff / shutdown / stand-by screen. Empty
    // whenever the machine is not busy, which is what keeps it off the logon
    // screen; XpTheme::StatusMessage(ShellStatusFor(state, action)) fills it.
    std::wstring shellStatusText;
    size_t       passwordLength = 0;
    bool         passwordFocused = true;
    bool         capsLockOn = false;
    bool         showCaret = true;
    float        errorShakeOffset = 0.0f; // pixels, driven by the shake timer
    PowerAction  hoveredPowerAction = PowerAction::None;
};

// A user as the renderer needs it: name, status line and avatar.
struct RenderUser {
    std::wstring displayName;
    std::wstring statusLine;   // "2 programs running"
    std::wstring avatarPath;
    bool         disabled = false;
};

class XpRenderer {
public:
    explicit XpRenderer(XpTheme theme);
    ~XpRenderer();

    XpRenderer(const XpRenderer&) = delete;
    XpRenderer& operator=(const XpRenderer&) = delete;

    void SetTheme(XpTheme theme);
    const XpTheme& Theme() const { return theme_; }

    void SetUsers(std::vector<RenderUser> users);

    // Microsoft's own artwork, baked out of logonui.exe by the build. When this
    // has not been loaded the renderer draws the screen from primitives
    // instead - the same shapes, without the exact pixels.
    bool LoadAssets(const std::wstring& packPath);
    bool UsingRealArtwork() const;

    // Draws a whole frame into `target`, double buffered. Never throws; on any
    // internal failure it falls back to a flat blue fill so the screen is still
    // usable rather than black.
    void Paint(HDC target, const FrameLayout& frame, const RenderState& state);

    // Frees cached avatars and fonts. Called when the theme changes and on
    // shutdown.
    void ReleaseResources();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    XpTheme theme_;
};

// GDI+ has to be started once per process before any renderer exists and shut
// down after the last one is gone. This does that with a refcount, because
// LogonUI may construct and destroy the provider several times.
class GdiPlusHost {
public:
    GdiPlusHost();
    ~GdiPlusHost();
    bool Ready() const { return ready_; }

private:
    bool ready_ = false;
};

} // namespace xplogin::ui::win32
