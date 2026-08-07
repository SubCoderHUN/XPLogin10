// xp-extract - inventory and unpack the resources of a Windows PE file.
//
//   xp-extract <file> --list                 what is in there
//   xp-extract <file> --dump <directory>     write everything out
//   xp-extract <file> --type UIFILE --print  print one resource type as text
//
// Used on logonui.exe (the welcome screen's DirectUI markup and artwork),
// msgina.dll (the classic dialogs) and luna.msstyles (the visual style), so
// the project can work from Microsoft's original geometry rather than from
// measurements taken off a screenshot.
#include "xplogin/assets/PeResources.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace xplogin::assets;

namespace {

std::vector<uint8_t> ReadWholeFile(const std::string& path, bool* ok) {
    *ok = false;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return {};
    }
    file.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return {};
    }
    *ok = true;
    return bytes;
}

std::string TypeLabel(const ResourceId& type) {
    if (type.isName) {
        return type.ToString();
    }
    switch (type.id) {
        case restype::kCursor: return "CURSOR";
        case restype::kBitmap: return "BITMAP";
        case restype::kIcon: return "ICON";
        case restype::kMenu: return "MENU";
        case restype::kDialog: return "DIALOG";
        case restype::kString: return "STRING";
        case restype::kFontDir: return "FONTDIR";
        case restype::kFont: return "FONT";
        case restype::kAccelerator: return "ACCELERATOR";
        case restype::kRcData: return "RCDATA";
        case restype::kMessageTable: return "MESSAGETABLE";
        case restype::kGroupCursor: return "GROUP_CURSOR";
        case restype::kGroupIcon: return "GROUP_ICON";
        case restype::kVersion: return "VERSION";
        case restype::kManifest: return "MANIFEST";
        default: return type.ToString();
    }
}

// Characters a file name cannot contain on Windows, plus path separators.
std::string SanitizeForFileName(const std::string& in) {
    std::string out;
    for (char c : in) {
        const bool bad = c == '\\' || c == '/' || c == ':' || c == '*' ||
                         c == '?' || c == '"' || c == '<' || c == '>' ||
                         c == '|' || static_cast<unsigned char>(c) < 0x20;
        out.push_back(bad ? '_' : c);
    }
    if (out.empty()) {
        out = "unnamed";
    }
    return out;
}

bool WriteFileBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

int DoList(const PeFile& pe, const std::string& path) {
    std::printf("%s\n", path.c_str());
    std::printf("  %s, %zu bytes, %zu resources\n\n",
                pe.Is64Bit() ? "PE32+ (x64)" : "PE32 (x86)", pe.ByteSize(),
                pe.Resources().size());

    // Summary first: what types exist and how much they weigh.
    std::map<std::string, std::pair<size_t, uint64_t>> byType;
    for (const ResourceEntry& entry : pe.Resources()) {
        auto& slot = byType[TypeLabel(entry.type)];
        slot.first += 1;
        slot.second += entry.size;
    }

    std::printf("  %-16s %8s %12s\n", "TYPE", "COUNT", "BYTES");
    std::printf("  ---------------------------------------\n");
    for (const auto& item : byType) {
        std::printf("  %-16s %8zu %12llu\n", item.first.c_str(), item.second.first,
                    static_cast<unsigned long long>(item.second.second));
    }

    std::printf("\n  %-16s %-24s %6s %10s\n", "TYPE", "NAME", "LANG", "BYTES");
    std::printf("  ------------------------------------------------------------\n");
    for (const ResourceEntry& entry : pe.Resources()) {
        std::printf("  %-16s %-24s %6u %10u\n", TypeLabel(entry.type).c_str(),
                    entry.name.ToString().c_str(), entry.language, entry.size);
    }
    return 0;
}

