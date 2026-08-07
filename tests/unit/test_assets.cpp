// Unit tests: bitmap decoding and the baked asset pack.
//
// Everything the welcome screen draws goes through here, so a bug shows up as
// a logon screen that is subtly - or entirely - wrong. The decoder is fed
// hand-built bitmaps covering every format that actually appears inside
// logonui.exe, and the last test in the file pins a specific pixel of the real
// artwork so a regression in the RLE or row-order handling cannot pass quietly.
#include "xplogin/assets/AssetPack.h"
#include "xplogin/assets/Image.h"
#include "xplogin/assets/PeResources.h"
#include "xptest.h"

#include <fstream>
#include <map>
#include <set>
#include <vector>

using namespace xplogin::assets;

namespace {

void PutU16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
    b[at] = static_cast<uint8_t>(v & 0xFF);
    b[at + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void PutU32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        b[at + static_cast<size_t>(i)] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
    }
}

// A BITMAPINFOHEADER with the given geometry. `height` may be negative for a
// top-down image.
std::vector<uint8_t> MakeHeader(int32_t width, int32_t height, uint16_t bpp,
                                uint32_t compression = 0,
                                uint32_t paletteEntries = 0) {
    std::vector<uint8_t> header(40 + paletteEntries * 4, 0);
    PutU32(header, 0, 40);
    PutU32(header, 4, static_cast<uint32_t>(width));
    PutU32(header, 8, static_cast<uint32_t>(height));
    PutU16(header, 12, 1);
    PutU16(header, 14, bpp);
    PutU32(header, 16, compression);
    PutU32(header, 32, paletteEntries);
    return header;
}

void SetPaletteEntry(std::vector<uint8_t>& bitmap, uint32_t index, uint8_t r,
                     uint8_t g, uint8_t b) {
    const size_t at = 40 + static_cast<size_t>(index) * 4;
    bitmap[at + 0] = b;
    bitmap[at + 1] = g;
    bitmap[at + 2] = r;
    bitmap[at + 3] = 0;
}

constexpr uint32_t Argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
           (static_cast<uint32_t>(g) << 8) | b;
}

// Finds one of XP's binaries if a developer has dropped a system32 into
// assets/. `name` is the file, e.g. "logonui.exe" or "msgina.dll".
std::vector<uint8_t> ReadReferenceBinary(const std::string& name) {
    const std::string candidates[] = {
        "assets/system32/" + name,
        "../assets/system32/" + name,
        "../../assets/system32/" + name,
    };
    for (const std::string& candidate : candidates) {
        const char* path = candidate.c_str();
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            continue;
        }
        const std::streamsize size = file.tellg();
        if (size <= 0) {
            continue;
        }
        file.seekg(0);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (file.read(reinterpret_cast<char*>(bytes.data()), size)) {
            return bytes;
        }
    }
    return {};
}

} // namespace

// ---------------------------------------------------------------------------
// 24bpp - the bulk of XP's artwork
// ---------------------------------------------------------------------------

TEST(BmpDecoder, Decodes24BitBottomUp) {
    // 2x2, stored bottom-up as BMP does by default. Row stride is padded to 4
    // bytes: 2 pixels * 3 bytes = 6, padded to 8.
    std::vector<uint8_t> bitmap = MakeHeader(2, 2, 24);
    const uint8_t pixels[] = {
        // bottom row first: blue, green, then 2 bytes of padding
        0xFF, 0x00, 0x00,  0x00, 0xFF, 0x00,  0x00, 0x00,
        // top row: red, white
        0x00, 0x00, 0xFF,  0xFF, 0xFF, 0xFF,  0x00, 0x00,
    };
    bitmap.insert(bitmap.end(), std::begin(pixels), std::end(pixels));

    Image image;
    REQUIRE_EQ(static_cast<int>(DecodeBitmapResource(bitmap, &image)),
               static_cast<int>(BmpStatus::Ok));
    CHECK_EQ(image.width, 2u);
    CHECK_EQ(image.height, 2u);

    // The decoder normalises to top-down, so the last stored row comes first.
    CHECK_EQ(image.At(0, 0), Argb(0xFF, 0xFF, 0x00, 0x00)); // red
    CHECK_EQ(image.At(1, 0), Argb(0xFF, 0xFF, 0xFF, 0xFF)); // white
    CHECK_EQ(image.At(0, 1), Argb(0xFF, 0x00, 0x00, 0xFF)); // blue
    CHECK_EQ(image.At(1, 1), Argb(0xFF, 0x00, 0xFF, 0x00)); // green
}

