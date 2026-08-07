// XPLogin10 - ICredentialProvider.
//
// Windows creates one of these per usage scenario. It owns the LogonController
// (and therefore the account list and the state machine) and hands LogonUI one
// credential tile per account, any of which puts the XP window on the screen
// when LogonUI selects it.
#pragma once

#include "XPCredential.h"

#include "xplogin/Config.h"
#include "xplogin/LogonController.h"
#include "xplogin/ui/XpTheme.h"

#include <windows.h>

#include <credentialprovider.h>

#include <memory>
#include <vector>

namespace xplogin::provider {

class XPProvider : public ICredentialProvider,
                   public ICredentialProviderSetUserArray,
                   public ICredentialHost,
                   public IControllerObserver {
public:
    explicit XPProvider(HINSTANCE moduleInstance);
    virtual ~XPProvider();

    // IUnknown
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;

    // ICredentialProvider
    IFACEMETHODIMP SetUsageScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario,
                                    DWORD flags) override;
    IFACEMETHODIMP SetSerialization(
        const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* serialization) override;
    IFACEMETHODIMP Advise(ICredentialProviderEvents* events,
                          UINT_PTR adviseContext) override;
    IFACEMETHODIMP UnAdvise() override;
    IFACEMETHODIMP GetFieldDescriptorCount(DWORD* count) override;
    IFACEMETHODIMP GetFieldDescriptorAt(
        DWORD index, CREDENTIAL_PROVIDER_FIELD_DESCRIPTOR** descriptor) override;
    IFACEMETHODIMP GetCredentialCount(DWORD* count, DWORD* defaultCredential,
                                      BOOL* autoLogonWithDefault) override;
    IFACEMETHODIMP GetCredentialAt(DWORD index,
                                   ICredentialProviderCredential** credential) override;

    // ICredentialProviderSetUserArray - the V2 way Windows tells a provider
    // which accounts it intends to show.
    IFACEMETHODIMP SetUserArray(ICredentialProviderUserArray* users) override;

    // ICredentialHost - the XP window submitted through the credential.
    void RequestSubmit() override;

    // IControllerObserver
    void OnStateChanged(UiState from, UiState to) override;
    void OnUsersChanged() override;
    void OnAuthError(const AuthResult& result) override;

private:
    void ReleaseCredentials();

    // Builds one credential per account, if the set is missing or stale.
    // Deliberately called from GetCredentialCount rather than from
    // SetUsageScenario: SetUserArray - the authoritative account list on a
    // domain or Entra-joined machine - arrives between the two, and building
    // early meant building from the wrong list.
    void EnsureCredentials();
    void RebuildCredentials();

    // Index of the credential the XP window has asked to submit, or -1.
    int  SubmitPendingIndex() const;
    // Index of the tile LogonUI should open on when it has no opinion.
    DWORD DefaultCredentialIndex() const;

    UsageScenario TranslateScenario(CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario) const;
    void CaptureLockedUser();

    long        refCount_ = 1;
    HINSTANCE   moduleInstance_ = nullptr;
    CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario_ = CPUS_INVALID;

    AppConfig                        config_;
    ui::XpTheme                      theme_;
    std::unique_ptr<LogonController> controller_;

    // One per account. Owned: released in ReleaseCredentials.
    std::vector<XPCredential*>       credentials_;
    bool                             credentialsStale_ = true;

    ICredentialProviderEvents*  events_ = nullptr;
    UINT_PTR                    adviseContext_ = 0;
    ICredentialProviderUserArray* userArray_ = nullptr;

    // False when the health monitor told us to stand down this boot.
    bool active_ = false;
};

} // namespace xplogin::provider
