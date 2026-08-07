// XPLogin10 - in-memory stand-ins for every OS service the logon flow uses.
//
// These are the machine the tests log in to: a fake SAM database, a fake LSA
// that knows the passwords, a fake Terminal Services session table and a fake
// registry. They are deliberately strict - the fake LSA enforces lockout and
// records every call - so an integration test failing here means the real thing
// would have failed too.
#pragma once

#include "xplogin/Interfaces.h"
#include "xplogin/LogonController.h"
#include "xplogin/StringUtil.h"
#include "xplogin/Types.h"

#include <algorithm>
#include <map>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace xplogin::testing {

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

class FakeClock : public IClock {
public:
    uint64_t NowMs() const override { return now_; }
    void Advance(uint64_t ms) { now_ += ms; }
    void SetNow(uint64_t ms) { now_ = ms; }

private:
    uint64_t now_ = 1'000'000;
};

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------

class FakeStateStore : public IStateStore {
public:
    bool ReadU64(const std::string& key, uint64_t* value) const override {
        auto it = numbers_.find(key);
        if (it == numbers_.end()) {
            return false;
        }
        if (value) *value = it->second;
        return true;
    }
    bool WriteU64(const std::string& key, uint64_t value) override {
        numbers_[key] = value;
        ++writes;
        return true;
    }
    bool ReadString(const std::string& key, std::wstring* value) const override {
        auto it = strings_.find(key);
        if (it == strings_.end()) {
            return false;
        }
        if (value) *value = it->second;
        return true;
    }
    bool WriteString(const std::string& key, const std::wstring& value) override {
        strings_[key] = value;
        ++writes;
        return true;
    }
    bool Remove(const std::string& key) override {
        return numbers_.erase(key) + strings_.erase(key) > 0;
    }

    int writes = 0;

private:
    std::map<std::string, uint64_t> numbers_;
    std::map<std::string, std::wstring> strings_;
};

// ---------------------------------------------------------------------------
// The fake account database, shared by the enumerator and the authenticator.
// ---------------------------------------------------------------------------

struct FakeAccount {
    UserAccount  account;
    std::wstring password;
    int          badPasswordCount = 0;
};

class FakeAccountDatabase {
public:
    explicit FakeAccountDatabase(std::wstring machineName = L"WINBOX")
        : machineName_(std::move(machineName)) {}

    FakeAccount& Add(const std::wstring& username, const std::wstring& password,
                     AccountSource source = AccountSource::Local) {
        FakeAccount entry;
        entry.account.username = username;
        entry.account.displayName = username;
        entry.account.domain =
            (source == AccountSource::Domain) ? domainName_ : machineName_;
        entry.account.sid = L"S-1-5-21-1004336348-1177238915-682003330-" +
                            std::to_wstring(1000 + static_cast<int>(accounts_.size()));
        entry.account.source = source;
        entry.account.blankPassword = password.empty();
        entry.password = password;
        accounts_.push_back(entry);
        return accounts_.back();
    }

    FakeAccount* Find(const std::wstring& domain, const std::wstring& username) {
        for (FakeAccount& entry : accounts_) {
            if (!EqualsNoCase(entry.account.username, username)) {
                continue;
            }
            if (domain.empty() || EqualsNoCase(entry.account.domain, domain)) {
                return &entry;
            }
        }
        return nullptr;
    }

    std::vector<FakeAccount>& Accounts() { return accounts_; }
    const std::wstring& MachineName() const { return machineName_; }
    const std::wstring& DomainName() const { return domainName_; }
    void SetDomainName(std::wstring name) { domainName_ = std::move(name); }
    void SetDomainJoined(bool joined) { domainJoined_ = joined; }
    bool DomainJoined() const { return domainJoined_; }

    std::vector<std::wstring> hiddenNames;
    int lockoutThreshold = 0; // 0 disables lockout

private:
    std::vector<FakeAccount> accounts_;
    std::wstring machineName_;
    std::wstring domainName_ = L"CONTOSO";
    bool domainJoined_ = false;
};

// ---------------------------------------------------------------------------
// Enumerator
// ---------------------------------------------------------------------------

class FakeUserEnumerator : public IUserEnumerator {
public:
    explicit FakeUserEnumerator(std::shared_ptr<FakeAccountDatabase> db)
        : db_(std::move(db)) {}

    std::vector<UserAccount> EnumerateLocalUsers() override {
        ++localQueryCount;
        std::vector<UserAccount> out;
        for (const FakeAccount& entry : db_->Accounts()) {
            if (entry.account.source != AccountSource::Domain) {
                out.push_back(entry.account);
            }
        }
        return out;
    }

    std::vector<UserAccount> EnumerateDomainUsers() override {
        ++domainQueryCount;
        std::vector<UserAccount> out;
        for (const FakeAccount& entry : db_->Accounts()) {
            if (entry.account.source == AccountSource::Domain) {
                out.push_back(entry.account);
            }
        }
        return out;
    }

    std::vector<std::wstring> HiddenAccountNames() override {
        return db_->hiddenNames;
    }

