// Unit tests: reading resources out of a PE file.
//
// The input here is an untrusted binary, so the tests care as much about the
// malformed cases as the happy path: a truncated file, a resource pointing
// outside the image, a tree deeper than a resource tree can legally be. Any of
// those reading out of bounds would be a security bug in a tool that people
// point at random executables.
//
// The fixture builds a PE byte by byte rather than shipping a binary, so the
// test is deterministic and reviewable. A second test runs against the real
// luna.msstyles when a developer has dropped one into assets/.
#include "xplogin/assets/PeResources.h"
#include "xptest.h"

#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace xplogin::assets;

namespace {

// ---------------------------------------------------------------------------
// A minimal PE32 with one .rsrc section holding a hand-laid-out resource tree.
// ---------------------------------------------------------------------------

constexpr uint32_t kResourceRva = 0x1000;
constexpr uint32_t kResourceRawOffset = 0x200;

void PutU16(std::vector<uint8_t>& buffer, size_t offset, uint16_t value) {
    buffer[offset] = static_cast<uint8_t>(value & 0xFF);
    buffer[offset + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
}

void PutU32(std::vector<uint8_t>& buffer, size_t offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        buffer[offset + static_cast<size_t>(i)] =
            static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

// A 2x2 32bpp bitmap resource: BITMAPINFOHEADER with no file header, which is
// exactly how RT_BITMAP is stored.
std::vector<uint8_t> MakeBitmapResource() {
    std::vector<uint8_t> bitmap(40 + 16, 0);
    PutU32(bitmap, 0, 40);   // biSize
    PutU32(bitmap, 4, 2);    // biWidth
    PutU32(bitmap, 8, 2);    // biHeight
    PutU16(bitmap, 12, 1);   // biPlanes
    PutU16(bitmap, 14, 32);  // biBitCount
    PutU32(bitmap, 16, 0);   // biCompression = BI_RGB
    for (size_t i = 0; i < 16; ++i) {
        bitmap[40 + i] = static_cast<uint8_t>(0x10 + i);
    }
    return bitmap;
}

// Builds the .rsrc blob. Layout is fixed and documented so the offsets in the
// directory entries below can be read against it.
//
//   0x000 type directory        (1 named "UIFILE", 1 id 2/BITMAP)
//   0x020 name directory for UIFILE     -> id 1
//   0x040 name directory for BITMAP     -> id 101
//   0x060 language directory            -> 1033, leaf at 0x0A0
//   0x080 language directory            -> 1033, leaf at 0x0B0
//   0x0A0 data entry for the UIFILE
//   0x0B0 data entry for the bitmap
//   0x100 the name string "UIFILE"
//   0x120 UIFILE payload  (up to 0x60 bytes)
//   0x180 bitmap payload
std::vector<uint8_t> BuildResourceBlob(const std::string& uifileText,
                                       const std::vector<uint8_t>& bitmap) {
    std::vector<uint8_t> blob(0x200, 0);
    // The two payload slots must not overlap; keeping the assertion here means
    // a longer sample text fails loudly instead of silently corrupting a test.
    if (uifileText.size() > 0x60 || 0x180 + bitmap.size() > blob.size()) {
        return {};
    }

    // --- type directory ---
    PutU16(blob, 0x00C, 1); // named entries
    PutU16(blob, 0x00E, 1); // id entries
    PutU32(blob, 0x010, 0x80000000u | 0x100); // name -> string at 0x100
    PutU32(blob, 0x014, 0x80000000u | 0x020); // subdirectory
    PutU32(blob, 0x018, 2);                   // RT_BITMAP
    PutU32(blob, 0x01C, 0x80000000u | 0x040);

    // --- name directory: UIFILE -> 1 ---
    PutU16(blob, 0x02C, 0);
    PutU16(blob, 0x02E, 1);
    PutU32(blob, 0x030, 1);
    PutU32(blob, 0x034, 0x80000000u | 0x060);

    // --- name directory: BITMAP -> 101 ---
    PutU16(blob, 0x04C, 0);
    PutU16(blob, 0x04E, 1);
    PutU32(blob, 0x050, 101);
    PutU32(blob, 0x054, 0x80000000u | 0x080);

    // --- language directories ---
    PutU16(blob, 0x06C, 0);
    PutU16(blob, 0x06E, 1);
    PutU32(blob, 0x070, 1033);
    PutU32(blob, 0x074, 0x0A0); // leaf, high bit clear

    PutU16(blob, 0x08C, 0);
    PutU16(blob, 0x08E, 1);
    PutU32(blob, 0x090, 1033);
    PutU32(blob, 0x094, 0x0B0);

    // --- data entries ---
    PutU32(blob, 0x0A0, kResourceRva + 0x120);
    PutU32(blob, 0x0A4, static_cast<uint32_t>(uifileText.size()));
    PutU32(blob, 0x0B0, kResourceRva + 0x180);
    PutU32(blob, 0x0B4, static_cast<uint32_t>(bitmap.size()));

    // --- the name string "UIFILE" ---
    const char16_t name[] = u"UIFILE";
    PutU16(blob, 0x100, 6);
    for (size_t i = 0; i < 6; ++i) {
        PutU16(blob, 0x102 + i * 2, static_cast<uint16_t>(name[i]));
    }

    // --- payloads ---
    for (size_t i = 0; i < uifileText.size(); ++i) {
        blob[0x120 + i] = static_cast<uint8_t>(uifileText[i]);
    }
    for (size_t i = 0; i < bitmap.size(); ++i) {
        blob[0x180 + i] = bitmap[i];
    }
    return blob;
}

std::vector<uint8_t> BuildPe(const std::vector<uint8_t>& resourceBlob,
                             bool includeResourceDirectory = true) {
    std::vector<uint8_t> pe(kResourceRawOffset + resourceBlob.size(), 0);

    // DOS header
    pe[0] = 'M';
    pe[1] = 'Z';
    PutU32(pe, 0x3C, 0x40); // e_lfanew

    // PE signature + file header
    pe[0x40] = 'P';
    pe[0x41] = 'E';
    PutU16(pe, 0x44, 0x014C); // machine = i386
    PutU16(pe, 0x46, 1);      // one section
    PutU16(pe, 0x54, 224);    // SizeOfOptionalHeader

    // Optional header (PE32)
    const size_t optional = 0x58;
    PutU16(pe, optional, 0x010B);

    // Data directory entry 2 = resources
    const size_t dataDirectory = optional + 96;
    if (includeResourceDirectory) {
        PutU32(pe, dataDirectory + 16, kResourceRva);
        PutU32(pe, dataDirectory + 20,
               static_cast<uint32_t>(resourceBlob.size()));
    }

    // Section table
    const size_t sectionTable = optional + 224;
    std::memcpy(&pe[sectionTable], ".rsrc", 5);
    PutU32(pe, sectionTable + 8, static_cast<uint32_t>(resourceBlob.size()));
    PutU32(pe, sectionTable + 12, kResourceRva);
    PutU32(pe, sectionTable + 16, static_cast<uint32_t>(resourceBlob.size()));
    PutU32(pe, sectionTable + 20, kResourceRawOffset);

    for (size_t i = 0; i < resourceBlob.size(); ++i) {
        pe[kResourceRawOffset + i] = resourceBlob[i];
    }
    return pe;
}

const char* kUiFileText = "<element layout=borderlayout><button/></element>";

std::vector<uint8_t> SamplePe() {
    return BuildPe(BuildResourceBlob(kUiFileText, MakeBitmapResource()));
}

std::string ToStdString(const std::u16string& in) {
    std::string out;
    for (char16_t c : in) {
        out.push_back(static_cast<char>(c & 0x7F));
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Happy path
// ---------------------------------------------------------------------------

TEST(PeResources, ReadsAWellFormedFile) {
    PeFile pe;
    REQUIRE_EQ(static_cast<int>(PeFile::Load(SamplePe(), &pe)),
               static_cast<int>(PeStatus::Ok));

    CHECK_EQ(pe.Machine(), uint16_t(0x014C));
    CHECK_FALSE(pe.Is64Bit());
    CHECK_EQ(pe.Resources().size(), size_t(2));
}

TEST(PeResources, FindsBothNumericAndNamedTypes) {
    PeFile pe;
    REQUIRE_EQ(static_cast<int>(PeFile::Load(SamplePe(), &pe)),
               static_cast<int>(PeStatus::Ok));

    const auto bitmaps = pe.OfType(restype::kBitmap);
    REQUIRE_EQ(bitmaps.size(), size_t(1));
    CHECK_EQ(bitmaps[0].name.id, 101u);
    CHECK_EQ(bitmaps[0].language, 1033u);

    const auto uifiles = pe.OfNamedType("UIFILE");
    REQUIRE_EQ(uifiles.size(), size_t(1));
    CHECK(uifiles[0].type.isName);
    CHECK_EQ(uifiles[0].name.id, 1u);
}

TEST(PeResources, NamedTypeLookupIsCaseInsensitive) {
    PeFile pe;
    REQUIRE_EQ(static_cast<int>(PeFile::Load(SamplePe(), &pe)),
               static_cast<int>(PeStatus::Ok));

    CHECK_EQ(pe.OfNamedType("uifile").size(), size_t(1));
    CHECK_EQ(pe.OfNamedType("UiFiLe").size(), size_t(1));
    CHECK_EQ(pe.OfNamedType("BITMAP").size(), size_t(0)); // it is a numeric type
}

TEST(PeResources, ReadsPayloadBytesExactly) {
    PeFile pe;
    REQUIRE_EQ(static_cast<int>(PeFile::Load(SamplePe(), &pe)),
               static_cast<int>(PeStatus::Ok));

    const auto uifiles = pe.OfNamedType("UIFILE");
    REQUIRE_EQ(uifiles.size(), size_t(1));
    const std::vector<uint8_t> bytes = pe.Read(uifiles[0]);
    CHECK_EQ(bytes.size(), std::strlen(kUiFileText));
    CHECK_EQ(std::string(bytes.begin(), bytes.end()), std::string(kUiFileText));

    const auto bitmaps = pe.OfType(restype::kBitmap);
    const std::vector<uint8_t> bitmap = pe.Read(bitmaps[0]);
    REQUIRE_EQ(bitmap.size(), size_t(56));
    CHECK_EQ(int(bitmap[40]), 0x10);
    CHECK_EQ(int(bitmap[55]), 0x1F);
}

TEST(PeResources, ResourceIdFormatsForHumans) {
    PeFile pe;
    REQUIRE_EQ(static_cast<int>(PeFile::Load(SamplePe(), &pe)),
               static_cast<int>(PeStatus::Ok));

    CHECK_EQ(pe.OfNamedType("UIFILE")[0].type.ToString(), std::string("UIFILE"));
    CHECK_EQ(pe.OfType(restype::kBitmap)[0].name.ToString(), std::string("#101"));
}

// ---------------------------------------------------------------------------
// Malformed input - the cases that matter for a tool pointed at arbitrary files
// ---------------------------------------------------------------------------

TEST(PeResources, RejectsSomethingThatIsNotAPeFile) {
    PeFile pe;
    std::vector<uint8_t> text(256, 'A');
    CHECK_EQ(static_cast<int>(PeFile::Load(text, &pe)),
             static_cast<int>(PeStatus::NotAPeFile));
}

TEST(PeResources, RejectsAnEmptyOrTinyFile) {
    PeFile pe;
    CHECK_EQ(static_cast<int>(PeFile::Load({}, &pe)),
             static_cast<int>(PeStatus::Truncated));
    CHECK_EQ(static_cast<int>(PeFile::Load(std::vector<uint8_t>{'M', 'Z'}, &pe)),
             static_cast<int>(PeStatus::Truncated));
}

TEST(PeResources, RejectsAFileWhosePeHeaderIsPastTheEnd) {
    std::vector<uint8_t> pe = SamplePe();
    PutU32(pe, 0x3C, 0x7FFFFFFF); // e_lfanew points into space

    PeFile parsed;
    CHECK_EQ(static_cast<int>(PeFile::Load(pe, &parsed)),
             static_cast<int>(PeStatus::Truncated));
}

TEST(PeResources, ReportsAFileWithNoResources) {
    PeFile pe;
    const auto bytes = BuildPe(BuildResourceBlob(kUiFileText, MakeBitmapResource()),
                               /*includeResourceDirectory=*/false);
    CHECK_EQ(static_cast<int>(PeFile::Load(bytes, &pe)),
             static_cast<int>(PeStatus::NoResourceSection));
}

TEST(PeResources, DropsAResourcePointingOutsideTheImage) {
    std::vector<uint8_t> blob =
        BuildResourceBlob(kUiFileText, MakeBitmapResource());
    // Point the UIFILE data entry at an RVA no section covers.
    PutU32(blob, 0x0A0, 0x7F000000);
    std::vector<uint8_t> bytes = BuildPe(blob);

    PeFile pe;
    REQUIRE_EQ(static_cast<int>(PeFile::Load(bytes, &pe)),
               static_cast<int>(PeStatus::Ok));

    // The bad entry is dropped rather than handed back as something that would
    // fail - or worse, read out of bounds - when the caller reads it.
    CHECK_EQ(pe.Resources().size(), size_t(1));
    CHECK_EQ(pe.OfNamedType("UIFILE").size(), size_t(0));
    CHECK_EQ(pe.OfType(restype::kBitmap).size(), size_t(1));
}

TEST(PeResources, RejectsATreeThatPointsAtItself) {
    std::vector<uint8_t> blob =
        BuildResourceBlob(kUiFileText, MakeBitmapResource());
    // Make the first type entry a subdirectory pointing back at the root. A
    // depth limit is the only thing between this and an infinite walk.
    PutU32(blob, 0x014, 0x80000000u | 0x000);
    std::vector<uint8_t> bytes = BuildPe(blob);

    PeFile pe;
    CHECK_EQ(static_cast<int>(PeFile::Load(bytes, &pe)),
             static_cast<int>(PeStatus::MalformedResourceTree));
}

TEST(PeResources, RejectsANameStringRunningPastTheEnd) {
    std::vector<uint8_t> blob =
        BuildResourceBlob(kUiFileText, MakeBitmapResource());
    PutU16(blob, 0x100, 0xFFFF); // claim a 65535-character name
    std::vector<uint8_t> bytes = BuildPe(blob);

    PeFile pe;
    CHECK_EQ(static_cast<int>(PeFile::Load(bytes, &pe)),
             static_cast<int>(PeStatus::MalformedResourceTree));
}

TEST(PeResources, ReadOnAnEntryWithABadRvaReturnsNothing) {
    PeFile pe;
    REQUIRE_EQ(static_cast<int>(PeFile::Load(SamplePe(), &pe)),
               static_cast<int>(PeStatus::Ok));

    ResourceEntry forged = pe.Resources()[0];
    forged.rva = 0x7F000000;
    CHECK(pe.Read(forged).empty());

    forged = pe.Resources()[0];
    forged.size = 0xFFFFFFF0;
    CHECK(pe.Read(forged).empty());
}

// ---------------------------------------------------------------------------
// Conversions
// ---------------------------------------------------------------------------

TEST(PeResources, BitmapGetsItsFileHeaderBack) {
    // RT_BITMAP has no BITMAPFILEHEADER. Putting the wrong one back is the
    // difference between a dumped bitmap opening and being subtly corrupt.
    const std::vector<uint8_t> file = BitmapResourceToFile(MakeBitmapResource());
    REQUIRE_EQ(file.size(), size_t(14 + 56));

    CHECK_EQ(char(file[0]), 'B');
    CHECK_EQ(char(file[1]), 'M');

    uint32_t fileSize = 0;
    for (int i = 0; i < 4; ++i) {
        fileSize |= static_cast<uint32_t>(file[2 + static_cast<size_t>(i)])
                    << (8 * i);
    }
    CHECK_EQ(fileSize, uint32_t(70));

    uint32_t pixelOffset = 0;
    for (int i = 0; i < 4; ++i) {
        pixelOffset |= static_cast<uint32_t>(file[10 + static_cast<size_t>(i)])
                       << (8 * i);
    }
    // 32bpp has no colour table, so pixels start right after the 40-byte DIB
    // header: 14 + 40.
    CHECK_EQ(pixelOffset, uint32_t(54));
    // And the payload survived unchanged.
    CHECK_EQ(int(file[54]), 0x10);
}

TEST(PeResources, PalettedBitmapPixelOffsetSkipsTheColourTable) {
    std::vector<uint8_t> bitmap(40 + 256 * 4 + 4, 0);
    PutU32(bitmap, 0, 40);
    PutU32(bitmap, 4, 2);
    PutU32(bitmap, 8, 2);
    PutU16(bitmap, 12, 1);
    PutU16(bitmap, 14, 8);  // 8bpp -> 256 palette entries
    PutU32(bitmap, 32, 0);  // biClrUsed = 0 means "all of them"

    const std::vector<uint8_t> file = BitmapResourceToFile(bitmap);
    REQUIRE_FALSE(file.empty());

    uint32_t pixelOffset = 0;
    for (int i = 0; i < 4; ++i) {
        pixelOffset |= static_cast<uint32_t>(file[10 + static_cast<size_t>(i)])
                       << (8 * i);
    }
    CHECK_EQ(pixelOffset, uint32_t(14 + 40 + 256 * 4));
}

TEST(PeResources, BitmapConversionRefusesSomethingTooSmall) {
    CHECK(BitmapResourceToFile({}).empty());
    CHECK(BitmapResourceToFile(std::vector<uint8_t>(20, 0)).empty());
}

TEST(PeResources, DecodesUtf8AndUtf16Resources) {
    const std::string ascii = "<element/>";
    CHECK_EQ(ResourceToUtf8Text(std::vector<uint8_t>(ascii.begin(), ascii.end())),
             ascii);

    // UTF-16LE with a BOM.
    std::vector<uint8_t> utf16 = {0xFF, 0xFE};
    for (char c : ascii) {
        utf16.push_back(static_cast<uint8_t>(c));
        utf16.push_back(0);
    }
    CHECK_EQ(ResourceToUtf8Text(utf16), ascii);

    // A UTF-8 BOM is stripped rather than becoming three junk characters.
    std::vector<uint8_t> withBom = {0xEF, 0xBB, 0xBF};
    withBom.insert(withBom.end(), ascii.begin(), ascii.end());
    CHECK_EQ(ResourceToUtf8Text(withBom), ascii);
}

TEST(PeResources, ParsesAStringBundle) {
    // RT_STRING packs 16 length-prefixed, unterminated UTF-16 runs together.
    std::vector<uint8_t> bundle;
    auto append = [&bundle](const char* text) {
        const size_t length = std::strlen(text);
        bundle.push_back(static_cast<uint8_t>(length & 0xFF));
        bundle.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
        for (size_t i = 0; i < length; ++i) {
            bundle.push_back(static_cast<uint8_t>(text[i]));
            bundle.push_back(0);
        }
    };
    append("first");
    append(""); // an empty slot is not the end of the bundle
    append("third");

    const auto entries = ParseStringBundle(bundle, 3);
    REQUIRE_EQ(entries.size(), size_t(2));
    // Bundle 3 covers ids 32..47, and the empty slot still consumes id 33.
    CHECK_EQ(entries[0].id, 32u);
    CHECK_EQ(ToStdString(entries[0].text), std::string("first"));
    CHECK_EQ(entries[1].id, 34u);
    CHECK_EQ(ToStdString(entries[1].text), std::string("third"));
}

TEST(PeResources, StringBundleStopsAtATruncatedEntry) {
    std::vector<uint8_t> bundle = {0x40, 0x00, 'a', 0}; // claims 64 chars, has 1
    CHECK(ParseStringBundle(bundle, 1).empty());
    CHECK(ParseStringBundle({}, 1).empty());
    CHECK(ParseStringBundle({0x01, 0x00, 'a', 0}, 0).empty()); // bundle 0 invalid
}

// ---------------------------------------------------------------------------
// Against a real file, when one has been dropped into assets/
// ---------------------------------------------------------------------------

TEST(PeResources, ReadsRealLunaMsstylesWhenPresent) {
    // Skipped on a machine without the reference material; the whole point of
    // the synthetic fixture above is that the suite stays green without it.
    const char* candidates[] = {
        "assets/Themes/Luna/luna.msstyles",
        "../assets/Themes/Luna/luna.msstyles",
        "../../assets/Themes/Luna/luna.msstyles",
    };

    std::vector<uint8_t> bytes;
    for (const char* path : candidates) {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) {
            continue;
        }
        const std::streamsize size = file.tellg();
        if (size <= 0) {
            continue;
        }
        file.seekg(0);
        bytes.resize(static_cast<size_t>(size));
        if (file.read(reinterpret_cast<char*>(bytes.data()), size)) {
            break;
        }
        bytes.clear();
    }
    if (bytes.empty()) {
        return;
    }

    PeFile pe;
    REQUIRE_EQ(static_cast<int>(PeFile::Load(std::move(bytes), &pe)),
               static_cast<int>(PeStatus::Ok));

    // Luna is a resource-only DLL: hundreds of bitmaps and the theme's class
    // data as TEXTFILE resources.
    CHECK_GT(pe.OfType(restype::kBitmap).size(), size_t(400));
    CHECK_GE(pe.OfNamedType("TEXTFILE").size(), size_t(1));

    // Every bitmap must survive the file-header reconstruction. This is the
    // assertion that would have caught a wrong pixel offset.
    size_t checked = 0;
    for (const ResourceEntry& entry : pe.OfType(restype::kBitmap)) {
        const std::vector<uint8_t> file = BitmapResourceToFile(pe.Read(entry));
        REQUIRE_FALSE(file.empty());
        CHECK_EQ(char(file[0]), 'B');
        CHECK_EQ(char(file[1]), 'M');

        uint32_t declaredSize = 0;
        for (int i = 0; i < 4; ++i) {
            declaredSize |= static_cast<uint32_t>(file[2 + static_cast<size_t>(i)])
                            << (8 * i);
        }
        CHECK_EQ(declaredSize, static_cast<uint32_t>(file.size()));

        if (++checked >= 40) {
            break; // a representative sample; the whole set takes a moment
        }
    }
    CHECK_GT(checked, size_t(0));
}
