#include "xplogin/assets/AssetPack.h"

#include <cstring>

namespace xplogin::assets {
namespace {

// "XPA1" - bumped if the layout below ever changes.
constexpr uint32_t kMagic = 0x31415058;
constexpr uint32_t kVersion = 1;
constexpr size_t   kHeaderSize = 16;   // magic, version, count, reserved
constexpr size_t   kIndexEntrySize = 20; // id, width, height, offset, size

// XP's transparent colour in the 24bpp artwork.
constexpr uint32_t kMagenta = 0x00FF00FFu;

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
    }
}

void SetU32(std::vector<uint8_t>& out, size_t at, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out[at + static_cast<size_t>(i)] =
            static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

bool GetU32(const std::vector<uint8_t>& in, size_t at, uint32_t* value) {
    if (at + 4 > in.size()) {
        return false;
    }
    *value = static_cast<uint32_t>(in[at]) |
             (static_cast<uint32_t>(in[at + 1]) << 8) |
             (static_cast<uint32_t>(in[at + 2]) << 16) |
             (static_cast<uint32_t>(in[at + 3]) << 24);
    return true;
}

} // namespace

const std::vector<AssetSpec>& WelcomeScreenAssets() {
    // The transparency keys are not guesses: each one is the third argument of
    // the rcbmp() call that references the bitmap in logonui.exe's UIFILE
    // markup, quoted in the comment beside it. A first pass at this table
    // assumed magenta everywhere and was wrong for eight of them - the small
    // icons declare -1, which means no key at all.
    static const std::vector<AssetSpec> assets = {
        {assetid::kBackgroundGlow,   0x000000u,   "background light source"},   // rcbmp(100,0,0,219rp,207rp,1,0)
        {assetid::kPasswordBox,      kMagenta,    "password field background"}, // rcbmp(102,6,#FF00FF,...)
        {assetid::kGoButton,         kNoColorKey, "green go arrow"},            // rcbmp(103,3,-1,26rp,26rp,0,0)
        {assetid::kGoButtonFocused,  kNoColorKey, "green go arrow, focused"},   // rcbmp(104,3,-1,...)
        {assetid::kHelpButton,       kNoColorKey, "blue question mark"},        // rcbmp(105,3,-1,28rp,28rp,0,0)
        {assetid::kHelpButtonFocused,kNoColorKey, "blue question mark, focused"},// rcbmp(106,3,-1,...)
        {assetid::kPowerIcon,        kNoColorKey, "turn off computer"},         // rcbmp(107,3,-1,26rp,26rp,0,0)
        {assetid::kUndockIcon,       kNoColorKey, "undock computer"},           // rcbmp(108,3,-1,...)
        // 109 and 110 are the one place the markup and the artwork disagree.
        // Both declare -1, but both bitmaps have magenta in their four rounded
        // corners - 12 pixels each - and nowhere else. Taking the markup at its
        // word would put four magenta specks on the scrollbar, so the artwork
        // wins here. (DirectUI keys scrollbar parts through the scrollviewer
        // sheet rather than the rcbmp argument, which explains the -1.)
        {assetid::kScrollUp,         kMagenta,    "scroll up"},                 // rcbmp(109,3,-1,...) - see above
        {assetid::kScrollDown,       kMagenta,    "scroll down"},               // rcbmp(110,3,-1,...) - see above
        {assetid::kScrollTrack,      kMagenta,    "scrollbar strip"},           // rcbmp(111,6,#FF00FF,...)
        {assetid::kSelectedTile,     kMagenta,    "selected account background"},// rcbmp(112,6,#FF00FF,...)
        // 113 and 119 declare a key of 255 rather than a colour. They are the
        // only 32bpp bitmaps in the set and they carry a real alpha channel, so
        // that is a "use the alpha" marker, not a palette index - keying on it
        // would punch holes in the frame.
        {assetid::kPictureFrame,     kNoColorKey, "account picture frame"},     // rcbmp(113,7,255,...)
        {assetid::kPictureFrameHot,  kNoColorKey, "account picture frame, hot"},// rcbmp(119,7,255,...)
        {assetid::kLogo,             kNoColorKey, "Windows XP logo"},           // rcbmp(123,3,-1,137,86,0,0)
        {assetid::kGradientStrip,    kMagenta,    "1px vertical gradient"},     // rcbmp(124,6,#FF00FF,...)
        {assetid::kTopDivider,       kMagenta,    "header divider sweep"},      // rcbmp(125,6,#FF00FF,...)
        {assetid::kBottomDivider,    kMagenta,    "footer divider sweep"},      // rcbmp(126,6,#FF00FF,...)

        // Present in logonui.exe but never referenced by the welcome screen's
        // markup - the classic-logon and dialog paths use them. Baked anyway
        // because they cost little and the alternative is discovering they are
        // needed on a machine that cannot log in.
        {assetid::kDefaultPicture,   kNoColorKey, "default account picture"},
        {assetid::kKeyboardIcon,     kNoColorKey, "accessibility keyboard"},
        {assetid::kKeyboardIconHot,  kNoColorKey, "accessibility keyboard, hot"},
        {assetid::kLogoShadow,       kNoColorKey, "Windows XP logo, shadow"},

        // --- msgina.dll: the "Turn off computer" dialog --------------------
        //
        // No markup to quote here: msgina draws this with a plain Win32
        // dialog (template #20100) and owner-drawn buttons, not DirectUI. The
        // keys come from the artwork instead. The panel is opaque - it is the
        // dialog's whole background - and the orb strip keys on magenta,
        // which is what fills the rounded corners of all ten frames.
        {assetid::kShutdownPanel, kNoColorKey,
         "turn off computer: dialog background", AssetSource::MsGina},
        {assetid::kShutdownFlag,  kNoColorKey,
         "turn off computer: Windows flag", AssetSource::MsGina},
        {assetid::kShutdownOrbs,  kMagenta,
         "turn off computer: orb strip", AssetSource::MsGina},
    };
    return assets;
}

