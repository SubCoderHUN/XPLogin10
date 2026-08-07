// XPLogin10 - a credential tile behind the XP screen.
//
// LogonUI's own tile is never seen: the XP window covers the whole desktop and
// takes all the input. The tile still has to exist, though, because it is the
// object LogonUI asks for a serialized credential and the object it reports the
// LSA result back to.
//
// There is one of these per account. That is not because the tiles are shown -
// they are not - but because Windows 8 and later hang credentials off *user*
// tiles: a credential is only ever displayed on the tile of the account its
// GetUserSid names. One credential would therefore only ever be reachable from
// one account's tile, and if LogonUI opened on a different account there would
// be nothing on the screen at all.
#pragma once

#include "xplogin/LogonController.h"
#include "xplogin/ui/win32/XpLogonWindow.h"

#include <windows.h>

#include <credentialprovider.h>

#include <memory>
#include <string>

namespace xplogin::provider {

// Implemented by XPProvider. Programmatic submission has to go through the
// *provider's* ICredentialProviderEvents::CredentialsChanged - the credential's
// own V1 event sink has no such method - so the credential asks its owner.
class ICredentialHost {
public:
    virtual ~ICredentialHost() = default;
    virtual void RequestSubmit() = 0;
};

// Field layout. LogonUI requires at least a submit button; everything else is
// hidden because the XP window draws its own controls.
enum FieldId {
    kFieldTileImage = 0,
    kFieldLabel     = 1,
    kFieldPassword  = 2,
    kFieldSubmit    = 3,
    kFieldCount     = 4,
};

class XPCredential : public ICredentialProviderCredential2,
                     public ui::win32::IWindowHost {
public:
    XPCredential();
    virtual ~XPCredential();

    // `userSid` is the account this tile is attached to - see GetUserSid, which
    // is what decides whether LogonUI ever puts the tile on the screen. It may
    // be empty only when the machine has no enumerable accounts at all.
    HRESULT Initialize(CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario,
                       LogonController* controller, ICredentialHost* host,
                       HINSTANCE moduleInstance, const ui::XpTheme& theme,
                       const std::wstring& userSid);

    // The SID this tile claims. The provider uses it to pick which of its
    // credentials should be the default one.
    const std::wstring& TileSid() const { return tileSid_; }

    // Called when the provider replaces its set of tiles. LogonUI may still
    // hold a reference to this object - it is refcounted - so releasing it is
    // not enough to get its window off the screen. Retire() does that part.
    void Retire();

    // True once the XP window has asked to sign in and LogonUI has not yet
    // collected the credentials. The provider reports this as
    // pbAutoLogonWithDefault so LogonUI comes back for GetSerialization.
    bool SubmitPending() const { return submitPending_; }

    // IUnknown
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;

    // ICredentialProviderCredential
    IFACEMETHODIMP Advise(ICredentialProviderCredentialEvents* events) override;
    IFACEMETHODIMP UnAdvise() override;
    IFACEMETHODIMP SetSelected(BOOL* autoLogon) override;
    IFACEMETHODIMP SetDeselected() override;
    IFACEMETHODIMP GetFieldState(
        DWORD fieldIndex, CREDENTIAL_PROVIDER_FIELD_STATE* fieldState,
        CREDENTIAL_PROVIDER_FIELD_INTERACTIVE_STATE* interactiveState) override;
    IFACEMETHODIMP GetStringValue(DWORD fieldIndex, PWSTR* value) override;
    IFACEMETHODIMP GetBitmapValue(DWORD fieldIndex, HBITMAP* bitmap) override;
    IFACEMETHODIMP GetCheckboxValue(DWORD fieldIndex, BOOL* checked,
                                    PWSTR* label) override;
    IFACEMETHODIMP GetSubmitButtonValue(DWORD fieldIndex,
                                        DWORD* adjacentTo) override;
    IFACEMETHODIMP GetComboBoxValueCount(DWORD fieldIndex, DWORD* items,
                                         DWORD* selectedItem) override;
    IFACEMETHODIMP GetComboBoxValueAt(DWORD fieldIndex, DWORD item,
                                      PWSTR* itemValue) override;
    IFACEMETHODIMP SetStringValue(DWORD fieldIndex, PCWSTR value) override;
    IFACEMETHODIMP SetCheckboxValue(DWORD fieldIndex, BOOL checked) override;
    IFACEMETHODIMP SetComboBoxSelectedValue(DWORD fieldIndex, DWORD item) override;
    IFACEMETHODIMP CommandLinkClicked(DWORD fieldIndex) override;
    IFACEMETHODIMP GetSerialization(
        CREDENTIAL_PROVIDER_GET_SERIALIZATION_RESPONSE* response,
        CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* serialization,
        PWSTR* optionalStatusText,
        CREDENTIAL_PROVIDER_STATUS_ICON* optionalStatusIcon) override;
    IFACEMETHODIMP ReportResult(NTSTATUS status, NTSTATUS substatus,
                                PWSTR* optionalStatusText,
                                CREDENTIAL_PROVIDER_STATUS_ICON* icon) override;

    // ICredentialProviderCredential2
    IFACEMETHODIMP GetUserSid(PWSTR* sid) override;

    // IWindowHost - called from the XP window's message loop.
    void OnSubmitRequested() override;
    void OnDismissRequested() override;
    void OnSigninOptionsRequested() override;

private:
    void DestroyWindow();
    HBITMAP TileBitmap();

    long                                  refCount_ = 1;
    CREDENTIAL_PROVIDER_USAGE_SCENARIO    scenario_ = CPUS_INVALID;
    LogonController*                      controller_ = nullptr;
    ICredentialHost*                      host_ = nullptr;
    ICredentialProviderCredentialEvents*  events_ = nullptr;
    HINSTANCE                             moduleInstance_ = nullptr;
    ui::XpTheme                           theme_;
    std::unique_ptr<ui::win32::XpLogonWindow> window_;

    // The account whose tile this credential lives on.
    std::wstring                          tileSid_;

    // Set when the XP window asks to submit; cleared once LogonUI has picked
    // the credentials up in GetSerialization.
    bool submitPending_ = false;

    // The tile picture handed to LogonUI. Created once, owned here.
    HBITMAP tileBitmap_ = nullptr;
};

} // namespace xplogin::provider
