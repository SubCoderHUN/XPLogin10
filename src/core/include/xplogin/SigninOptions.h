// XPLogin10 - which credential providers survive the filter.
//
// The filter is the one object here that can make a machine unusable. Hiding
// every tile but ours is the whole point of it, and it is also exactly what
// locks out anyone whose account has no usable password - a Microsoft account
// signed in with a PIN, a passwordless Entra account, a machine where Windows
// Hello is the only enrolled credential. On those, "hide everything else" means
// "nobody signs in".
//
// So the decision lives here, in portable code with tests, rather than inside
// an ICredentialProviderFilter that only runs on the Winlogon desktop where a
// mistake costs somebody their machine.
//
// The rule: XPLogin always replaces the *password* tiles, because that is the
// screen it is imitating. It never hides Windows Hello, PIN, smartcard or
// biometrics unless explicitly told to - those are alternative ways in, and
// taking them away buys nothing but risk.
#pragma once

#include "xplogin/Types.h"

#include <string>
#include <vector>

namespace xplogin {

// What a provider is for. Only the first is replaced by default.
enum class ProviderKind {
    Password,     // password / picture password - the XP screen replaces these
    Hello,        // PIN, face, fingerprint - a different way to prove identity
    Smartcard,    // physical token, often the only credential in enterprises
    Recovery,     // "other user", password reset, WinRE - deliberate escapes
    Unknown,      // anything not in the table: a third-party provider
};

struct KnownProvider {
    const wchar_t* clsid;   // upper case, braced, as StringFromGUID2 writes it
    const wchar_t* name;
    ProviderKind   kind;
};

// The credential providers Windows 10 and 11 ship, as far as this matters.
// Anything absent is ProviderKind::Unknown and treated as third-party.
const std::vector<KnownProvider>& KnownProviders();

// Case-insensitive lookup. Returns Unknown for a CLSID that is not listed.
ProviderKind ClassifyProvider(const std::wstring& clsid);

// Returns the provider's display name, or an empty string when unknown.
std::wstring ProviderName(const std::wstring& clsid);

// How aggressive the filter should be.
enum class SigninOptionsMode {
    // Replace the password tiles, keep every other way in. The default, and
    // the only one that cannot lock somebody out of their own machine.
    KeepAlternatives = 0,
    // Replace everything. Authentic, and genuinely dangerous: on a machine
    // whose accounts sign in with a PIN this leaves no way in but Safe Mode.
    ReplaceEverything,
    // Filter nothing - the XP tile appears alongside the Windows ones.
    KeepEverything,
};

const wchar_t* SigninOptionsModeName(SigninOptionsMode mode);
SigninOptionsMode ParseSigninOptionsMode(const std::wstring& text,
                                         SigninOptionsMode fallback);

struct FilterPolicy {
    SigninOptionsMode mode = SigninOptionsMode::KeepAlternatives;
    // Our own CLSID, which is never filtered out under any mode.
    std::wstring ownClsid;
    // Extra CLSIDs to keep regardless of kind, from XPLogin.ini. An escape
    // hatch for a third-party provider - an MFA agent, a smartcard middleware
    // - that a deployment cannot do without.
    std::vector<std::wstring> alsoAllow;
};

// The decision for one provider. `allowed` is what goes into the filter's
// BOOL array; `reason` exists so the log says why, which is the difference
// between a five-minute diagnosis and an afternoon.
struct FilterDecision {
    bool         allowed = true;
    const char*  reason = "";
};

FilterDecision DecideProvider(const std::wstring& clsid, const FilterPolicy& policy);

// Applies DecideProvider across a whole list, and then enforces the invariant
// that matters more than any of it: **at least one provider must survive**.
// If the policy would hide everything - our own provider missing from the list
// because Windows could not load the DLL, say - the filtering is abandoned and
// every tile comes back.
//
// Returns true when the plan was applied as decided, false when it had to be
// undone. `out` is resized to `clsids.size()`.
bool BuildFilterPlan(const std::vector<std::wstring>& clsids,
                     const FilterPolicy& policy, std::vector<bool>* out,
                     std::string* explanation = nullptr);

// ---------------------------------------------------------------------------
// Which tiles the provider builds.
//
// The other half of the same question, and the half that was wrong for longer.
// Filtering decides which of Windows' tiles disappear; this decides which of
// ours appear, and getting it wrong in the direction of "too few" produces the
// same empty screen as filtering everything.
//
// Windows 8 changed the shape of the logon screen: XP's screen owned the
// account list and drew the tiles itself, whereas LogonUI draws one tile per
// account and asks each credential which account it belongs to
// (ICredentialProviderCredential2::GetUserSid). A credential is only ever
// reachable from the tile of the account it names, so a provider that returns
// one credential is present on exactly one account's tile - and invisible if
// LogonUI happens to open on any other. One credential per account is what
// makes the screen appear no matter which account Windows opens on.
//
// Returns the SIDs to build tiles for, in the order the accounts were given.
// Accounts with no SID are skipped (there is no tile to attach them to; they
// still appear in the XP screen's own list, which is the list that matters to
// the person signing in) and repeated SIDs are collapsed, so one account can
// never end up carrying two XPLogin tiles.
std::vector<std::wstring> BuildTileSids(const std::vector<UserAccount>& users);

// True when this machine signs itself in with nobody typing anything: one
// account, and that account has no password.
//
// Windows does this by itself and XP did too, so the screen has to agree with
// it rather than argue. What it must not do is put up an account list with one
// tile on it, wait to be clicked, and be signed past a moment later - which is
// a flash of a screen nobody was meant to interact with. XP showed the welcome
// message for the whole of it, and so does this.
//
// Every condition here is about the *list*, which is why the rule lives beside
// the filter rules rather than in the provider: two accounts mean a choice and
// a choice means a screen, and an account that cannot be signed in to would
// have LSA refuse the blank password and leave the screen on an error with no
// tile to go back to.
bool SignsInWithoutAsking(const std::vector<UserAccount>& users);

} // namespace xplogin
