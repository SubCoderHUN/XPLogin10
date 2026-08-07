#include "xplogin/ui/win32/XpAssets.h"

#include "xplogin/Logging.h"
#include "xplogin/StringUtil.h"

#include <objidl.h>
#include <gdiplus.h>

#include <filesystem>
#include <fstream>

namespace xplogin::ui::win32 {

// A decoded image plus the GDI+ bitmap wrapped around it. The pixel buffer is
// owned here because Gdiplus::Bitmap(width, height, stride, format, scan0) does
// not copy - it borrows, and freeing the buffer first is a use-after-free that
// only shows up as garbage on the logon screen.
struct XpAssets::Entry {
    assets::Image image;
    std::unique_ptr<Gdiplus::Bitmap> bitmap;
};

XpAssets::XpAssets() = default;

XpAssets::~XpAssets() { Release(); }

void XpAssets::Release() {
    // Bitmaps first, then the buffers they point at.
    cache_.clear();
    pack_ = assets::AssetPack();
    loaded_ = false;
    source_.clear();
}

bool XpAssets::Load(const std::wstring& path) {
    Release();
    if (path.empty()) {
        return false;
    }

    // Via std::filesystem::path, not a UTF-8 narrow string: on Windows the
    // narrow fstream overload takes ACP-encoded bytes, so a UTF-8 path breaks
    // on any install directory with a character outside the active code page.
    // filesystem::path carries the wide form natively and is standard C++17,
    // so this still compiles everywhere.
    std::ifstream file(std::filesystem::path(path),
                       std::ios::binary | std::ios::ate);
    if (!file) {
        XPLOG_INFO("no asset pack at the configured path; drawing primitives");
        return false;
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return false;
    }
    file.seekg(0);

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return false;
    }

    const assets::PackStatus status = assets::AssetPack::Load(bytes, &pack_);
    if (status != assets::PackStatus::Ok) {
        XPLOG_ERROR("asset pack rejected: %s", assets::PackStatusText(status));
        pack_ = assets::AssetPack();
        return false;
    }

    loaded_ = pack_.Count() > 0;
    source_ = path;
    XPLOG_INFO("loaded %d assets", static_cast<int>(pack_.Count()));
    return loaded_;
}

bool XpAssets::LoadFromModule(HMODULE module, uint32_t resourceId) {
    Release();

    HRSRC found = ::FindResourceW(module, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!found) {
        return false;
    }
    const DWORD size = ::SizeofResource(module, found);
    HGLOBAL handle = ::LoadResource(module, found);
    if (!handle || size == 0) {
        return false;
    }
    const void* data = ::LockResource(handle);
    if (!data) {
        return false;
    }

    const uint8_t* begin = static_cast<const uint8_t*>(data);
    const std::vector<uint8_t> bytes(begin, begin + size);

    if (assets::AssetPack::Load(bytes, &pack_) != assets::PackStatus::Ok) {
        pack_ = assets::AssetPack();
        return false;
    }
    loaded_ = pack_.Count() > 0;
    source_ = L"(embedded)";
    return loaded_;
}

Gdiplus::Bitmap* XpAssets::Wrap(uint32_t assetId, const assets::Image& image) {
    auto entry = std::make_unique<Entry>();
    entry->image = image; // our own copy, so the pack can go away

    const INT stride = static_cast<INT>(entry->image.width * sizeof(uint32_t));
    entry->bitmap = std::make_unique<Gdiplus::Bitmap>(
        static_cast<INT>(entry->image.width), static_cast<INT>(entry->image.height),
        stride, PixelFormat32bppARGB,
        reinterpret_cast<BYTE*>(entry->image.pixels.data()));

    if (entry->bitmap->GetLastStatus() != Gdiplus::Ok) {
        return nullptr;
    }

    Gdiplus::Bitmap* raw = entry->bitmap.get();
    cache_.emplace(assetId, std::move(entry));
    return raw;
}

Gdiplus::Bitmap* XpAssets::Get(uint32_t assetId) {
    auto cached = cache_.find(assetId);
    if (cached != cache_.end()) {
        return cached->second->bitmap.get();
    }
    if (!loaded_) {
        return nullptr;
    }
    const assets::Image* image = pack_.Get(assetId);
    if (!image || image->empty()) {
        return nullptr;
    }
    return Wrap(assetId, *image);
}

} // namespace xplogin::ui::win32
