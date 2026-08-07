// Unit tests: KERB_INTERACTIVE_UNLOCK_LOGON serialization.
//
// If these pass but the blob is still rejected on a real machine, the layout
// constants below are the first place to look - they are the contract with LSA.
#include "xplogin/CredentialPacker.h"
#include "xplogin/StringUtil.h"
#include "xptest.h"

using namespace xplogin;

namespace {

std::u16string U(const wchar_t* text) { return WideToU16(text); }

uint16_t ReadU16(const std::vector<uint8_t>& buf, size_t offset) {
    return static_cast<uint16_t>(buf[offset] | (buf[offset + 1] << 8));
}

uint32_t ReadU32(const std::vector<uint8_t>& buf, size_t offset) {
    uint32_t v = 0;
    for (size_t i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(buf[offset + i]) << (8 * i);
    }
    return v;
}

} // namespace

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

TEST(CredentialPacker, Layout64BitMatchesWindowsHeaders) {
    // These numbers come from sizeof/offsetof on KERB_INTERACTIVE_UNLOCK_LOGON
    // in a 64-bit build with the Windows SDK. Do not "fix" them without
    // re-deriving them there.
    const KerbLayout layout = KerbLayout::For(8);
    CHECK_EQ(layout.unicodeStringSize, size_t(16));
    CHECK_EQ(layout.messageTypeOffset, size_t(0));
    CHECK_EQ(layout.domainOffset, size_t(8));
    CHECK_EQ(layout.userOffset, size_t(24));
    CHECK_EQ(layout.passwordOffset, size_t(40));
    CHECK_EQ(layout.logonIdOffset, size_t(56));
    CHECK_EQ(layout.totalSize, size_t(64));
}

TEST(CredentialPacker, Layout32BitMatchesWindowsHeaders) {
    const KerbLayout layout = KerbLayout::For(4);
    CHECK_EQ(layout.unicodeStringSize, size_t(8));
    CHECK_EQ(layout.domainOffset, size_t(4));
    CHECK_EQ(layout.userOffset, size_t(12));
    CHECK_EQ(layout.passwordOffset, size_t(20));
    CHECK_EQ(layout.logonIdOffset, size_t(28));
    CHECK_EQ(layout.totalSize, size_t(36));
}

#if defined(XPLOGIN_WIN32)
// On Windows the real headers are available, so pin the layout against them.
#include <windows.h>
#include <ntsecapi.h>
TEST(CredentialPacker, LayoutMatchesRealSdkStruct) {
    const KerbLayout layout = KerbLayout::For(sizeof(void*));
    CHECK_EQ(layout.totalSize, sizeof(KERB_INTERACTIVE_UNLOCK_LOGON));
    CHECK_EQ(layout.domainOffset,
             offsetof(KERB_INTERACTIVE_UNLOCK_LOGON, Logon.LogonDomainName));
    CHECK_EQ(layout.userOffset, offsetof(KERB_INTERACTIVE_UNLOCK_LOGON, Logon.UserName));
    CHECK_EQ(layout.passwordOffset,
             offsetof(KERB_INTERACTIVE_UNLOCK_LOGON, Logon.Password));
    CHECK_EQ(layout.logonIdOffset, offsetof(KERB_INTERACTIVE_UNLOCK_LOGON, LogonId));
}
#endif

// ---------------------------------------------------------------------------
// Packing
// ---------------------------------------------------------------------------

TEST(CredentialPacker, PacksHeaderAndPayloadForValidCredentials) {
    PackedCredential packed;
    const PackStatus status = PackKerbInteractiveUnlockLogon(
        U(L"WINBOX"), U(L"Bill"), U(L"hunter2"), KerbMessageType::InteractiveLogon, 0,
        8, &packed);

    REQUIRE_EQ(static_cast<int>(status), static_cast<int>(PackStatus::Ok));

    const KerbLayout layout = KerbLayout::For(8);
    // header + 6 + 4 + 7 characters, two bytes each
    CHECK_EQ(packed.size(), layout.totalSize + (6 + 4 + 7) * 2);

    CHECK_EQ(ReadU32(packed.bytes, layout.messageTypeOffset),
             static_cast<uint32_t>(KerbMessageType::InteractiveLogon));

    // Packed strings are not null terminated and MaximumLength == Length.
    CHECK_EQ(packed.domain.length, uint16_t(12));
    CHECK_EQ(packed.domain.maximumLength, uint16_t(12));
    CHECK_EQ(packed.user.length, uint16_t(8));
    CHECK_EQ(packed.password.length, uint16_t(14));

    // Payloads follow the header, in order, with no gaps.
    CHECK_EQ(packed.domain.bufferOffset, uint64_t(layout.totalSize));
    CHECK_EQ(packed.user.bufferOffset, uint64_t(layout.totalSize + 12));
    CHECK_EQ(packed.password.bufferOffset, uint64_t(layout.totalSize + 12 + 8));

    // The UNICODE_STRING headers in the blob must agree with what we reported.
    CHECK_EQ(ReadU16(packed.bytes, layout.userOffset), uint16_t(8));
    CHECK_EQ(ReadU16(packed.bytes, layout.userOffset + 2), uint16_t(8));
}

