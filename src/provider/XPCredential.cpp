#include "XPCredential.h"

#include "Guids.h"
#include "xplogin/AuthStatusMapping.h"
#include "xplogin/CredentialPacker.h"
#include "xplogin/HealthMonitor.h"
#include "xplogin/Logging.h"
#include "xplogin/StringUtil.h"
#include "xplogin/Win32Factories.h"

#include <ntsecapi.h>

#include <cstdint>
#include <new>

namespace xplogin {
namespace win32 {
// Defined in src/core/src/win32/Win32Authenticator.cpp.
bool RetrieveNegotiateAuthPackage(ULONG* packageId);
} // namespace win32
} // namespace xplogin

namespace xplogin::provider {
namespace {

// Allocates a copy of `text` with CoTaskMemAlloc, as every out-parameter in
// the credential provider API requires.
HRESULT CopyToCoTaskMem(const std::wstring& text, PWSTR* out) {
    if (!out) {
        return E_POINTER;
    }
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    auto* buffer = static_cast<PWSTR>(::CoTaskMemAlloc(bytes));
    if (!buffer) {
        *out = nullptr;
        return E_OUTOFMEMORY;
    }
    ::memcpy(buffer, text.c_str(), bytes);
    *out = buffer;
    return S_OK;
}

} // namespace

XPCredential::XPCredential() = default;

XPCredential::~XPCredential() {
    DestroyWindow();
    if (tileBitmap_) {
        ::DeleteObject(tileBitmap_);
        tileBitmap_ = nullptr;
    }
    if (events_) {
        events_->Release();
        events_ = nullptr;
    }
}

HRESULT XPCredential::Initialize(CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario,
                                 LogonController* controller,
                                 ICredentialHost* host, HINSTANCE moduleInstance,
                                 const ui::XpTheme& theme,
                                 const std::wstring& userSid) {
    if (!controller) {
        return E_INVALIDARG;
    }
    scenario_ = scenario;
    controller_ = controller;
    host_ = host;
    moduleInstance_ = moduleInstance;
    theme_ = theme;
    tileSid_ = userSid;
    return S_OK;
}

// LogonUI asks for a CPFT_TILE_IMAGE field's bitmap whether the field is
// displayed or not, and a provider that cannot produce one is a provider it
// can reasonably discard. Returning E_NOTIMPL here was a candidate for why
// the tile was never selected.
//
// A flat square in the XP centre-panel blue is enough: the XP window covers
// the whole screen, so this is never actually seen.
HBITMAP XPCredential::TileBitmap() {
    if (tileBitmap_) {
        return tileBitmap_;
    }

    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = 128;
    info.bmiHeader.biHeight = -128; // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HDC screen = ::GetDC(nullptr);
    tileBitmap_ = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels,
                                     nullptr, 0);
    ::ReleaseDC(nullptr, screen);

    if (tileBitmap_ && pixels) {
        auto* rgb = static_cast<uint32_t*>(pixels);
        for (int i = 0; i < 128 * 128; ++i) {
            rgb[i] = 0xFF5A7EDCu; // the centre panel blue, opaque
        }
    }
    return tileBitmap_;
}

void XPCredential::DestroyWindow() {
    if (window_) {
        window_->Destroy();
        window_.reset();
    }
}

void XPCredential::Retire() {
    DestroyWindow();
    host_ = nullptr;
    submitPending_ = false;
}

// ---------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------

IFACEMETHODIMP_(ULONG) XPCredential::AddRef() {
    return static_cast<ULONG>(::InterlockedIncrement(&refCount_));
}

IFACEMETHODIMP_(ULONG) XPCredential::Release() {
    const long count = ::InterlockedDecrement(&refCount_);
    if (count == 0) {
        delete this;
    }
    return static_cast<ULONG>(count);
}

