// XPLogin10 - reading resources out of a Windows PE file.
//
// This is how the project recovers Microsoft's original geometry and artwork
// instead of guessing at it: logonui.exe carries the welcome screen's DirectUI
// markup and bitmaps, msgina.dll the classic dialogs, luna.msstyles the whole
// Luna visual style. All three are ordinary PE files with a .rsrc section.
//
// Deliberately hand-rolled and platform independent, for two reasons: the
// extraction runs on the build machine (which may not be Windows), and parsing
// untrusted binaries with LoadLibrary is a bad idea when a plain byte reader
// will do. Every offset in here is bounds checked against the buffer, and the
// resource tree walk has a depth limit - a malformed file must return an error,
// never loop or read out of bounds.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xplogin::assets {

// Resource type ids from winuser.h, so the tool can name what it finds.
namespace restype {
constexpr uint32_t kCursor       = 1;
constexpr uint32_t kBitmap       = 2;
constexpr uint32_t kIcon         = 3;
constexpr uint32_t kMenu         = 4;
constexpr uint32_t kDialog       = 5;
constexpr uint32_t kString       = 6;
constexpr uint32_t kFontDir      = 7;
constexpr uint32_t kFont         = 8;
constexpr uint32_t kAccelerator  = 9;
constexpr uint32_t kRcData       = 10;
constexpr uint32_t kMessageTable = 11;
constexpr uint32_t kGroupCursor  = 12;
constexpr uint32_t kGroupIcon    = 14;
constexpr uint32_t kVersion      = 16;
constexpr uint32_t kManifest     = 24;
} // namespace restype

// A resource is identified either by a numeric id or by a name.
struct ResourceId {
    bool          isName = false;
    uint32_t      id = 0;
    std::u16string name;

    // "#101" or "UIFILE", for logs and file names.
    std::string ToString() const;
    bool Is(uint32_t value) const { return !isName && id == value; }
    bool IsNamed(const char* ascii) const;
};

struct ResourceEntry {
    ResourceId type;
    ResourceId name;
    uint32_t   language = 0;
    uint32_t   rva = 0;      // where the bytes live, as a virtual address
    uint32_t   size = 0;
    uint32_t   codePage = 0;
};

enum class PeStatus {
    Ok = 0,
    NotAPeFile,
    UnsupportedMachine,
    Truncated,
    NoResourceSection,
    MalformedResourceTree,
};

const char* PeStatusText(PeStatus status);

class PeFile {
public:
    // Takes ownership of the bytes. Never throws; failures come back as status.
    static PeStatus Load(std::vector<uint8_t> bytes, PeFile* out);

    const std::vector<ResourceEntry>& Resources() const { return resources_; }

    // The bytes of one resource. Empty when the entry is out of bounds, which
    // for a well-formed file cannot happen - Load already validated every entry.
    std::vector<uint8_t> Read(const ResourceEntry& entry) const;

    // Every entry of a given type, in tree order.
    std::vector<ResourceEntry> OfType(uint32_t typeId) const;
    std::vector<ResourceEntry> OfNamedType(const char* asciiType) const;

    uint16_t Machine() const { return machine_; }
    bool Is64Bit() const { return is64Bit_; }
    size_t ByteSize() const { return bytes_.size(); }

private:
    bool RvaToOffset(uint32_t rva, uint32_t size, size_t* offset) const;

    struct Section {
        uint32_t virtualAddress = 0;
        uint32_t virtualSize = 0;
        uint32_t rawOffset = 0;
        uint32_t rawSize = 0;
    };

    std::vector<uint8_t>       bytes_;
    std::vector<Section>       sections_;
    std::vector<ResourceEntry> resources_;
    uint16_t                   machine_ = 0;
    bool                       is64Bit_ = false;
};

// ---------------------------------------------------------------------------
// Helpers for turning raw resources into something usable
// ---------------------------------------------------------------------------

// RT_BITMAP stores a BITMAPINFOHEADER and pixels with no BITMAPFILEHEADER -
// the loader adds it. Anything writing a .bmp has to put those 14 bytes back,
// and get the pixel offset right, or every dumped bitmap is subtly corrupt.
// Returns an empty vector when the resource is too small to be a bitmap.
std::vector<uint8_t> BitmapResourceToFile(const std::vector<uint8_t>& resource);

// RT_STRING is stored in bundles of 16, each entry a length-prefixed UTF-16
// run with no terminator. `bundleId` is the resource name; the string ids it
// covers are (bundleId - 1) * 16 .. + 15.
struct StringTableEntry {
    uint32_t       id = 0;
    std::u16string text;
};
std::vector<StringTableEntry> ParseStringBundle(const std::vector<uint8_t>& resource,
                                                uint32_t bundleId);

// Best-effort text decode for UIFILE and other markup resources: strips a
// UTF-8/UTF-16 BOM and converts UTF-16LE to UTF-8 when that is what it is.
std::string ResourceToUtf8Text(const std::vector<uint8_t>& resource);

} // namespace xplogin::assets
