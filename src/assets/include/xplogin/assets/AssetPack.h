// XPLogin10 - the baked asset pack.
//
// The build decodes every bitmap the welcome screen needs out of logonui.exe,
// applies the colour keys, and writes the results here as straight BGRA. The
// credential provider then ships one file and, at logon time, does nothing more
// than look up an id and blit.
//
// That split is deliberate. Decoding RLE8 and hunting palettes is exactly the
// kind of work that should not happen inside LogonUI.exe on the secure desktop,
// where a mistake costs somebody their machine. Here it happens on the build
// machine, in code that has tests.
#pragma once

#include "xplogin/assets/Image.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace xplogin::assets {

// The ids the renderer asks for. These are logonui.exe's own resource numbers,
// kept as-is so the mapping back to the source is obvious.
namespace assetid {
constexpr uint32_t kBackgroundGlow   = 100; // 219x207 light source, upper left
constexpr uint32_t kPasswordBox      = 102; // 171x26 RLE8, 9-slice rect(3,3,5,5)
constexpr uint32_t kGoButton         = 103; // 26x26
constexpr uint32_t kGoButtonFocused  = 104; // [keyfocused], not [mousewithin]
constexpr uint32_t kHelpButton       = 105; // 28x28 - larger than the go button
constexpr uint32_t kHelpButtonFocused = 106;
constexpr uint32_t kPowerIcon        = 107; // 26x26
constexpr uint32_t kUndockIcon       = 108;
constexpr uint32_t kScrollUp         = 109; // 17x17
constexpr uint32_t kScrollDown       = 110;
constexpr uint32_t kScrollTrack      = 111; // 17x331 strip
constexpr uint32_t kSelectedTile     = 112; // 308x72 RLE8, 9-slice rect(8,8,0,8)
constexpr uint32_t kPictureFrame     = 113; // 58x58 32bpp, 9-slice rect(5,5,5,5)
constexpr uint32_t kDefaultPicture   = 114; // 48x48
constexpr uint32_t kPictureFrameHot  = 119; // [mousefocused] and [selected]
// 121 and 122 are the only two ids here that the welcome-screen markup never
// names, so unlike the rest these labels are inference, not quotation. The
// markup declares element id=atom(keyboard) with no content - set from code -
// and these are a matching 26x26 pair of accessibility glyphs. They are baked
// but not drawn; in particular they are NOT back arrows, which an earlier pass
// assumed and which put a keyboard icon next to the password field.
constexpr uint32_t kKeyboardIcon     = 121; // 26x26
constexpr uint32_t kKeyboardIconHot  = 122;
constexpr uint32_t kLogo             = 123; // 137x86
constexpr uint32_t kGradientStrip    = 124; // 1x340, stretched horizontally
constexpr uint32_t kTopDivider       = 125; // 800x2  glow sweep
constexpr uint32_t kBottomDivider    = 126; // 800x3  the orange sweep
constexpr uint32_t kLogoShadow       = 127; // 137x86, byte-identical to 123

// --- msgina.dll -----------------------------------------------------------
//
// The "Turn off computer" dialog is not part of the welcome screen. Clicking
// the power button on logonui's bottom band opens a separate modal owned by
// msgina.dll, which is why none of the artwork for it is in logonui.exe and
// why logonui's markup has no shutdown panel in it. Its resource numbers are
// in the 20000s and cannot collide with logonui's.
constexpr uint32_t kShutdownPanel = 20142; // 313x198 dialog background
constexpr uint32_t kShutdownFlag  = 20143; // 48x40 Windows flag, title band
// 32x320: ten 32x32 frames, three states per button plus one disabled.
// Turn Off 0-2, Stand By 3-5, Restart 6-8, Stand By disabled 9. Within a
// button the order is normal, pressed, hot - the middle frame of each triple
// is the darkened one.
constexpr uint32_t kShutdownOrbs  = 20150;
} // namespace assetid

// A single orb inside kShutdownOrbs, by frame index.
namespace shutdownorb {
constexpr int kTurnOff  = 0;
constexpr int kStandBy  = 3;
constexpr int kRestart  = 6;
constexpr int kDisabled = 9;
constexpr int kFrameSize = 32;
constexpr int kNormal   = 0; // add to a button's base index
constexpr int kPressed  = 1;
constexpr int kHot      = 2;
} // namespace shutdownorb

// What the baker knows about each resource: its id and the colour that means
// "transparent" for it. XP's markup names the key per bitmap; -1 (kNoColorKey)
// means the image has none.
constexpr uint32_t kNoColorKey = 0xFFFFFFFFu;

// Which XP binary a resource comes out of. The welcome screen is logonui.exe;
// the shutdown dialog it opens belongs to msgina.dll.
enum class AssetSource {
    LogonUi = 0,
    MsGina,
};

const char* AssetSourceFileName(AssetSource source);

struct AssetSpec {
    uint32_t    resourceId;
    uint32_t    colorKey;
    const char* description;
    AssetSource source = AssetSource::LogonUi;
};

// Every bitmap the welcome screen draws, in one place so the baker, the
// renderer and the tests cannot disagree about the list.
const std::vector<AssetSpec>& WelcomeScreenAssets();

// ---------------------------------------------------------------------------
// Container
// ---------------------------------------------------------------------------

enum class PackStatus {
    Ok = 0,
    BadMagic,
    UnsupportedVersion,
    Truncated,
    CorruptIndex,
};

const char* PackStatusText(PackStatus status);

class AssetPack {
public:
    // Serialises everything added so far. The format is deliberately trivial:
    // a header, a flat index, then raw BGRA runs.
    static std::vector<uint8_t> Build(const std::map<uint32_t, Image>& images);

    static PackStatus Load(const std::vector<uint8_t>& bytes, AssetPack* out);

    bool Has(uint32_t id) const { return images_.count(id) > 0; }
    const Image* Get(uint32_t id) const;
    size_t Count() const { return images_.size(); }
    std::vector<uint32_t> Ids() const;

private:
    std::map<uint32_t, Image> images_;
};

} // namespace xplogin::assets
