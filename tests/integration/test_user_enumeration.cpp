// Integration tests: dynamic account enumeration.
//
// Required coverage: "felhasznaloi fiokok dinamikus lekerdezesenek ellenorzese"
// - that the tile list really is built from the account database at the moment
// the screen appears, and that it applies the same visibility rules XP did.
#include "xplogin/UserDirectory.h"
#include "Mocks.h"
#include "xptest.h"

#include <memory>

using namespace xplogin;
using namespace xplogin::testing;

namespace {

struct Fixture {
    std::shared_ptr<FakeAccountDatabase> db =
        std::make_shared<FakeAccountDatabase>(L"WINBOX");
    std::shared_ptr<FakeUserEnumerator> enumerator;
    UserDirectoryOptions options;

    Fixture() { enumerator = std::make_shared<FakeUserEnumerator>(db); }

    UserDirectory Make() { return UserDirectory(enumerator, options); }
};

bool ListContains(const std::vector<UserAccount>& users, const wchar_t* name) {
    for (const UserAccount& user : users) {
        if (EqualsNoCase(user.username, name)) {
            return true;
        }
    }
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// Basic enumeration
// ---------------------------------------------------------------------------

TEST(UserEnumeration, ListsLocalAccounts) {
    Fixture fixture;
    fixture.db->Add(L"Bill", L"pw");
    fixture.db->Add(L"Alice", L"pw");

    UserDirectory directory = fixture.Make();
    directory.Refresh();

    CHECK_EQ(directory.Count(), size_t(2));
    CHECK(ListContains(directory.Users(), L"Bill"));
    CHECK(ListContains(directory.Users(), L"Alice"));
    CHECK_EQ(fixture.enumerator->localQueryCount, 1);
}

TEST(UserEnumeration, QualifiesLocalAccountsWithTheMachineName) {
    Fixture fixture;
    fixture.db->Add(L"Bill", L"pw");

    UserDirectory directory = fixture.Make();
    directory.Refresh();

    REQUIRE_EQ(directory.Count(), size_t(1));
    CHECK_EQ(directory.Users()[0].domain, std::wstring(L"WINBOX"));
    CHECK_EQ(directory.Users()[0].QualifiedName(), std::wstring(L"WINBOX\\Bill"));
    CHECK_EQ(directory.MachineName(), std::wstring(L"WINBOX"));
}

TEST(UserEnumeration, RefreshPicksUpAccountsAddedAfterTheFirstQuery) {
    Fixture fixture;
    fixture.db->Add(L"Bill", L"pw");

    UserDirectory directory = fixture.Make();
    directory.Refresh();
    CHECK_EQ(directory.Count(), size_t(1));

    // Somebody ran `net user testuser /add` while the welcome screen was up.
    fixture.db->Add(L"testuser", L"pw");
    directory.Refresh();

    CHECK_EQ(directory.Count(), size_t(2));
    CHECK(ListContains(directory.Users(), L"testuser"));
    CHECK_EQ(fixture.enumerator->localQueryCount, 2);
}

TEST(UserEnumeration, RefreshDropsDeletedAccounts) {
    Fixture fixture;
    fixture.db->Add(L"Bill", L"pw");
    fixture.db->Add(L"Temp", L"pw");

    UserDirectory directory = fixture.Make();
    directory.Refresh();
    CHECK_EQ(directory.Count(), size_t(2));

    fixture.db->Accounts().pop_back();
    directory.Refresh();

    CHECK_EQ(directory.Count(), size_t(1));
    CHECK_FALSE(ListContains(directory.Users(), L"Temp"));
}

TEST(UserEnumeration, DisplayNameFallsBackToTheAccountName) {
    Fixture fixture;
    FakeAccount& account = fixture.db->Add(L"bgates", L"pw");
    account.account.displayName.clear();

    UserDirectory directory = fixture.Make();
    directory.Refresh();

    REQUIRE_EQ(directory.Count(), size_t(1));
    CHECK_EQ(directory.Users()[0].displayName, std::wstring(L"bgates"));
}

// ---------------------------------------------------------------------------
// Domain accounts
// ---------------------------------------------------------------------------

TEST(UserEnumeration, MergesLocalAndDomainAccounts) {
    Fixture fixture;
    fixture.db->SetDomainJoined(true);
    fixture.db->Add(L"LocalAdmin", L"pw", AccountSource::Local);
    fixture.db->Add(L"alice", L"pw", AccountSource::Domain);
    fixture.db->Add(L"bob", L"pw", AccountSource::Domain);

    UserDirectory directory = fixture.Make();
    directory.Refresh();

    CHECK_EQ(directory.Count(), size_t(3));
    CHECK_EQ(fixture.enumerator->domainQueryCount, 1);

    const int aliceIndex = directory.IndexOfUser(L"CONTOSO\\alice");
    REQUIRE_GE(aliceIndex, 0);
    CHECK_EQ(static_cast<int>(directory.At(aliceIndex)->source),
             static_cast<int>(AccountSource::Domain));
    CHECK_EQ(directory.At(aliceIndex)->domain, std::wstring(L"CONTOSO"));
}

TEST(UserEnumeration, DomainAccountsCanBeSuppressed) {
    Fixture fixture;
    fixture.options.showDomainAccounts = false;
    fixture.db->Add(L"LocalAdmin", L"pw", AccountSource::Local);
    fixture.db->Add(L"alice", L"pw", AccountSource::Domain);

    UserDirectory directory = fixture.Make();
    directory.Refresh();

    CHECK_EQ(directory.Count(), size_t(1));
    CHECK_FALSE(ListContains(directory.Users(), L"alice"));
    // The domain enumerator is not even called, which matters when the DC is
    // unreachable and the call would block for 30 seconds.
    CHECK_EQ(fixture.enumerator->domainQueryCount, 0);
}

TEST(UserEnumeration, LocalAndDomainAccountsWithTheSameNameCoexist) {
    Fixture fixture;
    fixture.db->Add(L"alice", L"pw", AccountSource::Local);
    fixture.db->Add(L"alice", L"pw", AccountSource::Domain);

    UserDirectory directory = fixture.Make();
    directory.Refresh();

    // Different domains, so both tiles are shown - picking the wrong one is a
    // real support call, and hiding one would be worse.
    CHECK_EQ(directory.Count(), size_t(2));
    CHECK_GE(directory.IndexOfUser(L"WINBOX\\alice"), 0);
    CHECK_GE(directory.IndexOfUser(L"CONTOSO\\alice"), 0);
    CHECK_NE(directory.IndexOfUser(L"WINBOX\\alice"),
             directory.IndexOfUser(L"CONTOSO\\alice"));
}

// ---------------------------------------------------------------------------
// Visibility rules
// ---------------------------------------------------------------------------

TEST(UserEnumeration, HidesSystemAccounts) {
    Fixture fixture;
    fixture.db->Add(L"Bill", L"pw");
    fixture.db->Add(L"DefaultAccount", L"");
    fixture.db->Add(L"WDAGUtilityAccount", L"");
    fixture.db->Add(L"defaultuser0", L"");
    fixture.db->Add(L"WINBOX2$", L"");

    UserDirectory directory = fixture.Make();
    directory.Refresh();

    CHECK_EQ(directory.Count(), size_t(1));
    CHECK_EQ(directory.Users()[0].username, std::wstring(L"Bill"));
}

TEST(UserEnumeration, HidesGuestByDefault) {
    Fixture fixture;
    fixture.db->Add(L"Bill", L"pw");
    fixture.db->Add(L"Guest", L"");

    UserDirectory hidden = fixture.Make();
    hidden.Refresh();
    CHECK_FALSE(ListContains(hidden.Users(), L"Guest"));

    fixture.options.showGuestAccount = true;
    UserDirectory shown = fixture.Make();
    shown.Refresh();
    CHECK(ListContains(shown.Users(), L"Guest"));
}

TEST(UserEnumeration, HidesBuiltInAdministratorWhenOtherAccountsExist) {
    Fixture fixture;
    fixture.db->Add(L"Administrator", L"pw");
    fixture.db->Add(L"Bill", L"pw");

    UserDirectory directory = fixture.Make();
    directory.Refresh();

    CHECK_EQ(directory.Count(), size_t(1));
    CHECK_FALSE(ListContains(directory.Users(), L"Administrator"));
}

TEST(UserEnumeration, ShowsAdministratorWhenItIsTheOnlyWayIn) {
    // XP's behaviour, and the difference between an awkward screen and an
    // unusable machine.
    Fixture fixture;
    fixture.db->Add(L"Administrator", L"pw");
    fixture.db->Add(L"Guest", L"");

    UserDirectory directory = fixture.Make();
    directory.Refresh();

    CHECK_EQ(directory.Count(), size_t(1));
    CHECK(ListContains(directory.Users(), L"Administrator"));
}

TEST(UserEnumeration, HidesDisabledAccountsByDefault) {
    Fixture fixture;
    fixture.db->Add(L"Bill", L"pw");
    fixture.db->Add(L"Retired", L"pw").account.disabled = true;

    UserDirectory directory = fixture.Make();
    directory.Refresh();
    CHECK_EQ(directory.Count(), size_t(1));

    fixture.options.showDisabledAccounts = true;
    UserDirectory withDisabled = fixture.Make();
    withDisabled.Refresh();
    CHECK_EQ(withDisabled.Count(), size_t(2));
}

TEST(UserEnumeration, HonoursSpecialAccountsUserList) {
    Fixture fixture;
    fixture.db->Add(L"Bill", L"pw");
    fixture.db->Add(L"svc_sql", L"pw");
    fixture.db->hiddenNames.push_back(L"svc_sql");

    UserDirectory directory = fixture.Make();
    directory.Refresh();

    CHECK_EQ(directory.Count(), size_t(1));
    CHECK_FALSE(ListContains(directory.Users(), L"svc_sql"));
}

TEST(UserEnumeration, HonoursTheConfiguredHiddenList) {
    Fixture fixture;
    fixture.db->Add(L"Bill", L"pw");
    fixture.db->Add(L"kiosk", L"pw");
    fixture.options.extraHiddenNames.push_back(L"KIOSK"); // case insensitive

    UserDirectory directory = fixture.Make();
    directory.Refresh();

    CHECK_EQ(directory.Count(), size_t(1));
    CHECK_FALSE(ListContains(directory.Users(), L"kiosk"));
}

TEST(UserEnumeration, LockedOutAccountsAreListedButSortedLast) {
    Fixture fixture;
    fixture.db->Add(L"Zoe", L"pw");
    fixture.db->Add(L"Alice", L"pw").account.lockedOut = true;

    UserDirectory directory = fixture.Make();
    directory.Refresh();

    REQUIRE_EQ(directory.Count(), size_t(2));
    // Usable accounts come first even though "Alice" sorts before "Zoe".
    CHECK_EQ(directory.Users()[0].username, std::wstring(L"Zoe"));
    CHECK_EQ(directory.Users()[1].username, std::wstring(L"Alice"));
    CHECK(directory.Users()[1].lockedOut);
}

TEST(UserEnumeration, CapsTheAccountList) {
    Fixture fixture;
    for (int i = 0; i < 50; ++i) {
        fixture.db->Add(L"user" + std::to_wstring(i), L"pw");
    }
    fixture.options.maxAccounts = 10;

    UserDirectory directory = fixture.Make();
    directory.Refresh();
    CHECK_EQ(directory.Count(), size_t(10));
}

// ---------------------------------------------------------------------------
// Ordering and lookup
// ---------------------------------------------------------------------------

TEST(UserEnumeration, SortsAlphabeticallyIgnoringCase) {
    Fixture fixture;
    fixture.db->Add(L"zoe", L"pw");
    fixture.db->Add(L"Alice", L"pw");
    fixture.db->Add(L"bob", L"pw");

    UserDirectory directory = fixture.Make();
    directory.Refresh();

    REQUIRE_EQ(directory.Count(), size_t(3));
    CHECK_EQ(directory.Users()[0].username, std::wstring(L"Alice"));
    CHECK_EQ(directory.Users()[1].username, std::wstring(L"bob"));
    CHECK_EQ(directory.Users()[2].username, std::wstring(L"zoe"));
}

TEST(UserEnumeration, OrderIsStableAcrossRefreshes) {
    Fixture fixture;
    fixture.db->Add(L"Bill", L"pw");
    fixture.db->Add(L"Alice", L"pw");
    fixture.db->Add(L"Carol", L"pw");

    UserDirectory directory = fixture.Make();
    directory.Refresh();
    const std::vector<UserAccount> first = directory.Users();

    directory.Refresh();
    const std::vector<UserAccount>& second = directory.Users();

    REQUIRE_EQ(first.size(), second.size());
    for (size_t i = 0; i < first.size(); ++i) {
        // Tiles must not shuffle when the screen refreshes underneath the user.
        CHECK_EQ(first[i].username, second[i].username);
    }
}

TEST(UserEnumeration, LooksUpByNameAndBySid) {
    Fixture fixture;
    fixture.db->Add(L"Bill", L"pw");
    fixture.db->Add(L"Alice", L"pw");

    UserDirectory directory = fixture.Make();
    directory.Refresh();

    const int index = directory.IndexOfUser(L"bill"); // case insensitive
    REQUIRE_GE(index, 0);
    CHECK_EQ(directory.At(index)->username, std::wstring(L"Bill"));
    CHECK_EQ(directory.IndexOfUser(L"WINBOX\\Bill"), index);

    const std::wstring sid = directory.At(index)->sid;
    CHECK_EQ(directory.IndexOfSid(sid), index);
}

TEST(UserEnumeration, LookupMissesReturnMinusOne) {
    Fixture fixture;
    fixture.db->Add(L"Bill", L"pw");

    UserDirectory directory = fixture.Make();
    directory.Refresh();

    CHECK_EQ(directory.IndexOfUser(L"Clippy"), -1);
    CHECK_EQ(directory.IndexOfUser(L"OTHERBOX\\Bill"), -1);
    CHECK_EQ(directory.IndexOfUser(L""), -1);
    CHECK_EQ(directory.IndexOfSid(L"S-1-5-21-nope"), -1);
    CHECK_EQ(directory.IndexOfSid(L""), -1);
    CHECK(directory.At(-1) == nullptr);
    CHECK(directory.At(99) == nullptr);
}

TEST(UserEnumeration, DeduplicatesBySid) {
    // The same account arriving from both enumerators must not double up.
    std::vector<UserAccount> input;
    UserAccount account;
    account.username = L"Bill";
    account.domain = L"WINBOX";
    account.sid = L"S-1-5-21-1-2-3-1001";
    input.push_back(account);
    input.push_back(account);

    const auto visible =
        UserDirectory::ApplyVisibilityRules(input, {}, UserDirectoryOptions{});
    CHECK_EQ(visible.size(), size_t(1));
}

TEST(UserEnumeration, SurvivesAMissingEnumerator) {
    // If NetUserEnum fails we show an empty list rather than crashing LogonUI.
    UserDirectory directory(nullptr, UserDirectoryOptions{});
    directory.Refresh();
    CHECK_EQ(directory.Count(), size_t(0));
    CHECK_EQ(directory.IndexOfUser(L"Bill"), -1);
}

TEST(UserEnumeration, IdentifiesSystemAccountsDirectly) {
    CHECK(UserDirectory::IsSystemAccount(L"DefaultAccount"));
    CHECK(UserDirectory::IsSystemAccount(L"defaultaccount"));
    CHECK(UserDirectory::IsSystemAccount(L"WDAGUtilityAccount"));
    CHECK(UserDirectory::IsSystemAccount(L"SOMEMACHINE$"));
    CHECK_FALSE(UserDirectory::IsSystemAccount(L"Bill"));
    CHECK_FALSE(UserDirectory::IsSystemAccount(L""));
}

// ---------------------------------------------------------------------------
// Microsoft accounts
// ---------------------------------------------------------------------------
//
// A machine set up with a Microsoft account and no local one is the common
// case on a shop-bought PC, and Windows hands the sign-in screen a qualified
// name of "MicrosoftAccount\someone@example.com" for it. That is neither
// "WINBOX\Bill" nor "CONTOSO\alice", and treating it as the latter is how the
// only account on such a machine gets hidden by a setting about domains.

TEST(UserEnumeration, AMicrosoftAccountIsClassifiedAsOne) {
    CHECK_EQ(static_cast<int>(
                 ClassifyAccountDomain(L"MicrosoftAccount", L"WINBOX")),
             static_cast<int>(AccountSource::MicrosoftAccount));
    // The work-account spelling of the same idea.
    CHECK_EQ(static_cast<int>(ClassifyAccountDomain(L"AzureAD", L"WINBOX")),
             static_cast<int>(AccountSource::MicrosoftAccount));
    // Case is not significant anywhere else in a Windows account name either.
    CHECK_EQ(static_cast<int>(
                 ClassifyAccountDomain(L"microsoftaccount", L"WINBOX")),
             static_cast<int>(AccountSource::MicrosoftAccount));
}

TEST(UserEnumeration, TheMachineNameAndAnEmptyDomainAreBothLocal) {
    CHECK_EQ(static_cast<int>(ClassifyAccountDomain(L"WINBOX", L"WINBOX")),
             static_cast<int>(AccountSource::Local));
    CHECK_EQ(static_cast<int>(ClassifyAccountDomain(L"winbox", L"WINBOX")),
             static_cast<int>(AccountSource::Local));
    CHECK_EQ(static_cast<int>(ClassifyAccountDomain(L"", L"WINBOX")),
             static_cast<int>(AccountSource::Local));
}

TEST(UserEnumeration, AnythingElseIsADomainAccount) {
    CHECK_EQ(static_cast<int>(ClassifyAccountDomain(L"CONTOSO", L"WINBOX")),
             static_cast<int>(AccountSource::Domain));
}

// The switch about domain accounts must not reach a Microsoft account. On a
// machine with nothing else it would leave a sign-in screen with no tiles.
TEST(UserEnumeration, TurningOffDomainAccountsKeepsMicrosoftAccounts) {
    UserAccount msa;
    msa.username = L"someone@example.com";
    msa.displayName = L"Someone";
    msa.domain = L"MicrosoftAccount";
    msa.sid = L"S-1-5-21-1-2-3-1001";
    msa.source = AccountSource::MicrosoftAccount;

    UserDirectoryOptions options;
    options.showDomainAccounts = false;

    const auto visible = UserDirectory::ApplyVisibilityRules({msa}, {}, options);
    REQUIRE_EQ(visible.size(), size_t(1));
    CHECK_EQ(visible[0].username, std::wstring(L"someone@example.com"));
}

// And the switch that IS about them works, which it could not before: nothing
// ever set the source, so every Microsoft account looked like a domain one.
TEST(UserEnumeration, MicrosoftAccountsCanBeHiddenOnTheirOwn) {
    UserAccount msa;
    msa.username = L"someone@example.com";
    msa.domain = L"MicrosoftAccount";
    msa.sid = L"S-1-5-21-1-2-3-1001";
    msa.source = AccountSource::MicrosoftAccount;

    UserAccount local;
    local.username = L"Bill";
    local.domain = L"WINBOX";
    local.sid = L"S-1-5-21-1-2-3-1002";
    local.source = AccountSource::Local;

    UserDirectoryOptions options;
    options.showMicrosoftAccounts = false;

    const auto visible =
        UserDirectory::ApplyVisibilityRules({msa, local}, {}, options);
    REQUIRE_EQ(visible.size(), size_t(1));
    CHECK_EQ(visible[0].username, std::wstring(L"Bill"));
}

// The qualified name is what LSA is handed, and it has to survive the trip
// unchanged: "MicrosoftAccount\someone@example.com" is the whole identity, and
// the email half on its own authenticates nothing.
TEST(UserEnumeration, AMicrosoftAccountKeepsItsQualifiedName) {
    UserAccount msa;
    msa.username = L"someone@example.com";
    msa.domain = L"MicrosoftAccount";
    CHECK_EQ(msa.QualifiedName(),
             std::wstring(L"MicrosoftAccount\\someone@example.com"));
}