int DoDump(const PeFile& pe, const std::string& directory) {
    size_t written = 0;
    size_t failed = 0;

    for (const ResourceEntry& entry : pe.Resources()) {
        const std::vector<uint8_t> raw = pe.Read(entry);
        if (raw.empty()) {
            ++failed;
            continue;
        }

        const std::string typeLabel = SanitizeForFileName(TypeLabel(entry.type));
        const std::string nameLabel = SanitizeForFileName(entry.name.ToString());

        std::string extension = ".bin";
        std::vector<uint8_t> payload = raw;

        if (entry.type.Is(restype::kBitmap)) {
            // RT_BITMAP has no BITMAPFILEHEADER; put it back or the file is
            // unreadable by every image viewer.
            std::vector<uint8_t> bmp = BitmapResourceToFile(raw);
            if (!bmp.empty()) {
                payload = std::move(bmp);
                extension = ".bmp";
            }
        } else if (raw.size() > 8 && raw[0] == 0x89 && raw[1] == 'P' &&
                   raw[2] == 'N' && raw[3] == 'G') {
            extension = ".png";
        } else if (entry.type.IsNamed("UIFILE") ||
                   entry.type.IsNamed("XML") || entry.type.Is(restype::kManifest)) {
            const std::string text = ResourceToUtf8Text(raw);
            payload.assign(text.begin(), text.end());
            extension = ".txt";
        } else if (entry.type.Is(restype::kString)) {
            std::string text;
            for (const StringTableEntry& item :
                 ParseStringBundle(raw, entry.name.isName ? 0 : entry.name.id)) {
                text += std::to_string(item.id) + "\t";
                std::vector<uint8_t> utf16;
                for (char16_t c : item.text) {
                    utf16.push_back(static_cast<uint8_t>(c & 0xFF));
                    utf16.push_back(static_cast<uint8_t>((c >> 8) & 0xFF));
                }
                text += ResourceToUtf8Text(utf16);
                text += "\n";
            }
            payload.assign(text.begin(), text.end());
            extension = ".txt";
        }

        const std::string path = directory + "/" + typeLabel + "_" + nameLabel +
                                 "_" + std::to_string(entry.language) + extension;
        if (WriteFileBytes(path, payload)) {
            ++written;
        } else {
            ++failed;
        }
    }

    std::printf("wrote %zu resources to %s", written, directory.c_str());
    if (failed) {
        std::printf(" (%zu could not be written)", failed);
    }
    std::printf("\n");
    return failed == 0 ? 0 : 1;
}

int DoPrint(const PeFile& pe, const std::string& typeName) {
    std::vector<ResourceEntry> entries = pe.OfNamedType(typeName.c_str());
    if (entries.empty()) {
        // Maybe a numeric type.
        const uint32_t numeric =
            static_cast<uint32_t>(std::strtoul(typeName.c_str(), nullptr, 10));
        if (numeric != 0) {
            entries = pe.OfType(numeric);
        }
    }
    if (entries.empty()) {
        std::printf("no resources of type %s\n", typeName.c_str());
        return 1;
    }

    for (const ResourceEntry& entry : entries) {
        std::printf("======== %s / %s (lang %u, %u bytes) ========\n",
                    TypeLabel(entry.type).c_str(), entry.name.ToString().c_str(),
                    entry.language, entry.size);
        std::printf("%s\n", ResourceToUtf8Text(pe.Read(entry)).c_str());
    }
    return 0;
}

void PrintUsage(const char* argv0) {
    std::printf(
        "usage: %s <pe-file> [--list | --dump <dir> | --type <name> --print]\n"
        "\n"
        "  --list            inventory of every resource\n"
        "  --dump <dir>      write every resource out (bitmaps as .bmp,\n"
        "                    markup and string tables as .txt)\n"
        "  --type <name>     restrict --print to one resource type\n"
        "  --print           print the selected resources as text\n",
        argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        PrintUsage(argv[0]);
        return 2;
    }

    const std::string path = argv[1];
    std::string dumpDirectory;
    std::string typeName;
    bool list = argc == 2;
    bool print = false;

    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--list") == 0) {
            list = true;
        } else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dumpDirectory = argv[++i];
        } else if (std::strcmp(argv[i], "--type") == 0 && i + 1 < argc) {
            typeName = argv[++i];
        } else if (std::strcmp(argv[i], "--print") == 0) {
            print = true;
        } else {
            PrintUsage(argv[0]);
            return 2;
        }
    }

    bool ok = false;
    std::vector<uint8_t> bytes = ReadWholeFile(path, &ok);
    if (!ok) {
        std::printf("error: cannot read %s\n", path.c_str());
        return 2;
    }

    PeFile pe;
    const PeStatus status = PeFile::Load(std::move(bytes), &pe);
    if (status != PeStatus::Ok) {
        std::printf("error: %s: %s\n", path.c_str(), PeStatusText(status));
        return 3;
    }

    if (print) {
        return DoPrint(pe, typeName.empty() ? "UIFILE" : typeName);
    }
    if (!dumpDirectory.empty()) {
        return DoDump(pe, dumpDirectory);
    }
    if (list) {
        return DoList(pe, path);
    }

    PrintUsage(argv[0]);
    return 2;
}