TEST(CredentialPacker, RoundTripsEveryFieldAt64Bit) {
    PackedCredential packed;
    REQUIRE_EQ(static_cast<int>(PackKerbInteractiveUnlockLogon(
                   U(L"CONTOSO"), U(L"alice"), U(L"P@ssw0rd!"),
                   KerbMessageType::InteractiveLogon, 0, 8, &packed)),
               static_cast<int>(PackStatus::Ok));

    const KerbLayout layout = KerbLayout::For(8);
    std::u16string value;

    REQUIRE(ReadPackedString(packed, layout.domainOffset, 8, &value));
    CHECK_EQ(value, U(L"CONTOSO"));
    REQUIRE(ReadPackedString(packed, layout.userOffset, 8, &value));
    CHECK_EQ(value, U(L"alice"));
    REQUIRE(ReadPackedString(packed, layout.passwordOffset, 8, &value));
    CHECK_EQ(value, U(L"P@ssw0rd!"));
}

TEST(CredentialPacker, RoundTripsEveryFieldAt32Bit) {
    PackedCredential packed;
    REQUIRE_EQ(static_cast<int>(PackKerbInteractiveUnlockLogon(
                   U(L"WINBOX"), U(L"bob"), U(L"secret"),
                   KerbMessageType::WorkstationUnlockLogon, 0, 4, &packed)),
               static_cast<int>(PackStatus::Ok));

    const KerbLayout layout = KerbLayout::For(4);
    CHECK_EQ(packed.size(), layout.totalSize + (6 + 3 + 6) * 2);

    std::u16string value;
    REQUIRE(ReadPackedString(packed, layout.userOffset, 4, &value));
    CHECK_EQ(value, U(L"bob"));
    REQUIRE(ReadPackedString(packed, layout.passwordOffset, 4, &value));
    CHECK_EQ(value, U(L"secret"));
}

TEST(CredentialPacker, StoresUnlockMessageTypeAndLogonId) {
    PackedCredential packed;
    const uint64_t logonId = 0x0000000700ABCDEFull;
    REQUIRE_EQ(static_cast<int>(PackKerbInteractiveUnlockLogon(
                   U(L"WINBOX"), U(L"bill"), U(L"pw"),
                   KerbMessageType::WorkstationUnlockLogon, logonId, 8, &packed)),
               static_cast<int>(PackStatus::Ok));

    const KerbLayout layout = KerbLayout::For(8);
    CHECK_EQ(ReadU32(packed.bytes, layout.messageTypeOffset), uint32_t(7));

    uint64_t stored = 0;
    for (size_t i = 0; i < 8; ++i) {
        stored |= static_cast<uint64_t>(packed.bytes[layout.logonIdOffset + i])
                  << (8 * i);
    }
    CHECK_EQ(stored, logonId);
}

TEST(CredentialPacker, AcceptsEmptyDomainAndEmptyPassword) {
    PackedCredential packed;
    REQUIRE_EQ(static_cast<int>(PackKerbInteractiveUnlockLogon(
                   U(L""), U(L"kiosk"), U(L""), KerbMessageType::InteractiveLogon, 0, 8,
                   &packed)),
               static_cast<int>(PackStatus::Ok));

    CHECK_EQ(packed.domain.length, uint16_t(0));
    CHECK_EQ(packed.password.length, uint16_t(0));
    CHECK_EQ(packed.user.length, uint16_t(10));
    CHECK(ValidatePackedCredential(packed.bytes, 8));
}

