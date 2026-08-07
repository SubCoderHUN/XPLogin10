#include "xplogin/assets/PngWriter.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace xplogin::assets {
namespace {

// ---------------------------------------------------------------------------
// Checksums
// ---------------------------------------------------------------------------

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0xFFFFFFFFu) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[n] = c;
        }
        return t;
    }();

    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

uint32_t Adler32(const std::vector<uint8_t>& data) {
    uint32_t a = 1;
    uint32_t b = 0;
    for (uint8_t byte : data) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

// ---------------------------------------------------------------------------
// Deflate, fixed Huffman
// ---------------------------------------------------------------------------

// Bits go out least-significant first, which is deflate's convention and the
// opposite of the Huffman codes themselves - those are written MSB first. Two
// separate writers rather than one flag, because conflating them is the classic
// way to produce a stream that almost works.
class BitWriter {
public:
    void PutBits(uint32_t value, int count) {
        for (int i = 0; i < count; ++i) {
            bitBuffer_ |= ((value >> i) & 1u) << bitCount_;
            if (++bitCount_ == 8) {
                out_.push_back(static_cast<uint8_t>(bitBuffer_));
                bitBuffer_ = 0;
                bitCount_ = 0;
            }
        }
    }

    // Huffman codes are defined MSB first.
    void PutCode(uint32_t code, int count) {
        for (int i = count - 1; i >= 0; --i) {
            PutBits((code >> i) & 1u, 1);
        }
    }

    void Flush() {
        if (bitCount_ > 0) {
            out_.push_back(static_cast<uint8_t>(bitBuffer_));
            bitBuffer_ = 0;
            bitCount_ = 0;
        }
    }

    std::vector<uint8_t>& Bytes() { return out_; }

private:
    std::vector<uint8_t> out_;
    uint32_t bitBuffer_ = 0;
    int      bitCount_ = 0;
};

// RFC 1951 section 3.2.6: literals 0-143 are 8 bits at 0x30, 144-255 are 9 bits
// at 0x190, 256-279 are 7 bits at 0, 280-287 are 8 bits at 0xC0.
void PutLiteral(BitWriter& bits, uint32_t symbol) {
    if (symbol <= 143) {
        bits.PutCode(0x30 + symbol, 8);
    } else if (symbol <= 255) {
        bits.PutCode(0x190 + symbol - 144, 9);
    } else if (symbol <= 279) {
        bits.PutCode(symbol - 256, 7);
    } else {
        bits.PutCode(0xC0 + symbol - 280, 8);
    }
}

struct LengthCode {
    uint16_t code;
    uint8_t  extraBits;
    uint16_t base;
};

// RFC 1951 tables, transcribed.
constexpr LengthCode kLengthCodes[] = {
    {257, 0, 3},   {258, 0, 4},   {259, 0, 5},   {260, 0, 6},   {261, 0, 7},
    {262, 0, 8},   {263, 0, 9},   {264, 0, 10},  {265, 1, 11},  {266, 1, 13},
    {267, 1, 15},  {268, 1, 17},  {269, 2, 19},  {270, 2, 23},  {271, 2, 27},
    {272, 2, 31},  {273, 3, 35},  {274, 3, 43},  {275, 3, 51},  {276, 3, 59},
    {277, 4, 67},  {278, 4, 83},  {279, 4, 99},  {280, 4, 115}, {281, 5, 131},
    {282, 5, 163}, {283, 5, 195}, {284, 5, 227}, {285, 0, 258},
};

struct DistanceCode {
    uint16_t code;
    uint8_t  extraBits;
    uint16_t base;
};

constexpr DistanceCode kDistanceCodes[] = {
    {0, 0, 1},      {1, 0, 2},      {2, 0, 3},      {3, 0, 4},
    {4, 1, 5},      {5, 1, 7},      {6, 2, 9},      {7, 2, 13},
    {8, 3, 17},     {9, 3, 25},     {10, 4, 33},    {11, 4, 49},
    {12, 5, 65},    {13, 5, 97},    {14, 6, 129},   {15, 6, 193},
    {16, 7, 257},   {17, 7, 385},   {18, 8, 513},   {19, 8, 769},
    {20, 9, 1025},  {21, 9, 1537},  {22, 10, 2049}, {23, 10, 3073},
    {24, 11, 4097}, {25, 11, 6145}, {26, 12, 8193}, {27, 12, 12289},
    {28, 13, 16385},{29, 13, 24577},
};

void PutLength(BitWriter& bits, int length) {
    const LengthCode* chosen = &kLengthCodes[0];
    for (const LengthCode& candidate : kLengthCodes) {
        if (candidate.base <= length) {
            chosen = &candidate;
        }
    }
    PutLiteral(bits, chosen->code);
    if (chosen->extraBits > 0) {
        bits.PutBits(static_cast<uint32_t>(length - chosen->base), chosen->extraBits);
    }
}

void PutDistance(BitWriter& bits, int distance) {
    const DistanceCode* chosen = &kDistanceCodes[0];
    for (const DistanceCode& candidate : kDistanceCodes) {
        if (candidate.base <= distance) {
            chosen = &candidate;
        }
    }
    // Distance codes use a fixed 5-bit code in the fixed-Huffman alphabet.
    bits.PutCode(chosen->code, 5);
    if (chosen->extraBits > 0) {
        bits.PutBits(static_cast<uint32_t>(distance - chosen->base),
                     chosen->extraBits);
    }
}

constexpr int kWindowSize = 32768;
constexpr int kMinMatch = 3;
constexpr int kMaxMatch = 258;
constexpr int kHashBits = 15;
constexpr int kHashSize = 1 << kHashBits;
// A cap on how far back to walk a hash chain. Without it a run of identical
// bytes - which XP's flat backgrounds are full of - makes this quadratic.
constexpr int kMaxChainLength = 128;

uint32_t Hash3(const uint8_t* p) {
    return ((static_cast<uint32_t>(p[0]) << 16) ^
            (static_cast<uint32_t>(p[1]) << 8) ^ static_cast<uint32_t>(p[2])) *
           2654435761u >> (32 - kHashBits);
}

} // namespace

