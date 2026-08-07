// The one place <initguid.h> is included, so the two CLSIDs get exactly one
// definition each.
#include <initguid.h>

#include "Guids.h"

#include "xplogin/RegistrationPlan.h"

#include <cwchar>

// {6E2B1FA0-71D4-4C57-9E4A-3B2D5F8C1A77}
DEFINE_GUID(CLSID_XPLoginProvider, 0x6e2b1fa0, 0x71d4, 0x4c57, 0x9e, 0x4a, 0x3b,
            0x2d, 0x5f, 0x8c, 0x1a, 0x77);

// {9C1D3E82-40B6-4F1D-8A57-2E9C6B04D311}
DEFINE_GUID(CLSID_XPLoginFilter, 0x9c1d3e82, 0x40b6, 0x4f1d, 0x8a, 0x57, 0x2e,
            0x9c, 0x6b, 0x04, 0xd3, 0x11);

namespace {

// Compile-time check that the binary GUID above and the string the installer
// writes to the registry describe the same class. If these ever drift, Windows
// registers one CLSID and tries to load another, and the only symptom is that
// nothing happens - no error, anywhere.
constexpr bool HexDigitMatches(wchar_t c, unsigned value) {
    return (c >= L'0' && c <= L'9')   ? (static_cast<unsigned>(c - L'0') == value)
           : (c >= L'a' && c <= L'f') ? (static_cast<unsigned>(c - L'a' + 10) == value)
           : (c >= L'A' && c <= L'F') ? (static_cast<unsigned>(c - L'A' + 10) == value)
                                      : false;
}

// Checks the eight hex digits of Data1 and the braces, which is enough to catch
// any realistic typo without reimplementing a GUID parser at compile time.
constexpr bool ClsidStringMatches(const wchar_t* text, unsigned long data1) {
    return text[0] == L'{' && text[37] == L'}' && text[9] == L'-' &&
           HexDigitMatches(text[1], (data1 >> 28) & 0xF) &&
           HexDigitMatches(text[2], (data1 >> 24) & 0xF) &&
           HexDigitMatches(text[3], (data1 >> 20) & 0xF) &&
           HexDigitMatches(text[4], (data1 >> 16) & 0xF) &&
           HexDigitMatches(text[5], (data1 >> 12) & 0xF) &&
           HexDigitMatches(text[6], (data1 >> 8) & 0xF) &&
           HexDigitMatches(text[7], (data1 >> 4) & 0xF) &&
           HexDigitMatches(text[8], data1 & 0xF);
}

static_assert(ClsidStringMatches(xplogin::kProviderClsid, 0x6e2b1fa0),
              "kProviderClsid does not match CLSID_XPLoginProvider");
static_assert(ClsidStringMatches(xplogin::kFilterClsid, 0x9c1d3e82),
              "kFilterClsid does not match CLSID_XPLoginFilter");

} // namespace