TEST(CredentialPacker, RejectsEmptyUserName) {
    PackedCredential packed;
    CHECK_EQ(static_cast<int>(PackKerbInteractiveUnlockLogon(
                 U(L"WINBOX"), U(L""), U(L"pw"), KerbMessageType::InteractiveLogon, 0,
                 8, &packed)),
             static_cast<int>(PackStatus::EmptyUserName));
    CHECK(packed.empty());
}

TEST(CredentialPacker, RejectsUnsupportedPointerSize) {
    PackedCredential packed;
    CHECK_EQ(static_cast<int>(PackKerbInteractiveUnlockLogon(
                 U(L"W"), U(L"u"), U(L"p"), KerbMessageType::InteractiveLogon, 0, 2,
                 &packed)),
             static_cast<int>(PackStatus::BadPointerSize));
}

TEST(CredentialPacker, RejectsStringsTooLongForUnicodeString) {
    // UNICODE_STRING::Length is a USHORT, so 32768 wide characters is the wall.
    const std::u16string huge(40000, u'a');
    PackedCredential packed;
    CHECK_EQ(static_cast<int>(PackKerbInteractiveUnlockLogon(
                 U(L"WINBOX"), U(L"bill"), huge, KerbMessageType::InteractiveLogon, 0,
                 8, &packed)),
             static_cast<int>(PackStatus::StringTooLong));
}

TEST(CredentialPacker, HandlesNonAsciiAndSurrogatePairs) {
    // A password with an accented character and one outside the BMP: both have
    // to survive as UTF-16 code units.
    const std::u16string password = U(L"jélszó0");
    PackedCredential packed;
    REQUIRE_EQ(static_cast<int>(PackKerbInteractiveUnlockLogon(
                   U(L"WINBOX"), U(L"Zoltán"), password,
                   KerbMessageType::InteractiveLogon, 0, 8, &packed)),
               static_cast<int>(PackStatus::Ok));

    const KerbLayout layout = KerbLayout::For(8);
    std::u16string value;
    REQUIRE(ReadPackedString(packed, layout.userOffset, 8, &value));
    CHECK_EQ(value, U(L"Zoltán"));
    REQUIRE(ReadPackedString(packed, layout.passwordOffset, 8, &value));
    CHECK_EQ(value, password);
    // 6 characters, two bytes each - not 6 bytes.
    CHECK_EQ(packed.user.length, uint16_t(12));
}

// ---------------------------------------------------------------------------
// Validation - these are the checks LSA performs before trusting the blob.
// ---------------------------------------------------------------------------

TEST(CredentialPacker, ValidateAcceptsWellFormedBlob) {
    PackedCredential packed;
    REQUIRE_EQ(static_cast<int>(PackKerbInteractiveUnlockLogon(
                   U(L"WINBOX"), U(L"bill"), U(L"pw"),
                   KerbMessageType::InteractiveLogon, 0, 8, &packed)),
               static_cast<int>(PackStatus::Ok));
    CHECK(ValidatePackedCredential(packed.bytes, 8));
}

TEST(CredentialPacker, ValidateRejectsTruncatedBlob) {
    PackedCredential packed;
    REQUIRE_EQ(static_cast<int>(PackKerbInteractiveUnlockLogon(
                   U(L"WINBOX"), U(L"bill"), U(L"pw"),
                   KerbMessageType::InteractiveLogon, 0, 8, &packed)),
               static_cast<int>(PackStatus::Ok));

    std::vector<uint8_t> truncated = packed.bytes;
    truncated.resize(truncated.size() - 4);
    CHECK_FALSE(ValidatePackedCredential(truncated, 8));

    std::vector<uint8_t> headerOnly(KerbLayout::For(8).totalSize - 1, 0);
    CHECK_FALSE(ValidatePackedCredential(headerOnly, 8));
}

TEST(CredentialPacker, ValidateRejectsOffsetPointingIntoTheHeader) {
    PackedCredential packed;
    REQUIRE_EQ(static_cast<int>(PackKerbInteractiveUnlockLogon(
                   U(L"WINBOX"), U(L"bill"), U(L"pw"),
                   KerbMessageType::InteractiveLogon, 0, 8, &packed)),
               static_cast<int>(PackStatus::Ok));

    // Point the user name at offset 0 - a classic confused-deputy attempt.
    const KerbLayout layout = KerbLayout::For(8);
    for (size_t i = 0; i < 8; ++i) {
        packed.bytes[layout.userOffset + 8 + i] = 0;
    }
    CHECK_FALSE(ValidatePackedCredential(packed.bytes, 8));
}

