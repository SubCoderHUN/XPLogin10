#include "xplogin/assets/Image.h"

#include <cstring>

namespace xplogin::assets {
namespace {

constexpr uint32_t kBiRgb       = 0;
constexpr uint32_t kBiRle8      = 1;
constexpr uint32_t kBiRle4      = 2;
constexpr uint32_t kBiBitfields = 3;

// A single image is never legitimately this large in a UI resource, and a
// corrupt header claiming 2^31 pixels would otherwise try to allocate it.
constexpr uint32_t kMaxDimension = 8192;

bool ReadU16(const std::vector<uint8_t>& d, size_t at, uint16_t* v) {
    if (at + 2 > d.size()) return false;
    *v = static_cast<uint16_t>(d[at] | (d[at + 1] << 8));
    return true;
}

bool ReadU32(const std::vector<uint8_t>& d, size_t at, uint32_t* v) {
    if (at + 4 > d.size()) return false;
    *v = static_cast<uint32_t>(d[at]) | (static_cast<uint32_t>(d[at + 1]) << 8) |
         (static_cast<uint32_t>(d[at + 2]) << 16) |
         (static_cast<uint32_t>(d[at + 3]) << 24);
    return true;
}

bool ReadS32(const std::vector<uint8_t>& d, size_t at, int32_t* v) {
    uint32_t raw = 0;
    if (!ReadU32(d, at, &raw)) return false;
    *v = static_cast<int32_t>(raw);
    return true;
}

constexpr uint32_t MakeArgb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | b;
}

// How many bits shift right to move a mask's lowest set bit to bit 0, and how
// wide the mask is - used for BI_BITFIELDS.
void AnalyseMask(uint32_t mask, int* shift, int* bits) {
    *shift = 0;
    *bits = 0;
    if (mask == 0) {
        return;
    }
    while (((mask >> *shift) & 1) == 0) {
        ++(*shift);
    }
    uint32_t moved = mask >> *shift;
    while (moved & 1) {
        ++(*bits);
        moved >>= 1;
    }
}

uint8_t ScaleChannel(uint32_t value, int bits) {
    if (bits <= 0) return 0;
    if (bits >= 8) return static_cast<uint8_t>(value >> (bits - 8));
    // Replicate the high bits downward so 5 bits of 0x1F becomes 0xFF, not 0xF8.
    const uint32_t maxIn = (1u << bits) - 1;
    return static_cast<uint8_t>((value * 255 + maxIn / 2) / maxIn);
}

struct Palette {
    uint32_t colors[256] = {};
    uint32_t count = 0;
};

} // namespace

const char* BmpStatusText(BmpStatus status) {
    switch (status) {
        case BmpStatus::Ok: return "ok";
        case BmpStatus::TooSmall: return "too small to be a bitmap";
        case BmpStatus::UnsupportedHeader: return "unsupported header size";
        case BmpStatus::UnsupportedBitDepth: return "unsupported bit depth";
        case BmpStatus::UnsupportedCompression: return "unsupported compression";
        case BmpStatus::BadDimensions: return "bad dimensions";
        case BmpStatus::Truncated: return "pixel data is truncated";
        case BmpStatus::BadRleStream: return "malformed RLE stream";
    }
    return "?";
}

