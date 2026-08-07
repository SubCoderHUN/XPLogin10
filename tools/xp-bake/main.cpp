// xp-bake - turns XP's own binaries into XPLogin.assets.
//
//   xp-bake <logonui.exe> [--msgina <msgina.dll>] -o <XPLogin.assets> [--report]
//
// Two sources, because the screen is two programs. The welcome screen is
// logonui.exe. The "Turn off computer" dialog it opens is a separate modal
// owned by msgina.dll - which is why logonui's markup has no shutdown panel in
// it and why none of that artwork is in its resources. --msgina is optional:
// without it the dialog falls back to being drawn from primitives, exactly as
// the whole screen does without logonui.exe.
//
// Runs at build time. Reads every bitmap the welcome screen draws, decodes it
// (24bpp, 32bpp with alpha, 8bpp paletted, 8bpp RLE all appear in there),
// applies the transparency key the markup names for it, and writes one file of
// straight BGRA.
//
// Everything downstream of this is a blit. That is the point: no image decoding
// happens inside LogonUI.exe, where a bug is expensive.
#include "xplogin/assets/AssetPack.h"
#include "xplogin/assets/Image.h"
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

bool WriteWholeFile(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

} // namespace

int main(int argc, char** argv) {
    std::string sourcePath;
    std::string msginaPath;
    std::string outputPath;
    bool report = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (std::strcmp(argv[i], "--msgina") == 0 && i + 1 < argc) {
            msginaPath = argv[++i];
        } else if (std::strcmp(argv[i], "--report") == 0) {
            report = true;
        } else if (sourcePath.empty()) {
            sourcePath = argv[i];
        }
    }

    if (sourcePath.empty() || outputPath.empty()) {
        std::printf("usage: xp-bake <logonui.exe> [--msgina <msgina.dll>] "
                    "-o <XPLogin.assets> [--report]\n");
        return 2;
    }

    // logonui.exe is required; msgina.dll is not. A source that is absent
    // simply contributes nothing, and the specs that wanted it are reported
    // as missing along with everything else.
    std::map<AssetSource, PeFile> sources;
    const struct { AssetSource source; const std::string& path; bool required; }
        inputs[] = {
            {AssetSource::LogonUi, sourcePath, true},
            {AssetSource::MsGina,  msginaPath, false},
        };

    for (const auto& input : inputs) {
        if (input.path.empty()) {
            continue;
        }
        bool ok = false;
        std::vector<uint8_t> bytes = ReadWholeFile(input.path, &ok);
        if (!ok) {
            std::printf("xp-bake: cannot read %s\n", input.path.c_str());
            if (input.required) {
                return 2;
            }
            continue;
        }
        PeFile pe;
        const PeStatus status = PeFile::Load(std::move(bytes), &pe);
        if (status != PeStatus::Ok) {
            std::printf("xp-bake: %s: %s\n", input.path.c_str(),
                        PeStatusText(status));
            if (input.required) {
                return 3;
            }
            continue;
        }
        sources.emplace(input.source, std::move(pe));
    }

    std::map<uint32_t, Image> baked;
    size_t missing = 0;
    size_t failed = 0;

    for (const AssetSpec& spec : WelcomeScreenAssets()) {
        const auto source = sources.find(spec.source);
        if (source == sources.end()) {
            std::printf("  --   #%-5u no %s (%s)\n", spec.resourceId,
                        AssetSourceFileName(spec.source), spec.description);
            ++missing;
            continue;
        }
        const PeFile& pe = source->second;

        // Find the resource by numeric name under RT_BITMAP.
        const ResourceEntry* found = nullptr;
        for (const ResourceEntry& entry : pe.Resources()) {
            if (entry.type.Is(restype::kBitmap) && entry.name.Is(spec.resourceId)) {
                found = &entry;
                break;
            }
        }
        if (!found) {
            std::printf("  --   #%-5u not present in %s (%s)\n", spec.resourceId,
                        AssetSourceFileName(spec.source), spec.description);
            ++missing;
            continue;
        }

        Image image;
        const BmpStatus decoded = DecodeBitmapResource(pe.Read(*found), &image);
        if (decoded != BmpStatus::Ok) {
            std::printf("  FAIL #%-5u %s (%s)\n", spec.resourceId,
                        BmpStatusText(decoded), spec.description);
            ++failed;
            continue;
        }

        if (spec.colorKey != kNoColorKey) {
            ApplyColorKey(&image, spec.colorKey);
        }

        if (report) {
            std::printf("  ok   #%-5u %4ux%-4u  %-6s  %s\n", spec.resourceId,
                        image.width, image.height,
                        HasMeaningfulAlpha(image) ? "alpha" : "opaque",
                        spec.description);
        }
        baked.emplace(spec.resourceId, std::move(image));
    }

    if (baked.empty()) {
        std::printf("xp-bake: nothing could be decoded from %s\n",
                    sourcePath.c_str());
        return 4;
    }

    const std::vector<uint8_t> pack = AssetPack::Build(baked);

    // Prove the pack reads back before writing it: a corrupt pack shipped
    // inside the installer would only show up as a blank logon screen.
    AssetPack verify;
    const PackStatus verified = AssetPack::Load(pack, &verify);
    if (verified != PackStatus::Ok || verify.Count() != baked.size()) {
        std::printf("xp-bake: built a pack that will not load back (%s)\n",
                    PackStatusText(verified));
        return 5;
    }

    if (!WriteWholeFile(outputPath, pack)) {
        std::printf("xp-bake: cannot write %s\n", outputPath.c_str());
        return 2;
    }

    std::printf("xp-bake: %zu assets -> %s (%zu bytes)", baked.size(),
                outputPath.c_str(), pack.size());
    if (missing || failed) {
        std::printf("  [%zu missing, %zu failed]", missing, failed);
    }
    std::printf("\n");
    return failed == 0 ? 0 : 1;
}