TEST(BmpDecoder, NegativeHeightMeansTopDown) {
    std::vector<uint8_t> bitmap = MakeHeader(2, -2, 24);
    const uint8_t pixels[] = {
        0x00, 0x00, 0xFF,  0xFF, 0xFF, 0xFF,  0x00, 0x00,
        0xFF, 0x00, 0x00,  0x00, 0xFF, 0x00,  0x00, 0x00,
    };
    bitmap.insert(bitmap.end(), std::begin(pixels), std::end(pixels));

    Image image;
    REQUIRE_EQ(static_cast<int>(DecodeBitmapResource(bitmap, &image)),
               static_cast<int>(BmpStatus::Ok));
    CHECK_EQ(image.height, 2u);
    // Stored in the order it is drawn, so no flip.
    CHECK_EQ(image.At(0, 0), Argb(0xFF, 0xFF, 0x00, 0x00));
    CHECK_EQ(image.At(0, 1), Argb(0xFF, 0x00, 0x00, 0xFF));
}

TEST(BmpDecoder, RespectsRowPadding) {
    // 3 pixels * 3 bytes = 9, padded to 12. Getting the stride wrong shears
    // the image, which is the classic symptom.
    std::vector<uint8_t> bitmap = MakeHeader(3, 1, 24);
    const uint8_t pixels[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
                              0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC};
    bitmap.insert(bitmap.end(), std::begin(pixels), std::end(pixels));

    Image image;
    REQUIRE_EQ(static_cast<int>(DecodeBitmapResource(bitmap, &image)),
               static_cast<int>(BmpStatus::Ok));
    CHECK_EQ(image.At(0, 0), Argb(0xFF, 0x33, 0x22, 0x11));
    CHECK_EQ(image.At(2, 0), Argb(0xFF, 0x99, 0x88, 0x77));
}

// ---------------------------------------------------------------------------
// 32bpp - the account picture frames
// ---------------------------------------------------------------------------

TEST(BmpDecoder, Decodes32BitWithAlpha) {
    std::vector<uint8_t> bitmap = MakeHeader(2, 1, 32);
    const uint8_t pixels[] = {
        0x10, 0x20, 0x30, 0x80,   // B G R A
        0x40, 0x50, 0x60, 0xFF,
    };
    bitmap.insert(bitmap.end(), std::begin(pixels), std::end(pixels));

    Image image;
    REQUIRE_EQ(static_cast<int>(DecodeBitmapResource(bitmap, &image)),
               static_cast<int>(BmpStatus::Ok));
    CHECK_EQ(image.At(0, 0), Argb(0x80, 0x30, 0x20, 0x10));
    CHECK_EQ(image.At(1, 0), Argb(0xFF, 0x60, 0x50, 0x40));
    CHECK(HasMeaningfulAlpha(image));
}

TEST(BmpDecoder, AllZeroAlphaIsTreatedAsOpaque) {
    // Plenty of "32bpp" Windows bitmaps never fill the alpha channel in.
    // Taking that at face value makes the image invisible, which is how you
    // lose half of XP's artwork without any error anywhere.
    std::vector<uint8_t> bitmap = MakeHeader(2, 1, 32);
    const uint8_t pixels[] = {
        0x10, 0x20, 0x30, 0x00,
        0x40, 0x50, 0x60, 0x00,
    };
    bitmap.insert(bitmap.end(), std::begin(pixels), std::end(pixels));

    Image image;
    REQUIRE_EQ(static_cast<int>(DecodeBitmapResource(bitmap, &image)),
               static_cast<int>(BmpStatus::Ok));
    CHECK_EQ(int((image.At(0, 0) >> 24) & 0xFF), 0xFF);
    CHECK_EQ(int((image.At(1, 0) >> 24) & 0xFF), 0xFF);
    CHECK_FALSE(HasMeaningfulAlpha(image));
}