    std::wstring MachineName() override { return db_->MachineName(); }
    bool IsDomainJoined() override { return db_->DomainJoined(); }

    int localQueryCount = 0;
    int domainQueryCount = 0;

private:
    std::shared_ptr<FakeAccountDatabase> db_;
};

// ---------------------------------------------------------------------------
// Authenticator - a stand-in for LSA that actually checks the password.
// ---------------------------------------------------------------------------

class FakeAuthenticator : public IAuthenticator {
public:
    explicit FakeAuthenticator(std::shared_ptr<FakeAccountDatabase> db)
        : db_(std::move(db)) {}

    AuthResult Authenticate(const LogonRequest& request) override {
        ++attempts;
        lastRequest = request;

        if (asynchronous) {
            pending_ = request;
            hasPending_ = true;
            AuthResult result;
            result.status = AuthStatus::Pending;
            return result;
        }
        return Evaluate(request);
    }

    bool IsAsynchronous() const override { return asynchronous; }

    void Cancel() override {
        ++cancels;
        hasPending_ = false;
    }

    // For the asynchronous flow: resolve whatever is outstanding, the way
    // LogonUI's ReportResult callback eventually does.
    AuthResult ResolvePending() {
        if (!hasPending_) {
            return AuthResult::Fail(AuthStatus::InternalError);
        }
        hasPending_ = false;
        return Evaluate(pending_);
    }
    bool HasPending() const { return hasPending_; }

    AuthResult Evaluate(const LogonRequest& request) {
        FakeAccount* entry = db_->Find(request.domain, request.username);
        if (!entry) {
            return AuthResult::Fail(AuthStatus::UnknownUser);
        }
        if (entry->account.lockedOut) {
            return AuthResult::Fail(AuthStatus::AccountLockedOut);
        }
        if (entry->account.disabled) {
            return AuthResult::Fail(AuthStatus::AccountDisabled);
        }
        if (entry->password != request.password) {
            ++entry->badPasswordCount;
            if (db_->lockoutThreshold > 0 &&
                entry->badPasswordCount >= db_->lockoutThreshold) {
                entry->account.lockedOut = true;
                return AuthResult::Fail(AuthStatus::AccountLockedOut);
            }
            return AuthResult::Fail(AuthStatus::BadPassword);
        }
        if (entry->account.passwordExpired) {
            return AuthResult::Fail(AuthStatus::PasswordExpired);
        }
        entry->badPasswordCount = 0;
        return AuthResult::Success();
    }

    bool asynchronous = false;
    int attempts = 0;
    int cancels = 0;
    LogonRequest lastRequest;

private:
    std::shared_ptr<FakeAccountDatabase> db_;
    LogonRequest pending_;
    bool hasPending_ = false;
};

// ---------------------------------------------------------------------------
// Sessions
// ---------------------------------------------------------------------------

class FakeSessionManager : public ISessionManager {
public:
    FakeSessionManager() {
        // Session 0 is the services session; the logon screen lives in 1.
        SessionInfo services;
        services.sessionId = 0;
        services.state = SessionState::Disconnected;
        sessions_.push_back(services);

        SessionInfo console;
        console.sessionId = 1;
        console.state = SessionState::Connected;
        sessions_.push_back(console);
    }

    std::vector<SessionInfo> EnumerateSessions() override {
        ++enumerateCount;
        return sessions_;
    }

    uint32_t FindSessionForUser(const std::wstring& domain,
                                const std::wstring& username) override {
        for (const SessionInfo& session : sessions_) {
            if (session.sessionId == 0 || session.username.empty()) {
                continue;
            }
            if (EqualsNoCase(session.username, username) &&
                (domain.empty() || EqualsNoCase(session.domain, domain))) {
                return session.sessionId;
            }
        }
        return 0;
    }

    bool ConnectSession(uint32_t sessionId) override {
        SessionInfo* session = Get(sessionId);
        if (!session) {
            return false;
        }
        // Reconnecting is what fast user switching does: the other active
        // console session goes to Disconnected first.
        for (SessionInfo& other : sessions_) {
            if (other.sessionId != sessionId && other.state == SessionState::Active) {
                other.state = SessionState::Disconnected;
            }
        }
        session->state = SessionState::Active;
        session->locked = false;
        ++connectCount;
        return true;
    }

    // No EnsureShellRunning: ISessionManager has no such operation any more.
    // `shellRunning` stays on SessionInfo because the real implementation
    // reports it in EnumerateSessions - reading whether the shell is up is
    // fine, it is starting one that never was.

    bool LockSession(uint32_t sessionId) override {
        SessionInfo* session = Get(sessionId);
        if (!session) {
            return false;
        }
        session->locked = true;
        ++lockCount;
        return true;
    }

    uint32_t CurrentSessionId() override { return currentSessionId; }

    // ---- test helpers ------------------------------------------------------

    uint32_t CreateUserSession(const std::wstring& domain,
                               const std::wstring& username) {
        SessionInfo session;
        session.sessionId = nextSessionId_++;
        session.domain = domain;
        session.username = username;
        session.state = SessionState::Active;
        sessions_.push_back(session);
        return session.sessionId;
    }

