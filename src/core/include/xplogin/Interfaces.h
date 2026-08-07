// XPLogin10 - the seams between portable logic and the operating system.
//
// Every OS touch point the logon flow needs is expressed here as a pure virtual
// interface. Production code binds the Win32 implementations from
// src/core/src/win32/; the test suite binds the fakes from tests/mocks/.
#pragma once

#include "xplogin/Types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace xplogin {

// ---------------------------------------------------------------------------
// Authentication
// ---------------------------------------------------------------------------

// Two very different implementations sit behind this:
//
//  * DeferredLsaAuthenticator (production credential-provider path) packs a
//    KERB_INTERACTIVE_UNLOCK_LOGON, hands it to LogonUI and returns Pending.
//    Winlogon/LSA performs the real authentication, creates the session and
//    starts the shell; the answer comes back through ReportResult().
//
//  * Win32Authenticator (standalone/preview path and pre-flight checks) calls
//    LogonUserW directly.
class IAuthenticator {
public:
    virtual ~IAuthenticator() = default;

    // Synchronous or Pending. Never blocks longer than a few seconds.
    virtual AuthResult Authenticate(const LogonRequest& request) = 0;

    // True when the result arrives later through LogonController::OnAuthReported.
    virtual bool IsAsynchronous() const = 0;

    // Abort an in-flight attempt (user pressed Escape).
    virtual void Cancel() {}
};

// ---------------------------------------------------------------------------
// Account enumeration
// ---------------------------------------------------------------------------

class IUserEnumerator {
public:
    virtual ~IUserEnumerator() = default;

    // Local SAM accounts (NetUserEnum + LookupAccountName).
    virtual std::vector<UserAccount> EnumerateLocalUsers() = 0;

    // Domain accounts that have logged on to this machine before. Returns an
    // empty list on a workgroup machine.
    virtual std::vector<UserAccount> EnumerateDomainUsers() = 0;

    // Names listed under SpecialAccounts\UserList with value 0 - XP hid these.
    virtual std::vector<std::wstring> HiddenAccountNames() = 0;

    // The machine's NetBIOS name, used as the domain for local accounts.
    virtual std::wstring MachineName() = 0;

    // True when the machine is joined to a domain (welcome screen is normally
    // disabled there, but we still support it).
    virtual bool IsDomainJoined() = 0;
};

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

class ISessionManager {
public:
    virtual ~ISessionManager() = default;

    virtual std::vector<SessionInfo> EnumerateSessions() = 0;

    // Session already owned by this account, or 0 when there is none.
    virtual uint32_t FindSessionForUser(const std::wstring& domain,
                                        const std::wstring& username) = 0;

    // Reconnect a disconnected session (fast user switching).
    virtual bool ConnectSession(uint32_t sessionId) = 0;

    // There is deliberately no "start the shell" operation here.
    //
    // There was, as a fallback for the case where Winlogon somehow had not
    // started explorer.exe. That case does not exist - Winlogon always starts
    // the shell - and the fallback could only ever do harm, because the moment
    // it could run is a moment before Winlogon has got there. It checked first
    // and the check was always false, so it won the race and started a second
    // explorer.exe with no arguments, which is a My Computer window opening on
    // top of the user's desktop at every single sign-in.
    //
    virtual bool LockSession(uint32_t sessionId) = 0;

    virtual uint32_t CurrentSessionId() = 0;
};

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------

class IPowerController {
public:
    virtual ~IPowerController() = default;

    virtual bool IsActionAvailable(PowerAction action) = 0;
    virtual bool Execute(PowerAction action) = 0;
};

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------

enum class SoundEvent {
    Logon,      // "Windows XP Startup" / logon sound
    Logoff,
    Error,      // wrong password ding
    Click,      // tile click
    Shutdown,
};

class ISoundPlayer {
public:
    virtual ~ISoundPlayer() = default;
    virtual bool Play(SoundEvent event) = 0;
    virtual void StopAll() {}
};

// ---------------------------------------------------------------------------
// Clock (injected so animation and lockout timing are deterministic in tests)
// ---------------------------------------------------------------------------

class IClock {
public:
    virtual ~IClock() = default;
    virtual uint64_t NowMs() const = 0;
};

// ---------------------------------------------------------------------------
// System Restore
// ---------------------------------------------------------------------------

enum class RestorePointResult {
    Created = 0,
    Disabled,        // System Protection is turned off for the system drive
    RateLimited,     // Windows made one in the last 24 hours and skipped this
    NotSupported,    // Server SKU: SrClient.dll is not present
    Failed,
};

class ISystemRestore {
public:
    virtual ~ISystemRestore() = default;

    // Takes a checkpoint before an application install. `sequenceNumber` is
    // filled in on success so the caller can name it in its output.
    virtual RestorePointResult CreateCheckpoint(const std::wstring& description,
                                                int64_t* sequenceNumber) = 0;

    // Windows skips a checkpoint if one was made recently. These two clear that
    // rate limit and put the original setting back; the caller must pair them.
    virtual bool SuspendRateLimit() { return false; }
    virtual void RestoreRateLimit() {}

    virtual bool IsAvailable() = 0;
};

// ---------------------------------------------------------------------------
// Persistent key/value store used by the health monitor. Backed by
// HKLM\SOFTWARE\XPLogin10\Health on Windows, by a map in tests.
// ---------------------------------------------------------------------------

class IStateStore {
public:
    virtual ~IStateStore() = default;
    virtual bool ReadU64(const std::string& key, uint64_t* value) const = 0;
    virtual bool WriteU64(const std::string& key, uint64_t value) = 0;
    virtual bool ReadString(const std::string& key, std::wstring* value) const = 0;
    virtual bool WriteString(const std::string& key, const std::wstring& value) = 0;
    virtual bool Remove(const std::string& key) = 0;
};

} // namespace xplogin
