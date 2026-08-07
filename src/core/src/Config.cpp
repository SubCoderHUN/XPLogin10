#include "xplogin/Config.h"

#include "xplogin/StringUtil.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace xplogin {
namespace {

std::string LowerAscii(const std::string& in) {
    std::string out = in;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

// Splits a comma or semicolon separated list, trimming each entry and
// dropping empties. Used for the credential-provider allow list.
std::vector<std::wstring> SplitList(const std::wstring& text) {
    std::vector<std::wstring> parts;
    std::wstring current;
    for (wchar_t c : text) {
        if (c == L',' || c == L';') {
            const std::wstring trimmed = Trim(current);
            if (!trimmed.empty()) {
                parts.push_back(trimmed);
            }
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    const std::wstring trimmed = Trim(current);
    if (!trimmed.empty()) {
        parts.push_back(trimmed);
    }
    return parts;
}

std::string Unquote(const std::string& in) {
    if (in.size() >= 2 && in.front() == '"' && in.back() == '"') {
        return in.substr(1, in.size() - 2);
    }
    return in;
}

bool ParseHexByte(const std::string& text, size_t offset, uint32_t* out) {
    uint32_t value = 0;
    for (size_t i = 0; i < 2; ++i) {
        char c = text[offset + i];
        uint32_t digit;
        if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A' + 10);
        else return false;
        value = (value << 4) | digit;
    }
    *out = value;
    return true;
}

} // namespace

bool IniFile::ParseString(const std::string& utf8Text) {
    errors_.clear();
    std::istringstream stream(utf8Text);
    std::string line;
    std::string section = "";
    int lineNumber = 0;
    bool ok = true;

    while (std::getline(stream, line)) {
        ++lineNumber;
        std::string trimmed = TrimAscii(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') {
            continue;
        }
        if (trimmed.front() == '[') {
            size_t close = trimmed.find(']');
            if (close == std::string::npos) {
                errors_.push_back("line " + std::to_string(lineNumber) +
                                  ": unterminated section header");
                ok = false;
                continue;
            }
            section = LowerAscii(TrimAscii(trimmed.substr(1, close - 1)));
            continue;
        }
        size_t equals = trimmed.find('=');
        if (equals == std::string::npos) {
            errors_.push_back("line " + std::to_string(lineNumber) +
                              ": expected key = value");
            ok = false;
            continue;
        }
        std::string key = LowerAscii(TrimAscii(trimmed.substr(0, equals)));
        std::string value = Unquote(TrimAscii(trimmed.substr(equals + 1)));
        if (key.empty()) {
            errors_.push_back("line " + std::to_string(lineNumber) + ": empty key");
            ok = false;
            continue;
        }
        data_[section][key] = Utf8ToWide(value);
    }
    return ok;
}

bool IniFile::LoadFromFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        errors_.push_back("cannot open " + path);
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    // Strip a UTF-8 BOM if present; Notepad loves adding one.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text = text.substr(3);
    }
    return ParseString(text);
}

bool IniFile::Has(const std::string& section, const std::string& key) const {
    auto s = data_.find(LowerAscii(section));
    if (s == data_.end()) {
        return false;
    }
    return s->second.count(LowerAscii(key)) > 0;
}

std::wstring IniFile::GetString(const std::string& section, const std::string& key,
                                const std::wstring& fallback) const {
    auto s = data_.find(LowerAscii(section));
    if (s == data_.end()) {
        return fallback;
    }
    auto k = s->second.find(LowerAscii(key));
    if (k == s->second.end()) {
        return fallback;
    }
    return k->second;
}

int IniFile::GetInt(const std::string& section, const std::string& key,
                    int fallback) const {
    if (!Has(section, key)) {
        return fallback;
    }
    std::string text = WideToUtf8(GetString(section, key));
    if (text.empty()) {
        return fallback;
    }
    char* end = nullptr;
    long value = std::strtol(text.c_str(), &end, 0);
    if (end == text.c_str()) {
        return fallback;
    }
    return static_cast<int>(value);
}

double IniFile::GetDouble(const std::string& section, const std::string& key,
                          double fallback) const {
    if (!Has(section, key)) {
        return fallback;
    }
    std::string text = WideToUtf8(GetString(section, key));
    if (text.empty()) {
        return fallback;
    }
    char* end = nullptr;
    double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str()) {
        return fallback;
    }
    return value;
}

