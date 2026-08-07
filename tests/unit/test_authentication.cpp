// Unit tests: authentication logic.
//
// Covers the required cases directly - valid password, invalid password,
// non-existent user - plus the NTSTATUS mapping that decides which XP error
// message the user actually sees.
#include "xplogin/AuthStatusMapping.h"
#include "xplogin/StringUtil.h"
#include "Mocks.h"
#include "xptest.h"

#include <memory>

using namespace xplogin;
using namespace xplogin::testing;

namespace {

std::shared_ptr<FakeAccountDatabase> MakeDatabase() {
    auto db = std::make_shared<FakeAccountDatabase>(L"WINBOX");
    db->Add(L"Bill", L"hunter2");
    db->Add(L"Alice", L"Correct Horse Battery Staple");
    db->Add(L"Kiosk", L""); // blank password, like XP's default account
    return db;
}

LogonRequest MakeRequest(const wchar_t* user, const wchar_t* password,
                         const wchar_t* domain = L"WINBOX") {
    LogonRequest request;
    request.domain = domain;
    request.username = user;
    request.password = password;
    request.scenario = UsageScenario::Logon;
    return request;
}

} // namespace

// ---------------------------------------------------------------------------
// The three required cases
// ---------------------------------------------------------------------------

TEST(Authentication, AcceptsCorrectPassword) {
    FakeAuthenticator auth(MakeDatabase());
    const AuthResult result = auth.Authenticate(MakeRequest(L"Bill", L"hunter2"));

    CHECK(result.Ok());
    CHECK_EQ(static_cast<int>(result.status), static_cast<int>(AuthStatus::Success));
    CHECK_EQ(auth.attempts, 1);
}

TEST(Authentication, RejectsWrongPassword) {
    FakeAuthenticator auth(MakeDatabase());
    const AuthResult result = auth.Authenticate(MakeRequest(L"Bill", L"hunter3"));

    CHECK_FALSE(result.Ok());
    CHECK_EQ(static_cast<int>(result.status),
             static_cast<int>(AuthStatus::BadPassword));
}

TEST(Authentication, RejectsNonExistentUser) {
    FakeAuthenticator auth(MakeDatabase());
    const AuthResult result = auth.Authenticate(MakeRequest(L"Clippy", L"anything"));

    CHECK_FALSE(result.Ok());
    CHECK_EQ(static_cast<int>(result.status),
             static_cast<int>(AuthStatus::UnknownUser));
}

TEST(Authentication, PasswordComparisonIsCaseSensitive) {
    FakeAuthenticator auth(MakeDatabase());
    CHECK_FALSE(auth.Authenticate(MakeRequest(L"Bill", L"HUNTER2")).Ok());
    CHECK(auth.Authenticate(MakeRequest(L"Bill", L"hunter2")).Ok());
}

TEST(Authentication, UserNameComparisonIsCaseInsensitive) {
    FakeAuthenticator auth(MakeDatabase());
    // Windows account names are case preserving but not case sensitive.
    CHECK(auth.Authenticate(MakeRequest(L"bill", L"hunter2")).Ok());
    CHECK(auth.Authenticate(MakeRequest(L"BILL", L"hunter2")).Ok());
}

TEST(Authentication, AcceptsBlankPasswordForBlankPasswordAccount) {
    FakeAuthenticator auth(MakeDatabase());
    CHECK(auth.Authenticate(MakeRequest(L"Kiosk", L"")).Ok());
    CHECK_FALSE(auth.Authenticate(MakeRequest(L"Kiosk", L"anything")).Ok());
}

TEST(Authentication, RejectsWrongDomain) {
    FakeAuthenticator auth(MakeDatabase());
    const AuthResult result =
        auth.Authenticate(MakeRequest(L"Bill", L"hunter2", L"OTHERBOX"));
    CHECK_EQ(static_cast<int>(result.status),
             static_cast<int>(AuthStatus::UnknownUser));
}

TEST(Authentication, ReportsDisabledAccountBeforeCheckingPassword) {
    auto db = MakeDatabase();
    db->Find(L"", L"Alice")->account.disabled = true;

    FakeAuthenticator auth(db);
    // Even with the right password: the account state wins, and the wrong
    // password must not be counted against a disabled account.
    const AuthResult result =
        auth.Authenticate(MakeRequest(L"Alice", L"Correct Horse Battery Staple"));
    CHECK_EQ(static_cast<int>(result.status),
             static_cast<int>(AuthStatus::AccountDisabled));
}

TEST(Authentication, ReportsLockedOutAccount) {
    auto db = MakeDatabase();
    db->Find(L"", L"Bill")->account.lockedOut = true;

    FakeAuthenticator auth(db);
    CHECK_EQ(static_cast<int>(auth.Authenticate(MakeRequest(L"Bill", L"hunter2")).status),
             static_cast<int>(AuthStatus::AccountLockedOut));
}