TEST(CredentialPacker, ValidateRejectsOddLength) {
    PackedCredential packed;
    REQUIRE_EQ(static_cast<int>(PackKerbInteractiveUnlockLogon(
                   U(L"WINBOX"), U(L"bill"), U(L"pw"),
                   KerbMessageType::InteractiveLogon, 0, 8, &packed)),
               static_cast<int>(PackStatus::Ok));

    const KerbLayout layout = KerbLayout::For(8);
    packed.bytes[layout.userOffset] = 7; // odd byte count for a UTF-16 string
    CHECK_FALSE(ValidatePackedCredential(packed.bytes, 8));
}

TEST(CredentialPacker, ReadPackedStringRefusesOutOfBoundsOffset) {
    PackedCredential packed;
    REQUIRE_EQ(static_cast<int>(PackKerbInteractiveUnlockLogon(
                   U(L"WINBOX"), U(L"bill"), U(L"pw"),
                   KerbMessageType::InteractiveLogon, 0, 8, &packed)),
               static_cast<int>(PackStatus::Ok));

    const KerbLayout layout = KerbLayout::For(8);
    packed.bytes[layout.passwordOffset + 8] = 0xFF;
    packed.bytes[layout.passwordOffset + 9] = 0xFF;

    std::u16string value;
    CHECK_FALSE(ReadPackedString(packed, layout.passwordOffset, 8, &value));
}

// ---------------------------------------------------------------------------
// Scenario mapping
// ---------------------------------------------------------------------------

TEST(CredentialPacker, MapsScenarioToMessageType) {
    CHECK_EQ(static_cast<int>(MessageTypeForScenario(UsageScenario::Logon)),
             static_cast<int>(KerbMessageType::InteractiveLogon));
    CHECK_EQ(static_cast<int>(MessageTypeForScenario(UsageScenario::UnlockWorkstation)),
             static_cast<int>(KerbMessageType::WorkstationUnlockLogon));
    CHECK_EQ(static_cast<int>(MessageTypeForScenario(UsageScenario::CredUI)),
             static_cast<int>(KerbMessageType::InteractiveLogon));
}

TEST(CredentialPacker, ProtectsPasswordOnlyForCredUi) {
    CHECK(ShouldProtectPassword(UsageScenario::CredUI, false));
    CHECK_FALSE(ShouldProtectPassword(UsageScenario::Logon, false));
    CHECK_FALSE(ShouldProtectPassword(UsageScenario::UnlockWorkstation, false));
    // Never double-protect: CredProtect on already-protected data corrupts it.
    CHECK_FALSE(ShouldProtectPassword(UsageScenario::CredUI, true));
}

TEST(CredentialPacker, SplitsQualifiedNameFromLogonRequest) {
    LogonRequest request;
    request.username = L"CONTOSO\\alice";
    request.password = L"pw";
    request.scenario = UsageScenario::Logon;

    PackedCredential packed;
    REQUIRE_EQ(static_cast<int>(
                   PackKerbInteractiveUnlockLogon(request, 0, 8, &packed)),
               static_cast<int>(PackStatus::Ok));

    const KerbLayout layout = KerbLayout::For(8);
    std::u16string value;
    REQUIRE(ReadPackedString(packed, layout.domainOffset, 8, &value));
    CHECK_EQ(value, U(L"CONTOSO"));
    REQUIRE(ReadPackedString(packed, layout.userOffset, 8, &value));
    CHECK_EQ(value, U(L"alice"));
}

TEST(CredentialPacker, KeepsUpnIntactWithEmptyDomain) {
    // LSA resolves "alice@contoso.com" itself; splitting it would break it.
    LogonRequest request;
    request.username = L"alice@contoso.com";
    request.password = L"pw";

    PackedCredential packed;
    REQUIRE_EQ(static_cast<int>(
                   PackKerbInteractiveUnlockLogon(request, 0, 8, &packed)),
               static_cast<int>(PackStatus::Ok));

    const KerbLayout layout = KerbLayout::For(8);
    std::u16string value;
    REQUIRE(ReadPackedString(packed, layout.domainOffset, 8, &value));
    CHECK_EQ(value, U(L""));
    REQUIRE(ReadPackedString(packed, layout.userOffset, 8, &value));
    CHECK_EQ(value, U(L"alice@contoso.com"));
}
