// XPLogin10 - Microsoft's own artwork, ready to blit.
//
// Wraps the baked asset pack and hands out GDI+ bitmaps. Everything expensive
// happened at build time; this is a lookup, a one-off wrap of an existing pixel
// buffer, and a draw.
//
// The whole class is optional by design. When XPLogin.assets is missing - a
// build made without a reference logonui.exe - Loaded() is false and the
// renderer falls back to drawing the screen from primitives. A missing file
// must never be the reason somebody cannot sign in.
#pragma once

#include "xplogin/assets/AssetPack.h"

#include <windows.h>

#include <map>
#include <memory>
#include <string>

namespace Gdiplus {
class Bitmap;
class Graphics;
} // namespace Gdiplus

namespace xplogin::ui::win32 {

struct Rect;

class XpAssets {
public:
    XpAssets();
    ~XpAssets();

    XpAssets(const XpAssets&) = delete;
    XpAssets& operator=(const XpAssets&) = delete;

    // Reads XPLogin.assets. Returns false when the file is absent or unusable,
    // which is not an error - it is the primitives path.
    bool Load(const std::wstring& path);

    // Also tries the embedded RCDATA copy, so the preview host works from a
    // build directory without a deployed pack.
    bool LoadFromModule(HMODULE module, uint32_t resourceId);

    bool Loaded() const { return loaded_; }
    size_t Count() const { return pack_.Count(); }
    const std::wstring& Source() const { return source_; }

    // Null when the id is not in the pack.
    Gdiplus::Bitmap* Get(uint32_t assetId);

    void Release();

private:
    Gdiplus::Bitmap* Wrap(uint32_t assetId, const assets::Image& image);

    assets::AssetPack pack_;
    bool              loaded_ = false;
    std::wstring      source_;

    // GDI+ does not copy the pixels a Bitmap is constructed over, so the buffer
    // has to outlive the bitmap. Both are kept here, in step.
    struct Entry;
    std::map<uint32_t, std::unique_ptr<Entry>> cache_;
};

} // namespace xplogin::ui::win32
