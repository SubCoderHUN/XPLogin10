#include "xplogin/ui/win32/XpLogonWindow.h"

#include "xplogin/Logging.h"
#include "xplogin/StringUtil.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

namespace xplogin::ui::win32 {
namespace {

const wchar_t* const kWindowClassName = L"XPLogin10WelcomeScreen";

constexpr UINT_PTR kAnimationTimer = 1;
constexpr UINT_PTR kCaretTimer = 2;
constexpr UINT kAnimationIntervalMs = 16; // ~60fps while animating
constexpr UINT kCaretIntervalMs = 530;    // the Windows default blink rate
constexpr uint64_t kShakeDurationMs = 400;

uint64_t NowTick() { return ::GetTickCount64(); }

// Where XPLogin.assets is, relative to whatever module we are running in. The
// installer drops it next to XPLoginProvider.dll; a build tree leaves it one
// level up from bin/, which is the only reason for the second candidate.
std::vector<std::wstring> AssetPackCandidates(HINSTANCE module) {
    wchar_t path[MAX_PATH] = {};
    const DWORD length =
        ::GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) {
        return {};
    }

    std::wstring directory(path, length);
    const size_t slash = directory.find_last_of(L'\\');
    if (slash == std::wstring::npos) {
        return {};
    }
    directory.resize(slash);

    std::vector<std::wstring> candidates;
    candidates.push_back(directory + L"\\XPLogin.assets");

    const size_t parent = directory.find_last_of(L'\\');
    if (parent != std::wstring::npos) {
        candidates.push_back(directory.substr(0, parent) + L"\\XPLogin.assets");
    }
    return candidates;
}

} // namespace

XpLogonWindow::XpLogonWindow(LogonController& controller, XpTheme theme,
                             IWindowHost* host)
    : controller_(controller),
      host_(host),
      layout_(theme),
      renderer_(std::move(theme)) {}

XpLogonWindow::~XpLogonWindow() { Destroy(); }

bool XpLogonWindow::Create(HINSTANCE instance) {
    instance_ = instance;

    // Microsoft's own artwork, if it was deployed. Deliberately not fatal: the
    // renderer draws the same screen from primitives without it, and a missing
    // file must never be why somebody cannot sign in.
    for (const std::wstring& candidate : AssetPackCandidates(instance)) {
        if (renderer_.LoadAssets(candidate)) {
            XPLOG_INFO("using the original XP artwork from %s",
                       WideToUtf8(candidate).c_str());
            break;
        }
    }
    if (!renderer_.UsingRealArtwork()) {
        XPLOG_INFO("no XPLogin.assets found; drawing the screen from primitives");
    }

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    // No CS_DBLCLKS: a double click on a tile must not skip the password box.
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    windowClass.lpfnWndProc = &XpLogonWindow::WndProcThunk;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr; // we paint every pixel ourselves
    windowClass.lpszClassName = kWindowClassName;

    if (!::RegisterClassExW(&windowClass)) {
        const DWORD error = ::GetLastError();
        // A second provider instance in the same LogonUI process is normal.
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            XPLOG_ERROR("RegisterClassEx failed: %lu",
                        static_cast<unsigned long>(error));
            return false;
        }
    } else {
        classRegistered_ = true;
    }

    screenWidth_ = ::GetSystemMetrics(SM_CXSCREEN);
    screenHeight_ = ::GetSystemMetrics(SM_CYSCREEN);

    // Deliberately NOT WS_EX_NOACTIVATE. This window has to take the keyboard:
    // it draws its own password box and reads WM_CHAR, and a window that never
    // activates never receives one. With NOACTIVATE the screen appeared and
    // then could not be typed into, because the keystrokes were going to
    // LogonUI's own hidden password field underneath.
    hwnd_ = ::CreateWindowExW(
        WS_EX_TOPMOST, kWindowClassName, L"", WS_POPUP, 0, 0,
        screenWidth_, screenHeight_, nullptr, nullptr, instance, this);

    if (!hwnd_) {
        XPLOG_ERROR("CreateWindowEx failed: %lu",
                    static_cast<unsigned long>(::GetLastError()));
        return false;
    }

    RebuildRenderUsers();
    ::SetTimer(hwnd_, kCaretTimer, kCaretIntervalMs, nullptr);

    // The shell-status screen has to be on the glass before Windows is told to
    // shut down, because after that this thread may never paint again. See
    // LogonController::SetPowerActionPresenter - PaintNow is exactly the
    // "and do not return until it is drawn" that it asks for.
    controller_.SetPowerActionPresenter([this]() { PaintNow(); });

    XPLOG_INFO("welcome screen window created (%dx%d)", screenWidth_, screenHeight_);
    return true;
}

