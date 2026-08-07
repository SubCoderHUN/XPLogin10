// XPLogin10 - the two authentication paths.
//
//  * DeferredLsaAuthenticator is what the credential provider uses. It does not
//    validate anything: LogonUI takes the serialized blob, LSA authenticates,
//    creates the token and the session, loads the profile and starts the shell,
//    and the outcome arrives back through ICredentialProviderCredential::
//    ReportResult. This is the only supported way to sign a user in from a
//    credential provider, and it is why the XP screen ends up on a real Windows
//    desktop rather than a fake one.
//
//  * Win32Authenticator calls LogonUserW. It cannot create an interactive
//    session, so it is used by the preview host and for optional pre-flight
//    checks only.
#include "Win32Common.h"

#include "xplogin/AuthStatusMapping.h"
#include "xplogin/Interfaces.h"
#include "xplogin/Logging.h"
#include "xplogin/StringUtil.h"
#include "xplogin/Win32Factories.h"

namespace xplogin {
namespace {

class DeferredLsaAuthenticator : public IAuthenticator {
public:
    AuthResult Authenticate(const LogonRequest& request) override {
        // Nothing to do here on purpose. The provider has already put the
        // credentials in the controller; GetSerialization() will pack them and
        // hand them to LogonUI on the way out of this call.
        XPLOG_DEBUG("deferring authentication of %ls to LSA",
                    request.username.c_str());
        AuthResult result;
        result.status = AuthStatus::Pending;
        return result;
    }

    bool IsAsynchronous() const override { return true; }
};

class Win32Authenticator : public IAuthenticator {
public:
    AuthResult Authenticate(const LogonRequest& request) override {
        if (request.username.empty()) {
            return AuthResult::Fail(AuthStatus::UnknownUser);
        }

        // Split a qualified name; LogonUserW wants the parts separately unless
        // the caller passed a UPN, which goes in the user field with a null
        // domain.
        std::wstring domain = request.domain;
        std::wstring username = request.username;
        if (domain.empty() && username.find(L'\\') != std::wstring::npos) {
            SplitQualifiedName(request.username, &domain, &username);
        }

        win32::ScopedHandle token;
        ::SetLastError(ERROR_SUCCESS);
        const BOOL ok = ::LogonUserW(
            username.c_str(), domain.empty() ? nullptr : domain.c_str(),
            request.password.c_str(), LOGON32_LOGON_INTERACTIVE,
            LOGON32_PROVIDER_DEFAULT, token.Receive());

        if (ok) {
            return AuthResult::Success();
        }

        const DWORD error = ::GetLastError();
        XPLOG_INFO("LogonUserW rejected %ls: win32 error %lu", username.c_str(),
                   static_cast<unsigned long>(error));
        return AuthResultFromWin32Error(error);
    }

    bool IsAsynchronous() const override { return false; }
};

} // namespace

std::shared_ptr<IAuthenticator> MakeDeferredLsaAuthenticator() {
    return std::make_shared<DeferredLsaAuthenticator>();
}

std::shared_ptr<IAuthenticator> MakeWin32Authenticator() {
    return std::make_shared<Win32Authenticator>();
}

namespace win32 {

// Asks LSA for the ID of the Negotiate package. LogonUI needs it alongside the
// credential blob; without it the blob is ignored.
bool RetrieveNegotiateAuthPackage(ULONG* packageId) {
    if (!packageId) {
        return false;
    }

    HANDLE lsa = nullptr;
    NTSTATUS status = ::LsaConnectUntrusted(&lsa);
    if (status != 0) {
        XPLOG_ERROR("LsaConnectUntrusted failed: 0x%08lx",
                    static_cast<unsigned long>(status));
        return false;
    }

    // The package name LSA expects, as an ANSI LSA_STRING. <sspi.h> spells
    // this NEGOSSP_NAME_A, but reaching that macro means defining
    // SECURITY_WIN32 before windows.h in every win32 translation unit, and
    // this is the only line in the project that wants it. The value is part of
    // the SSPI contract and has not changed since NT 4.
    LSA_STRING name = {};
    char packageName[] = "Negotiate";
    name.Buffer = packageName;
    name.Length = static_cast<USHORT>(sizeof(packageName) - 1);
    name.MaximumLength = static_cast<USHORT>(sizeof(packageName));

    ULONG id = 0;
    status = ::LsaLookupAuthenticationPackage(lsa, &name, &id);
    ::LsaDeregisterLogonProcess(lsa);

    if (status != 0) {
        XPLOG_ERROR("LsaLookupAuthenticationPackage failed: 0x%08lx",
                    static_cast<unsigned long>(status));
        return false;
    }

    *packageId = id;
    return true;
}

} // namespace win32
} // namespace xplogin
