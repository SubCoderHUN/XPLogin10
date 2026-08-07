// Tests for the PNG encoder.
//
// The encoder exists so documentation images can carry XP's real pixels without
// adding a dependency. That means nothing else in the build validates its
// output, so these tests carry the whole load: the deflate stream is checked by
// decompressing it here with an independent inflate, and the PNG framing is
// checked field by field.
#include "xptest.h"

#include "xplogin/assets/PngWriter.h"

#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

using namespace xplogin::assets;

namespace {

// ---------------------------------------------------------------------------
// An independent inflate, so a round trip proves something. Deliberately not
// sharing any code with the encoder - fixed Huffman only, which is all the
// encoder emits.
// ---------------------------------------------------------------------------

class BitReader {
public:
    explicit BitReader(const std::vector<uint8_t>& data, size_t start)
        : data_(data), position_(start) {}

    // Deflate reads bits least-significant first.
    uint32_t Bits(int count) {
        uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            value |= Bit() << i;
        }
        return value;
    }

    // Huffman codes are packed MSB first.
    uint32_t Code(int count) {
        uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            value = (value << 1) | Bit();
        }
        return value;
    }

    bool Exhausted() const { return position_ >= data_.size(); }

private:
    uint32_t Bit() {
        if (position_ >= data_.size()) {
            return 0;
        }
        const uint32_t bit = (data_[position_] >> bitIndex_) & 1u;
        if (++bitIndex_ == 8) {
            bitIndex_ = 0;
            ++position_;
        }
        return bit;
    }

    const std::vector<uint8_t>& data_;
    size_t position_ = 0;
    int    bitIndex_ = 0;
};

const int kLengthBase[] = {3,  4,  5,  6,  7,  8,  9,  10,  11,  13,
                           15, 17, 19, 23, 27, 31, 35, 43,  51,  59,
                           67, 83, 99, 115, 131, 163, 195, 227, 258};
const int kLengthExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                            2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const int kDistBase[] = {1,    2,    3,    4,    5,    7,     9,     13,
                         17,   25,   33,   49,   65,   97,    129,   193,
                         257,  385,  513,  769,  1025, 1537,  2049,  3073,
                         4097, 6145, 8193, 12289, 16385, 24577};
const int kDistExtra[] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                          6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Decodes one symbol using the fixed literal/length code from RFC 1951 3.2.6.
int DecodeFixedSymbol(BitReader& reader) {
    uint32_t code = reader.Code(7);
    if (code <= 0x17) {
        return static_cast<int>(code) + 256; // 7-bit: 256-279
    }
    code = (code << 1) | reader.Code(1);
    if (code >= 0x30 && code <= 0xBF) {
        return static_cast<int>(code) - 0x30; // 8-bit: 0-143
    }
    if (code >= 0xC0 && code <= 0xC7) {
        return static_cast<int>(code) - 0xC0 + 280; // 8-bit: 280-287
    }
    code = (code << 1) | reader.Code(1);
    return static_cast<int>(code) - 0x190 + 144; // 9-bit: 144-255
}

// Returns the inflated bytes, or an empty vector on a malformed stream.
std::vector<uint8_t> Inflate(const std::vector<uint8_t>& zlib) {
    if (zlib.size() < 6) {
        return {};
    }
    BitReader reader(zlib, 2); // skip the 2-byte zlib header
    std::vector<uint8_t> out;

    for (int block = 0; block < 64; ++block) {
        const uint32_t final = reader.Bits(1);
        const uint32_t type = reader.Bits(2);
        if (type != 1) {
            return {}; // the encoder only ever emits fixed Huffman
        }

        for (int guard = 0; guard < 100000000; ++guard) {
            const int symbol = DecodeFixedSymbol(reader);
            if (symbol == 256) {
                break;
            }
            if (symbol < 256) {
                out.push_back(static_cast<uint8_t>(symbol));
                continue;
            }
            const int lengthIndex = symbol - 257;
            if (lengthIndex < 0 || lengthIndex > 28) {
                return {};
            }
            const int length = kLengthBase[lengthIndex] +
                               static_cast<int>(reader.Bits(kLengthExtra[lengthIndex]));

            const int distIndex = static_cast<int>(reader.Code(5));
            if (distIndex < 0 || distIndex > 29) {
                return {};
            }
            const int distance =
                kDistBase[distIndex] +
                static_cast<int>(reader.Bits(kDistExtra[distIndex]));
            if (distance <= 0 || static_cast<size_t>(distance) > out.size()) {
                return {};
            }
            for (int i = 0; i < length; ++i) {
                out.push_back(out[out.size() - static_cast<size_t>(distance)]);
            }
        }
        if (final) {
            break;
        }
        if (reader.Exhausted()) {
            break;
        }
    }
    return out;
}