std::vector<uint8_t> DeflateCompress(const std::vector<uint8_t>& input) {
    BitWriter bits;

    // zlib header: deflate, 32K window, default level, no preset dictionary.
    bits.Bytes().push_back(0x78);
    bits.Bytes().push_back(0x9C);

    // One final block, fixed Huffman.
    bits.PutBits(1, 1); // BFINAL
    bits.PutBits(1, 2); // BTYPE = 01, fixed

    const int size = static_cast<int>(input.size());
    std::vector<int> head(kHashSize, -1);
    std::vector<int> prev(input.size(), -1);

    int position = 0;
    while (position < size) {
        int bestLength = 0;
        int bestDistance = 0;

        if (position + kMinMatch <= size) {
            const uint32_t hash = Hash3(&input[static_cast<size_t>(position)]);
            int candidate = head[hash];
            int chain = 0;
            while (candidate >= 0 && chain < kMaxChainLength) {
                const int distance = position - candidate;
                if (distance <= 0 || distance > kWindowSize) {
                    break;
                }
                int length = 0;
                const int limit = (std::min)(kMaxMatch, size - position);
                while (length < limit && input[static_cast<size_t>(candidate + length)] ==
                                             input[static_cast<size_t>(position + length)]) {
                    ++length;
                }
                if (length > bestLength) {
                    bestLength = length;
                    bestDistance = distance;
                    if (length >= kMaxMatch) {
                        break;
                    }
                }
                candidate = prev[static_cast<size_t>(candidate)];
                ++chain;
            }

            prev[static_cast<size_t>(position)] = head[hash];
            head[hash] = position;
        }

        if (bestLength >= kMinMatch) {
            PutLength(bits, bestLength);
            PutDistance(bits, bestDistance);
            // Insert the positions the match covered so later matches can find
            // them; skipping this costs far more than it saves.
            for (int i = 1; i < bestLength; ++i) {
                const int at = position + i;
                if (at + kMinMatch <= size) {
                    const uint32_t hash = Hash3(&input[static_cast<size_t>(at)]);
                    prev[static_cast<size_t>(at)] = head[hash];
                    head[hash] = at;
                }
            }
            position += bestLength;
        } else {
            PutLiteral(bits, input[static_cast<size_t>(position)]);
            ++position;
        }
    }

    PutLiteral(bits, 256); // end of block
    bits.Flush();

    const uint32_t adler = Adler32(input);
    bits.Bytes().push_back(static_cast<uint8_t>((adler >> 24) & 0xFF));
    bits.Bytes().push_back(static_cast<uint8_t>((adler >> 16) & 0xFF));
    bits.Bytes().push_back(static_cast<uint8_t>((adler >> 8) & 0xFF));
    bits.Bytes().push_back(static_cast<uint8_t>(adler & 0xFF));

    return bits.Bytes();
}