// ---------------------------------------------------------------------------
// Paletted and RLE - #102 and #112 in logonui.exe are RLE8
// ---------------------------------------------------------------------------

TEST(BmpDecoder, Decodes8BitPaletted) {
    std::vector<uint8_t> bitmap = MakeHeader(2, 1, 8, 0, 4);
    SetPaletteEntry(bitmap, 0, 0xFF, 0x00, 0x00);
    SetPaletteEntry(bitmap, 1, 0x00, 0xFF, 0x00);
    const uint8_t pixels[] = {0x00, 0x01, 0x00, 0x00}; // padded to 4
    bitmap.insert(bitmap.end(), std::begin(pixels), std::end(pixels));

    Image image;
    REQUIRE_EQ(static_cast<int>(DecodeBitmapResource(bitmap, &image)),
               static_cast<int>(BmpStatus::Ok));
    CHECK_EQ(image.At(0, 0), Argb(0xFF, 0xFF, 0x00, 0x00));
    CHECK_EQ(image.At(1, 0), Argb(0xFF, 0x00, 0xFF, 0x00));
}

TEST(BmpDecoder, DecodesRle8EncodedRuns) {
    std::vector<uint8_t> bitmap = MakeHeader(4, 2, 8, 1, 4);
    SetPaletteEntry(bitmap, 0, 0x11, 0x11, 0x11);
    SetPaletteEntry(bitmap, 1, 0x22, 0x22, 0x22);

    const uint8_t stream[] = {
        4, 0,        // four pixels of palette 0
        0, 0,        // end of line
        4, 1,        // four pixels of palette 1
        0, 1,        // end of bitmap
    };
    bitmap.insert(bitmap.end(), std::begin(stream), std::end(stream));

    Image image;
    REQUIRE_EQ(static_cast<int>(DecodeBitmapResource(bitmap, &image)),
               static_cast<int>(BmpStatus::Ok));
    // Bottom-up: the first decoded row is the bottom one.
    CHECK_EQ(image.At(0, 1), Argb(0xFF, 0x11, 0x11, 0x11));
    CHECK_EQ(image.At(3, 1), Argb(0xFF, 0x11, 0x11, 0x11));
    CHECK_EQ(image.At(0, 0), Argb(0xFF, 0x22, 0x22, 0x22));
}

TEST(BmpDecoder, DecodesRle8AbsoluteRunsWithWordPadding) {
    std::vector<uint8_t> bitmap = MakeHeader(3, 1, 8, 1, 4);
    SetPaletteEntry(bitmap, 0, 0x10, 0x10, 0x10);
    SetPaletteEntry(bitmap, 1, 0x20, 0x20, 0x20);
    SetPaletteEntry(bitmap, 2, 0x30, 0x30, 0x30);

    const uint8_t stream[] = {
        0, 3,          // absolute run of three literal indices
        0, 1, 2,
        0,             // padding to a word boundary - miss this and the
                       // next opcode is read one byte early
        0, 1,          // end of bitmap
    };
    bitmap.insert(bitmap.end(), std::begin(stream), std::end(stream));

    Image image;
    REQUIRE_EQ(static_cast<int>(DecodeBitmapResource(bitmap, &image)),
               static_cast<int>(BmpStatus::Ok));
    CHECK_EQ(image.At(0, 0), Argb(0xFF, 0x10, 0x10, 0x10));
    CHECK_EQ(image.At(1, 0), Argb(0xFF, 0x20, 0x20, 0x20));
    CHECK_EQ(image.At(2, 0), Argb(0xFF, 0x30, 0x30, 0x30));
}