uint32_t ReadU32Be(const std::vector<uint8_t>& data, size_t at) {
    return (static_cast<uint32_t>(data[at]) << 24) |
           (static_cast<uint32_t>(data[at + 1]) << 16) |
           (static_cast<uint32_t>(data[at + 2]) << 8) |
           static_cast<uint32_t>(data[at + 3]);
}

Image MakeImage(uint32_t width, uint32_t height, uint32_t fill) {
    Image image;
    image.width = width;
    image.height = height;
    image.pixels.assign(static_cast<size_t>(width) * height, fill);
    return image;
}

} // namespace

// ---------------------------------------------------------------------------
// Deflate
// ---------------------------------------------------------------------------

TEST(PngWriter, DeflateRoundTripsAnEmptyInput) {
    const std::vector<uint8_t> compressed = DeflateCompress({});
    CHECK(Inflate(compressed).empty());
}

TEST(PngWriter, DeflateRoundTripsShortLiterals) {
    // Under kMinMatch, so this exercises the literal path only.
    const std::vector<uint8_t> input = {'X', 'P'};
    CHECK_EQ(Inflate(DeflateCompress(input)), input);
}

TEST(PngWriter, DeflateRoundTripsEveryByteValue) {
    std::vector<uint8_t> input(256);
    std::iota(input.begin(), input.end(), 0);
    // Covers all four ranges of the fixed literal alphabet, including the
    // 9-bit codes for 144-255 that a naive encoder gets wrong.
    CHECK_EQ(Inflate(DeflateCompress(input)), input);
}

TEST(PngWriter, DeflateRoundTripsALongRun) {
    // A flat colour run is what XP's backgrounds mostly are, and it is the case
    // where the match finder emits its longest codes.
    const std::vector<uint8_t> input(5000, 0xAB);
    const std::vector<uint8_t> compressed = DeflateCompress(input);
    CHECK_EQ(Inflate(compressed), input);
    // ...and it had better actually compress.
    CHECK_LT(compressed.size(), input.size() / 10);
}

TEST(PngWriter, DeflateRoundTripsRepeatingStructure) {
    std::vector<uint8_t> input;
    for (int i = 0; i < 2000; ++i) {
        input.push_back(static_cast<uint8_t>(i % 7));
        input.push_back(static_cast<uint8_t>(i % 13));
        input.push_back(static_cast<uint8_t>(i % 251));
    }
    CHECK_EQ(Inflate(DeflateCompress(input)), input);
}

TEST(PngWriter, DeflateRoundTripsPseudoRandomData) {
    // Incompressible input: the encoder must still produce a valid stream
    // rather than a shorter, broken one.
    std::vector<uint8_t> input;
    uint32_t state = 12345;
    for (int i = 0; i < 8000; ++i) {
        state = state * 1103515245u + 12345u;
        input.push_back(static_cast<uint8_t>(state >> 16));
    }
    CHECK_EQ(Inflate(DeflateCompress(input)), input);
}