TEST(Authentication, ReportsExpiredPassword) {
    auto db = MakeDatabase();
    db->Find(L"", L"Bill")->account.passwordExpired = true;

    FakeAuthenticator auth(db);
    CHECK_EQ(static_cast<int>(auth.Authenticate(MakeRequest(L"Bill", L"hunter2")).status),
             static_cast<int>(AuthStatus::PasswordExpired));
}

TEST(Authentication, LocksOutAfterRepeatedBadPasswords) {
    auto db = MakeDatabase();
    db->lockoutThreshold = 3;
    FakeAuthenticator auth(db);

    CHECK_EQ(static_cast<int>(auth.Authenticate(MakeRequest(L"Bill", L"x")).status),
             static_cast<int>(AuthStatus::BadPassword));
    CHECK_EQ(static_cast<int>(auth.Authenticate(MakeRequest(L"Bill", L"x")).status),
             static_cast<int>(AuthStatus::BadPassword));
    CHECK_EQ(static_cast<int>(auth.Authenticate(MakeRequest(L"Bill", L"x")).status),
             static_cast<int>(AuthStatus::AccountLockedOut));

    // Once locked, even the correct password is refused.
    CHECK_EQ(static_cast<int>(
                 auth.Authenticate(MakeRequest(L"Bill", L"hunter2")).status),
             static_cast<int>(AuthStatus::AccountLockedOut));
}

TEST(Authentication, SuccessfulLogonResetsBadPasswordCount) {
    auto db = MakeDatabase();
    db->lockoutThreshold = 3;
    FakeAuthenticator auth(db);

    auth.Authenticate(MakeRequest(L"Bill", L"x"));
    auth.Authenticate(MakeRequest(L"Bill", L"x"));
    CHECK(auth.Authenticate(MakeRequest(L"Bill", L"hunter2")).Ok());

    // The counter is back to zero, so two more misses still do not lock out.
    CHECK_EQ(static_cast<int>(auth.Authenticate(MakeRequest(L"Bill", L"x")).status),
             static_cast<int>(AuthStatus::BadPassword));
    CHECK_EQ(static_cast<int>(auth.Authenticate(MakeRequest(L"Bill", L"x")).status),
             static_cast<int>(AuthStatus::BadPassword));
}

// ---------------------------------------------------------------------------
// Asynchronous (credential provider) flow
// ---------------------------------------------------------------------------

TEST(Authentication, AsynchronousModeDefersTheAnswer) {
    FakeAuthenticator auth(MakeDatabase());
    auth.asynchronous = true;

    const AuthResult immediate = auth.Authenticate(MakeRequest(L"Bill", L"hunter2"));
    CHECK_EQ(static_cast<int>(immediate.status), static_cast<int>(AuthStatus::Pending));
    CHECK(auth.HasPending());

    const AuthResult resolved = auth.ResolvePending();
    CHECK(resolved.Ok());
    CHECK_FALSE(auth.HasPending());
}

TEST(Authentication, CancelDropsThePendingAttempt) {
    FakeAuthenticator auth(MakeDatabase());
    auth.asynchronous = true;

    auth.Authenticate(MakeRequest(L"Bill", L"hunter2"));
    auth.Cancel();
    CHECK_FALSE(auth.HasPending());
    CHECK_EQ(auth.cancels, 1);
}

// ---------------------------------------------------------------------------
// NTSTATUS mapping
// ---------------------------------------------------------------------------

TEST(AuthStatusMapping, MapsCommonNtStatusValues) {
    CHECK_EQ(static_cast<int>(AuthResultFromNtStatus(ntstatus::kSuccess).status),
             static_cast<int>(AuthStatus::Success));
    CHECK_EQ(static_cast<int>(AuthResultFromNtStatus(ntstatus::kWrongPassword).status),
             static_cast<int>(AuthStatus::BadPassword));
    CHECK_EQ(static_cast<int>(AuthResultFromNtStatus(ntstatus::kNoSuchUser).status),
             static_cast<int>(AuthStatus::UnknownUser));
    CHECK_EQ(static_cast<int>(AuthResultFromNtStatus(ntstatus::kAccountDisabled).status),
             static_cast<int>(AuthStatus::AccountDisabled));
    CHECK_EQ(
        static_cast<int>(AuthResultFromNtStatus(ntstatus::kAccountLockedOut).status),
        static_cast<int>(AuthStatus::AccountLockedOut));
    CHECK_EQ(static_cast<int>(AuthResultFromNtStatus(ntstatus::kPasswordExpired).status),
             static_cast<int>(AuthStatus::PasswordExpired));
    CHECK_EQ(
        static_cast<int>(AuthResultFromNtStatus(ntstatus::kInvalidLogonHours).status),
        static_cast<int>(AuthStatus::TimeRestriction));
}