BmpStatus DecodeBitmapResource(const std::vector<uint8_t>& resource, Image* out) {
    if (!out) {
        return BmpStatus::TooSmall;
    }
    *out = Image();

    if (resource.size() < 40) {
        return BmpStatus::TooSmall;
    }

    uint32_t headerSize = 0;
    int32_t declaredWidth = 0;
    int32_t declaredHeight = 0;
    uint16_t bitCount = 0;
    uint32_t compression = 0;
    uint32_t colorsUsed = 0;

    ReadU32(resource, 0, &headerSize);
    ReadS32(resource, 4, &declaredWidth);
    ReadS32(resource, 8, &declaredHeight);
    ReadU16(resource, 14, &bitCount);
    ReadU32(resource, 16, &compression);
    ReadU32(resource, 32, &colorsUsed);

    // BITMAPINFOHEADER (40) through BITMAPV5HEADER (124). Anything smaller is
    // an OS/2 header we do not need to handle.
    if (headerSize < 40 || headerSize > resource.size()) {
        return BmpStatus::UnsupportedHeader;
    }
    if (declaredWidth <= 0 || declaredHeight == 0) {
        return BmpStatus::BadDimensions;
    }

    const bool topDown = declaredHeight < 0;
    const uint32_t width = static_cast<uint32_t>(declaredWidth);
    const uint32_t height = static_cast<uint32_t>(
        declaredHeight < 0 ? -static_cast<int64_t>(declaredHeight) : declaredHeight);

    if (width > kMaxDimension || height > kMaxDimension) {
        return BmpStatus::BadDimensions;
    }
    if (bitCount != 1 && bitCount != 4 && bitCount != 8 && bitCount != 16 &&
        bitCount != 24 && bitCount != 32) {
        return BmpStatus::UnsupportedBitDepth;
    }
    if (compression != kBiRgb && compression != kBiRle8 && compression != kBiRle4 &&
        compression != kBiBitfields) {
        return BmpStatus::UnsupportedCompression;
    }

    // ---- palette / bitfield masks -----------------------------------------
    size_t cursor = headerSize;
    Palette palette;
    uint32_t redMask = 0;
    uint32_t greenMask = 0;
    uint32_t blueMask = 0;
    uint32_t alphaMask = 0;

    if (compression == kBiBitfields && headerSize == 40) {
        // The three masks sit where the palette would be.
        if (!ReadU32(resource, cursor, &redMask) ||
            !ReadU32(resource, cursor + 4, &greenMask) ||
            !ReadU32(resource, cursor + 8, &blueMask)) {
            return BmpStatus::Truncated;
        }
        cursor += 12;
    } else if (headerSize >= 56) {
        // V4/V5 carry the masks inside the header itself.
        ReadU32(resource, 40, &redMask);
        ReadU32(resource, 44, &greenMask);
        ReadU32(resource, 48, &blueMask);
        ReadU32(resource, 52, &alphaMask);
    }

    if (bitCount <= 8) {
        palette.count = colorsUsed != 0 ? colorsUsed : (1u << bitCount);
        if (palette.count > 256) {
            palette.count = 256;
        }
        for (uint32_t i = 0; i < palette.count; ++i) {
            const size_t at = cursor + static_cast<size_t>(i) * 4;
            if (at + 4 > resource.size()) {
                return BmpStatus::Truncated;
            }
            palette.colors[i] =
                MakeArgb(0xFF, resource[at + 2], resource[at + 1], resource[at]);
        }
        cursor += static_cast<size_t>(palette.count) * 4;
    }

    const size_t pixelStart = cursor;
    out->width = width;
    out->height = height;
    out->pixels.assign(out->PixelCount(), 0);

    auto writePixel = [&](uint32_t x, uint32_t y, uint32_t argb) {
        // Bottom-up is the BMP default; store top-down so the rest of the
        // project never has to think about it again.
        const uint32_t row = topDown ? y : (height - 1 - y);
        out->pixels[static_cast<size_t>(row) * width + x] = argb;
    };

    // ---- RLE -------------------------------------------------------------
    if (compression == kBiRle8 || compression == kBiRle4) {
        const bool rle4 = compression == kBiRle4;
        size_t at = pixelStart;
        uint32_t x = 0;
        uint32_t y = 0;

        auto put = [&](uint8_t index) {
            if (x < width && y < height) {
                writePixel(x, y, index < palette.count ? palette.colors[index]
                                                       : MakeArgb(0xFF, 0, 0, 0));
            }
            ++x;
        };

        while (at + 1 < resource.size()) {
            const uint8_t first = resource[at];
            const uint8_t second = resource[at + 1];
            at += 2;

            if (first != 0) {
                // Encoded run: `first` pixels of colour `second`.
                for (uint8_t i = 0; i < first; ++i) {
                    if (rle4) {
                        put(static_cast<uint8_t>((i & 1) ? (second & 0x0F)
                                                         : (second >> 4)));
                    } else {
                        put(second);
                    }
                }
                continue;
            }

            if (second == 0) {       // end of line
                x = 0;
                ++y;
                continue;
            }
            if (second == 1) {       // end of bitmap
                break;
            }
            if (second == 2) {       // delta
                if (at + 1 >= resource.size()) {
                    return BmpStatus::BadRleStream;
                }
                x += resource[at];
                y += resource[at + 1];
                at += 2;
                continue;
            }

            // Absolute mode: `second` literal pixels, padded to a word boundary.
            const uint32_t count = second;
            const size_t byteCount = rle4 ? ((count + 1) / 2) : count;
            if (at + byteCount > resource.size()) {
                return BmpStatus::BadRleStream;
            }
            for (uint32_t i = 0; i < count; ++i) {
                if (rle4) {
                    const uint8_t pair = resource[at + i / 2];
                    put(static_cast<uint8_t>((i & 1) ? (pair & 0x0F) : (pair >> 4)));
                } else {
                    put(resource[at + i]);
                }
            }
            at += byteCount;
            if (byteCount & 1) {
                ++at; // word alignment
            }
        }
        return BmpStatus::Ok;
    }

    // ---- uncompressed ----------------------------------------------------
    const size_t stride =
        ((static_cast<size_t>(width) * bitCount + 31) / 32) * 4;
    if (pixelStart + stride * height > resource.size()) {
        return BmpStatus::Truncated;
    }

    int redShift = 0, redBits = 0, greenShift = 0, greenBits = 0;
    int blueShift = 0, blueBits = 0, alphaShift = 0, alphaBits = 0;
    if (compression == kBiBitfields || (bitCount == 16 && redMask == 0)) {
        if (redMask == 0 && greenMask == 0 && blueMask == 0) {
            // 16bpp with no masks is X1R5G5B5 by definition.
            redMask = 0x7C00;
            greenMask = 0x03E0;
            blueMask = 0x001F;
        }
        AnalyseMask(redMask, &redShift, &redBits);
        AnalyseMask(greenMask, &greenShift, &greenBits);
        AnalyseMask(blueMask, &blueShift, &blueBits);
        AnalyseMask(alphaMask, &alphaShift, &alphaBits);
    }

    bool sawNonZeroAlpha = false;

    for (uint32_t y = 0; y < height; ++y) {
        const size_t rowStart = pixelStart + static_cast<size_t>(y) * stride;
        for (uint32_t x = 0; x < width; ++x) {
            uint32_t argb = 0;
            switch (bitCount) {
                case 1: {
                    const uint8_t byte = resource[rowStart + x / 8];
                    const uint8_t index = (byte >> (7 - (x % 8))) & 1;
                    argb = index < palette.count ? palette.colors[index]
                                                 : MakeArgb(0xFF, 0, 0, 0);
                    break;
                }
                case 4: {
                    const uint8_t byte = resource[rowStart + x / 2];
                    const uint8_t index =
                        static_cast<uint8_t>((x & 1) ? (byte & 0x0F) : (byte >> 4));
                    argb = index < palette.count ? palette.colors[index]
                                                 : MakeArgb(0xFF, 0, 0, 0);
                    break;
                }
                case 8: {
                    const uint8_t index = resource[rowStart + x];
                    argb = index < palette.count ? palette.colors[index]
                                                 : MakeArgb(0xFF, 0, 0, 0);
                    break;
                }
                case 16: {
                    const size_t at = rowStart + static_cast<size_t>(x) * 2;
                    const uint32_t value =
                        static_cast<uint32_t>(resource[at]) |
                        (static_cast<uint32_t>(resource[at + 1]) << 8);
                    argb = MakeArgb(
                        0xFF,
                        ScaleChannel((value & redMask) >> redShift, redBits),
                        ScaleChannel((value & greenMask) >> greenShift, greenBits),
                        ScaleChannel((value & blueMask) >> blueShift, blueBits));
                    break;
                }
                case 24: {
                    const size_t at = rowStart + static_cast<size_t>(x) * 3;
                    argb = MakeArgb(0xFF, resource[at + 2], resource[at + 1],
                                    resource[at]);
                    break;
                }
                case 32:
                default: {
                    const size_t at = rowStart + static_cast<size_t>(x) * 4;
                    const uint8_t alpha = resource[at + 3];
                    if (alpha != 0) {
                        sawNonZeroAlpha = true;
                    }
                    argb = MakeArgb(alpha, resource[at + 2], resource[at + 1],
                                    resource[at]);
                    break;
                }
            }
            writePixel(x, y, argb);
        }
    }

    // A 32bpp bitmap whose alpha channel is entirely zero is not a fully
    // transparent image - it is an image whose author never filled the channel
    // in. Windows treats those as opaque, and so must we, or half of XP's
    // artwork disappears.
    if (bitCount == 32 && !sawNonZeroAlpha) {
        for (uint32_t& pixel : out->pixels) {
            pixel |= 0xFF000000u;
        }
    }

    return BmpStatus::Ok;
}

