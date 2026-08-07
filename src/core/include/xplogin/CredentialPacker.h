// XPLogin10 - KERB_INTERACTIVE_UNLOCK_LOGON serialization.
//
// A credential provider does not authenticate anybody. It hands LogonUI a blob
// that LSA understands, and LSA does the real work: validating the password,
// creating the token, building the session and starting the shell. Getting this
// blob wrong is the single most common reason a credential provider "does
// nothing" when you press Enter.
//
// The blob is a KERB_INTERACTIVE_UNLOCK_LOGON followed by the three strings,
// with every UNICODE_STRING::Buffer replaced by a *byte offset* from the start
// of the blob (this is what LsaLogonUser expects from a packed submit buffer).
//
// The layout depends on the pointer width of the process that consumes it, so
// it is expressed here as data rather than as a C struct. That keeps it honest
// and, more importantly, testable on a machine that has no LSA at all: the
// Win32 build asserts at compile time that KerbLayout::For(sizeof(void*))
// matches the real headers, and the unit tests exercise both widths.
#pragma once

#include "xplogin/Types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xplogin {

// KERB_LOGON_SUBMIT_TYPE values we use (ntsecapi.h).
enum class KerbMessageType : uint32_t {
    InteractiveLogon = 2,  // KerbInteractiveLogon
    WorkstationUnlockLogon = 7, // KerbWorkstationUnlockLogon
};

// Byte offsets inside the packed header. `pointerSize` is 4 for a 32-bit
// consumer, 8 for a 64-bit one.
struct KerbLayout {
    size_t pointerSize        = 8;
    size_t unicodeStringSize  = 16; // USHORT + USHORT + (pad) + PTR
    size_t messageTypeOffset  = 0;
    size_t domainOffset       = 8;
    size_t userOffset         = 24;
    size_t passwordOffset     = 40;
    size_t logonIdOffset      = 56;
    size_t totalSize          = 64;

    static KerbLayout For(size_t pointerSize);
};

// Offsets of the three UNICODE_STRING fields inside a packed blob, handed back
// so callers (and tests) can verify what was produced.
struct PackedStringRef {
    uint16_t length        = 0; // bytes, excluding any terminator
    uint16_t maximumLength = 0; // equals `length` in a packed blob
    uint64_t bufferOffset  = 0; // bytes from the start of the blob
};

struct PackedCredential {
    std::vector<uint8_t> bytes;
    PackedStringRef domain;
    PackedStringRef user;
    PackedStringRef password;

    bool empty() const { return bytes.empty(); }
    size_t size() const { return bytes.size(); }
};

enum class PackStatus {
    Ok = 0,
    EmptyUserName,
    StringTooLong,   // a UNICODE_STRING cannot describe more than 65535 bytes
    BadPointerSize,
};

// Builds the blob. `logonId` is the LUID of the session being unlocked; it is
// zero for CPUS_LOGON and filled from CPUS_UNLOCK_WORKSTATION by the caller.
PackStatus PackKerbInteractiveUnlockLogon(const std::u16string& domain,
                                          const std::u16string& username,
                                          const std::u16string& password,
                                          KerbMessageType messageType,
                                          uint64_t logonId,
                                          size_t pointerSize,
                                          PackedCredential* out);

// Convenience overload taking wide strings and a usage scenario.
PackStatus PackKerbInteractiveUnlockLogon(const LogonRequest& request,
                                          uint64_t logonId,
                                          size_t pointerSize,
                                          PackedCredential* out);

// Reads a field back out of a packed blob. Used by the round-trip tests and by
// the provider's self-check in debug builds. Returns false on any bounds
// violation, which is exactly the check LSA performs before trusting the blob.
bool ReadPackedString(const PackedCredential& packed,
                      size_t fieldOffset,
                      size_t pointerSize,
                      std::u16string* out);

// Validates that a blob is internally consistent: every string lies inside the
// buffer, lengths are even, and nothing overlaps the header.
bool ValidatePackedCredential(const std::vector<uint8_t>& bytes, size_t pointerSize);

// The message type LSA needs for a given scenario.
KerbMessageType MessageTypeForScenario(UsageScenario scenario);

// CredProtect policy. The sample credential provider protects the password for
// CPUS_CREDUI only; for logon and unlock LSA wants the plaintext (it is being
// handed over on the secure desktop, in-process, and is zeroed right after).
bool ShouldProtectPassword(UsageScenario scenario, bool alreadyProtected);

} // namespace xplogin