TEST(AuthStatusMapping, CollapsesLogonFailureToBadPasswordWithoutSubstatus) {
    // LSA hides whether it was the user name or the password that was wrong.
    // Showing "unknown user" here would be an account enumeration oracle.
    const AuthResult result = AuthResultFromNtStatus(ntstatus::kLogonFailure, 0);
    CHECK_EQ(static_cast<int>(result.status), static_cast<int>(AuthStatus::BadPassword));
}

TEST(AuthStatusMapping, UsesSubstatusWhenLsaProvidesOne) {
    const AuthResult result =
        AuthResultFromNtStatus(ntstatus::kLogonFailure, ntstatus::kAccountDisabled);
    CHECK_EQ(static_cast<int>(result.status),
             static_cast<int>(AuthStatus::AccountDisabled));
    CHECK_EQ(result.ntStatus, ntstatus::kLogonFailure);
    CHECK_EQ(result.subStatus, ntstatus::kAccountDisabled);
}

TEST(AuthStatusMapping, UnknownNtStatusBecomesInternalError) {
    const AuthResult result =
        AuthResultFromNtStatus(static_cast<int32_t>(0xC0000999));
    CHECK_EQ(static_cast<int>(result.status),
             static_cast<int>(AuthStatus::InternalError));
    CHECK_EQ(result.ntStatus, static_cast<int32_t>(0xC0000999));
}

TEST(AuthStatusMapping, MapsWin32ErrorsFromLogonUser) {
    CHECK_EQ(static_cast<int>(AuthResultFromWin32Error(0).status),
             static_cast<int>(AuthStatus::Success));
    CHECK_EQ(static_cast<int>(AuthResultFromWin32Error(1326).status),
             static_cast<int>(AuthStatus::BadPassword));
    CHECK_EQ(static_cast<int>(AuthResultFromWin32Error(1331).status),
             static_cast<int>(AuthStatus::AccountDisabled));
    CHECK_EQ(static_cast<int>(AuthResultFromWin32Error(1909).status),
             static_cast<int>(AuthStatus::AccountLockedOut));
    CHECK_EQ(static_cast<int>(AuthResultFromWin32Error(1385).status),
             static_cast<int>(AuthStatus::LogonTypeNotGranted));
    CHECK_EQ(static_cast<int>(AuthResultFromWin32Error(1223).status),
             static_cast<int>(AuthStatus::Cancelled));
}

// ---------------------------------------------------------------------------
// Name handling, which is where a lot of real logon bugs live
// ---------------------------------------------------------------------------

TEST(Authentication, SplitsQualifiedNames) {
    std::wstring domain;
    std::wstring user;

    REQUIRE(SplitQualifiedName(L"CONTOSO\\alice", &domain, &user));
    CHECK_EQ(domain, std::wstring(L"CONTOSO"));
    CHECK_EQ(user, std::wstring(L"alice"));

    REQUIRE(SplitQualifiedName(L"bob", &domain, &user));
    CHECK_EQ(domain, std::wstring(L""));
    CHECK_EQ(user, std::wstring(L"bob"));

    REQUIRE(SplitQualifiedName(L"alice@contoso.com", &domain, &user));
    CHECK_EQ(domain, std::wstring(L""));
    CHECK_EQ(user, std::wstring(L"alice@contoso.com"));
}

TEST(Authentication, RejectsMalformedQualifiedNames) {
    std::wstring domain;
    std::wstring user;
    CHECK_FALSE(SplitQualifiedName(L"", &domain, &user));
    CHECK_FALSE(SplitQualifiedName(L"   ", &domain, &user));
    CHECK_FALSE(SplitQualifiedName(L"CONTOSO\\", &domain, &user));
    CHECK_FALSE(SplitQualifiedName(L"\\alice", &domain, &user));
    CHECK_FALSE(SplitQualifiedName(L"A\\B\\C", &domain, &user));
}

TEST(Authentication, QualifiedNameRoundTrip) {
    UserAccount account;
    account.username = L"alice";
    account.domain = L"CONTOSO";
    CHECK_EQ(account.QualifiedName(), std::wstring(L"CONTOSO\\alice"));

    UserAccount noDomain;
    noDomain.username = L"kiosk";
    CHECK_EQ(noDomain.QualifiedName(), std::wstring(L"kiosk"));
}

TEST(Authentication, PasswordBufferIsWipedOnClear) {
    std::wstring password = L"hunter2";
    const wchar_t* raw = password.data();
    SecureClear(password);
    CHECK(password.empty());
    // The characters themselves are gone, not just the length.
    CHECK_EQ(static_cast<int>(raw[0]), 0);
}
