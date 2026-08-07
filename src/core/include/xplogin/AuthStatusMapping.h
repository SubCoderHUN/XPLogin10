// XPLogin10 - NTSTATUS -> AuthStatus.
//
// Kept out of the Win32 files so the mapping table itself is unit tested: a
// wrong entry here shows the user the wrong XP error message, and that is
// exactly the sort of bug nobody notices until they are locked out.
#pragma once

#include "xplogin/Types.h"

#include <cstdint>

namespace xplogin {

// Subset of ntstatus.h we care about. Declared locally so the core does not
// need the Windows SDK.
namespace ntstatus {
constexpr int32_t kSuccess              = 0x00000000;
constexpr int32_t kNoSuchUser           = static_cast<int32_t>(0xC0000064);
constexpr int32_t kWrongPassword        = static_cast<int32_t>(0xC000006A);
constexpr int32_t kInvalidLogonHours    = static_cast<int32_t>(0xC000006F);
constexpr int32_t kInvalidWorkstation   = static_cast<int32_t>(0xC0000070);
constexpr int32_t kPasswordExpired      = static_cast<int32_t>(0xC0000071);
constexpr int32_t kAccountDisabled      = static_cast<int32_t>(0xC0000072);
constexpr int32_t kAccountLockedOut     = static_cast<int32_t>(0xC0000234);
constexpr int32_t kPasswordMustChange   = static_cast<int32_t>(0xC0000224);
constexpr int32_t kLogonFailure         = static_cast<int32_t>(0xC000006D);
constexpr int32_t kAccountRestriction   = static_cast<int32_t>(0xC000006E);
constexpr int32_t kLogonTypeNotGranted  = static_cast<int32_t>(0xC000015B);
constexpr int32_t kAccountExpired       = static_cast<int32_t>(0xC0000193);
constexpr int32_t kCancelled            = static_cast<int32_t>(0xC0000120);
} // namespace ntstatus

// `subStatus` is the STATUS_ACCOUNT_RESTRICTION detail LSA reports alongside
// STATUS_LOGON_FAILURE; pass 0 when there is none.
AuthResult AuthResultFromNtStatus(int32_t ntStatus, int32_t subStatus = 0);

// Win32 error codes from LogonUserW (GetLastError), for the direct path.
AuthResult AuthResultFromWin32Error(uint32_t win32Error);

} // namespace xplogin
