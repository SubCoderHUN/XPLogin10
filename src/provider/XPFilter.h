// XPLogin10 - hides the stock Windows credential providers.
#pragma once

#include "xplogin/Config.h"

#include <windows.h>

#include <credentialprovider.h>

namespace xplogin::provider {

class XPFilter : public ICredentialProviderFilter {
public:
    explicit XPFilter(HINSTANCE moduleInstance);
    virtual ~XPFilter() = default;

    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;

    IFACEMETHODIMP Filter(CREDENTIAL_PROVIDER_USAGE_SCENARIO scenario, DWORD flags,
                          GUID* providerClsids, BOOL* allow,
                          DWORD providerCount) override;
    IFACEMETHODIMP UpdateRemoteCredential(
        const CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* in,
        CREDENTIAL_PROVIDER_CREDENTIAL_SERIALIZATION* out) override;

private:
    long      refCount_ = 1;
    AppConfig config_;
};

} // namespace xplogin::provider