    // Binds the console session to an account, which is what Winlogon does the
    // moment LSA hands back a token.
    void AssignCurrentSession(const std::wstring& domain,
                              const std::wstring& username) {
        SessionInfo* session = Get(currentSessionId);
        if (session) {
            session->domain = domain;
            session->username = username;
            session->state = SessionState::Active;
        }
    }

    SessionInfo* Get(uint32_t sessionId) {
        for (SessionInfo& session : sessions_) {
            if (session.sessionId == sessionId) {
                return &session;
            }
        }
        return nullptr;
    }

    void LogOff(uint32_t sessionId) {
        sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                       [&](const SessionInfo& s) {
                                           return s.sessionId == sessionId;
                                       }),
                        sessions_.end());
    }

    uint32_t currentSessionId = 1;
    int enumerateCount = 0;
    int connectCount = 0;
    int lockCount = 0;

private:
    std::vector<SessionInfo> sessions_;
    uint32_t nextSessionId_ = 2;
};

// ---------------------------------------------------------------------------
// Power
// ---------------------------------------------------------------------------

class FakePowerController : public IPowerController {
public:
    bool IsActionAvailable(PowerAction action) override {
        if (action == PowerAction::Hibernate) {
            return hibernateSupported;
        }
        if (action == PowerAction::StandBy) {
            return standBySupported;
        }
        return shutdownAllowed;
    }

    bool Execute(PowerAction action) override {
        if (!IsActionAvailable(action)) {
            return false;
        }
        executed.push_back(action);
        if (onExecute) {
            onExecute();
        }
        return true;
    }

    bool shutdownAllowed = true;
    bool standBySupported = true;
    bool hibernateSupported = false;
    std::vector<PowerAction> executed;
    // Stands in for the machine going away underneath the caller. Everything
    // that has to be on screen by the time Windows is told has to have happened
    // before this runs.
    std::function<void()> onExecute;
};

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------

class FakeSoundPlayer : public ISoundPlayer {
public:
    bool Play(SoundEvent event) override {
        played.push_back(event);
        return true;
    }
    void StopAll() override { ++stopCount; }

    int CountOf(SoundEvent event) const {
        int count = 0;
        for (SoundEvent played_event : played) {
            if (played_event == event) {
                ++count;
            }
        }
        return count;
    }

    std::vector<SoundEvent> played;
    int stopCount = 0;
};

// ---------------------------------------------------------------------------
// System Restore
// ---------------------------------------------------------------------------

class FakeSystemRestore : public ISystemRestore {
public:
    RestorePointResult CreateCheckpoint(const std::wstring& description,
                                        int64_t* sequenceNumber) override {
        ++attempts;
        lastDescription = description;

        if (!available) {
            return RestorePointResult::NotSupported;
        }
        if (!protectionEnabled) {
            return RestorePointResult::Disabled;
        }
        // Windows skips a checkpoint if one was made recently, unless the rate
        // limit has been suspended.
        if (recentCheckpointExists && !rateLimitSuspended) {
            return RestorePointResult::RateLimited;
        }

        recentCheckpointExists = true;
        if (sequenceNumber) {
            *sequenceNumber = nextSequenceNumber;
        }
        created.push_back(nextSequenceNumber++);
        return RestorePointResult::Created;
    }

    bool SuspendRateLimit() override {
        if (!available) {
            return false;
        }
        rateLimitSuspended = true;
        ++suspendCalls;
        return true;
    }

    void RestoreRateLimit() override {
        if (rateLimitSuspended) {
            ++restoreCalls;
        }
        rateLimitSuspended = false;
    }

    bool IsAvailable() override { return available; }

    bool available = true;
    bool protectionEnabled = true;
    bool recentCheckpointExists = false;
    bool rateLimitSuspended = false;
    int  attempts = 0;
    int  suspendCalls = 0;
    int  restoreCalls = 0;
    int64_t nextSequenceNumber = 42;
    std::wstring lastDescription;
    std::vector<int64_t> created;
};

// ---------------------------------------------------------------------------
// Observer - records the callback stream so tests can assert on ordering.
// ---------------------------------------------------------------------------

class RecordingObserver : public IControllerObserver {
public:
    void OnStateChanged(UiState from, UiState to) override {
        transitions.push_back({from, to});
    }
    void OnUsersChanged() override { ++usersChangedCount; }
    void OnAuthError(const AuthResult& result) override {
        authErrors.push_back(result.status);
    }
    void OnLogonComplete(const UserAccount& user, uint32_t sessionId) override {
        ++logonCompleteCount;
        logonUser = user.username;
        logonSessionId = sessionId;
    }
    void OnPowerAction(PowerAction action) override { powerActions.push_back(action); }

    bool SawTransitionTo(UiState state) const {
        for (const auto& transition : transitions) {
            if (transition.second == state) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::pair<UiState, UiState>> transitions;
    std::vector<AuthStatus> authErrors;
    std::vector<PowerAction> powerActions;
    int usersChangedCount = 0;
    int logonCompleteCount = 0;
    std::wstring logonUser;
    uint32_t logonSessionId = 0;
};

} // namespace xplogin::testing