TEST(BmpDecoder, Rle8DeltaSkipsPixels) {
    std::vector<uint8_t> bitmap = MakeHeader(4, 2, 8, 1, 4);
    SetPaletteEntry(bitmap, 1, 0xAB, 0xCD, 0xEF);

    const uint8_t stream[] = {
        0, 2, 2, 1,   // delta: right 2, down 1
        1, 1,         // one pixel of palette 1
        0, 1,
    };
    bitmap.insert(bitmap.end(), std::begin(stream), std::end(stream));

    Image image;
    REQUIRE_EQ(static_cast<int>(DecodeBitmapResource(bitmap, &image)),
               static_cast<int>(BmpStatus::Ok));
    // Row 1 of a 2-row bottom-up image is the top row on screen.
    CHECK_EQ(image.At(2, 0), Argb(0xFF, 0xAB, 0xCD, 0xEF));
    CHECK_EQ(image.At(0, 0), 0u); // untouched
}

TEST(BmpDecoder, RejectsATruncatedRleStream) {
    std::vector<uint8_t> bitmap = MakeHeader(4, 1, 8, 1, 4);
    const uint8_t stream[] = {0, 4, 1, 2}; // claims 4 literals, supplies 2
    bitmap.insert(bitmap.end(), std::begin(stream), std::end(stream));

    Image image;
    CHECK_EQ(static_cast<int>(DecodeBitmapResource(bitmap, &image)),
             static_cast<int>(BmpStatus::BadRleStream));
}

// ---------------------------------------------------------------------------
// Malformed input
// ---------------------------------------------------------------------------

TEST(BmpDecoder, RejectsMalformedHeaders) {
    Image image;
    CHECK_EQ(static_cast<int>(DecodeBitmapResource({}, &image)),
             static_cast<int>(BmpStatus::TooSmall));
    CHECK_EQ(static_cast<int>(
                 DecodeBitmapResource(std::vector<uint8_t>(20, 0), &image)),
             static_cast<int>(BmpStatus::TooSmall));

    CHECK_EQ(static_cast<int>(DecodeBitmapResource(MakeHeader(0, 4, 24), &image)),
             static_cast<int>(BmpStatus::BadDimensions));
    CHECK_EQ(static_cast<int>(DecodeBitmapResource(MakeHeader(4, 0, 24), &image)),
             static_cast<int>(BmpStatus::BadDimensions));
    // A header claiming a gigantic image must be refused, not allocated.
    CHECK_EQ(static_cast<int>(
                 DecodeBitmapResource(MakeHeader(100000, 100000, 24), &image)),
             static_cast<int>(BmpStatus::BadDimensions));
    CHECK_EQ(static_cast<int>(DecodeBitmapResource(MakeHeader(4, 4, 7), &image)),
             static_cast<int>(BmpStatus::UnsupportedBitDepth));
}

TEST(BmpDecoder, RejectsTruncatedPixelData) {
    std::vector<uint8_t> bitmap = MakeHeader(64, 64, 24);
    bitmap.insert(bitmap.end(), 100, 0); // nowhere near 64 rows
    Image image;
    CHECK_EQ(static_cast<int>(DecodeBitmapResource(bitmap, &image)),
             static_cast<int>(BmpStatus::Truncated));
}

TEST(BmpDecoder, AcceptsAFullBmpFileToo) {
    std::vector<uint8_t> bitmap = MakeHeader(1, 1, 24);
    const uint8_t pixels[] = {0x01, 0x02, 0x03, 0x00};
    bitmap.insert(bitmap.end(), std::begin(pixels), std::end(pixels));

    std::vector<uint8_t> file = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0};
    file.insert(file.end(), bitmap.begin(), bitmap.end());

    Image image;
    REQUIRE_EQ(static_cast<int>(DecodeBitmapFile(file, &image)),
               static_cast<int>(BmpStatus::Ok));
    CHECK_EQ(image.At(0, 0), Argb(0xFF, 0x03, 0x02, 0x01));

    Image rejected;
    CHECK_EQ(static_cast<int>(DecodeBitmapFile({'N', 'O'}, &rejected)),
             static_cast<int>(BmpStatus::TooSmall));
}

