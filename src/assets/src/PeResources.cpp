#include "xplogin/assets/PeResources.h"

#include <algorithm>
#include <cstring>

namespace xplogin::assets {
namespace {

// A resource tree is three levels deep (type, name, language). Anything deeper
// is malformed, and a self-referential offset would otherwise recurse forever.
constexpr int kMaxResourceDepth = 4;

// Guard against a file claiming an absurd number of entries.
constexpr uint32_t kMaxResourceEntries = 200000;

bool ReadU16(const std::vector<uint8_t>& data, size_t offset, uint16_t* value) {
    if (offset + 2 > data.size()) {
        return false;
    }
    *value = static_cast<uint16_t>(data[offset] | (data[offset + 1] << 8));
    return true;
}

bool ReadU32(const std::vector<uint8_t>& data, size_t offset, uint32_t* value) {
    if (offset + 4 > data.size()) {
        return false;
    }
    *value = static_cast<uint32_t>(data[offset]) |
             (static_cast<uint32_t>(data[offset + 1]) << 8) |
             (static_cast<uint32_t>(data[offset + 2]) << 16) |
             (static_cast<uint32_t>(data[offset + 3]) << 24);
    return true;
}

std::string Utf16ToUtf8(const std::u16string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char32_t cp = in[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < in.size()) {
            const char32_t low = in[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// ResourceId
// ---------------------------------------------------------------------------

std::string ResourceId::ToString() const {
    if (!isName) {
        return "#" + std::to_string(id);
    }
    return Utf16ToUtf8(name);
}

bool ResourceId::IsNamed(const char* ascii) const {
    if (!isName || !ascii) {
        return false;
    }
    const std::string text = Utf16ToUtf8(name);
    if (text.size() != std::strlen(ascii)) {
        return false;
    }
    for (size_t i = 0; i < text.size(); ++i) {
        char a = text[i];
        char b = ascii[i];
        if (a >= 'a' && a <= 'z') a = static_cast<char>(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = static_cast<char>(b - 'a' + 'A');
        if (a != b) {
            return false;
        }
    }
    return true;
}

const char* PeStatusText(PeStatus status) {
    switch (status) {
        case PeStatus::Ok: return "ok";
        case PeStatus::NotAPeFile: return "not a PE file";
        case PeStatus::UnsupportedMachine: return "unsupported machine type";
        case PeStatus::Truncated: return "file is truncated";
        case PeStatus::NoResourceSection: return "no resource section";
        case PeStatus::MalformedResourceTree: return "malformed resource tree";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// PeFile
// ---------------------------------------------------------------------------

bool PeFile::RvaToOffset(uint32_t rva, uint32_t size, size_t* offset) const {
    for (const Section& section : sections_) {
        if (rva < section.virtualAddress) {
            continue;
        }
        const uint32_t delta = rva - section.virtualAddress;
        // The virtual size can exceed the raw size (zero-filled tail); data we
        // want to read must be inside what is physically in the file.
        if (delta >= section.rawSize) {
            continue;
        }
        if (size > section.rawSize - delta) {
            continue;
        }
        const size_t candidate = static_cast<size_t>(section.rawOffset) + delta;
        if (candidate + size > bytes_.size()) {
            return false;
        }
        *offset = candidate;
        return true;
    }
    return false;
}

PeStatus PeFile::Load(std::vector<uint8_t> bytes, PeFile* out) {
    if (!out) {
        return PeStatus::NotAPeFile;
    }
    *out = PeFile();

    if (bytes.size() < 0x40) {
        return PeStatus::Truncated;
    }
    if (bytes[0] != 'M' || bytes[1] != 'Z') {
        return PeStatus::NotAPeFile;
    }

    uint32_t peOffset = 0;
    if (!ReadU32(bytes, 0x3C, &peOffset)) {
        return PeStatus::Truncated;
    }
    if (peOffset + 24 > bytes.size()) {
        return PeStatus::Truncated;
    }
    if (bytes[peOffset] != 'P' || bytes[peOffset + 1] != 'E' ||
        bytes[peOffset + 2] != 0 || bytes[peOffset + 3] != 0) {
        return PeStatus::NotAPeFile;
    }

    uint16_t machine = 0;
    uint16_t sectionCount = 0;
    uint16_t optionalHeaderSize = 0;
    if (!ReadU16(bytes, peOffset + 4, &machine) ||
        !ReadU16(bytes, peOffset + 6, &sectionCount) ||
        !ReadU16(bytes, peOffset + 20, &optionalHeaderSize)) {
        return PeStatus::Truncated;
    }

    const size_t optionalHeaderOffset = peOffset + 24;
    uint16_t optionalMagic = 0;
    if (!ReadU16(bytes, optionalHeaderOffset, &optionalMagic)) {
        return PeStatus::Truncated;
    }

    bool is64Bit = false;
    size_t dataDirectoryOffset = 0;
    if (optionalMagic == 0x10B) { // PE32
        dataDirectoryOffset = optionalHeaderOffset + 96;
    } else if (optionalMagic == 0x20B) { // PE32+
        is64Bit = true;
        dataDirectoryOffset = optionalHeaderOffset + 112;
    } else {
        return PeStatus::NotAPeFile;
    }

    // Data directory entry 2 is the resource table.
    const size_t resourceDirEntry = dataDirectoryOffset + 2 * 8;
    uint32_t resourceRva = 0;
    uint32_t resourceSize = 0;
    if (!ReadU32(bytes, resourceDirEntry, &resourceRva) ||
        !ReadU32(bytes, resourceDirEntry + 4, &resourceSize)) {
        return PeStatus::Truncated;
    }

    // Section table follows the optional header.
    const size_t sectionTableOffset = optionalHeaderOffset + optionalHeaderSize;
    std::vector<Section> sections;
    sections.reserve(sectionCount);
    for (uint16_t i = 0; i < sectionCount; ++i) {
        const size_t base = sectionTableOffset + static_cast<size_t>(i) * 40;
        if (base + 40 > bytes.size()) {
            return PeStatus::Truncated;
        }
        Section section;
        ReadU32(bytes, base + 8, &section.virtualSize);
        ReadU32(bytes, base + 12, &section.virtualAddress);
        ReadU32(bytes, base + 16, &section.rawSize);
        ReadU32(bytes, base + 20, &section.rawOffset);

        // A section claiming data past the end of the file is not usable.
        if (static_cast<size_t>(section.rawOffset) + section.rawSize >
            bytes.size()) {
            section.rawSize =
                section.rawOffset < bytes.size()
                    ? static_cast<uint32_t>(bytes.size() - section.rawOffset)
                    : 0;
        }
        sections.push_back(section);
    }

    out->bytes_ = std::move(bytes);
    out->sections_ = std::move(sections);
    out->machine_ = machine;
    out->is64Bit_ = is64Bit;

    if (resourceRva == 0 || resourceSize == 0) {
        return PeStatus::NoResourceSection;
    }

    size_t resourceBase = 0;
    if (!out->RvaToOffset(resourceRva, 1, &resourceBase)) {
        return PeStatus::NoResourceSection;
    }

    // ---- walk the three-level tree -------------------------------------
    // Offsets inside the tree are relative to the start of the resource
    // section, not to the file.
    struct Frame {
        size_t     directoryOffset; // relative to resourceBase
        int        depth;
        ResourceId type;
        ResourceId name;
    };

    std::vector<Frame> stack;
    stack.push_back(Frame{0, 0, ResourceId{}, ResourceId{}});

    const std::vector<uint8_t>& data = out->bytes_;
    uint32_t entriesSeen = 0;

    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();

        if (frame.depth >= kMaxResourceDepth) {
            return PeStatus::MalformedResourceTree;
        }

        const size_t dirOffset = resourceBase + frame.directoryOffset;
        uint16_t namedCount = 0;
        uint16_t idCount = 0;
        if (!ReadU16(data, dirOffset + 12, &namedCount) ||
            !ReadU16(data, dirOffset + 14, &idCount)) {
            return PeStatus::MalformedResourceTree;
        }

        const uint32_t total = static_cast<uint32_t>(namedCount) + idCount;
        entriesSeen += total;
        if (entriesSeen > kMaxResourceEntries) {
            return PeStatus::MalformedResourceTree;
        }

        for (uint32_t i = 0; i < total; ++i) {
            const size_t entryOffset = dirOffset + 16 + static_cast<size_t>(i) * 8;
            uint32_t nameField = 0;
            uint32_t dataField = 0;
            if (!ReadU32(data, entryOffset, &nameField) ||
                !ReadU32(data, entryOffset + 4, &dataField)) {
                return PeStatus::MalformedResourceTree;
            }

            ResourceId identifier;
            if (nameField & 0x80000000u) {
                const size_t stringOffset =
                    resourceBase + (nameField & 0x7FFFFFFFu);
                uint16_t length = 0;
                if (!ReadU16(data, stringOffset, &length)) {
                    return PeStatus::MalformedResourceTree;
                }
                if (stringOffset + 2 + static_cast<size_t>(length) * 2 >
                    data.size()) {
                    return PeStatus::MalformedResourceTree;
                }
                identifier.isName = true;
                identifier.name.reserve(length);
                for (uint16_t c = 0; c < length; ++c) {
                    uint16_t unit = 0;
                    ReadU16(data, stringOffset + 2 + static_cast<size_t>(c) * 2,
                            &unit);
                    identifier.name.push_back(static_cast<char16_t>(unit));
                }
            } else {
                identifier.id = nameField;
            }

            if (dataField & 0x80000000u) {
                Frame child;
                child.directoryOffset = dataField & 0x7FFFFFFFu;
                child.depth = frame.depth + 1;
                child.type = frame.depth == 0 ? identifier : frame.type;
                child.name = frame.depth == 1 ? identifier : frame.name;
                if (resourceBase + child.directoryOffset + 16 > data.size()) {
                    return PeStatus::MalformedResourceTree;
                }
                stack.push_back(child);
                continue;
            }

            // Leaf: IMAGE_RESOURCE_DATA_ENTRY
            const size_t leafOffset = resourceBase + dataField;
            ResourceEntry entry;
            if (!ReadU32(data, leafOffset, &entry.rva) ||
                !ReadU32(data, leafOffset + 4, &entry.size) ||
                !ReadU32(data, leafOffset + 8, &entry.codePage)) {
                return PeStatus::MalformedResourceTree;
            }
            entry.type = frame.depth == 0 ? identifier : frame.type;
            entry.name = frame.depth == 1 ? identifier : frame.name;
            entry.language = (frame.depth == 2 && !identifier.isName)
                                 ? identifier.id
                                 : 0;

            // Drop anything that does not actually resolve, rather than
            // handing the caller an entry that will fail to read later.
            size_t unused = 0;
            if (entry.size > 0 && out->RvaToOffset(entry.rva, entry.size, &unused)) {
                out->resources_.push_back(std::move(entry));
            }
        }
    }

    // Tree order: the stack walk reverses it, so sort into something stable and
    // human-readable for the extractor's output.
    std::stable_sort(out->resources_.begin(), out->resources_.end(),
                     [](const ResourceEntry& a, const ResourceEntry& b) {
                         if (a.type.isName != b.type.isName) {
                             return !a.type.isName;
                         }
                         if (a.type.isName) {
                             if (a.type.name != b.type.name) {
                                 return a.type.name < b.type.name;
                             }
                         } else if (a.type.id != b.type.id) {
                             return a.type.id < b.type.id;
                         }
                         if (a.name.isName != b.name.isName) {
                             return !a.name.isName;
                         }
                         if (a.name.isName) {
                             return a.name.name < b.name.name;
                         }
                         return a.name.id < b.name.id;
                     });

    return PeStatus::Ok;
}

std::vector<uint8_t> PeFile::Read(const ResourceEntry& entry) const {
    size_t offset = 0;
    if (!RvaToOffset(entry.rva, entry.size, &offset)) {
        return {};
    }
    return std::vector<uint8_t>(bytes_.begin() + static_cast<long>(offset),
                                bytes_.begin() +
                                    static_cast<long>(offset + entry.size));
}

std::vector<ResourceEntry> PeFile::OfType(uint32_t typeId) const {
    std::vector<ResourceEntry> out;
    for (const ResourceEntry& entry : resources_) {
        if (entry.type.Is(typeId)) {
            out.push_back(entry);
        }
    }
    return out;
}

std::vector<ResourceEntry> PeFile::OfNamedType(const char* asciiType) const {
    std::vector<ResourceEntry> out;
    for (const ResourceEntry& entry : resources_) {
        if (entry.type.IsNamed(asciiType)) {
            out.push_back(entry);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------

std::vector<uint8_t> BitmapResourceToFile(const std::vector<uint8_t>& resource) {
    // The resource starts at BITMAPINFOHEADER; a .bmp file needs the 14-byte
    // BITMAPFILEHEADER in front, with bfOffBits pointing past the colour table.
    if (resource.size() < 40) {
        return {};
    }

    uint32_t headerSize = 0;
    ReadU32(resource, 0, &headerSize);
    if (headerSize < 40 || headerSize > resource.size()) {
        return {};
    }

    uint16_t bitCount = 0;
    uint32_t compression = 0;
    uint32_t colorsUsed = 0;
    ReadU16(resource, 14, &bitCount);
    ReadU32(resource, 16, &compression);
    ReadU32(resource, 32, &colorsUsed);

    // Palette size: explicit when given, otherwise implied by the bit depth.
    uint32_t paletteEntries = colorsUsed;
    if (paletteEntries == 0 && bitCount <= 8) {
        paletteEntries = 1u << bitCount;
    }
    uint32_t paletteBytes = paletteEntries * 4;

    // BI_BITFIELDS puts three masks where the palette would be.
    if (compression == 3 && headerSize == 40) {
        paletteBytes += 12;
    }

    const uint64_t pixelOffset =
        14ull + headerSize + paletteBytes;
    if (pixelOffset > 14ull + resource.size()) {
        return {};
    }

    std::vector<uint8_t> file;
    file.reserve(14 + resource.size());
    const uint32_t fileSize = static_cast<uint32_t>(14 + resource.size());

    file.push_back('B');
    file.push_back('M');
    for (int i = 0; i < 4; ++i) {
        file.push_back(static_cast<uint8_t>((fileSize >> (8 * i)) & 0xFF));
    }
    file.insert(file.end(), 4, 0); // bfReserved1/2
    for (int i = 0; i < 4; ++i) {
        file.push_back(static_cast<uint8_t>((pixelOffset >> (8 * i)) & 0xFF));
    }
    file.insert(file.end(), resource.begin(), resource.end());
    return file;
}

std::vector<StringTableEntry> ParseStringBundle(const std::vector<uint8_t>& resource,
                                                uint32_t bundleId) {
    std::vector<StringTableEntry> out;
    if (bundleId == 0) {
        return out;
    }

    const uint32_t firstId = (bundleId - 1) * 16;
    size_t offset = 0;
    for (uint32_t index = 0; index < 16; ++index) {
        uint16_t length = 0;
        if (!ReadU16(resource, offset, &length)) {
            break;
        }
        offset += 2;
        if (length == 0) {
            continue; // an empty slot, not the end of the bundle
        }
        if (offset + static_cast<size_t>(length) * 2 > resource.size()) {
            break;
        }

        StringTableEntry entry;
        entry.id = firstId + index;
        entry.text.reserve(length);
        for (uint16_t c = 0; c < length; ++c) {
            uint16_t unit = 0;
            ReadU16(resource, offset + static_cast<size_t>(c) * 2, &unit);
            entry.text.push_back(static_cast<char16_t>(unit));
        }
        offset += static_cast<size_t>(length) * 2;
        out.push_back(std::move(entry));
    }
    return out;
}

std::string ResourceToUtf8Text(const std::vector<uint8_t>& resource) {
    if (resource.empty()) {
        return {};
    }

    // UTF-16LE BOM, or a strong hint of UTF-16 (ASCII text has a zero in every
    // second byte). DirectUI's UIFILE resources are plain UTF-8 in XP, but
    // other markup resources are not, so detect rather than assume.
    const bool hasUtf16Bom =
        resource.size() >= 2 && resource[0] == 0xFF && resource[1] == 0xFE;

    bool looksUtf16 = hasUtf16Bom;
    if (!looksUtf16 && resource.size() >= 16) {
        size_t zeros = 0;
        const size_t sample = std::min<size_t>(resource.size(), 64);
        for (size_t i = 1; i < sample; i += 2) {
            if (resource[i] == 0) {
                ++zeros;
            }
        }
        looksUtf16 = zeros > (sample / 2) * 3 / 4;
    }

    if (looksUtf16) {
        std::u16string wide;
        for (size_t i = hasUtf16Bom ? 2 : 0; i + 1 < resource.size(); i += 2) {
            wide.push_back(
                static_cast<char16_t>(resource[i] | (resource[i + 1] << 8)));
        }
        return Utf16ToUtf8(wide);
    }

    size_t start = 0;
    if (resource.size() >= 3 && resource[0] == 0xEF && resource[1] == 0xBB &&
        resource[2] == 0xBF) {
        start = 3;
    }
    return std::string(resource.begin() + static_cast<long>(start),
                       resource.end());
}

} // namespace xplogin::assets