BmpStatus DecodeBitmapFile(const std::vector<uint8_t>& file, Image* out) {
    if (file.size() < 14 || file[0] != 'B' || file[1] != 'M') {
        return BmpStatus::TooSmall;
    }
    // Strip the file header; the rest is exactly an RT_BITMAP resource.
    return DecodeBitmapResource(
        std::vector<uint8_t>(file.begin() + 14, file.end()), out);
}

void ApplyColorKey(Image* image, uint32_t rgbKey) {
    if (!image) {
        return;
    }
    const uint32_t key = rgbKey & 0x00FFFFFFu;
    for (uint32_t& pixel : image->pixels) {
        if ((pixel & 0x00FFFFFFu) == key) {
            pixel = 0; // fully transparent, and zeroed so it cannot bleed
        }
    }
}

bool HasMeaningfulAlpha(const Image& image) {
    bool sawOpaque = false;
    bool sawTransparent = false;
    for (uint32_t pixel : image.pixels) {
        const uint8_t alpha = static_cast<uint8_t>((pixel >> 24) & 0xFF);
        if (alpha == 0xFF) {
            sawOpaque = true;
        } else {
            sawTransparent = true;
        }
        if (sawOpaque && sawTransparent) {
            return true;
        }
    }
    return false;
}

} // namespace xplogin::assets
