// XPLogin10 - a decoded image, and the BMP decoder that produces one.
//
// Everything Microsoft drew the welcome screen with is an RT_BITMAP inside
// logonui.exe, in four different flavours: 24bpp, 32bpp with an alpha channel,
// 8bpp paletted, and 8bpp RLE-compressed. Rather than teach the credential
// provider to read all of that at logon time, the build decodes them once into
// straight BGRA and bakes the result into an asset pack. The code that runs
// inside LogonUI then only has to blit.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xplogin::assets {

// Straight (non-premultiplied) BGRA, top-down, one uint32 per pixel laid out as
// 0xAARRGGBB - which is both GDI+'s ARGB order and what a Gdiplus::Bitmap can
// be constructed over without a copy.
struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint32_t> pixels;

    bool empty() const { return width == 0 || height == 0 || pixels.empty(); }
    size_t PixelCount() const {
        return static_cast<size_t>(width) * static_cast<size_t>(height);
    }
    uint32_t At(uint32_t x, uint32_t y) const {
        if (x >= width || y >= height) {
            return 0;
        }
        return pixels[static_cast<size_t>(y) * width + x];
    }
    void Set(uint32_t x, uint32_t y, uint32_t argb) {
        if (x < width && y < height) {
            pixels[static_cast<size_t>(y) * width + x] = argb;
        }
    }
};

enum class BmpStatus {
    Ok = 0,
    TooSmall,
    UnsupportedHeader,
    UnsupportedBitDepth,
    UnsupportedCompression,
    BadDimensions,
    Truncated,
    BadRleStream,
};

const char* BmpStatusText(BmpStatus status);

// Decodes an RT_BITMAP resource - that is, a BITMAPINFOHEADER followed by the
// palette and pixels, with no BITMAPFILEHEADER. Handles 1/4/8/16/24/32 bpp,
// BI_RGB, BI_BITFIELDS and BI_RLE8/BI_RLE4, and both bottom-up and top-down
// row order.
BmpStatus DecodeBitmapResource(const std::vector<uint8_t>& resource, Image* out);

// Same, for a complete .bmp file (with the 14-byte file header).
BmpStatus DecodeBitmapFile(const std::vector<uint8_t>& file, Image* out);

// Turns one colour fully transparent. XP marks transparent pixels with magenta
// (#FF00FF) in the 24bpp bitmaps; the markup names the key per bitmap, and -1
// means the image has no key at all.
void ApplyColorKey(Image* image, uint32_t rgbKey);

// True when a 32bpp source actually carried alpha. A surprising number of
// "32bpp" Windows bitmaps have a zeroed alpha channel, which would make the
// whole image invisible if taken at face value - so the decoder treats an
// all-zero alpha channel as opaque and this reports which way it went.
bool HasMeaningfulAlpha(const Image& image);

} // namespace xplogin::assets
