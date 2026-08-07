// XPLogin10 - a minimal PNG encoder.
//
// Exists so the build can turn decoded XP artwork into something a browser will
// display, without adding libpng, zlib, or any other dependency to a project
// whose whole promise is that it installs out of the box.
//
// It is a real deflate stream, not stored blocks: fixed Huffman codes with a
// hash-chain match finder. That is enough to get XP's flat, dithered artwork
// down by roughly an order of magnitude, which is the difference between a
// documentation image somebody will look at and one nobody will download.
#pragma once

#include "xplogin/assets/Image.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xplogin::assets {

// Encodes `image` (0xAARRGGBB, top-down) as an 8-bit RGBA PNG.
std::vector<uint8_t> EncodePng(const Image& image);

// The same bytes, base64 encoded and prefixed for use in an <img src> or an
// SVG <image href>.
std::string EncodePngDataUri(const Image& image);

// Exposed for tests: a raw deflate stream (zlib wrapper included) of `input`.
std::vector<uint8_t> DeflateCompress(const std::vector<uint8_t>& input);

std::string Base64(const std::vector<uint8_t>& bytes);

} // namespace xplogin::assets
