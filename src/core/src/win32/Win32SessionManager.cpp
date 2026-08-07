// XPLogin10 - Terminal Services session handling.
//
// Enumerating sessions and reconnecting a disconnected one. Nothing here
// starts a process: Winlogon creates the session, loads the profile and
// launches whatever HKLM\...\Winlogon\Shell names, and anything of ours that
// tried to help with that only ever raced it and won.
#include "Win32Common.h"

#include "xplogin/Interfaces.h"
#include "xplogin/Logging.h"
#include "xplogin/StringUtil.h"
#include "xplogin/Win32Factories.h"

#include <tlhelp32.h>

#include <vector>

namespace xplogin {
namespace {

SessionState TranslateState(WTS_CONNECTSTATE_CLASS state) {
    switch (state) {
        case WTSActive: return SessionState::Active;
        case WTSConnected: return SessionState::Connected;
        case WTSDisconnected: return SessionState::Disconnected;
        case WTSIdle: return SessionState::Idle;
        case WTSListen: return SessionState::Listening;
        case WTSDown:
        case WTSReset:
        case WTSInit:
        case WTSShadow:
        case WTSConnectQuery:
            return SessionState::Down;
    }
    return SessionState::Unknown;
}

std::wstring QuerySessionString(DWORD sessionId, WTS_INFO_CLASS infoClass) {
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (!::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, infoClass,
                                       &buffer, &bytes)) {
        return std::wstring();
    }
    std::wstring value = buffer ? buffer : L"";
    ::WTSFreeMemory(buffer);
    return value;
}

// Looks for a process by name inside one session.
bool IsProcessRunningInSession(const wchar_t* imageName, DWORD sessionId) {
    win32::ScopedHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.Valid()) {
        return false;
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (!::Process32FirstW(snapshot.Get(), &entry)) {
        return false;
    }

    do {
        if (::_wcsicmp(entry.szExeFile, imageName) != 0) {
            continue;
        }
        DWORD processSession = 0;
        if (::ProcessIdToSessionId(entry.th32ProcessID, &processSession) &&
            processSession == sessionId) {
            return true;
        }
    } while (::Process32NextW(snapshot.Get(), &entry));

    return false;
}


class Win32SessionManager : public ISessionManager {
public:
    std::vector<SessionInfo> EnumerateSessions() override {
        std::vector<SessionInfo> sessions;

        win32::WtsBuffer<WTS_SESSION_INFOW> buffer;
        DWORD count = 0;
        if (!::WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1,
                                     buffer.Receive(), &count)) {
            XPLOG_ERROR("WTSEnumerateSessions failed: %lu",
                        static_cast<unsigned long>(::GetLastError()));
            return sessions;
        }

        const WTS_SESSION_INFOW* entries = buffer.Get();
        for (DWORD i = 0; i < count; ++i) {
            SessionInfo session;
            session.sessionId = entries[i].SessionId;
            session.state = TranslateState(entries[i].State);
            session.username = QuerySessionString(session.sessionId, WTSUserName);
            session.domain = QuerySessionString(session.sessionId, WTSDomainName);
            session.shellRunning =
                !session.username.empty() &&
                IsProcessRunningInSession(L"explorer.exe", session.sessionId);
            sessions.push_back(std::move(session));
        }
        return sessions;
    }

    uint32_t FindSessionForUser(const std::wstring& domain,
                                const std::wstring& username) override {
        if (username.empty()) {
            return 0;
        }
        for (const SessionInfo& session : EnumerateSessions()) {
            if (session.sessionId == 0 || session.username.empty()) {
                continue;
            }
            if (!EqualsNoCase(session.username, username)) {
                continue;
            }
            if (domain.empty() || EqualsNoCase(session.domain, domain)) {
                return session.sessionId;
            }
        }
        return 0;
    }

    bool ConnectSession(uint32_t sessionId) override {
        // Reconnecting another session to the console needs SeTcbPrivilege,
        // which LogonUI has and a normal process does not.
        if (!::WTSConnectSessionW(sessionId, WTS_CURRENT_SESSION,
                                  const_cast<LPWSTR>(L""), FALSE)) {
            XPLOG_INFO("WTSConnectSession(%u) failed: %lu",
                       static_cast<unsigned>(sessionId),
                       static_cast<unsigned long>(::GetLastError()));
            return false;
        }
        return true;
    }


    bool LockSession(uint32_t sessionId) override {
        if (sessionId == CurrentSessionId()) {
            return ::LockWorkStation() != FALSE;
        }
        return ::WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, sessionId, FALSE) !=
               FALSE;
    }

    uint32_t CurrentSessionId() override {
        DWORD sessionId = 0;
        if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &sessionId)) {
            return 0;
        }
        return sessionId;
    }
};

} // namespace

std::shared_ptr<ISessionManager> MakeWin32SessionManager() {
    return std::make_shared<Win32SessionManager>();
}

} // namespace xplogin