// ---------------------------------------------------------------------------
// Colour keying
// ---------------------------------------------------------------------------

TEST(Assets, ColourKeyOnlyClearsTheKeyedColour) {
    Image image;
    image.width = 3;
    image.height = 1;
    image.pixels = {Argb(0xFF, 0xFF, 0x00, 0xFF),  // magenta - the key
                    Argb(0xFF, 0xFE, 0x00, 0xFF),  // one off, must survive
                    Argb(0xFF, 0x12, 0x34, 0x56)};

    ApplyColorKey(&image, 0x00FF00FFu);
    CHECK_EQ(image.At(0, 0), 0u);
    CHECK_EQ(image.At(1, 0), Argb(0xFF, 0xFE, 0x00, 0xFF));
    CHECK_EQ(image.At(2, 0), Argb(0xFF, 0x12, 0x34, 0x56));
}

// ---------------------------------------------------------------------------
// The pack
// ---------------------------------------------------------------------------

TEST(AssetPack, RoundTripsImages) {
    Image first;
    first.width = 2;
    first.height = 2;
    first.pixels = {1, 2, 3, 4};

    Image second;
    second.width = 1;
    second.height = 3;
    second.pixels = {0xAABBCCDD, 0x11223344, 0};

    std::map<uint32_t, Image> images;
    images[125] = first;
    images[126] = second;

    AssetPack pack;
    REQUIRE_EQ(static_cast<int>(AssetPack::Load(AssetPack::Build(images), &pack)),
               static_cast<int>(PackStatus::Ok));

    CHECK_EQ(pack.Count(), size_t(2));
    CHECK(pack.Has(125));
    CHECK(pack.Has(126));
    CHECK_FALSE(pack.Has(999));
    CHECK(pack.Get(999) == nullptr);

    const Image* loaded = pack.Get(126);
    REQUIRE(loaded != nullptr);
    CHECK_EQ(loaded->width, 1u);
    CHECK_EQ(loaded->height, 3u);
    CHECK_EQ(loaded->At(0, 0), 0xAABBCCDDu);
    CHECK_EQ(loaded->At(0, 1), 0x11223344u);
}

TEST(AssetPack, RejectsSomethingThatIsNotAPack) {
    AssetPack pack;
    CHECK_EQ(static_cast<int>(AssetPack::Load({}, &pack)),
             static_cast<int>(PackStatus::Truncated));

    std::vector<uint8_t> garbage(64, 0x5A);
    CHECK_EQ(static_cast<int>(AssetPack::Load(garbage, &pack)),
             static_cast<int>(PackStatus::BadMagic));
}

TEST(AssetPack, RejectsAWrongVersion) {
    std::map<uint32_t, Image> images;
    Image image;
    image.width = 1;
    image.height = 1;
    image.pixels = {0};
    images[1] = image;

    std::vector<uint8_t> bytes = AssetPack::Build(images);
    bytes[4] = 99; // version

    AssetPack pack;
    CHECK_EQ(static_cast<int>(AssetPack::Load(bytes, &pack)),
             static_cast<int>(PackStatus::UnsupportedVersion));
}

TEST(AssetPack, RejectsATruncatedOrLyingIndex) {
    std::map<uint32_t, Image> images;
    Image image;
    image.width = 4;
    image.height = 4;
    image.pixels.assign(16, 0x12345678);
    images[7] = image;

    const std::vector<uint8_t> good = AssetPack::Build(images);

    // Chopped in half: the payload the index promises is not there.
    std::vector<uint8_t> chopped(good.begin(), good.begin() + good.size() / 2);
    AssetPack pack;
    CHECK_EQ(static_cast<int>(AssetPack::Load(chopped, &pack)),
             static_cast<int>(PackStatus::Truncated));

    // Dimensions that do not match the declared byte count. Trusting this is
    // how a corrupt pack becomes an out-of-bounds read inside LogonUI.
    std::vector<uint8_t> lying = good;
    lying[16 + 4] = 99; // width
    CHECK_EQ(static_cast<int>(AssetPack::Load(lying, &pack)),
             static_cast<int>(PackStatus::CorruptIndex));
}

