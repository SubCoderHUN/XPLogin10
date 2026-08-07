// Tests for the credential provider filter policy.
//
// This is the code that decides which of Windows' sign-in tiles disappear. Get
// it wrong in one direction and the XP screen shares the logon desktop with the
// Windows one; get it wrong in the other and a machine whose accounts sign in
// with a PIN has no way in at all. That asymmetry is why the rule lives in
// portable code and why these tests are as blunt as they are.
#include "xptest.h"

#include "xplogin/Config.h"
#include "xplogin/SigninOptions.h"
#include "xplogin/StringUtil.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace xplogin;

namespace {

// A few real CLSIDs, written out here rather than referenced from the table so
// that a typo in the table is a test failure instead of a shared assumption.
const wchar_t* const kPassword   = L"{60B78E88-EAD8-445C-9CFD-0B87F74EA6CD}";
const wchar_t* const kPicture    = L"{8AF662BF-65A0-4D0A-A540-A338A999D57F}";
const wchar_t* const kHelloPin   = L"{CB82EA12-9F71-446D-89E1-8D0924E1256E}";
const wchar_t* const kHelloFace  = L"{8A2C93D0-D55F-4045-99D7-B27F5E263407}";
const wchar_t* const kFinger     = L"{BEC09223-B018-416D-A0AC-523971B639F5}";
const wchar_t* const kSmartcard  = L"{8FD7E19C-3BF7-489B-A72C-846AB3678C96}";
const wchar_t* const kThirdParty = L"{11111111-2222-3333-4444-555555555555}";
const wchar_t* const kOurs       = L"{6E2B1FA0-71D4-4C57-9E4A-3B2D5F8C1A77}";

FilterPolicy DefaultPolicy(SigninOptionsMode mode =
                               SigninOptionsMode::KeepAlternatives) {
    FilterPolicy policy;
    policy.mode = mode;
    policy.ownClsid = kOurs;
    return policy;
}

// A realistic Windows 11 logon desktop: ours, password, PIN, face, and the
// picture password tile.
std::vector<std::wstring> TypicalDesktop() {
    return {kOurs, kPassword, kHelloPin, kHelloFace, kPicture};
}

bool AllowedFor(const std::vector<std::wstring>& clsids,
                const std::vector<bool>& plan, const std::wstring& clsid) {
    for (size_t i = 0; i < clsids.size(); ++i) {
        if (clsids[i] == clsid) {
            return plan[i];
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Classification
// ---------------------------------------------------------------------------

TEST(SigninOptions, ClassifiesTheProvidersWindowsShips) {
    CHECK_EQ(ClassifyProvider(kPassword), ProviderKind::Password);
    CHECK_EQ(ClassifyProvider(kPicture), ProviderKind::Password);
    CHECK_EQ(ClassifyProvider(kHelloPin), ProviderKind::Hello);
    CHECK_EQ(ClassifyProvider(kHelloFace), ProviderKind::Hello);
    CHECK_EQ(ClassifyProvider(kFinger), ProviderKind::Hello);
    CHECK_EQ(ClassifyProvider(kSmartcard), ProviderKind::Smartcard);
}

TEST(SigninOptions, AnUnlistedProviderIsUnknownRatherThanPassword) {
    // The default for something we have never seen has to be "leave it alone".
    // Defaulting to Password would hide a corporate MFA tile.
    CHECK_EQ(ClassifyProvider(kThirdParty), ProviderKind::Unknown);
    CHECK_EQ(ClassifyProvider(L""), ProviderKind::Unknown);
    CHECK_EQ(ClassifyProvider(L"not a guid"), ProviderKind::Unknown);
}

TEST(SigninOptions, ClsidComparisonIgnoresCase) {
    CHECK_EQ(ClassifyProvider(L"{60b78e88-ead8-445c-9cfd-0b87f74ea6cd}"),
             ProviderKind::Password);
    CHECK_EQ(ClassifyProvider(L"{cb82ea12-9f71-446d-89e1-8d0924e1256e}"),
             ProviderKind::Hello);
}

TEST(SigninOptions, TheProviderTableHasNoDuplicates) {
    // A duplicated CLSID with two different kinds would make the classification
    // depend on table order, which is not a property anybody should rely on.
    std::vector<std::wstring> seen;
    for (const KnownProvider& provider : KnownProviders()) {
        const std::wstring clsid = provider.clsid;
        for (const std::wstring& other : seen) {
            REQUIRE_FALSE(EqualsNoCase(clsid, other));
        }
        seen.push_back(clsid);

        // Every entry is a well-formed braced GUID and carries a name.
        REQUIRE_EQ(clsid.size(), size_t(38));
        CHECK_EQ(clsid.front(), L'{');
        CHECK_EQ(clsid.back(), L'}');
        CHECK(provider.name != nullptr && provider.name[0] != L'\0');
    }
    CHECK_GT(seen.size(), size_t(10));
}

// ---------------------------------------------------------------------------
// The default: replace the password tiles, keep every other way in
// ---------------------------------------------------------------------------

TEST(SigninOptions, DefaultReplacesPasswordTilesOnly) {
    const FilterPolicy policy = DefaultPolicy();

    CHECK_FALSE(DecideProvider(kPassword, policy).allowed);
    CHECK_FALSE(DecideProvider(kPicture, policy).allowed);

    CHECK(DecideProvider(kHelloPin, policy).allowed);
    CHECK(DecideProvider(kHelloFace, policy).allowed);
    CHECK(DecideProvider(kFinger, policy).allowed);
    CHECK(DecideProvider(kSmartcard, policy).allowed);
    CHECK(DecideProvider(kThirdParty, policy).allowed);
}

TEST(SigninOptions, WeNeverFilterOurselvesOut) {
    for (SigninOptionsMode mode : {SigninOptionsMode::KeepAlternatives,
                                   SigninOptionsMode::ReplaceEverything,
                                   SigninOptionsMode::KeepEverything}) {
        CHECK(DecideProvider(kOurs, DefaultPolicy(mode)).allowed);
    }
}

TEST(SigninOptions, PinSurvivesOnATypicalDesktop) {
    // The scenario that matters most: a Microsoft account with a PIN. If this
    // regresses, that machine needs Safe Mode to get back in.
    const std::vector<std::wstring> clsids = TypicalDesktop();
    std::vector<bool> plan;
    REQUIRE(BuildFilterPlan(clsids, DefaultPolicy(), &plan));
    REQUIRE_EQ(plan.size(), clsids.size());

    CHECK(AllowedFor(clsids, plan, kOurs));
    CHECK(AllowedFor(clsids, plan, kHelloPin));
    CHECK(AllowedFor(clsids, plan, kHelloFace));
    CHECK_FALSE(AllowedFor(clsids, plan, kPassword));
    CHECK_FALSE(AllowedFor(clsids, plan, kPicture));
}

// ---------------------------------------------------------------------------
// The strict mode, and the invariant that protects it
// ---------------------------------------------------------------------------

TEST(SigninOptions, StrictModeHidesEverythingButUs) {
    const std::vector<std::wstring> clsids = TypicalDesktop();
    std::vector<bool> plan;
    REQUIRE(BuildFilterPlan(clsids, DefaultPolicy(SigninOptionsMode::ReplaceEverything),
                            &plan));

    CHECK(AllowedFor(clsids, plan, kOurs));
    for (const std::wstring& clsid : {std::wstring(kPassword), std::wstring(kHelloPin),
                                      std::wstring(kHelloFace), std::wstring(kPicture)}) {
        CHECK_FALSE(AllowedFor(clsids, plan, clsid));
    }
}

TEST(SigninOptions, KeepEverythingModeFiltersNothing) {
    const std::vector<std::wstring> clsids = TypicalDesktop();
    std::vector<bool> plan;
    REQUIRE(BuildFilterPlan(clsids, DefaultPolicy(SigninOptionsMode::KeepEverything),
                            &plan));
    for (bool allowed : plan) {
        CHECK(allowed);
    }
}

TEST(SigninOptions, FilteringIsAbandonedWhenOurProviderIsMissing) {
    // Windows could not load XPLoginProvider.dll, so it is not in the list. If
    // we still hid the password tile the screen would have no tiles at all.
    const std::vector<std::wstring> clsids = {kPassword, kPicture};
    std::vector<bool> plan;
    std::string explanation;

    CHECK_FALSE(BuildFilterPlan(clsids, DefaultPolicy(), &plan, &explanation));
    REQUIRE_EQ(plan.size(), clsids.size());
    for (bool allowed : plan) {
        CHECK(allowed);
    }
    CHECK(explanation.find("abandoned") != std::string::npos);
}

TEST(SigninOptions, FilteringIsAbandonedWhenNothingWouldRemain) {
    // Strict mode with a provider list that somehow does not contain us.
    const std::vector<std::wstring> clsids = {kPassword, kHelloPin};
    std::vector<bool> plan;
    CHECK_FALSE(BuildFilterPlan(
        clsids, DefaultPolicy(SigninOptionsMode::ReplaceEverything), &plan));
    for (bool allowed : plan) {
        CHECK(allowed);
    }
}

TEST(SigninOptions, StrictModeWithOnlyOurselvesIsStillValid) {
    // One surviving tile is enough - it happens to be ours.
    const std::vector<std::wstring> clsids = {kOurs, kPassword};
    std::vector<bool> plan;
    REQUIRE(BuildFilterPlan(
        clsids, DefaultPolicy(SigninOptionsMode::ReplaceEverything), &plan));
    CHECK(plan[0]);
    CHECK_FALSE(plan[1]);
}

TEST(SigninOptions, AnEmptyProviderListIsHandled) {
    std::vector<bool> plan;
    CHECK(BuildFilterPlan({}, DefaultPolicy(), &plan));
    CHECK(plan.empty());
}

TEST(SigninOptions, BuildFilterPlanRejectsANullOutput) {
    CHECK_FALSE(BuildFilterPlan(TypicalDesktop(), DefaultPolicy(), nullptr));
}

// ---------------------------------------------------------------------------
// The configured allow list
// ---------------------------------------------------------------------------

TEST(SigninOptions, ConfiguredProvidersSurviveEvenInStrictMode) {
    // The escape hatch for a deployment whose only working credential is a
    // third-party MFA tile.
    FilterPolicy policy = DefaultPolicy(SigninOptionsMode::ReplaceEverything);
    policy.alsoAllow = {kThirdParty};

    CHECK(DecideProvider(kThirdParty, policy).allowed);
    CHECK_FALSE(DecideProvider(kPassword, policy).allowed);
}

TEST(SigninOptions, TheAllowListIgnoresCaseAndEmptyEntries) {
    FilterPolicy policy = DefaultPolicy(SigninOptionsMode::ReplaceEverything);
    policy.alsoAllow = {L"", L"{11111111-2222-3333-4444-555555555555}"};
    CHECK(DecideProvider(L"{11111111-2222-3333-4444-555555555555}", policy).allowed);
    // An empty entry must not match an empty CLSID into an accidental allow.
    CHECK_FALSE(DecideProvider(L"", policy).allowed);
}

// ---------------------------------------------------------------------------
// Mode parsing
// ---------------------------------------------------------------------------

TEST(SigninOptions, ModeNamesRoundTrip) {
    for (SigninOptionsMode mode : {SigninOptionsMode::KeepAlternatives,
                                   SigninOptionsMode::ReplaceEverything,
                                   SigninOptionsMode::KeepEverything}) {
        CHECK_EQ(ParseSigninOptionsMode(SigninOptionsModeName(mode),
                                        SigninOptionsMode::KeepEverything),
                 mode);
    }
}

TEST(SigninOptions, ModeParsingAcceptsTheWordsPeopleWrite) {
    const SigninOptionsMode other = SigninOptionsMode::KeepEverything;
    CHECK_EQ(ParseSigninOptionsMode(L"auto", other),
             SigninOptionsMode::KeepAlternatives);
    CHECK_EQ(ParseSigninOptionsMode(L"  SAFE  ", other),
             SigninOptionsMode::KeepAlternatives);
    CHECK_EQ(ParseSigninOptionsMode(L"Everything", other),
             SigninOptionsMode::ReplaceEverything);
    CHECK_EQ(ParseSigninOptionsMode(L"strict", other),
             SigninOptionsMode::ReplaceEverything);
    CHECK_EQ(ParseSigninOptionsMode(L"off", SigninOptionsMode::KeepAlternatives),
             SigninOptionsMode::KeepEverything);
}

TEST(SigninOptions, UnrecognisedModesFallBackRatherThanGuessing) {
    // A typo in XPLogin.ini must not silently turn on the mode that can lock
    // somebody out.
    CHECK_EQ(ParseSigninOptionsMode(L"replace-everythng",
                                    SigninOptionsMode::KeepAlternatives),
             SigninOptionsMode::KeepAlternatives);
    CHECK_EQ(ParseSigninOptionsMode(L"", SigninOptionsMode::KeepAlternatives),
             SigninOptionsMode::KeepAlternatives);
}

TEST(SigninOptions, TheShippedDefaultIsTheSafeOne) {
    // If this ever flips, every machine that updates gets the lockout risk.
    CHECK_EQ(AppConfig().signinOptions, SigninOptionsMode::KeepAlternatives);
}

TEST(SigninOptions, ConfigReadsTheModeAndTheAllowList) {
    IniFile ini;
    ini.ParseString(
        "[general]\n"
        "signinoptions = replaceeverything\n"
        "alwaysallowproviders = {11111111-2222-3333-4444-555555555555} ; "
        "{22222222-3333-4444-5555-666666666666}\n");

    const AppConfig config = AppConfig::FromIni(ini);
    CHECK_EQ(config.signinOptions, SigninOptionsMode::ReplaceEverything);
    REQUIRE_EQ(config.alwaysAllowProviders.size(), size_t(2));
    CHECK_EQ(config.alwaysAllowProviders[0],
             std::wstring(L"{11111111-2222-3333-4444-555555555555}"));
    CHECK_EQ(config.alwaysAllowProviders[1],
             std::wstring(L"{22222222-3333-4444-5555-666666666666}"));
}

TEST(SigninOptions, AnAbsentConfigLeavesTheSafeDefault) {
    IniFile ini;
    ini.ParseString("[general]\nenabled = true\n");
    const AppConfig config = AppConfig::FromIni(ini);
    CHECK_EQ(config.signinOptions, SigninOptionsMode::KeepAlternatives);
    CHECK(config.alwaysAllowProviders.empty());
}

// ---------------------------------------------------------------------------
// BuildTileSids - which accounts get an XPLogin tile.
//
// This is the invariant that, when it was wrong, produced a logon screen with
// nothing on it but LogonUI's own corner buttons. The provider used to hand
// Windows one credential that claimed no account at all; Windows 8 and later
// place a credential on the tile of the account its GetUserSid names, so a
// credential naming nobody was drawn nowhere, and the Windows password tiles
// had already been filtered away.
// ---------------------------------------------------------------------------

namespace {

UserAccount MakeAccount(const wchar_t* name, const wchar_t* sid) {
    UserAccount account;
    account.username = name;
    account.displayName = name;
    account.domain = L"XPBOX";
    account.sid = sid ? sid : L"";
    account.source = AccountSource::Local;
    return account;
}

} // namespace

TEST(SigninOptions, EveryAccountWithASidGetsATile) {
    // The point of one tile per account: LogonUI opens on whichever account it
    // pleases, usually the last one to sign in. If our credential is not on
    // that account's tile, nothing of ours is on the screen.
    const std::vector<UserAccount> users = {
        MakeAccount(L"Bill", L"S-1-5-21-1-2-3-1001"),
        MakeAccount(L"Melinda", L"S-1-5-21-1-2-3-1002"),
        MakeAccount(L"Guest", L"S-1-5-21-1-2-3-501"),
    };

    const std::vector<std::wstring> sids = BuildTileSids(users);
    REQUIRE_EQ(sids.size(), size_t(3));
    CHECK_EQ(sids[0], std::wstring(L"S-1-5-21-1-2-3-1001"));
    CHECK_EQ(sids[1], std::wstring(L"S-1-5-21-1-2-3-1002"));
    CHECK_EQ(sids[2], std::wstring(L"S-1-5-21-1-2-3-501"));
}

TEST(SigninOptions, AccountsWithoutASidAreSkipped) {
    // There is no tile to attach them to. They still appear in the XP screen's
    // own account list, which is the one the person signing in actually uses.
    const std::vector<UserAccount> users = {
        MakeAccount(L"Bill", L"S-1-5-21-1-2-3-1001"),
        MakeAccount(L"Ghost", nullptr),
        MakeAccount(L"Melinda", L"S-1-5-21-1-2-3-1002"),
    };

    const std::vector<std::wstring> sids = BuildTileSids(users);
    REQUIRE_EQ(sids.size(), size_t(2));
    CHECK_EQ(sids[0], std::wstring(L"S-1-5-21-1-2-3-1001"));
    CHECK_EQ(sids[1], std::wstring(L"S-1-5-21-1-2-3-1002"));
}

TEST(SigninOptions, ARepeatedSidNeverCarriesTwoTiles) {
    // The SAM and the LogonUI user array can both name the same account - a
    // roamed profile, a renamed user. Two credentials claiming one SID would
    // put two XPLogin entries on one tile.
    const std::vector<UserAccount> users = {
        MakeAccount(L"Bill", L"S-1-5-21-1-2-3-1001"),
        MakeAccount(L"XPBOX\\Bill", L"s-1-5-21-1-2-3-1001"), // SIDs are not case sensitive
        MakeAccount(L"Melinda", L"S-1-5-21-1-2-3-1002"),
    };

    const std::vector<std::wstring> sids = BuildTileSids(users);
    REQUIRE_EQ(sids.size(), size_t(2));
    CHECK_EQ(sids[0], std::wstring(L"S-1-5-21-1-2-3-1001"));
    CHECK_EQ(sids[1], std::wstring(L"S-1-5-21-1-2-3-1002"));
}

TEST(SigninOptions, NoAccountsMeansNoTiles) {
    // The provider falls back to a single unattached credential in this case,
    // and logs an error: a machine we could not enumerate is a machine whose
    // screen we cannot be sure of.
    CHECK(BuildTileSids({}).empty());
    CHECK(BuildTileSids({MakeAccount(L"Ghost", nullptr)}).empty());
}

// ---------------------------------------------------------------------------
// The machine that signs itself in
// ---------------------------------------------------------------------------
//
// One account with no password: Windows lets it straight through and XP did
// too. The screen has to agree with that rather than put up a one-tile list
// that something clicks past a moment later.

namespace {

UserAccount PasswordlessAccount(const wchar_t* name) {
    UserAccount account;
    account.username = name;
    account.displayName = name;
    account.domain = L"WINBOX";
    account.sid = std::wstring(L"S-1-5-21-1-2-3-") + name;
    account.blankPassword = true;
    return account;
}

} // namespace

TEST(SigninOptions, OneAccountWithNoPasswordSignsInWithoutAsking) {
    CHECK(SignsInWithoutAsking({PasswordlessAccount(L"Kiosk")}));
}

TEST(SigninOptions, AnAccountWithAPasswordAlwaysGetsAScreen) {
    UserAccount bill = PasswordlessAccount(L"Bill");
    bill.blankPassword = false;
    CHECK_FALSE(SignsInWithoutAsking({bill}));
}

TEST(SigninOptions, TwoAccountsMeanAChoiceAndAChoiceMeansAScreen) {
    // Even when both have no password: which of them is a question, and a
    // question needs the list.
    CHECK_FALSE(SignsInWithoutAsking(
        {PasswordlessAccount(L"Kiosk"), PasswordlessAccount(L"Guest")}));
}

TEST(SigninOptions, NoAccountsNeverSignsInByItself) {
    CHECK_FALSE(SignsInWithoutAsking({}));
}

// An account LSA will refuse must not be handed a blank password unattended:
// the screen would land on an error with no tile behind it to go back to.
TEST(SigninOptions, AnAccountThatCannotSignInIsNotSignedInAutomatically) {
    UserAccount disabled = PasswordlessAccount(L"Kiosk");
    disabled.disabled = true;
    CHECK_FALSE(SignsInWithoutAsking({disabled}));

    UserAccount lockedOut = PasswordlessAccount(L"Kiosk");
    lockedOut.lockedOut = true;
    CHECK_FALSE(SignsInWithoutAsking({lockedOut}));

    UserAccount expired = PasswordlessAccount(L"Kiosk");
    expired.passwordExpired = true;
    CHECK_FALSE(SignsInWithoutAsking({expired}));
}