const char* AssetSourceFileName(AssetSource source) {
    switch (source) {
        case AssetSource::LogonUi: return "logonui.exe";
        case AssetSource::MsGina:  return "msgina.dll";
    }
    return "?";
}

const char* PackStatusText(PackStatus status) {
    switch (status) {
        case PackStatus::Ok: return "ok";
        case PackStatus::BadMagic: return "not an XPLogin asset pack";
        case PackStatus::UnsupportedVersion: return "unsupported pack version";
        case PackStatus::Truncated: return "pack is truncated";
        case PackStatus::CorruptIndex: return "pack index is corrupt";
    }
    return "?";
}

std::vector<uint8_t> AssetPack::Build(const std::map<uint32_t, Image>& images) {
    std::vector<uint8_t> out;

    PutU32(out, kMagic);
    PutU32(out, kVersion);
    PutU32(out, static_cast<uint32_t>(images.size()));
    PutU32(out, 0); // reserved

    // Index first, with placeholder offsets; the payload offsets are only known
    // once the index length is fixed, which it is as soon as the count is.
    const size_t indexStart = out.size();
    out.resize(indexStart + images.size() * kIndexEntrySize, 0);

    size_t entry = indexStart;
    for (const auto& item : images) {
        const Image& image = item.second;
        const uint32_t byteCount =
            static_cast<uint32_t>(image.PixelCount() * sizeof(uint32_t));

        SetU32(out, entry + 0, item.first);
        SetU32(out, entry + 4, image.width);
        SetU32(out, entry + 8, image.height);
        SetU32(out, entry + 12, static_cast<uint32_t>(out.size()));
        SetU32(out, entry + 16, byteCount);
        entry += kIndexEntrySize;

        for (uint32_t pixel : image.pixels) {
            PutU32(out, pixel);
        }
    }
    return out;
}

PackStatus AssetPack::Load(const std::vector<uint8_t>& bytes, AssetPack* out) {
    if (!out) {
        return PackStatus::CorruptIndex;
    }
    *out = AssetPack();

    if (bytes.size() < kHeaderSize) {
        return PackStatus::Truncated;
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t count = 0;
    GetU32(bytes, 0, &magic);
    GetU32(bytes, 4, &version);
    GetU32(bytes, 8, &count);

    if (magic != kMagic) {
        return PackStatus::BadMagic;
    }
    if (version != kVersion) {
        return PackStatus::UnsupportedVersion;
    }
    if (kHeaderSize + static_cast<size_t>(count) * kIndexEntrySize > bytes.size()) {
        return PackStatus::Truncated;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const size_t entry = kHeaderSize + static_cast<size_t>(i) * kIndexEntrySize;
        uint32_t id = 0, width = 0, height = 0, offset = 0, size = 0;
        GetU32(bytes, entry + 0, &id);
        GetU32(bytes, entry + 4, &width);
        GetU32(bytes, entry + 8, &height);
        GetU32(bytes, entry + 12, &offset);
        GetU32(bytes, entry + 16, &size);

        // Every claim in the index is checked against the buffer before it is
        // used. The pack is our own file, but it arrives from disk and a
        // truncated copy must not become an out-of-bounds read inside LogonUI.
        const uint64_t expected =
            static_cast<uint64_t>(width) * height * sizeof(uint32_t);
        if (width == 0 || height == 0 || expected != size) {
            return PackStatus::CorruptIndex;
        }
        if (static_cast<uint64_t>(offset) + size > bytes.size()) {
            return PackStatus::Truncated;
        }

        Image image;
        image.width = width;
        image.height = height;
        image.pixels.resize(static_cast<size_t>(width) * height);
        for (size_t p = 0; p < image.pixels.size(); ++p) {
            GetU32(bytes, offset + p * 4, &image.pixels[p]);
        }
        out->images_.emplace(id, std::move(image));
    }
    return PackStatus::Ok;
}

const Image* AssetPack::Get(uint32_t id) const {
    auto it = images_.find(id);
    return it == images_.end() ? nullptr : &it->second;
}

std::vector<uint32_t> AssetPack::Ids() const {
    std::vector<uint32_t> ids;
    ids.reserve(images_.size());
    for (const auto& item : images_) {
        ids.push_back(item.first);
    }
    return ids;
}

} // namespace xplogin::assets
