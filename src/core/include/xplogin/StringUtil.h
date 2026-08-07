// XPLogin10 - string helpers.
//
// wchar_t is 16-bit on Windows and 32-bit on Linux/macOS, so anything that has
// to match a binary layout LSA understands works on char16_t instead. These
// helpers are the bridge.
#pragma once

#include <string>
#include <vector>

namespace xplogin {

// UTF-16 conversion. On Windows this is a straight copy; elsewhere surrogate
// pairs are generated for code points outside the BMP so the byte layout of the
// packed credential is identical to what LSA sees on the target machine.
std::u16string WideToU16(const std::wstring& in);
std::wstring   U16ToWide(const std::u16string& in);

// UTF-8 <-> wide, used for log files and .ini parsing.
std::string  WideToUtf8(const std::wstring& in);
std::wstring Utf8ToWide(const std::string& in);

// Case-insensitive compare for ASCII (account names). Returns <0, 0, >0.
int CompareNoCase(const std::wstring& a, const std::wstring& b);
bool EqualsNoCase(const std::wstring& a, const std::wstring& b);

std::wstring ToLowerAscii(const std::wstring& in);
std::wstring Trim(const std::wstring& in);
std::string  TrimAscii(const std::string& in);

bool StartsWithNoCase(const std::wstring& value, const std::wstring& prefix);

// Splits "DOMAIN\user", "user@contoso.com" or a bare "user" into its parts.
// Returns false when the input is empty or malformed (e.g. "DOMAIN\").
bool SplitQualifiedName(const std::wstring& qualified,
                        std::wstring* domain,
                        std::wstring* username);

// Overwrites the buffer before releasing it. Used for every password copy.
void SecureZero(void* data, size_t bytes);
void SecureClear(std::wstring& s);
void SecureClear(std::u16string& s);

} // namespace xplogin
