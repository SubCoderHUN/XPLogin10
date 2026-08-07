#include "xplogin/StringUtil.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace xplogin {
namespace {

constexpr char32_t kReplacement = 0xFFFD;

void AppendUtf16(std::u16string& out, char32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        cp = kReplacement;
    }
    if (cp < 0x10000) {
        out.push_back(static_cast<char16_t>(cp));
    } else {
        cp -= 0x10000;
        out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
        out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    }
}

void AppendUtf8(std::string& out, char32_t cp) {
    if (cp > 0x10FFFF) {
        cp = kReplacement;
    }
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Decodes one code point from a wide string, transparently handling both the
// 16-bit (Windows) and 32-bit (POSIX) flavours of wchar_t.
char32_t NextWideCodePoint(const std::wstring& in, size_t* index) {
    char32_t cp = static_cast<char32_t>(static_cast<uint32_t>(in[*index]));
    ++(*index);
    if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && *index < in.size()) {
        char32_t low = static_cast<char32_t>(static_cast<uint32_t>(in[*index]));
        if (low >= 0xDC00 && low <= 0xDFFF) {
            ++(*index);
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        }
    }
    return cp;
}

} // namespace

std::u16string WideToU16(const std::wstring& in) {
    std::u16string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        AppendUtf16(out, NextWideCodePoint(in, &i));
    }
    return out;
}

std::wstring U16ToWide(const std::u16string& in) {
    std::wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        char32_t cp = static_cast<char32_t>(in[i]);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < in.size()) {
            char32_t low = static_cast<char32_t>(in[i + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                if (sizeof(wchar_t) == 2) {
                    // Keep the pair verbatim; wchar_t is already UTF-16.
                    out.push_back(static_cast<wchar_t>(in[i]));
                    out.push_back(static_cast<wchar_t>(low));
                    ++i;
                    continue;
                }
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        out.push_back(static_cast<wchar_t>(cp));
    }
    return out;
}

std::string WideToUtf8(const std::wstring& in) {
    std::string out;
    out.reserve(in.size());
    size_t i = 0;
    while (i < in.size()) {
        AppendUtf8(out, NextWideCodePoint(in, &i));
    }
    return out;
}

std::wstring Utf8ToWide(const std::string& in) {
    std::u16string utf16;
    size_t i = 0;
    while (i < in.size()) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        char32_t cp = kReplacement;
        size_t extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1Fu;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0Fu;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07u;
            extra = 3;
        }
        if (i + extra >= in.size()) {
            extra = 0;
            cp = kReplacement;
        }
        for (size_t k = 1; k <= extra; ++k) {
            unsigned char cc = static_cast<unsigned char>(in[i + k]);
            if ((cc & 0xC0) != 0x80) {
                cp = kReplacement;
                extra = 0;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        i += extra + 1;
        AppendUtf16(utf16, cp);
    }
    return U16ToWide(utf16);
}

int CompareNoCase(const std::wstring& a, const std::wstring& b) {
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        wchar_t ca = a[i];
        wchar_t cb = b[i];
        if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca - L'A' + L'a');
        if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb - L'A' + L'a');
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && CompareNoCase(a, b) == 0;
}

std::wstring ToLowerAscii(const std::wstring& in) {
    std::wstring out = in;
    for (wchar_t& c : out) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
    return out;
}

std::wstring Trim(const std::wstring& in) {
    size_t b = 0;
    size_t e = in.size();
    auto isSpace = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
    };
    while (b < e && isSpace(in[b])) ++b;
    while (e > b && isSpace(in[e - 1])) --e;
    return in.substr(b, e - b);
}

std::string TrimAscii(const std::string& in) {
    size_t b = 0;
    size_t e = in.size();
    auto isSpace = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (b < e && isSpace(in[b])) ++b;
    while (e > b && isSpace(in[e - 1])) --e;
    return in.substr(b, e - b);
}

bool StartsWithNoCase(const std::wstring& value, const std::wstring& prefix) {
    if (prefix.size() > value.size()) {
        return false;
    }
    return CompareNoCase(value.substr(0, prefix.size()), prefix) == 0;
}

bool SplitQualifiedName(const std::wstring& qualified,
                        std::wstring* domain,
                        std::wstring* username) {
    if (domain) domain->clear();
    if (username) username->clear();

    std::wstring value = Trim(qualified);
    if (value.empty()) {
        return false;
    }

    size_t backslash = value.find(L'\\');
    if (backslash != std::wstring::npos) {
        std::wstring d = value.substr(0, backslash);
        std::wstring u = value.substr(backslash + 1);
        // A second separator means this is not a name we understand.
        if (d.empty() || u.empty() || u.find(L'\\') != std::wstring::npos) {
            return false;
        }
        if (domain) *domain = d;
        if (username) *username = u;
        return true;
    }

    size_t at = value.find(L'@');
    if (at != std::wstring::npos) {
        // UPN: LSA accepts it verbatim with an empty domain.
        if (at == 0 || at + 1 >= value.size()) {
            return false;
        }
        if (username) *username = value;
        return true;
    }

    if (username) *username = value;
    return true;
}

void SecureZero(void* data, size_t bytes) {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(data);
    while (bytes--) {
        *p++ = 0;
    }
}

void SecureClear(std::wstring& s) {
    if (!s.empty()) {
        SecureZero(&s[0], s.size() * sizeof(wchar_t));
    }
    s.clear();
}

void SecureClear(std::u16string& s) {
    if (!s.empty()) {
        SecureZero(&s[0], s.size() * sizeof(char16_t));
    }
    s.clear();
}

} // namespace xplogin
