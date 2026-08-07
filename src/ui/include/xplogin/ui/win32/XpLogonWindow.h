// XPLogin10 - the full-screen window that *is* the XP welcome screen.
//
// A credential provider is loaded into LogonUI.exe, which already owns the
// Winlogon desktop. That means we can create a normal top-level window there
// and cover the Windows logon UI with it - no separate process, no injection.
// The window lives and dies with the provider.
#pragma once

#include "xplogin/LogonController.h"
#include "xplogin/ui/XpLayout.h"
#include "xplogin/ui/win32/XpRenderer.h"

#include <windows.h>

#include <memory>
#include <string>

namespace xplogin::ui::win32 {

// Raised to the owner when the user finishes with the screen.
class IWindowHost {
public:
    virtual ~IWindowHost() = default;

    // The user pressed Enter / clicked the go arrow. The host is expected to
    // ask LogonUI to collect the credentials (CredentialsChanged +
    // SubmitButtonClicked).
    virtual void OnSubmitRequested() = 0;

    // The screen wants to go away (successful logon or power action).
    virtual void OnDismissRequested() = 0;

    // The user asked for Windows' own sign-in tiles - a PIN, a fingerprint, a
    // smartcard. XP had nothing like this, so the affordance is deliberately
    // quiet, but it has to exist: on an account that signs in with a PIN the XP
    // password box cannot authenticate at all, and without a way through this
    // screen would be a wall. The host hides the window and lets LogonUI show
    // what the filter left standing.
    virtual void OnSigninOptionsRequested() = 0;
};

class XpLogonWindow {
public:
    XpLogonWindow(LogonController& controller, XpTheme theme, IWindowHost* host);
    ~XpLogonWindow();

    XpLogonWindow(const XpLogonWindow&) = delete;
    XpLogonWindow& operator=(const XpLogonWindow&) = delete;

    // Creates the window on the calling thread's desktop and shows it. Returns
    // false if the window class or the window itself could not be created, in
    // which case the caller must fall back to the stock Windows UI.
    bool Create(HINSTANCE instance);

    void Destroy();
    void Show();
    void Hide();

    // Steps aside so Windows' own tiles (PIN, Hello, smartcard) are reachable.
    void RequestSigninOptions();

    HWND Handle() const { return hwnd_; }

    // The password as typed. The provider reads it in GetSerialization and
    // wipes it immediately afterwards.
    const std::wstring& Password() const { return password_; }
    void ClearPassword();

    // Repaint after the controller state changed underneath us.
    void Refresh();

    // Shows the XP error text and starts the shake animation.
    void ShowError(const std::wstring& message);

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                         LPARAM lParam);
    LRESULT WndProc(UINT message, WPARAM wParam, LPARAM lParam);

    void OnPaint();
    void OnMouseMove(int x, int y);
    void OnLeftButtonDown(int x, int y);
    void OnLeftButtonUp(int x, int y);
    void OnKeyDown(WPARAM key);
    void OnChar(wchar_t character);
    void OnTimer(UINT_PTR timerId);

    void Activate(HitTarget target, int index);
    void RebuildRenderUsers();
    LayoutInput CurrentLayoutInput() const;
    FrameLayout CurrentFrame() const;
    void StartSlideAnimation();
    void Invalidate();
    // Repaints before returning. Used on the way into a shutdown, where the
    // usual invalidate-and-wait would lose the race with the end-session
    // messages.
    void PaintNow();

    LogonController&  controller_;
    IWindowHost*      host_ = nullptr;
    // Declaration order is destruction order in reverse: GDI+ must still be
    // running when the renderer releases its bitmaps and fonts, so the host has
    // to be constructed first and destroyed last.
    GdiPlusHost       gdiplus_;
    XpLayout          layout_;
    XpRenderer        renderer_;
    RenderState       renderState_;

    HWND      hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    int       screenWidth_ = 0;
    int       screenHeight_ = 0;

    std::wstring password_;
    int      scrollOffset_ = 0;
    uint64_t slideStartTick_ = 0;
    uint64_t shakeStartTick_ = 0;
    bool     classRegistered_ = false;
};

} // namespace xplogin::ui::win32