TEST(AssetPack, EmptyPackIsValidButHasNothingInIt) {
    AssetPack pack;
    REQUIRE_EQ(static_cast<int>(AssetPack::Load(AssetPack::Build({}), &pack)),
               static_cast<int>(PackStatus::Ok));
    CHECK_EQ(pack.Count(), size_t(0));
    CHECK(pack.Ids().empty());
}

TEST(Assets, ManifestIsInternallyConsistent) {
    std::set<uint32_t> ids;
    for (const AssetSpec& spec : WelcomeScreenAssets()) {
        CHECK(ids.insert(spec.resourceId).second); // no duplicates
        CHECK(spec.description != nullptr);
    }
    CHECK_GE(WelcomeScreenAssets().size(), size_t(20));

    // The two 32bpp frames carry a real alpha channel, so keying them would
    // punch holes in the artwork. Their markup argument of 255 is a marker,
    // not a colour.
    for (const AssetSpec& spec : WelcomeScreenAssets()) {
        if (spec.resourceId == assetid::kPictureFrame ||
            spec.resourceId == assetid::kPictureFrameHot) {
            CHECK_EQ(spec.colorKey, kNoColorKey);
        }
    }
}

// ---------------------------------------------------------------------------
// Against the real artwork, when logonui.exe has been dropped into assets/
// ---------------------------------------------------------------------------

namespace {

// Bakes the whole spec list the way xp-bake does, from whichever of XP's
// binaries are present. Empty when the reference material is not on this
// machine, which is the normal case for a contributor without XP files.
std::map<uint32_t, Image> BakeReferenceAssets() {
    std::map<AssetSource, PeFile> sources;
    for (AssetSource source : {AssetSource::LogonUi, AssetSource::MsGina}) {
        std::vector<uint8_t> bytes =
            ReadReferenceBinary(AssetSourceFileName(source));
        if (bytes.empty()) {
            continue;
        }
        PeFile pe;
        if (PeFile::Load(std::move(bytes), &pe) == PeStatus::Ok) {
            sources.emplace(source, std::move(pe));
        }
    }

    std::map<uint32_t, Image> baked;
    for (const AssetSpec& spec : WelcomeScreenAssets()) {
        const auto source = sources.find(spec.source);
        if (source == sources.end()) {
            continue;
        }
        for (const ResourceEntry& entry : source->second.Resources()) {
            if (!entry.type.Is(restype::kBitmap) ||
                !entry.name.Is(spec.resourceId)) {
                continue;
            }
            Image image;
            if (DecodeBitmapResource(source->second.Read(entry), &image) !=
                BmpStatus::Ok) {
                break;
            }
            if (spec.colorKey != kNoColorKey) {
                ApplyColorKey(&image, spec.colorKey);
            }
            baked.emplace(spec.resourceId, std::move(image));
            break;
        }
    }
    return baked;
}

} // namespace

