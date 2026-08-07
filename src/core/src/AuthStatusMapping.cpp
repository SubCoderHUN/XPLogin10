#include "xplogin/AuthStatusMapping.h"

namespace xplogin {
namespace {

// Win32 error codes (winerror.h) produced by LogonUserW.
constexpr uint32_t kErrorLogonFailure         = 1326;
constexpr uint32_t kErrorAccountRestriction   = 1327;
constexpr uint32_t kErrorInvalidLogonHours    = 1328;
constexpr uint32_t kErrorInvalidWorkstation   = 1329;
constexpr uint32_t kErrorPasswordExpired      = 1330;
constexpr uint32_t kErrorAccountDisabled      = 1331;
constexpr uint32_t kErrorNoSuchUser           = 1317;
constexpr uint32_t kErrorAccountLockedOut     = 1909;
constexpr uint32_t kErrorPasswordMustChange   = 1907;
constexpr uint32_t kErrorAccountExpired       = 1793;
constexpr uint32_t kErrorLogonTypeNotGranted  = 1385;
constexpr uint32_t kErrorCancelled            = 1223;

AuthResult Make(AuthStatus status, int32_t ntStatus, int32_t subStatus) {
    AuthResult result;
    result.status = status;
    result.ntStatus = ntStatus;
    result.subStatus = subStatus;
    return result;
}

} // namespace

AuthResult AuthResultFromNtStatus(int32_t ntStatus, int32_t subStatus) {
    switch (ntStatus) {
        case ntstatus::kSuccess:
            return Make(AuthStatus::Success, ntStatus, subStatus);

        case ntstatus::kNoSuchUser:
            return Make(AuthStatus::UnknownUser, ntStatus, subStatus);

        case ntstatus::kWrongPassword:
            return Make(AuthStatus::BadPassword, ntStatus, subStatus);

        case ntstatus::kAccountDisabled:
            return Make(AuthStatus::AccountDisabled, ntStatus, subStatus);

        case ntstatus::kAccountLockedOut:
            return Make(AuthStatus::AccountLockedOut, ntStatus, subStatus);

        case ntstatus::kPasswordExpired:
        case ntstatus::kAccountExpired:
            return Make(AuthStatus::PasswordExpired, ntStatus, subStatus);

        case ntstatus::kPasswordMustChange:
            return Make(AuthStatus::PasswordMustChange, ntStatus, subStatus);

        case ntstatus::kInvalidLogonHours:
            return Make(AuthStatus::TimeRestriction, ntStatus, subStatus);

        case ntstatus::kLogonTypeNotGranted:
        case ntstatus::kInvalidWorkstation:
            return Make(AuthStatus::LogonTypeNotGranted, ntStatus, subStatus);

        case ntstatus::kCancelled:
            return Make(AuthStatus::Cancelled, ntStatus, subStatus);

        case ntstatus::kLogonFailure:
        case ntstatus::kAccountRestriction:
            // LSA deliberately collapses "no such user" and "wrong password"
            // into STATUS_LOGON_FAILURE so an attacker cannot enumerate
            // accounts. The substatus, when present, carries the real reason -
            // it is only populated for callers holding SeTcbPrivilege.
            if (subStatus != 0 && subStatus != ntstatus::kLogonFailure) {
                AuthResult detailed = AuthResultFromNtStatus(subStatus, 0);
                if (detailed.status != AuthStatus::InternalError) {
                    detailed.ntStatus = ntStatus;
                    detailed.subStatus = subStatus;
                    return detailed;
                }
            }
            return Make(AuthStatus::BadPassword, ntStatus, subStatus);

        default:
            return Make(AuthStatus::InternalError, ntStatus, subStatus);
    }
}

AuthResult AuthResultFromWin32Error(uint32_t win32Error) {
    switch (win32Error) {
        case 0:
            return Make(AuthStatus::Success, 0, 0);
        case kErrorNoSuchUser:
            return Make(AuthStatus::UnknownUser, 0, 0);
        case kErrorLogonFailure:
            return Make(AuthStatus::BadPassword, 0, 0);
        case kErrorAccountDisabled:
            return Make(AuthStatus::AccountDisabled, 0, 0);
        case kErrorAccountLockedOut:
            return Make(AuthStatus::AccountLockedOut, 0, 0);
        case kErrorPasswordExpired:
        case kErrorAccountExpired:
            return Make(AuthStatus::PasswordExpired, 0, 0);
        case kErrorPasswordMustChange:
            return Make(AuthStatus::PasswordMustChange, 0, 0);
        case kErrorInvalidLogonHours:
            return Make(AuthStatus::TimeRestriction, 0, 0);
        case kErrorLogonTypeNotGranted:
        case kErrorInvalidWorkstation:
        case kErrorAccountRestriction:
            return Make(AuthStatus::LogonTypeNotGranted, 0, 0);
        case kErrorCancelled:
            return Make(AuthStatus::Cancelled, 0, 0);
        default:
            return Make(AuthStatus::InternalError, 0, 0);
    }
}

} // namespace xplogin
