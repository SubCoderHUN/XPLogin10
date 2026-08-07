// XPLogin10 - shared Windows includes and small helpers.
//
// Nothing in this header is visible to the portable core; it exists so the
// win32/ translation units agree on include order and on the RAII helpers.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <lm.h>
#include <ntsecapi.h>
#include <sddl.h>
#include <wtsapi32.h>

#include <string>

namespace xplogin::win32 {

// Closes a HANDLE unless it is NULL or INVALID_HANDLE_VALUE.
class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : handle_(handle) {}
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.Release()) {}
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            Reset(other.Release());
        }
        return *this;
    }
    ~ScopedHandle() { Reset(); }

    HANDLE Get() const { return handle_; }
    HANDLE* Receive() {
        Reset();
        return &handle_;
    }
    bool Valid() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

    HANDLE Release() {
        HANDLE value = handle_;
        handle_ = nullptr;
        return value;
    }
    void Reset(HANDLE handle = nullptr) {
        if (Valid()) {
            ::CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

// Frees a buffer allocated by the NetApi32 functions.
template <typename T>
class NetApiBuffer {
public:
    NetApiBuffer() = default;
    NetApiBuffer(const NetApiBuffer&) = delete;
    NetApiBuffer& operator=(const NetApiBuffer&) = delete;
    ~NetApiBuffer() { Reset(); }

    T** Receive() {
        Reset();
        return &buffer_;
    }
    T* Get() const { return buffer_; }
    void Reset() {
        if (buffer_) {
            ::NetApiBufferFree(buffer_);
            buffer_ = nullptr;
        }
    }

private:
    T* buffer_ = nullptr;
};

// Frees a buffer allocated by WTSEnumerateSessions / WTSQuerySessionInformation.
template <typename T>
class WtsBuffer {
public:
    WtsBuffer() = default;
    WtsBuffer(const WtsBuffer&) = delete;
    WtsBuffer& operator=(const WtsBuffer&) = delete;
    ~WtsBuffer() { Reset(); }

    T** Receive() {
        Reset();
        return &buffer_;
    }
    T* Get() const { return buffer_; }
    void Reset() {
        if (buffer_) {
            ::WTSFreeMemory(buffer_);
            buffer_ = nullptr;
        }
    }

private:
    T* buffer_ = nullptr;
};

// Reads a REG_SZ / REG_DWORD from HKLM. Returns false when absent.
bool ReadRegistryString(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                        std::wstring* out);
bool ReadRegistryDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                       DWORD* out);
bool WriteRegistryDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                        DWORD value);
bool WriteRegistryString(HKEY root, const wchar_t* subKey, const wchar_t* valueName,
                         const std::wstring& value);

// SID <-> string, using ConvertSidToStringSidW.
std::wstring SidToString(PSID sid);

// The directory the given module lives in, with a trailing backslash.
std::wstring ModuleDirectory(HMODULE module);

// True when this process is running on the Winlogon (secure) desktop.
bool IsOnSecureDesktop();

// Enables a privilege on the current process token; returns false if it is not
// held. Used for SeShutdownPrivilege and SeTcbPrivilege.
bool EnablePrivilege(const wchar_t* privilegeName);

} // namespace xplogin::win32