bool IniFile::GetBool(const std::string& section, const std::string& key,
                      bool fallback) const {
    if (!Has(section, key)) {
        return fallback;
    }
    std::string text = LowerAscii(WideToUtf8(GetString(section, key)));
    if (text == "1" || text == "true" || text == "yes" || text == "on") {
        return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
        return false;
    }
    return fallback;
}

uint32_t IniFile::GetColor(const std::string& section, const std::string& key,
                           uint32_t fallback) const {
    if (!Has(section, key)) {
        return fallback;
    }
    std::string text = TrimAscii(WideToUtf8(GetString(section, key)));
    if (text.empty()) {
        return fallback;
    }

    if (text[0] == '#') {
        std::string hex = text.substr(1);
        uint32_t a = 0xFF;
        uint32_t r = 0;
        uint32_t g = 0;
        uint32_t b = 0;
        if (hex.size() == 6) {
            if (!ParseHexByte(hex, 0, &r) || !ParseHexByte(hex, 2, &g) ||
                !ParseHexByte(hex, 4, &b)) {
                return fallback;
            }
        } else if (hex.size() == 8) {
            if (!ParseHexByte(hex, 0, &a) || !ParseHexByte(hex, 2, &r) ||
                !ParseHexByte(hex, 4, &g) || !ParseHexByte(hex, 6, &b)) {
                return fallback;
            }
        } else {
            return fallback;
        }
        return (a << 24) | (r << 16) | (g << 8) | b;
    }

    // "R, G, B" form.
    int channels[3] = {0, 0, 0};
    size_t start = 0;
    for (int i = 0; i < 3; ++i) {
        size_t comma = text.find(',', start);
        std::string part = (comma == std::string::npos)
                               ? text.substr(start)
                               : text.substr(start, comma - start);
        part = TrimAscii(part);
        if (part.empty()) {
            return fallback;
        }
        char* end = nullptr;
        long v = std::strtol(part.c_str(), &end, 10);
        if (end == part.c_str() || v < 0 || v > 255) {
            return fallback;
        }
        channels[i] = static_cast<int>(v);
        if (comma == std::string::npos) {
            if (i != 2) {
                return fallback;
            }
            break;
        }
        start = comma + 1;
    }
    return 0xFF000000u | (static_cast<uint32_t>(channels[0]) << 16) |
           (static_cast<uint32_t>(channels[1]) << 8) |
           static_cast<uint32_t>(channels[2]);
}

std::vector<std::string> IniFile::Sections() const {
    std::vector<std::string> out;
    out.reserve(data_.size());
    for (const auto& entry : data_) {
        out.push_back(entry.first);
    }
    return out;
}

std::vector<std::string> IniFile::KeysIn(const std::string& section) const {
    std::vector<std::string> out;
    auto s = data_.find(LowerAscii(section));
    if (s == data_.end()) {
        return out;
    }
    out.reserve(s->second.size());
    for (const auto& entry : s->second) {
        out.push_back(entry.first);
    }
    return out;
}

void IniFile::Set(const std::string& section, const std::string& key,
                  const std::wstring& value) {
    data_[LowerAscii(section)][LowerAscii(key)] = value;
}