TEST(PngWriter, DeflateWritesAZlibHeaderAndAdler) {
    const std::vector<uint8_t> input(100, 0x42);
    const std::vector<uint8_t> compressed = DeflateCompress(input);

    REQUIRE_GE(compressed.size(), size_t(6));
    CHECK_EQ(int(compressed[0]), 0x78);
    CHECK_EQ(int(compressed[1]), 0x9C);
    // CMF/FLG must be a multiple of 31 or a conforming decoder rejects it.
    CHECK_EQ((compressed[0] * 256 + compressed[1]) % 31, 0);

    // The trailing Adler-32, computed independently.
    uint32_t a = 1;
    uint32_t b = 0;
    for (uint8_t byte : input) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t expected = (b << 16) | a;
    CHECK_EQ(ReadU32Be(compressed, compressed.size() - 4), expected);
}

// ---------------------------------------------------------------------------
// PNG framing
// ---------------------------------------------------------------------------

TEST(PngWriter, EmptyImageProducesNoFile) {
    CHECK(EncodePng(Image()).empty());
    CHECK(EncodePngDataUri(Image()).empty());
}

TEST(PngWriter, WritesASignatureAndAnIhdr) {
    const std::vector<uint8_t> png = EncodePng(MakeImage(4, 3, 0xFF102030u));
    REQUIRE_GE(png.size(), size_t(33));

    const std::vector<uint8_t> signature = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (size_t i = 0; i < signature.size(); ++i) {
        REQUIRE_EQ(int(png[i]), int(signature[i]));
    }

    CHECK_EQ(ReadU32Be(png, 8), 13u); // IHDR is always 13 bytes
    CHECK_EQ(png[12], uint8_t('I'));
    CHECK_EQ(png[13], uint8_t('H'));
    CHECK_EQ(ReadU32Be(png, 16), 4u); // width
    CHECK_EQ(ReadU32Be(png, 20), 3u); // height
    CHECK_EQ(int(png[24]), 8);        // bit depth
    CHECK_EQ(int(png[25]), 6);        // RGBA
    CHECK_EQ(int(png[26]), 0);        // deflate
    CHECK_EQ(int(png[27]), 0);        // adaptive filtering
    CHECK_EQ(int(png[28]), 0);        // no interlace
}

TEST(PngWriter, EndsWithIend) {
    const std::vector<uint8_t> png = EncodePng(MakeImage(2, 2, 0xFFFFFFFFu));
    REQUIRE_GE(png.size(), size_t(12));
    const size_t at = png.size() - 8;
    CHECK_EQ(png[at], uint8_t('I'));
    CHECK_EQ(png[at + 1], uint8_t('E'));
    CHECK_EQ(png[at + 2], uint8_t('N'));
    CHECK_EQ(png[at + 3], uint8_t('D'));
}

TEST(PngWriter, EveryChunkCrcIsCorrect) {
    // Walks the chunk list the way a decoder does. A wrong CRC is the single
    // most common way a hand-rolled PNG fails, and it fails silently in some
    // viewers and loudly in others.
    const std::vector<uint8_t> png = EncodePng(MakeImage(16, 9, 0xC0FF8040u));
    REQUIRE_GT(png.size(), size_t(8));

    static const auto crc32 = [](const uint8_t* data, size_t size) {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < size; ++i) {
            crc ^= data[i];
            for (int k = 0; k < 8; ++k) {
                crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
            }
        }
        return crc ^ 0xFFFFFFFFu;
    };

    size_t at = 8;
    int chunks = 0;
    while (at + 12 <= png.size()) {
        const uint32_t length = ReadU32Be(png, at);
        REQUIRE_LE(at + 12 + length, png.size());
        const uint32_t stored = ReadU32Be(png, at + 8 + length);
        REQUIRE_EQ(crc32(&png[at + 4], length + 4), stored);
        at += 12 + length;
        ++chunks;
    }
    CHECK_EQ(at, png.size());
    CHECK_EQ(chunks, 3); // IHDR, IDAT, IEND
}

