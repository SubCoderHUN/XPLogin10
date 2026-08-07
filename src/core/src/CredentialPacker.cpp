#include "xplogin/CredentialPacker.h"

#include "xplogin/StringUtil.h"

#include <cstring>
#include <limits>

namespace xplogin {
namespace {

// Rounds `value` up to the next multiple of `alignment`.
size_t AlignUp(size_t value, size_t alignment) {
    if (alignment == 0) {
        return value;
    }
    size_t rem = value % alignment;
    return rem == 0 ? value : value + (alignment - rem);
}

void WriteU16LE(std::vector<uint8_t>& buf, size_t offset, uint16_t value) {
    buf[offset + 0] = static_cast<uint8_t>(value & 0xFF);
    buf[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void WriteU32LE(std::vector<uint8_t>& buf, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        buf[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

void WriteU64LE(std::vector<uint8_t>& buf, size_t offset, uint64_t value) {
    for (size_t i = 0; i < 8; ++i) {
        buf[offset + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

void WritePointerSized(std::vector<uint8_t>& buf, size_t offset, uint64_t value,
                       size_t pointerSize) {
    if (pointerSize == 4) {
        WriteU32LE(buf, offset, static_cast<uint32_t>(value));
    } else {
        WriteU64LE(buf, offset, value);
    }
}

uint16_t ReadU16LE(const std::vector<uint8_t>& buf, size_t offset) {
    return static_cast<uint16_t>(buf[offset] | (buf[offset + 1] << 8));
}

uint64_t ReadPointerSized(const std::vector<uint8_t>& buf, size_t offset,
                          size_t pointerSize) {
    uint64_t value = 0;
    for (size_t i = 0; i < pointerSize; ++i) {
        value |= static_cast<uint64_t>(buf[offset + i]) << (8 * i);
    }
    return value;
}

// Writes one UNICODE_STRING header and appends its payload, mirroring
// _UnicodeStringPackedUnicodeStringCopy from the Microsoft sample: the packed
// copy is *not* null terminated and MaximumLength == Length.
void EmitString(std::vector<uint8_t>& buf,
                size_t fieldOffset,
                size_t pointerSize,
                const std::u16string& value,
                PackedStringRef* ref) {
    const uint16_t byteLength = static_cast<uint16_t>(value.size() * sizeof(char16_t));
    const uint64_t payloadOffset = buf.size();

    WriteU16LE(buf, fieldOffset + 0, byteLength);
    WriteU16LE(buf, fieldOffset + 2, byteLength);
    WritePointerSized(buf, fieldOffset + (pointerSize == 4 ? 4 : 8), payloadOffset,
                      pointerSize);

    for (char16_t c : value) {
        buf.push_back(static_cast<uint8_t>(c & 0xFF));
        buf.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
    }

    if (ref) {
        ref->length = byteLength;
        ref->maximumLength = byteLength;
        ref->bufferOffset = payloadOffset;
    }
}

} // namespace

KerbLayout KerbLayout::For(size_t pointerSize) {
    KerbLayout l;
    l.pointerSize = pointerSize;

    // UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWSTR Buffer; }
    // -> 8 bytes on x86, 16 bytes on x64 (4 bytes of padding before Buffer).
    l.unicodeStringSize = (pointerSize == 4) ? 8 : 16;

    // KERB_INTERACTIVE_LOGON starts with a 4-byte enum, then three
    // UNICODE_STRINGs aligned to the pointer width.
    l.messageTypeOffset = 0;
    size_t cursor = AlignUp(4, pointerSize);
    l.domainOffset = cursor;
    cursor += l.unicodeStringSize;
    l.userOffset = cursor;
    cursor += l.unicodeStringSize;
    l.passwordOffset = cursor;
    cursor += l.unicodeStringSize;

    // LUID { LONG LowPart; LONG HighPart; } - 4-byte aligned, 8 bytes wide.
    l.logonIdOffset = AlignUp(cursor, 4);
    cursor = l.logonIdOffset + 8;

    // The whole struct is padded to its own alignment (the pointer width).
    l.totalSize = AlignUp(cursor, pointerSize);
    return l;
}

KerbMessageType MessageTypeForScenario(UsageScenario scenario) {
    switch (scenario) {
        case UsageScenario::UnlockWorkstation:
            return KerbMessageType::WorkstationUnlockLogon;
        case UsageScenario::Logon:
        case UsageScenario::ChangePassword:
        case UsageScenario::CredUI:
        case UsageScenario::Invalid:
        default:
            return KerbMessageType::InteractiveLogon;
    }
}

bool ShouldProtectPassword(UsageScenario scenario, bool alreadyProtected) {
    if (alreadyProtected) {
        return false;
    }
    return scenario == UsageScenario::CredUI;
}

PackStatus PackKerbInteractiveUnlockLogon(const std::u16string& domain,
                                          const std::u16string& username,
                                          const std::u16string& password,
                                          KerbMessageType messageType,
                                          uint64_t logonId,
                                          size_t pointerSize,
                                          PackedCredential* out) {
    if (!out) {
        return PackStatus::BadPointerSize;
    }
    out->bytes.clear();
    out->domain = PackedStringRef{};
    out->user = PackedStringRef{};
    out->password = PackedStringRef{};

    if (pointerSize != 4 && pointerSize != 8) {
        return PackStatus::BadPointerSize;
    }
    if (username.empty()) {
        return PackStatus::EmptyUserName;
    }

    const size_t kMaxChars = std::numeric_limits<uint16_t>::max() / sizeof(char16_t);
    if (domain.size() > kMaxChars || username.size() > kMaxChars ||
        password.size() > kMaxChars) {
        return PackStatus::StringTooLong;
    }

    const KerbLayout layout = KerbLayout::For(pointerSize);

    std::vector<uint8_t>& buf = out->bytes;
    buf.assign(layout.totalSize, 0);

    WriteU32LE(buf, layout.messageTypeOffset, static_cast<uint32_t>(messageType));
    WriteU64LE(buf, layout.logonIdOffset, logonId);

    // Order matters only for readability, but keeping the sample's order
    // (domain, user, password) makes blobs diffable against a reference dump.
    EmitString(buf, layout.domainOffset, pointerSize, domain, &out->domain);
    EmitString(buf, layout.userOffset, pointerSize, username, &out->user);
    EmitString(buf, layout.passwordOffset, pointerSize, password, &out->password);

    return PackStatus::Ok;
}

PackStatus PackKerbInteractiveUnlockLogon(const LogonRequest& request,
                                          uint64_t logonId,
                                          size_t pointerSize,
                                          PackedCredential* out) {
    std::u16string domain = WideToU16(request.domain);
    std::u16string user = WideToU16(request.username);
    std::u16string password = WideToU16(request.password);

    // A UPN ("bill@contoso.com") must go in the username with an empty domain.
    if (domain.empty() && request.username.find(L'\\') != std::wstring::npos) {
        std::wstring d;
        std::wstring u;
        if (SplitQualifiedName(request.username, &d, &u)) {
            domain = WideToU16(d);
            user = WideToU16(u);
        }
    }

    PackStatus status = PackKerbInteractiveUnlockLogon(
        domain, user, password, MessageTypeForScenario(request.scenario), logonId,
        pointerSize, out);

    SecureClear(password);
    return status;
}

bool ReadPackedString(const PackedCredential& packed,
                      size_t fieldOffset,
                      size_t pointerSize,
                      std::u16string* out) {
    if (out) {
        out->clear();
    }
    if (pointerSize != 4 && pointerSize != 8) {
        return false;
    }

    const std::vector<uint8_t>& buf = packed.bytes;
    const size_t headerEnd = fieldOffset + (pointerSize == 4 ? 8u : 16u);
    if (headerEnd > buf.size()) {
        return false;
    }

    const uint16_t length = ReadU16LE(buf, fieldOffset);
    const uint64_t offset =
        ReadPointerSized(buf, fieldOffset + (pointerSize == 4 ? 4 : 8), pointerSize);

    if (length % sizeof(char16_t) != 0) {
        return false;
    }
    if (offset > buf.size() || offset + length > buf.size()) {
        return false;
    }

    if (out) {
        out->reserve(length / sizeof(char16_t));
        for (size_t i = 0; i < length; i += 2) {
            const size_t at = static_cast<size_t>(offset) + i;
            out->push_back(static_cast<char16_t>(buf[at] | (buf[at + 1] << 8)));
        }
    }
    return true;
}

bool ValidatePackedCredential(const std::vector<uint8_t>& bytes, size_t pointerSize) {
    if (pointerSize != 4 && pointerSize != 8) {
        return false;
    }
    const KerbLayout layout = KerbLayout::For(pointerSize);
    if (bytes.size() < layout.totalSize) {
        return false;
    }

    const size_t fields[] = {layout.domainOffset, layout.userOffset,
                             layout.passwordOffset};
    for (size_t fieldOffset : fields) {
        const uint16_t length = ReadU16LE(bytes, fieldOffset);
        const uint16_t maxLength = ReadU16LE(bytes, fieldOffset + 2);
        const uint64_t offset = ReadPointerSized(
            bytes, fieldOffset + (pointerSize == 4 ? 4 : 8), pointerSize);

        if (length % sizeof(char16_t) != 0) {
            return false;
        }
        if (maxLength < length) {
            return false;
        }
        if (length == 0) {
            continue; // an empty domain or password is legitimate
        }
        // Payload must live after the header and inside the buffer.
        if (offset < layout.totalSize) {
            return false;
        }
        if (offset + length > bytes.size()) {
            return false;
        }
    }
    return true;
}

} // namespace xplogin