AppConfig AppConfig::FromIni(const IniFile& ini) {
    AppConfig config;

    config.enabled = ini.GetBool("general", "enabled", config.enabled);
    config.filterOtherProviders =
        ini.GetBool("general", "filterotherproviders", config.filterOtherProviders);
    config.signinOptions = ParseSigninOptionsMode(
        ini.GetString("general", "signinoptions", std::wstring()),
        config.signinOptions);
    config.alwaysAllowProviders =
        SplitList(ini.GetString("general", "alwaysallowproviders", std::wstring()));
    config.allowShutdown = ini.GetBool("general", "allowshutdown", config.allowShutdown);
    config.maxFailedAttempts =
        ini.GetInt("general", "maxfailedattempts", config.maxFailedAttempts);
    config.logPath = ini.GetString("general", "logpath", config.logPath);
    config.logLevel = ini.GetInt("general", "loglevel", config.logLevel);

    config.users.showDisabledAccounts =
        ini.GetBool("users", "showdisabled", config.users.showDisabledAccounts);
    config.users.showDomainAccounts =
        ini.GetBool("users", "showdomain", config.users.showDomainAccounts);
    config.users.showBuiltInAdministrator = ini.GetBool(
        "users", "showadministrator", config.users.showBuiltInAdministrator);
    config.users.showGuestAccount =
        ini.GetBool("users", "showguest", config.users.showGuestAccount);
    config.users.showMicrosoftAccounts = ini.GetBool(
        "users", "showmicrosoftaccounts", config.users.showMicrosoftAccounts);
    config.users.autoLogonBlankPassword = ini.GetBool(
        "users", "autologonblankpassword", config.users.autoLogonBlankPassword);
    config.users.maxAccounts = static_cast<size_t>(
        ini.GetInt("users", "maxaccounts", static_cast<int>(config.users.maxAccounts)));

    // "hidden = Guest, kiosk, svc_backup"
    std::wstring hidden = ini.GetString("users", "hidden");
    if (!hidden.empty()) {
        size_t start = 0;
        while (start <= hidden.size()) {
            size_t comma = hidden.find(L',', start);
            std::wstring name = Trim(comma == std::wstring::npos
                                         ? hidden.substr(start)
                                         : hidden.substr(start, comma - start));
            if (!name.empty()) {
                config.users.extraHiddenNames.push_back(name);
            }
            if (comma == std::wstring::npos) {
                break;
            }
            start = comma + 1;
        }
    }

    config.ui.themePath = ini.GetString("ui", "theme", config.ui.themePath);
    config.ui.avatarDirectory =
        ini.GetString("ui", "avatardirectory", config.ui.avatarDirectory);
    config.ui.defaultAvatarPath =
        ini.GetString("ui", "defaultavatar", config.ui.defaultAvatarPath);
    config.ui.animationsEnabled =
        ini.GetBool("ui", "animations", config.ui.animationsEnabled);
    config.ui.tileSlideDurationMs =
        ini.GetInt("ui", "tileslidems", config.ui.tileSlideDurationMs);
    config.ui.showStatusLine =
        ini.GetBool("ui", "showstatusline", config.ui.showStatusLine);
    config.ui.showHintButton =
        ini.GetBool("ui", "showhintbutton", config.ui.showHintButton);

    config.sounds.enabled = ini.GetBool("sounds", "enabled", config.sounds.enabled);
    config.sounds.logonPath = ini.GetString("sounds", "logon", config.sounds.logonPath);
    config.sounds.logoffPath =
        ini.GetString("sounds", "logoff", config.sounds.logoffPath);
    config.sounds.errorPath = ini.GetString("sounds", "error", config.sounds.errorPath);
    config.sounds.clickPath = ini.GetString("sounds", "click", config.sounds.clickPath);
    config.sounds.shutdownPath =
        ini.GetString("sounds", "shutdown", config.sounds.shutdownPath);
    config.sounds.volumePercent =
        ini.GetInt("sounds", "volume", config.sounds.volumePercent);
    if (config.sounds.volumePercent < 0) config.sounds.volumePercent = 0;
    if (config.sounds.volumePercent > 100) config.sounds.volumePercent = 100;

    config.health.enabled = ini.GetBool("safety", "enabled", config.health.enabled);
    config.health.unfilterThreshold = static_cast<uint32_t>(ini.GetInt(
        "safety", "unfilterthreshold",
        static_cast<int>(config.health.unfilterThreshold)));
    config.health.bypassThreshold = static_cast<uint32_t>(
        ini.GetInt("safety", "bypassthreshold",
                   static_cast<int>(config.health.bypassThreshold)));

    return config;
}

} // namespace xplogin
