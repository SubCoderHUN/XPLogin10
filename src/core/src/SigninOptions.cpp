#include "xplogin/SigninOptions.h"

#include "xplogin/StringUtil.h"

#include <iterator>
#include <sstream>

namespace xplogin {
namespace {

// Windows' own credential providers, from
// HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Authentication\Credential Providers
// on Windows 10 22H2 and Windows 11 23H2. The CLSIDs are stable across both.
//
// Only the two password ones are replaced by default. Everything else is a
// different way of proving who you are, and hiding those is how people end up
// unable to sign in to a machine that was working ten minutes ago.
const KnownProvider kProviders[] = {
    // --- password: the tiles the XP screen stands in for -------------------
    {L"{60B78E88-EAD8-445C-9CFD-0B87F74EA6CD}", L"Password",
     ProviderKind::Password},
    {L"{8AF662BF-65A0-4D0A-A540-A338A999D57F}", L"Picture password",
     ProviderKind::Password},
    {L"{6F45DC1E-5384-457A-BC13-2CD81B0D28ED}", L"Password (v1 wrapper)",
     ProviderKind::Password},

    // --- Windows Hello ------------------------------------------------------
    {L"{CB82EA12-9F71-446D-89E1-8D0924E1256E}", L"PIN (Windows Hello)",
     ProviderKind::Hello},
    {L"{D6886603-9D2F-4EB2-B667-1971041FA96B}", L"PIN (work or school)",
     ProviderKind::Hello},
    {L"{8A2C93D0-D55F-4045-99D7-B27F5E263407}", L"Face (Windows Hello)",
     ProviderKind::Hello},
    {L"{BEC09223-B018-416D-A0AC-523971B639F5}", L"Fingerprint (Windows Hello)",
     ProviderKind::Hello},
    {L"{27FBDB57-B613-4AF2-9D7E-4FA7A66C21AD}", L"Trusted signal",
     ProviderKind::Hello},
    {L"{F8A0B131-5F68-486C-8040-7E8FC3C85BB6}", L"Windows Hello for Business",
     ProviderKind::Hello},
    {L"{C885AA15-1764-4293-B82A-0586ADD46B35}", L"FIDO / security key",
     ProviderKind::Hello},

    // --- smartcard ----------------------------------------------------------
    {L"{8FD7E19C-3BF7-489B-A72C-846AB3678C96}", L"Smartcard",
     ProviderKind::Smartcard},
    {L"{3DD6BEC0-8193-4FFE-AE25-E08E39EA4063}", L"Smartcard PIN",
     ProviderKind::Smartcard},
    {L"{94596C7E-3744-41CE-893E-BBF09122F76A}", L"Smartcard reader selection",
     ProviderKind::Smartcard},
    {L"{1EE7337F-85AC-45E2-A23C-37C753209769}", L"Smartcard WinRT",
     ProviderKind::Smartcard},

    // --- deliberate escapes -------------------------------------------------
    {L"{CB82EA12-9F71-446D-89E1-8D0924E1256F}", L"Other user",
     ProviderKind::Recovery},
    {L"{5537E283-B1E7-4EF8-9C6E-7AB0AFE5056D}", L"Password reset",
     ProviderKind::Recovery},
    {L"{2135F72A-90B5-4ED3-A7F1-8BB705AC276A}", L"PicturePassword (logon)",
     ProviderKind::Recovery},
};

bool SameClsid(const std::wstring& a, const wchar_t* b) {
    return EqualsNoCase(a, b);
}

} // namespace

const std::vector<KnownProvider>& KnownProviders() {
    static const std::vector<KnownProvider> list(
        std::begin(kProviders), std::end(kProviders));
    return list;
}

ProviderKind ClassifyProvider(const std::wstring& clsid) {
    for (const KnownProvider& provider : kProviders) {
        if (SameClsid(clsid, provider.clsid)) {
            return provider.kind;
        }
    }
    return ProviderKind::Unknown;
}

std::wstring ProviderName(const std::wstring& clsid) {
    for (const KnownProvider& provider : kProviders) {
        if (SameClsid(clsid, provider.clsid)) {
            return provider.name;
        }
    }
    return std::wstring();
}

const wchar_t* SigninOptionsModeName(SigninOptionsMode mode) {
    switch (mode) {
        case SigninOptionsMode::KeepAlternatives:  return L"keepalternatives";
        case SigninOptionsMode::ReplaceEverything: return L"replaceeverything";
        case SigninOptionsMode::KeepEverything:    return L"keepeverything";
    }
    return L"keepalternatives";
}

SigninOptionsMode ParseSigninOptionsMode(const std::wstring& text,
                                         SigninOptionsMode fallback) {
    const std::wstring value = ToLowerAscii(Trim(text));
    if (value.empty()) {
        return fallback;
    }
    // The names people actually type, not just the canonical ones.
    if (value == L"keepalternatives" || value == L"alternatives" ||
        value == L"auto" || value == L"safe" || value == L"hello") {
        return SigninOptionsMode::KeepAlternatives;
    }
    if (value == L"replaceeverything" || value == L"everything" ||
        value == L"all" || value == L"exclusive" || value == L"strict") {
        return SigninOptionsMode::ReplaceEverything;
    }
    if (value == L"keepeverything" || value == L"none" || value == L"off" ||
        value == L"alongside") {
        return SigninOptionsMode::KeepEverything;
    }
    return fallback;
}

FilterDecision DecideProvider(const std::wstring& clsid,
                              const FilterPolicy& policy) {
    // Never, under any mode, hide ourselves. A filter that removes its own
    // provider produces a logon screen with no tiles at all.
    if (!policy.ownClsid.empty() && SameClsid(clsid, policy.ownClsid.c_str())) {
        return {true, "our own provider"};
    }
    for (const std::wstring& allowed : policy.alsoAllow) {
        if (!allowed.empty() && EqualsNoCase(clsid, allowed)) {
            return {true, "explicitly allowed by configuration"};
        }
    }

    switch (policy.mode) {
        case SigninOptionsMode::KeepEverything:
            return {true, "filtering disabled"};
        case SigninOptionsMode::ReplaceEverything:
            return {false, "replaced (mode=replaceeverything)"};
        case SigninOptionsMode::KeepAlternatives:
            break;
    }

    switch (ClassifyProvider(clsid)) {
        case ProviderKind::Password:
            // The one thing the XP screen actually replaces.
            return {false, "password tile, replaced by the XP screen"};
        case ProviderKind::Hello:
            return {true, "Windows Hello - another way in, left alone"};
        case ProviderKind::Smartcard:
            return {true, "smartcard - another way in, left alone"};
        case ProviderKind::Recovery:
            return {true, "recovery path, left alone"};
        case ProviderKind::Unknown:
            break;
    }
    // A provider we do not recognise. It could be an MFA agent or a
    // corporate SSO tile, and hiding it might be the only credential that
    // works on this machine, so it stays.
    return {true, "unrecognised provider, left alone"};
}

bool BuildFilterPlan(const std::vector<std::wstring>& clsids,
                     const FilterPolicy& policy, std::vector<bool>* out,
                     std::string* explanation) {
    if (!out) {
        return false;
    }
    out->assign(clsids.size(), true);
    if (clsids.empty()) {
        return true;
    }

    std::ostringstream log;
    bool anyAllowed = false;
    bool ourselvesPresent = false;

    for (size_t i = 0; i < clsids.size(); ++i) {
        const FilterDecision decision = DecideProvider(clsids[i], policy);
        (*out)[i] = decision.allowed;
        anyAllowed = anyAllowed || decision.allowed;
        if (!policy.ownClsid.empty() &&
            EqualsNoCase(clsids[i], policy.ownClsid)) {
            ourselvesPresent = true;
        }
        log << (decision.allowed ? "  keep " : "  hide ")
            << WideToUtf8(clsids[i]) << "  " << decision.reason << "\n";
    }

    // The invariant. Two ways it can be violated: every provider was filtered
    // out, or ours was not in the list at all - which means Windows failed to
    // load the DLL, and the tiles we hid were the only ones there were.
    if (!anyAllowed || !ourselvesPresent) {
        out->assign(clsids.size(), true);
        log << (ourselvesPresent
                    ? "  -> nothing would remain; filtering abandoned\n"
                    : "  -> XPLogin's own provider is absent; filtering "
                      "abandoned\n");
        if (explanation) {
            *explanation = log.str();
        }
        return false;
    }

    if (explanation) {
        *explanation = log.str();
    }
    return true;
}

std::vector<std::wstring> BuildTileSids(const std::vector<UserAccount>& users) {
    std::vector<std::wstring> sids;
    sids.reserve(users.size());
    for (const UserAccount& user : users) {
        if (user.sid.empty()) {
            continue;
        }
        bool seen = false;
        for (const std::wstring& existing : sids) {
            if (EqualsNoCase(existing, user.sid)) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            sids.push_back(user.sid);
        }
    }
    return sids;
}

bool SignsInWithoutAsking(const std::vector<UserAccount>& users) {
    // Exactly one, and nothing to type. Two accounts means a choice, and a
    // choice means a screen.
    if (users.size() != 1) {
        return false;
    }
    const UserAccount& only = users.front();
    // An account nobody can sign in to would have LSA refuse the blank password
    // and leave the screen sitting on an error with no tile to go back to.
    if (only.disabled || only.lockedOut || only.passwordExpired) {
        return false;
    }
    return only.blankPassword;
}

} // namespace xplogin