TEST(Assets, BakesTheRealWelcomeScreenArtwork) {
    std::map<uint32_t, Image> baked = BakeReferenceAssets();
    if (baked.empty()) {
        return; // no reference material on this machine
    }

    // Every asset the screen needs must be present and decodable.
    CHECK_EQ(baked.size(), WelcomeScreenAssets().size());

    // Geometry, straight out of the file.
    REQUIRE(baked.count(assetid::kBottomDivider) == 1);
    const Image& footer = baked[assetid::kBottomDivider];
    CHECK_EQ(footer.width, 800u);
    CHECK_EQ(footer.height, 3u);

    // THE assertion. XP's footer rule is not a flat orange line, it is an
    // 800px sweep whose highlight sits about a third of the way across. This
    // pins the exact pixel: get the row order, the stride or the RLE handling
    // wrong and this changes.
    CHECK_EQ(footer.At(250, 1) & 0x00FFFFFFu, 0x00F69538u);
    // ...and it really is a sweep: the edges are the panel's dark blue.
    CHECK_EQ(footer.At(0, 1) & 0x00FFFFFFu, 0x00003399u);
    CHECK_EQ(footer.At(799, 1) & 0x00FFFFFFu, 0x00003399u);

    // The header rule is the same idea in light blue.
    const Image& header = baked[assetid::kTopDivider];
    CHECK_EQ(header.width, 800u);
    CHECK_EQ(header.At(250, 1) & 0x00FFFFFFu, 0x00C6DCFEu);

    // The two RLE8 bitmaps decoded to something with real content rather than
    // a single flat colour, which is what a broken RLE decoder produces.
    const Image& button = baked[assetid::kSelectedTile];
    CHECK_EQ(button.width, 308u);
    CHECK_EQ(button.height, 72u);
    std::set<uint32_t> distinct;
    for (uint32_t pixel : button.pixels) {
        distinct.insert(pixel);
    }
    CHECK_GT(distinct.size(), size_t(20));

    // The picture frame is 32bpp with a real alpha channel: rounded transparent
    // corners and a soft drop shadow down the right and bottom edges.
    const Image& frame = baked[assetid::kPictureFrame];
    CHECK_EQ(frame.width, 58u);
    CHECK_EQ(frame.height, 58u);
    CHECK(HasMeaningfulAlpha(frame));
    CHECK_EQ(frame.At(0, 0) >> 24, 0u);       // corner: transparent
    CHECK_EQ(frame.At(29, 29) >> 24, 255u);   // middle: opaque

    // It is a plate, not a cut-out frame, and that distinction decides the
    // draw order in the renderer: the flat interior is exactly the 48x48 well
    // at 5..52 that the markup's borderthickness rect(5,5,5,5) describes, so
    // the account picture goes ON it, not under it. Painting the picture first
    // would leave it completely hidden.
    const uint32_t well = frame.At(29, 29);
    for (uint32_t x = 5; x <= 52; ++x) {
        REQUIRE_EQ(frame.At(x, 5), well);
        REQUIRE_EQ(frame.At(x, 52), well);
    }
    // ...and the ring just outside the well is a different colour, which is
    // what makes it a border rather than a uniform square.
    CHECK(frame.At(4, 29) != well);
    CHECK(frame.At(53, 29) != well);

    // And the logo is the size the markup says it is.
    const Image& logo = baked[assetid::kLogo];
    CHECK_EQ(logo.width, 137u);
    CHECK_EQ(logo.height, 86u);

    // Finally, the whole set survives a pack round trip.
    AssetPack pack;
    REQUIRE_EQ(static_cast<int>(AssetPack::Load(AssetPack::Build(baked), &pack)),
               static_cast<int>(PackStatus::Ok));
    CHECK_EQ(pack.Count(), baked.size());
    REQUIRE(pack.Get(assetid::kBottomDivider) != nullptr);
    CHECK_EQ(pack.Get(assetid::kBottomDivider)->At(250, 1) & 0x00FFFFFFu,
             0x00F69538u);
}

// ---------------------------------------------------------------------------
// msgina.dll - the "Turn off computer" dialog.
//
// The welcome screen's power button opens a modal that logonui.exe does not
// own: there is no shutdown panel anywhere in its UIFILE markup and none of
// the artwork is in its resources. It belongs to msgina.dll, drawn as a plain
// Win32 dialog (template #20100) with owner-drawn buttons. These pin what was
// found there, because none of it is quotable from markup the way the welcome
// screen's geometry is - the evidence is the bitmaps themselves.
// ---------------------------------------------------------------------------