IFACEMETHODIMP XPCredential::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) {
        return E_POINTER;
    }
    if (::IsEqualIID(riid, IID_IUnknown) ||
        ::IsEqualIID(riid, IID_ICredentialProviderCredential) ||
        ::IsEqualIID(riid, IID_ICredentialProviderCredential2)) {
        *ppv = static_cast<ICredentialProviderCredential2*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

IFACEMETHODIMP XPCredential::Advise(ICredentialProviderCredentialEvents* events) {
    // The first call LogonUI makes on the credential. If this appears and
    // SetSelected does not, the tile was built and then rejected.
    XPLOG_DEBUG("credential: Advise");
    if (events_) {
        events_->Release();
        events_ = nullptr;
    }
    if (events) {
        events_ = events;
        events_->AddRef();
    }
    return S_OK;
}

IFACEMETHODIMP XPCredential::UnAdvise() {
    if (events_) {
        events_->Release();
        events_ = nullptr;
    }
    return S_OK;
}

IFACEMETHODIMP XPCredential::SetSelected(BOOL* autoLogon) {
    if (autoLogon) {
        *autoLogon = FALSE;
    }
    if (!controller_) {
        return E_UNEXPECTED;
    }

    // A submission that never came back must not survive into this screen.
    //
    // Handing credentials to LogonUI is not a call that returns a result: the
    // tile is left in Authenticating waiting for ReportResult, and LogonUI is
    // under no obligation to send one. It can re-select the tile instead -
    // which is how we get here - and then the screen would come up refusing
    // keystrokes and still holding the last password, because Authenticating
    // is a state nothing else leaves.
    if (submitPending_ || controller_->State() == UiState::Authenticating) {
        submitPending_ = false;
        controller_->CancelAuthentication();
        if (window_) {
            window_->ClearPassword();
        }
    }

    // This is the moment the XP screen appears. We are on LogonUI's UI thread,
    // on the Winlogon desktop, so a plain top-level window is all it takes.
    XPLOG_INFO("SetSelected: showing the XP screen");
    if (!window_) {
        window_ = std::make_unique<ui::win32::XpLogonWindow>(*controller_, theme_,
                                                             this);
        if (!window_->Create(moduleInstance_)) {
            // Could not create the window: fall back to LogonUI's own tile
            // rather than leaving the user staring at nothing.
            window_.reset();
            XPLOG_ERROR("failed to create the XP window; using the default tile");
            return S_OK;
        }
    }
    window_->Show();

    // One account with no password signs itself in, the way XP did: the welcome
    // screen comes up and the machine lets itself through with nothing to type.
    // We prime the credentials once - the first time the tile is selected - and
    // set *autoLogon so LogonUI collects them straight away, exactly as it does
    // after the user presses Enter. The controller has already confirmed with
    // LSA (via the blank-password probe) that the empty password will be taken;
    // if it is turned down anyway the controller disarms itself and the next
    // SetSelected leaves an ordinary password box.
    if (autoLogon && !autoSignInAttempted_ && controller_->SignsInAutomatically()) {
        autoSignInAttempted_ = true;
        if (controller_->PrepareAutomaticSignIn()) {
            submitPending_ = true;
            *autoLogon = TRUE;
            XPLOG_INFO("one account and no password: signing in without asking");
        }
    }

    // The screen is up and taking input, so this start was not a crash-loop.
    // Clearing the strike counter here rather than only on a successful logon
    // is what stops ordinary Win+L cycles from switching the screen off after
    // two or three of them.
    HealthMonitor(MakeWin32StateStore(), MakeWin32Clock(),
                  controller_->Config().health)
        .ReportUiReady();
    return S_OK;
}

IFACEMETHODIMP XPCredential::SetDeselected() {
    XPLOG_INFO("SetDeselected: hiding the XP screen");
    if (window_) {
        window_->ClearPassword();
        window_->Hide();
    }
    return S_OK;
}

// ---------------------------------------------------------------------------
// Fields.
//
// These used to all be CPFS_HIDDEN, on the reasoning that the XP window is the
// user interface so LogonUI's own tile need not draw anything. That reasoning
// is circular, and it is why the logon screen came up empty: a credential with
// nothing displayable gives LogonUI nothing to render, so it never renders the
// tile, never selects it, and never calls SetSelected - which is the only
// place the XP window is created. The log showed the handshake completing
// perfectly and then simply stopping after GetCredentialAt.
//
// So the tile is a normal, renderable one. What it looks like does not matter:
// the XP window covers the whole screen on top of it. It only has to exist.
// ---------------------------------------------------------------------------

IFACEMETHODIMP XPCredential::GetFieldState(
    DWORD fieldIndex, CREDENTIAL_PROVIDER_FIELD_STATE* fieldState,
    CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* interactiveState) {
    if (fieldIndex >= kFieldCount || !fieldState || !interactiveState) {
        return E_INVALIDARG;
    }

    XPLOG_DEBUG("credential: GetFieldState(%lu)",
                static_cast<unsigned long>(fieldIndex));
    switch (fieldIndex) {
        case kFieldLabel:
            // Something to draw in both the collapsed and the selected tile,
            // which is what makes LogonUI treat this as a real tile at all.
            *fieldState = CPFS_DISPLAY_IN_BOTH;
            *interactiveState = CPFIS_NONE;
            break;

        case kFieldPassword:
            *fieldState = CPFS_DISPLAY_IN_SELECTED_TILE;
            *interactiveState = CPFIS_FOCUSED;
            break;

        case kFieldSubmit:
            *fieldState = CPFS_DISPLAY_IN_SELECTED_TILE;
            *interactiveState = CPFIS_NONE;
            break;

        case kFieldTileImage:
        default:
            // Stays hidden: GetBitmapValue has no HBITMAP to give, and a
            // displayed image field with no bitmap is worse than none.
            *fieldState = CPFS_HIDDEN;
            *interactiveState = CPFIS_NONE;
            break;
    }
    return S_OK;
}

IFACEMETHODIMP XPCredential::GetStringValue(DWORD fieldIndex, PWSTR* value) {
    if (fieldIndex >= kFieldCount || !value) {
        return E_INVALIDARG;
    }
    XPLOG_DEBUG("credential: GetStringValue(%lu)",
                static_cast<unsigned long>(fieldIndex));
    if (fieldIndex == kFieldLabel) {
        return CopyToCoTaskMem(L"Windows XP", value);
    }
    return CopyToCoTaskMem(L"", value);
}

IFACEMETHODIMP XPCredential::GetBitmapValue(DWORD fieldIndex, HBITMAP* bitmap) {
    if (fieldIndex != kFieldTileImage || !bitmap) {
        return E_INVALIDARG;
    }
    HBITMAP source = TileBitmap();
    if (!source) {
        *bitmap = nullptr;
        XPLOG_ERROR("GetBitmapValue: could not create the tile bitmap");
        return E_FAIL;
    }
    // LogonUI takes ownership of what it is given, so hand over a copy.
    *bitmap = static_cast<HBITMAP>(
        ::CopyImage(source, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
    XPLOG_DEBUG("GetBitmapValue(%lu) -> %s", static_cast<unsigned long>(fieldIndex),
                *bitmap ? "bitmap" : "null");
    return *bitmap ? S_OK : E_FAIL;
}

IFACEMETHODIMP XPCredential::GetCheckboxValue(DWORD, BOOL*, PWSTR*) {
    return E_NOTIMPL;
}

IFACEMETHODIMP XPCredential::GetSubmitButtonValue(DWORD fieldIndex,
                                                  DWORD* adjacentTo) {
    if (fieldIndex != kFieldSubmit || !adjacentTo) {
        return E_INVALIDARG;
    }
    XPLOG_DEBUG("credential: GetSubmitButtonValue");
    *adjacentTo = kFieldPassword;
    return S_OK;
}

IFACEMETHODIMP XPCredential::GetComboBoxValueCount(DWORD, DWORD*, DWORD*) {
    return E_NOTIMPL;
}

IFACEMETHODIMP XPCredential::GetComboBoxValueAt(DWORD, DWORD, PWSTR*) {
    return E_NOTIMPL;
}

IFACEMETHODIMP XPCredential::SetStringValue(DWORD, PCWSTR) { return S_OK; }
IFACEMETHODIMP XPCredential::SetCheckboxValue(DWORD, BOOL) { return E_NOTIMPL; }
IFACEMETHODIMP XPCredential::SetComboBoxSelectedValue(DWORD, DWORD) {
    return E_NOTIMPL;
}
IFACEMETHODIMP XPCredential::CommandLinkClicked(DWORD) { return E_NOTIMPL; }

// ---------------------------------------------------------------------------
// The handover to LSA
// ---------------------------------------------------------------------------

IFACEMETHODIMP XPCredential::GetSerialization(
    CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* response,
    CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* serialization,
    PWSTR* optionalStatusText, CREDENTIAL_PROVIDER_STATUS_ICON* optionalStatusIcon) {
    if (!response || !serialization) {
        return E_INVALIDARG;
    }
    if (optionalStatusText) {
        *optionalStatusText = nullptr;
    }
    if (optionalStatusIcon) {
        *optionalStatusIcon = CPSI_NONE;
    }

    if (!controller_ || !submitPending_) {
        // LogonUI polls; there is nothing to hand over yet.
        *response = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
        return S_OK;
    }
    submitPending_ = false;

    if (!controller_->CanSerialize()) {
        *response = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
        return S_OK;
    }

    // CPUS_UNLOCK needs the LUID of the session being unlocked so LSA knows
    // which logon session the credentials belong to.
    ULONGLONG logonId = 0;
    if (scenario_ == CPUS_UNLOCK_WORKSTATION) {
        LUID luid = {};
        DWORD size = 0;
        HANDLE token = nullptr;
        if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
            TOKEN_STATISTICS statistics = {};
            if (::GetTokenInformation(token, TokenStatistics, &statistics,
                                      sizeof(statistics), &size)) {
                luid = statistics.AuthenticationId;
            }
            ::CloseHandle(token);
        }
        logonId = (static_cast<ULONGLONG>(static_cast<ULONG>(luid.HighPart)) << 32) |
                  luid.LowPart;
    }

    PackedCredential packed;
    if (!controller_->BuildSerialization(logonId, sizeof(void*), &packed) ||
        packed.empty()) {
        XPLOG_ERROR("failed to pack the credential blob");
        *response = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
        return S_OK;
    }

    ULONG authPackage = 0;
    if (!xplogin::win32::RetrieveNegotiateAuthPackage(&authPackage)) {
        *response = CPGSR_NO_CREDENTIAL_NOT_FINISHED;
        return S_OK;
    }

    auto* buffer = static_cast<BYTE*>(::CoTaskMemAlloc(packed.size()));
    if (!buffer) {
        return E_OUTOFMEMORY;
    }
    ::memcpy(buffer, packed.bytes.data(), packed.size());

    serialization->ulAuthenticationPackage = authPackage;
    serialization->clsidCredentialProvider = CLSID_XPLoginProvider;
    serialization->cbSerialization = static_cast<ULONG>(packed.size());
    serialization->rgbSerialization = buffer;

    // The plaintext existed for exactly as long as it took to build the blob.
    SecureZero(packed.bytes.data(), packed.bytes.size());
    if (window_) {
        window_->ClearPassword();
    }

    *response = CPGSR_RETURN_CREDENTIAL_FINISHED;
    XPLOG_INFO("credential handed to LSA (%u bytes, package %lu)",
               static_cast<unsigned>(packed.size()),
               static_cast<unsigned long>(authPackage));
    return S_OK;
}

IFACEMETHODIMP XPCredential::ReportResult(
    NTSTATUS status, NTSTATUS substatus, PWSTR* optionalStatusText,
    CREDENTIAL_PROVIDER_STATUS_ICON* icon) {
    if (optionalStatusText) {
        *optionalStatusText = nullptr;
    }
    if (icon) {
        *icon = CPSI_NONE;
    }
    if (!controller_) {
        return S_OK;
    }

    const AuthResult result = AuthResultFromNtStatus(static_cast<int32_t>(status),
                                                     static_cast<int32_t>(substatus));
    controller_->OnAuthReported(result);

    if (window_) {
        if (result.Ok()) {
            // Stay up, and repaint into the LoggedOn state.
            //
            // This used to hide the window, on the theory that the XP screen
            // would otherwise flash over the user's wallpaper. It cannot: this
            // window lives on the Winlogon desktop, and signing in switches
            // the input desktop to Default, where the window does not exist.
            // Hiding it only uncovered LogonUI's own spinner - so the last
            // thing seen before the desktop appeared was Windows 10's welcome
            // screen, in the middle of an otherwise complete XP sign-in.
            //
            // Left visible, the screen shows what XP showed while Winlogon
            // built the session: the account's tile and "Loading your personal
            // settings...".
            window_->Refresh();
        } else {
            window_->ClearPassword();
            window_->ShowError(controller_->Snapshot().statusText);
        }
    }

    // Returning S_OK with no status text keeps LogonUI's own error banner off
    // the screen - the XP window has already shown the message.
    return S_OK;
}

// THE method that decides whether this tile is ever shown.
//
// Windows 8 and later build the logon screen around *users*, not around
// providers: LogonUI draws one tile per account and asks every credential
// which account it belongs to. A credential that answers "none" is not placed
// on anybody's tile, and with the Windows password tiles filtered away that
// leaves a screen with no tiles at all - LogonUI's own chrome in the corner
// (network, ease of access, power) and nothing else. That is exactly what this
// screen was doing.
//
// The old answer here was S_FALSE whenever no account had been picked yet,
// reasoning that the user is still choosing. On XP that would be right,
// because XP's screen owned the account list. On Windows 10 it is backwards:
// the tile has to be attached to an account *before* it can be shown at all,
// and it is only by being shown and selected that SetSelected fires and the XP
// window - which is where the choosing actually happens - ever gets created.
//
// Which account this particular tile names barely matters to the user, because
// the XP window covers the whole screen and does its own choosing; the blob
// handed to LSA carries whatever name was typed into that window, not this
// SID. What matters is that *every* account has one of our credentials on it,
// so LogonUI cannot open on a tile we are not on. XPProvider builds the set.
IFACEMETHODIMP XPCredential::GetUserSid(PWSTR* sid) {
    if (!sid) {
        return E_POINTER;
    }
    *sid = nullptr;

    if (!tileSid_.empty()) {
        XPLOG_DEBUG("GetUserSid: %s", WideToUtf8(tileSid_).c_str());
        return CopyToCoTaskMem(tileSid_, sid);
    }

    // Only reached when the machine produced no account with a SID at all.
    // S_FALSE is the honest answer - the credential is then an unattached one,
    // the way "Other user" works - and the log says why.
    XPLOG_ERROR("GetUserSid: this tile is attached to no account; LogonUI has "
                "nowhere to put it");
    return S_FALSE;
}

// ---------------------------------------------------------------------------
// IWindowHost
// ---------------------------------------------------------------------------

void XPCredential::OnSubmitRequested() {
    if (!controller_) {
        return;
    }
    if (controller_->Submit() != TransitionResult::Accepted) {
        return;
    }

    // The controller is now in Authenticating and CanSerialize() is true. Ask
    // the provider to tell LogonUI the credentials are ready; LogonUI responds
    // by re-enumerating and calling GetSerialization on this tile.
    submitPending_ = true;
    if (host_) {
        host_->RequestSubmit();
    }
}

void XPCredential::OnDismissRequested() {
    if (window_) {
        window_->Hide();
    }
}

void XPCredential::OnSigninOptionsRequested() {
    // The window has already hidden itself. Nothing else to do: LogonUI is
    // still showing its own tile strip underneath, and whatever the filter left
    // visible - the PIN tile, a fingerprint reader - is now reachable. Picking
    // the XPLogin tile again comes back through SetSelected and reopens this
    // window, so this is a step aside rather than a way out.
    XPLOG_INFO("stepped aside for the Windows sign-in options");
}

} // namespace xplogin::provider
