// XPLogin10 - configuration, backed by an .ini file next to the DLL and
// overridable from HKLM\SOFTWARE\XPLogin10 (group policy friendly).
#pragma once

#include "xplogin/HealthMonitor.h"
#include "xplogin/SigninOptions.h"
#include "xplogin/UserDirectory.h"

#include <map>
#include <string>
#include <vector>

namespace xplogin {

// Minimal, dependency-free .ini reader. Sections, `key = value`, `;` and `#`
// comments, optional double quotes around values.
class IniFile {
public:
    bool ParseString(const std::string& utf8Text);
    bool LoadFromFile(const std::string& path);

    bool Has(const std::string& section, const std::string& key) const;

    std::wstring GetString(const std::string& section,
                           const std::string& key,
                           const std::wstring& fallback = L"") const;
    int GetInt(const std::string& section, const std::string& key,
               int fallback = 0) const;
    double GetDouble(const std::string& section, const std::string& key,
                     double fallback = 0.0) const;
    bool GetBool(const std::string& section, const std::string& key,
                 bool fallback = false) const;
    // "#RRGGBB", "#AARRGGBB" or "R,G,B". Returns 0xAARRGGBB.
    uint32_t GetColor(const std::string& section, const std::string& key,
                      uint32_t fallback) const;

    std::vector<std::string> Sections() const;
    std::vector<std::string> KeysIn(const std::string& section) const;

    void Set(const std::string& section, const std::string& key,
             const std::wstring& value);

    // Parse errors, reported rather than thrown - a malformed theme must never
    // stop the logon screen from coming up.
    const std::vector<std::string>& Errors() const { return errors_; }

private:
    // section -> key -> value, all keys lowercased for lookup.
    std::map<std::string, std::map<std::string, std::wstring>> data_;
    std::vector<std::string> errors_;
};

struct SoundConfig {
    bool         enabled   = true;
    std::wstring logonPath;   // e.g. C:\Windows\Media\XP\Windows XP Logon Sound.wav
    std::wstring logoffPath;
    std::wstring errorPath;
    std::wstring clickPath;
    std::wstring shutdownPath;
    int          volumePercent = 100;
};

struct UiConfig {
    std::wstring themePath;         // .ini describing colors and metrics
    std::wstring avatarDirectory;   // where per-user tile bitmaps live
    std::wstring defaultAvatarPath;
    bool         animationsEnabled = true;
    int          tileSlideDurationMs = 220;
    bool         showStatusLine = true;   // "2 programs running"
    bool         showHintButton = true;   // the blue "?" next to the go button
    bool         highContrastFallback = true;
};

struct AppConfig {
    bool                 enabled = true;
    UserDirectoryOptions users;
    UiConfig             ui;
    SoundConfig          sounds;
    HealthPolicy         health;
    bool                 filterOtherProviders = true;
    // Which of Windows' own tiles the filter replaces. The default keeps PIN,
    // Hello and smartcard reachable, because on a machine that signs in with a
    // PIN, hiding them is a lockout. See SigninOptions.h.
    SigninOptionsMode    signinOptions = SigninOptionsMode::KeepAlternatives;
    std::vector<std::wstring> alwaysAllowProviders;
    bool                 allowShutdown = true;
    int                  maxFailedAttempts = 0;
    std::wstring         logPath;
    int                  logLevel = 2; // 0=off 1=error 2=info 3=debug

    // Reads whatever is present, falling back to the defaults above. Missing
    // files are not an error.
    static AppConfig FromIni(const IniFile& ini);
};

} // namespace xplogin