TEST(Assets, TheShutdownDialogArtworkIsWhatWeThinkItIs) {
    std::map<uint32_t, Image> baked = BakeReferenceAssets();
    if (baked.empty()) {
        return;
    }
    REQUIRE(baked.count(assetid::kShutdownPanel) == 1);
    REQUIRE(baked.count(assetid::kShutdownOrbs) == 1);
    REQUIRE(baked.count(assetid::kShutdownFlag) == 1);

    // The background is what fixes the dialog's size, and through it the
    // dialog-unit conversion: template #20100 is 208x122du, and
    // 208 * 6/4 = 312, 122 * 13/8 = 198.25. If this bitmap were another size
    // every control rectangle in XpMetrics would be placed wrongly.
    const Image& panel = baked[assetid::kShutdownPanel];
    CHECK_EQ(panel.width, 313u);
    CHECK_EQ(panel.height, 198u);

    // Three bands: navy title, the welcome screen's own blue, navy again.
    // XpMetrics puts the title at y 0..41, the orbs at 80 and the labels at
    // 119 - all inside the blue - and Cancel at 167, back in the navy.
    CHECK_EQ(panel.At(4, 10)  & 0x00FFFFFFu, 0x00003399u); // title band
    CHECK_EQ(panel.At(4, 100) & 0x00FFFFFFu, 0x005A7DDEu); // body
    CHECK_EQ(panel.At(4, 190) & 0x00FFFFFFu, 0x00003399u); // Cancel band
    // The bands really do end where the metrics assume.
    CHECK_NE(panel.At(4, 42) & 0x00FFFFFFu, panel.At(4, 100) & 0x00FFFFFFu);
    CHECK_NE(panel.At(4, 157) & 0x00FFFFFFu, panel.At(4, 100) & 0x00FFFFFFu);

    // Ten 32x32 frames stacked: three states each for Turn Off, Stand By and
    // Restart, then a disabled Stand By. The strip is the reason the renderer
    // needs a frame blitter at all.
    const Image& orbs = baked[assetid::kShutdownOrbs];
    CHECK_EQ(orbs.width, static_cast<uint32_t>(shutdownorb::kFrameSize));
    CHECK_EQ(orbs.height, 320u);
    CHECK_EQ(orbs.height / shutdownorb::kFrameSize, 10u);

    // Each frame's corners are the magenta key, so after keying they are
    // transparent - which is what lets a round orb sit on the blue panel.
    for (int frame = 0; frame < 10; ++frame) {
        const uint32_t corner =
            orbs.At(0, static_cast<uint32_t>(frame * shutdownorb::kFrameSize));
        CHECK_EQ(corner >> 24, 0u);
    }

    // And the frames are the colours the labels promise. Centre pixel of the
    // first frame of each button: red for Turn Off, amber for Stand By, green
    // for Restart. This is what says the strip is in the order the renderer
    // indexes it by - getting it wrong would put a green orb over "Turn Off".
    auto centreOf = [&orbs](int frame) {
        return orbs.At(16, static_cast<uint32_t>(frame * shutdownorb::kFrameSize + 16)) &
               0x00FFFFFFu;
    };
    const uint32_t turnOff = centreOf(shutdownorb::kTurnOff);
    const uint32_t standBy = centreOf(shutdownorb::kStandBy);
    const uint32_t restart = centreOf(shutdownorb::kRestart);

    auto red   = [](uint32_t c) { return (c >> 16) & 0xFF; };
    auto green = [](uint32_t c) { return (c >> 8) & 0xFF; };
    auto blue  = [](uint32_t c) { return c & 0xFF; };

    // Turn Off's centre is the white power glyph, so look at the orb body.
    const uint32_t turnOffBody =
        orbs.At(4, static_cast<uint32_t>(shutdownorb::kTurnOff * 32 + 16)) & 0x00FFFFFFu;
    CHECK_GT(red(turnOffBody), green(turnOffBody));
    CHECK_GT(red(turnOffBody), blue(turnOffBody));

    CHECK_GT(red(standBy), blue(standBy));    // amber: red and green, little blue
    CHECK_GT(green(standBy), blue(standBy));
    CHECK_GT(green(restart), red(restart));   // green
    CHECK_GT(green(restart), blue(restart));
    (void)turnOff;

    // The flag in the title band is the size the template reserves for it.
    const Image& flag = baked[assetid::kShutdownFlag];
    CHECK_EQ(flag.width, 48u);
    CHECK_EQ(flag.height, 40u);
}