namespace {

void PutU32Be(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void PutChunk(std::vector<uint8_t>& out, const char type[5],
              const std::vector<uint8_t>& data) {
    PutU32Be(out, static_cast<uint32_t>(data.size()));
    const size_t crcStart = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    const uint32_t crc =
        Crc32(out.data() + crcStart, out.size() - crcStart) ^ 0xFFFFFFFFu;
    PutU32Be(out, crc);
}

} // namespace

std::vector<uint8_t> EncodePng(const Image& image) {
    if (image.empty()) {
        return {};
    }

    const size_t width = image.width;
    const size_t height = image.height;

    // Raw scanlines, each prefixed with a filter byte. Filter 1 (Sub) beats
    // None on this artwork because the horizontal runs are what compress.
    std::vector<uint8_t> raw;
    raw.reserve(height * (1 + width * 4));
    std::vector<uint8_t> row(width * 4);
    for (size_t y = 0; y < height; ++y) {
        for (size_t x = 0; x < width; ++x) {
            const uint32_t pixel = image.pixels[y * width + x];
            row[x * 4 + 0] = static_cast<uint8_t>((pixel >> 16) & 0xFF); // R
            row[x * 4 + 1] = static_cast<uint8_t>((pixel >> 8) & 0xFF);  // G
            row[x * 4 + 2] = static_cast<uint8_t>(pixel & 0xFF);         // B
            row[x * 4 + 3] = static_cast<uint8_t>((pixel >> 24) & 0xFF); // A
        }
        raw.push_back(1); // Sub
        for (size_t i = 0; i < row.size(); ++i) {
            const uint8_t left = i >= 4 ? row[i - 4] : 0;
            raw.push_back(static_cast<uint8_t>(row[i] - left));
        }
    }

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

    std::vector<uint8_t> ihdr;
    PutU32Be(ihdr, static_cast<uint32_t>(width));
    PutU32Be(ihdr, static_cast<uint32_t>(height));
    ihdr.push_back(8); // bit depth
    ihdr.push_back(6); // colour type: truecolour with alpha
    ihdr.push_back(0); // deflate
    ihdr.push_back(0); // adaptive filtering
    ihdr.push_back(0); // no interlace
    PutChunk(png, "IHDR", ihdr);
    PutChunk(png, "IDAT", DeflateCompress(raw));
    PutChunk(png, "IEND", {});

    return png;
}

std::string Base64(const std::vector<uint8_t>& bytes) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);

    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t triple =
            (static_cast<uint32_t>(bytes[i]) << 16) |
            (static_cast<uint32_t>(bytes[i + 1]) << 8) | bytes[i + 2];
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
    }

    const size_t remaining = bytes.size() - i;
    if (remaining == 1) {
        const uint32_t triple = static_cast<uint32_t>(bytes[i]) << 16;
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.append("==");
    } else if (remaining == 2) {
        const uint32_t triple = (static_cast<uint32_t>(bytes[i]) << 16) |
                                (static_cast<uint32_t>(bytes[i + 1]) << 8);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string EncodePngDataUri(const Image& image) {
    const std::vector<uint8_t> png = EncodePng(image);
    if (png.empty()) {
        return {};
    }
    return "data:image/png;base64," + Base64(png);
}

} // namespace xplogin::assets