TEST(PngWriter, PixelsSurviveTheRoundTrip) {
    // The real assertion: decompress IDAT, undo the Sub filter, and compare
    // against the source pixels channel by channel.
    Image image = MakeImage(5, 4, 0);
    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            image.pixels[y * image.width + x] =
                (uint32_t(0x80 + y) << 24) | (uint32_t(x * 20) << 16) |
                (uint32_t(y * 30) << 8) | uint32_t(x + y);
        }
    }

    const std::vector<uint8_t> png = EncodePng(image);

    // Find IDAT.
    size_t at = 8;
    std::vector<uint8_t> idat;
    while (at + 12 <= png.size()) {
        const uint32_t length = ReadU32Be(png, at);
        if (png[at + 4] == 'I' && png[at + 5] == 'D' && png[at + 6] == 'A' &&
            png[at + 7] == 'T') {
            idat.assign(png.begin() + static_cast<long>(at) + 8,
                        png.begin() + static_cast<long>(at + 8 + length));
            break;
        }
        at += 12 + length;
    }
    REQUIRE_FALSE(idat.empty());

    const std::vector<uint8_t> raw = Inflate(idat);
    const size_t stride = image.width * 4;
    REQUIRE_EQ(raw.size(), image.height * (1 + stride));

    for (uint32_t y = 0; y < image.height; ++y) {
        const size_t rowStart = y * (1 + stride);
        REQUIRE_EQ(int(raw[rowStart]), 1); // filter type Sub

        // Undo Sub: each byte was stored as (value - the byte 4 to its left).
        std::vector<uint8_t> row(stride);
        for (size_t i = 0; i < stride; ++i) {
            const uint8_t left = i >= 4 ? row[i - 4] : 0;
            row[i] = static_cast<uint8_t>(raw[rowStart + 1 + i] + left);
        }

        for (uint32_t x = 0; x < image.width; ++x) {
            const uint32_t pixel = image.pixels[y * image.width + x];
            REQUIRE_EQ(int(row[x * 4 + 0]), int((pixel >> 16) & 0xFF)); // R
            REQUIRE_EQ(int(row[x * 4 + 1]), int((pixel >> 8) & 0xFF));  // G
            REQUIRE_EQ(int(row[x * 4 + 2]), int(pixel & 0xFF));         // B
            REQUIRE_EQ(int(row[x * 4 + 3]), int((pixel >> 24) & 0xFF)); // A
        }
    }
}

// ---------------------------------------------------------------------------
// Base64
// ---------------------------------------------------------------------------

TEST(PngWriter, Base64MatchesTheRfcExamples) {
    const auto encode = [](const std::string& text) {
        return Base64(std::vector<uint8_t>(text.begin(), text.end()));
    };
    CHECK_EQ(encode(""), std::string(""));
    CHECK_EQ(encode("f"), std::string("Zg=="));
    CHECK_EQ(encode("fo"), std::string("Zm8="));
    CHECK_EQ(encode("foo"), std::string("Zm9v"));
    CHECK_EQ(encode("foob"), std::string("Zm9vYg=="));
    CHECK_EQ(encode("fooba"), std::string("Zm9vYmE="));
    CHECK_EQ(encode("foobar"), std::string("Zm9vYmFy"));
}

TEST(PngWriter, Base64HandlesHighBytes) {
    const std::vector<uint8_t> bytes = {0xFF, 0xFE, 0xFD};
    CHECK_EQ(Base64(bytes), std::string("//79"));
}

TEST(PngWriter, DataUriIsUsableInMarkup) {
    const std::string uri = EncodePngDataUri(MakeImage(3, 3, 0xFF00FF00u));
    REQUIRE_GT(uri.size(), size_t(30));
    CHECK_EQ(uri.substr(0, 22), std::string("data:image/png;base64,"));
    // Nothing in the payload may need escaping inside an XML attribute.
    CHECK_EQ(uri.find_first_of("<>&\"'"), std::string::npos);
}
