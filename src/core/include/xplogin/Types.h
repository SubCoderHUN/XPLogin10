// XPLogin10 - core value types shared by every layer.
//
// Nothing in this header may include <windows.h>. The core is compiled on the
// build machine (Linux/macOS/Windows) so the logic can be unit tested without a
// logon session; the Win32 bindings live behind the interfaces in this folder.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace xplogin {

// Where an account came from. Drives the domain string we hand to LSA.
enum class AccountSource {
    Unknown = 0,
    Local,            // SAM account on this machine
    Domain,           // Active Directory account
    MicrosoftAccount, // AzureAD\ or MicrosoftAccount\ prefixed identity
};

// Mirrors CREDENTIAL_PROVIDER_USAGE_SCENARIO. Kept as our own enum so the core
// never has to see credentialprovider.h.
enum class UsageScenario {
    Invalid = 0,
    Logon,          // CPUS_LOGON            - welcome screen at boot / after logoff
    UnlockWorkstation, // CPUS_UNLOCK_WORKSTATION - Win+L / screensaver
    ChangePassword, // CPUS_CHANGE_PASSWORD
    CredUI,         // CPUS_CREDUI
};

// A single tile on the XP welcome screen.
struct UserAccount {
    std::wstring username;      // sAMAccountName, e.g. "Bill"
    std::wstring displayName;   // full name shown under the avatar
    std::wstring domain;        // machine name for local accounts, NetBIOS name for domain
    std::wstring sid;           // S-1-5-21-...  (empty when unknown)
    std::wstring avatarPath;    // absolute path to a 48x48 tile bitmap, may be empty
    AccountSource source = AccountSource::Unknown;

    bool disabled        = false;
    bool lockedOut       = false;
    bool passwordExpired = false;
    bool blankPassword   = false; // account has no password -> XP logs straight in
    bool hidden          = false; // excluded via SpecialAccounts\UserList

    // Status line under the name on the welcome screen ("2 programs running").
    int  programsRunning = 0;
    int  unreadMail      = 0;
    bool sessionLocked   = false; // has a disconnected/locked session waiting

    // "DOMAIN\user" as LSA wants it. Local accounts use the machine name.
    std::wstring QualifiedName() const {
        if (domain.empty()) {
            return username;
        }
        return domain + L"\\" + username;
    }

    bool CanLogOn() const { return !disabled && !lockedOut; }
};

// Outcome of an authentication attempt, decoupled from NTSTATUS.
enum class AuthStatus {
    Success = 0,
    Pending,            // handed to LSA, result arrives asynchronously
    BadPassword,
    UnknownUser,
    AccountDisabled,
    AccountLockedOut,
    PasswordExpired,
    PasswordMustChange,
    LogonTypeNotGranted,
    TimeRestriction,    // logon hours violation
    Cancelled,
    InternalError,
};

struct AuthResult {
    AuthStatus   status = AuthStatus::InternalError;
    int32_t      ntStatus = 0;      // raw NTSTATUS when it came from LSA
    int32_t      subStatus = 0;
    std::wstring message;           // localized text for the XP error balloon

    bool Ok() const { return status == AuthStatus::Success; }

    static AuthResult Success() {
        AuthResult r;
        r.status = AuthStatus::Success;
        return r;
    }
    static AuthResult Fail(AuthStatus s, std::wstring msg = L"") {
        AuthResult r;
        r.status = s;
        r.message = std::move(msg);
        return r;
    }
};

// Credentials as typed on the XP password box.
struct LogonRequest {
    std::wstring domain;
    std::wstring username;
    std::wstring password;
    UsageScenario scenario = UsageScenario::Logon;
};

// Terminal Services session state (subset of WTS_CONNECTSTATE_CLASS).
enum class SessionState {
    Unknown = 0,
    Active,
    Connected,
    Disconnected,
    Idle,
    Listening,
    Down,
};

struct SessionInfo {
    uint32_t     sessionId = 0;
    std::wstring username;
    std::wstring domain;
    SessionState state = SessionState::Unknown;
    bool         locked = false;
    bool         shellRunning = false;
};

// What the user picked in the "Turn off computer" dialog.
enum class PowerAction {
    None = 0,
    StandBy,
    TurnOff,
    Restart,
    Hibernate, // shown instead of Stand By when Shift is held, exactly like XP
};

} // namespace xplogin