void XpLogonWindow::Destroy() {
    // Before anything else. The presenter captures `this`, and the controller
    // outlives this window - the provider keeps one across tile rebuilds and
    // re-selections, so a power action after a Destroy would otherwise call
    // PaintNow on a window that is gone.
    controller_.SetPowerActionPresenter(nullptr);
    if (hwnd_) {
        ::KillTimer(hwnd_, kAnimationTimer);
        ::KillTimer(hwnd_, kCaretTimer);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    renderer_.ReleaseResources();
    SecureClear(password_);
    if (classRegistered_ && instance_) {
        ::UnregisterClassW(kWindowClassName, instance_);
        classRegistered_ = false;
    }
}

namespace {

// Num Lock on, so the number pad types digits.
//
// The installer writes InitialKeyboardIndicators under .DEFAULT, which is the
// documented mechanism and the one that survives a reboot - but it only takes
// effect when Winlogon next builds the sign-in desktop, and it cannot help on
// a machine where something else has already turned Num Lock off. There is no
// way to reach the setting from the sign-in screen itself, so a password typed
// on the number pad just silently produces nothing.
//
// Toggling is the only way: there is no API to set the state directly. The key
// state has to be read first, or this turns Num Lock *off* on every machine
// that already had it on.
void EnsureNumLockOn() {
    if ((::GetKeyState(VK_NUMLOCK) & 0x0001) != 0) {
        return;
    }
    ::keybd_event(VK_NUMLOCK, 0x45, KEYEVENTF_EXTENDEDKEY, 0);
    ::keybd_event(VK_NUMLOCK, 0x45, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
    XPLOG_INFO("Num Lock was off at the sign-in screen; turned it on");
}

} // namespace

void XpLogonWindow::Show() {
    if (!hwnd_) {
        return;
    }
    EnsureNumLockOn();
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, screenWidth_, screenHeight_,
                   SWP_SHOWWINDOW);
    // Take the foreground and the focus, or every keystroke goes to the
    // LogonUI tile hidden underneath this window.
    ::SetForegroundWindow(hwnd_);
    ::SetActiveWindow(hwnd_);
    ::SetFocus(hwnd_);
    XPLOG_INFO("welcome screen shown and focused");
    Invalidate();
}

void XpLogonWindow::Hide() {
    if (hwnd_) {
        ::ShowWindow(hwnd_, SW_HIDE);
    }
}

void XpLogonWindow::RequestSigninOptions() {
    // Get out of the way and let LogonUI show what the filter left standing -
    // the PIN tile, a fingerprint reader, a smartcard. Selecting the XPLogin
    // tile again brings this window back through SetSelected.
    XPLOG_INFO("user asked for the Windows sign-in options; hiding the XP screen");
    controller_.CancelSelection();
    ClearPassword();
    slideStartTick_ = 0;
    Hide();
    if (host_) {
        host_->OnSigninOptionsRequested();
    }
}

void XpLogonWindow::ClearPassword() {
    SecureClear(password_);
    renderState_.passwordLength = 0;
    Invalidate();
}

void XpLogonWindow::Refresh() {
    RebuildRenderUsers();
    Invalidate();
}

void XpLogonWindow::ShowError(const std::wstring& message) {
    renderState_.errorText = message;
    shakeStartTick_ = NowTick();
    if (hwnd_) {
        ::SetTimer(hwnd_, kAnimationTimer, kAnimationIntervalMs, nullptr);
    }
    Invalidate();
}

void XpLogonWindow::Invalidate() {
    if (hwnd_) {
        ::InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void XpLogonWindow::PaintNow() {
    if (hwnd_) {
        ::InvalidateRect(hwnd_, nullptr, FALSE);
        ::UpdateWindow(hwnd_); // WM_PAINT now, not whenever the queue empties
    }
}

void XpLogonWindow::RebuildRenderUsers() {
    std::vector<RenderUser> users;
    users.reserve(controller_.Users().size());

    for (const UserAccount& account : controller_.Users()) {
        RenderUser user;
        user.displayName =
            account.displayName.empty() ? account.username : account.displayName;
        user.avatarPath = account.avatarPath;
        user.disabled = !account.CanLogOn();

        // The XP status line under the name.
        if (account.lockedOut) {
            user.statusLine = L"Account locked";
        } else if (account.disabled) {
            user.statusLine = L"Account disabled";
        } else if (account.programsRunning == 1) {
            user.statusLine = L"1 program running";
        } else if (account.programsRunning > 1) {
            user.statusLine =
                std::to_wstring(account.programsRunning) + L" programs running";
        } else if (account.sessionLocked) {
            user.statusLine = L"Logged on";
        }

        users.push_back(std::move(user));
    }
    renderer_.SetUsers(std::move(users));
}

LayoutInput XpLogonWindow::CurrentLayoutInput() const {
    LayoutInput input;
    input.screenWidth = screenWidth_;
    input.screenHeight = screenHeight_;
    input.state = controller_.State();
    input.userCount = static_cast<int>(controller_.Users().size());
    input.selectedUser = controller_.SelectedIndex();
    // The pointer position decides which tiles are dimmed, so the layout needs
    // it as much as the renderer does.
    input.hoveredTile = renderState_.hoveredTile;
    input.scrollOffset = scrollOffset_;
    input.showHintButton = controller_.Config().ui.showHintButton;
    input.signingInAutomatically = controller_.SignsInAutomatically();
    input.showBackButton = controller_.Snapshot().backButtonVisible;
    // Only offered when the filter actually left something to fall through to.
    // In ReplaceEverything mode there is nothing behind us, so advertising it
    // would just blank the screen.
    input.showSigninOptions =
        controller_.Config().signinOptions != SigninOptionsMode::ReplaceEverything;

    const uint64_t duration =
        controller_.Config().ui.animationsEnabled
            ? static_cast<uint64_t>(std::max(0, controller_.Config().ui.tileSlideDurationMs))
            : 0;
    const uint64_t elapsed =
        slideStartTick_ == 0 ? duration : NowTick() - slideStartTick_;
    input.slideProgress = XpLayout::SlideProgress(elapsed, duration);
    return input;
}

FrameLayout XpLogonWindow::CurrentFrame() const {
    return layout_.Compute(CurrentLayoutInput());
}

void XpLogonWindow::StartSlideAnimation() {
    if (!controller_.Config().ui.animationsEnabled) {
        slideStartTick_ = 0;
        return;
    }
    slideStartTick_ = NowTick();
    if (hwnd_) {
        ::SetTimer(hwnd_, kAnimationTimer, kAnimationIntervalMs, nullptr);
    }
}

// ---------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------

LRESULT CALLBACK XpLogonWindow::WndProcThunk(HWND hwnd, UINT message, WPARAM wParam,
                                             LPARAM lParam) {
    XpLogonWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<XpLogonWindow*>(create->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<XpLogonWindow*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return self->WndProc(message, wParam, lParam);
}

LRESULT XpLogonWindow::WndProc(UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT:
            OnPaint();
            return 0;

        case WM_ERASEBKGND:
            return 1; // every pixel is painted in WM_PAINT

        case WM_MOUSEMOVE:
            OnMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_LBUTTONDOWN:
            OnLeftButtonDown(GET_X_LPARAM(lParam),
                             GET_Y_LPARAM(lParam));
            return 0;

        case WM_LBUTTONUP:
            OnLeftButtonUp(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;

        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const LayoutInput input = CurrentLayoutInput();
            scrollOffset_ =
                layout_.ClampScroll(input, scrollOffset_ - (delta > 0 ? 1 : -1));
            Invalidate();
            return 0;
        }

        case WM_KEYDOWN:
            OnKeyDown(wParam);
            return 0;

        case WM_CHAR:
            OnChar(static_cast<wchar_t>(wParam));
            return 0;

        case WM_TIMER:
            OnTimer(wParam);
            return 0;

        case WM_DISPLAYCHANGE:
            screenWidth_ = ::GetSystemMetrics(SM_CXSCREEN);
            screenHeight_ = ::GetSystemMetrics(SM_CYSCREEN);
            ::SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, screenWidth_, screenHeight_,
                           SWP_NOACTIVATE);
            Invalidate();
            return 0;

        case WM_DESTROY:
            ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wParam, lParam);
}

void XpLogonWindow::OnPaint() {
    PAINTSTRUCT paint = {};
    HDC dc = ::BeginPaint(hwnd_, &paint);
    if (!dc) {
        return;
    }

    renderState_.state = controller_.State();
    renderState_.selectedUser = controller_.SelectedIndex();
    renderState_.statusText = controller_.Snapshot().statusText;
    // The logoff / shutdown / stand-by line. Chosen in portable code from the
    // state and the pending action, worded from XP's own string tables.
    // "welcome" from the first frame on a machine that signs itself in: the
    // state is still UserList at that point, because nothing has been submitted
    // yet, but there is no list to show and nobody to show it to.
    renderState_.shellStatusText = renderer_.Theme().StatusMessage(
        controller_.SignsInAutomatically()
            ? ShellStatus::Welcome
            : ShellStatusFor(controller_.State(),
                             controller_.Snapshot().pendingAction));
    renderState_.passwordLength = password_.size();
    renderState_.capsLockOn = (::GetKeyState(VK_CAPITAL) & 0x0001) != 0;

    if (controller_.State() != UiState::AuthFailed) {
        renderState_.errorText.clear();
    } else if (renderState_.errorText.empty()) {
        renderState_.errorText = controller_.Snapshot().statusText;
    }

    // The XP "wrong password" shake: a decaying sine, 400ms.
    renderState_.errorShakeOffset = 0.0f;
    if (shakeStartTick_ != 0) {
        const uint64_t elapsed = NowTick() - shakeStartTick_;
        if (elapsed >= kShakeDurationMs) {
            shakeStartTick_ = 0;
        } else {
            const float t = static_cast<float>(elapsed) / kShakeDurationMs;
            const float decay = 1.0f - t;
            renderState_.errorShakeOffset =
                std::sin(t * 6.2831853f * 3.0f) * 6.0f * decay;
        }
    }

    renderer_.Paint(dc, CurrentFrame(), renderState_);
    ::EndPaint(hwnd_, &paint);
}

void XpLogonWindow::OnMouseMove(int x, int y) {
    const FrameLayout frame = CurrentFrame();
    const HitResult hit = XpLayout::HitTest(frame, Point{x, y});

    const int previousTile = renderState_.hoveredTile;
    const HitTarget previousTarget = renderState_.hoveredTarget;

    renderState_.hoveredTarget = hit.target;
    renderState_.hoveredTile = hit.target == HitTarget::UserTile ? hit.index : -1;

    if (previousTile != renderState_.hoveredTile ||
        previousTarget != renderState_.hoveredTarget) {
        Invalidate();
    }
}

void XpLogonWindow::OnLeftButtonDown(int x, int y) {
    const FrameLayout frame = CurrentFrame();
    const HitResult hit = XpLayout::HitTest(frame, Point{x, y});
    renderState_.pressedTarget = hit.target;
    ::SetCapture(hwnd_);
    Invalidate();
}

void XpLogonWindow::OnLeftButtonUp(int x, int y) {
    ::ReleaseCapture();

    const FrameLayout frame = CurrentFrame();
    const HitResult hit = XpLayout::HitTest(frame, Point{x, y});
    const HitTarget pressed = renderState_.pressedTarget;
    renderState_.pressedTarget = HitTarget::None;

    // Only act when the release lands on the control the press started on.
    if (pressed == hit.target && pressed != HitTarget::None) {
        Activate(hit.target, hit.index);
    } else {
        Invalidate();
    }
}

void XpLogonWindow::Activate(HitTarget target, int index) {
    switch (target) {
        case HitTarget::UserTile:
            if (controller_.SelectUser(index) == TransitionResult::Accepted) {
                ClearPassword();
                StartSlideAnimation();
            }
            break;

        case HitTarget::GoButton:
        case HitTarget::PasswordBox:
            if (target == HitTarget::GoButton && host_) {
                host_->OnSubmitRequested();
            }
            break;

        case HitTarget::BackButton:
            controller_.CancelSelection();
            ClearPassword();
            slideStartTick_ = 0;
            break;

        case HitTarget::HintButton:
            // XP showed the password hint here. Windows 10 does not expose
            // hints for local accounts to the logon desktop, so this is a
            // deliberate no-op rather than a lie about what we know.
            break;

        case HitTarget::TurnOffButton:
            controller_.OpenTurnOffDialog();
            break;

        case HitTarget::SigninOptions:
            RequestSigninOptions();
            break;

        case HitTarget::DialogStandBy:
            controller_.ChoosePowerAction(PowerAction::StandBy);
            break;

        case HitTarget::DialogTurnOff:
            controller_.ChoosePowerAction(PowerAction::TurnOff);
            break;

        case HitTarget::DialogRestart:
            controller_.ChoosePowerAction(PowerAction::Restart);
            break;

        case HitTarget::DialogCancel:
        case HitTarget::DialogBackdrop:
            controller_.CloseTurnOffDialog();
            break;

        case HitTarget::ScrollUp:
        case HitTarget::ScrollDown:
        case HitTarget::None:
            break;
    }

    // A power action has already been handed to the OS by the time we get
    // here, and WM_PAINT is the lowest-priority message there is: an ordinary
    // invalidate would queue behind WM_QUERYENDSESSION and the shutdown line
    // would never be drawn at all. Paint it synchronously instead, so the XP
    // screen is what covers the machine while it goes down.
    if (controller_.State() == UiState::PowerActionPending) {
        PaintNow();
        return;
    }
    Invalidate();
}

void XpLogonWindow::OnKeyDown(WPARAM key) {
    switch (key) {
        case VK_RETURN:
            if (controller_.State() == UiState::TurnOffDialog) {
                controller_.ChoosePowerAction(PowerAction::TurnOff);
            } else if (host_) {
                host_->OnSubmitRequested();
            }
            break;

        case VK_ESCAPE:
            if (controller_.State() == UiState::TurnOffDialog) {
                controller_.CloseTurnOffDialog();
            } else if (controller_.State() == UiState::UserList &&
                       controller_.Config().signinOptions !=
                           SigninOptionsMode::ReplaceEverything) {
                // Already back at the account list: a second Escape asks for
                // Windows' own tiles. This is the keyboard route to the same
                // place as the footer link, and it is the one that saves a
                // machine whose accounts have no password at all.
                RequestSigninOptions();
            } else {
                controller_.CancelSelection();
                ClearPassword();
                slideStartTick_ = 0;
            }
            break;

        case VK_UP:
        case VK_DOWN: {
            if (controller_.State() != UiState::UserList) {
                break;
            }
            const int count = static_cast<int>(controller_.Users().size());
            if (count == 0) {
                break;
            }
            int next = renderState_.hoveredTile;
            next += (key == VK_DOWN) ? 1 : -1;
            renderState_.hoveredTile = std::clamp(next, 0, count - 1);
            LayoutInput input = CurrentLayoutInput();
            input.selectedUser = renderState_.hoveredTile;
            scrollOffset_ = layout_.ClampScroll(input, scrollOffset_);
            break;
        }

        case VK_SPACE:
            if (controller_.State() == UiState::UserList &&
                renderState_.hoveredTile >= 0) {
                Activate(HitTarget::UserTile, renderState_.hoveredTile);
                return;
            }
            break;

        case VK_TAB:
            // Nothing to tab between: the password box is the only field.
            break;

        default:
            break;
    }
    Invalidate();
}

void XpLogonWindow::OnChar(wchar_t character) {
    if (!controller_.Snapshot().passwordBoxVisible) {
        return;
    }

    if (character == VK_BACK) {
        if (!password_.empty()) {
            password_.pop_back();
        }
    } else if (character == VK_RETURN || character == VK_ESCAPE ||
               character == VK_TAB) {
        return; // handled in WM_KEYDOWN
    } else if (character >= 0x20) {
        // A password longer than this cannot be described by a UNICODE_STRING.
        if (password_.size() < 30000) {
            password_.push_back(character);
        }
    }

    controller_.SetPassword(password_);
    renderState_.passwordLength = password_.size();
    renderState_.showCaret = true;
    Invalidate();
}

void XpLogonWindow::OnTimer(UINT_PTR timerId) {
    if (timerId == kCaretTimer) {
        renderState_.showCaret = !renderState_.showCaret;
        if (controller_.Snapshot().passwordBoxVisible) {
            Invalidate();
        }
        return;
    }

    if (timerId != kAnimationTimer) {
        return;
    }

    bool stillAnimating = false;

    if (slideStartTick_ != 0) {
        const uint64_t duration = static_cast<uint64_t>(
            std::max(0, controller_.Config().ui.tileSlideDurationMs));
        if (NowTick() - slideStartTick_ < duration) {
            stillAnimating = true;
        } else {
            slideStartTick_ = 0;
        }
    }
    if (shakeStartTick_ != 0) {
        if (NowTick() - shakeStartTick_ < kShakeDurationMs) {
            stillAnimating = true;
        } else {
            shakeStartTick_ = 0;
        }
    }

    Invalidate();
    if (!stillAnimating) {
        ::KillTimer(hwnd_, kAnimationTimer);
    }
}

} // namespace xplogin::ui::win32
