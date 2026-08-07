#include "Payload.h"

#include "xplogin/DeploymentPlan.h"

#include <windows.h>

#include <fstream>

namespace xplogin::setup {
namespace {

// There is deliberately no second list here. What the setup embeds, what it
// writes out, and what the uninstall removes are all PayloadManifest() - the
// three used to be separate tables and a file added to one of them was a bug
// nobody would notice until an uninstall left something behind.

// Returns the bytes of one embedded resource, or nullptr when it is absent.
const void* FindPayload(PayloadId id, DWORD* size) {
    HMODULE module = ::GetModuleHandleW(nullptr);
    HRSRC resource = ::FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) {
        return nullptr;
    }
    const DWORD bytes = ::SizeofResource(module, resource);
    HGLOBAL handle = ::LoadResource(module, resource);
    if (!handle || bytes == 0) {
        return nullptr;
    }
    if (size) {
        *size = bytes;
    }
    return ::LockResource(handle);
}

bool WriteFileBytes(const std::wstring& path, const void* data, DWORD size) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const BOOL ok = ::WriteFile(file, data, size, &written, nullptr);
    ::CloseHandle(file);
    return ok && written == size;
}

} // namespace

bool HasEmbeddedPayload() {
    DWORD size = 0;
    return FindPayload(kPayloadProvider, &size) != nullptr;
}

const std::vector<std::wstring>& PayloadFileNames() {
    static const std::vector<std::wstring> names = [] {
        std::vector<std::wstring> list;
        for (const PayloadFile& entry : PayloadManifest()) {
            list.emplace_back(entry.name);
        }
        return list;
    }();
    return names;
}

bool ExtractAll(const std::wstring& directory, std::wstring* error) {
    ::CreateDirectoryW(directory.c_str(), nullptr);

    for (const PayloadFile& entry : PayloadManifest()) {
        DWORD size = 0;
        const void* data =
            FindPayload(static_cast<PayloadId>(entry.resourceId), &size);
        if (!data) {
            if (entry.required) {
                if (error) {
                    *error = std::wstring(L"missing embedded payload: ") +
                             entry.name;
                }
                return false;
            }
            continue;
        }

        const std::wstring target = directory + L"\\" + entry.name;
        if (!WriteFileBytes(target, data, size)) {
            if (error) {
                *error = std::wstring(L"could not write ") + target;
            }
            return false;
        }
    }
    return true;
}

} // namespace xplogin::setup
