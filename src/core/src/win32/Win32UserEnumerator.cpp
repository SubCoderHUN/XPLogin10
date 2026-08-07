// XPLogin10 - real account enumeration.
//
// Local accounts come from NetUserEnum on the local SAM. Domain accounts are
// deliberately NOT pulled from the directory - on a large domain that is
// hundreds of thousands of tiles and a blocking network call on the logon
// desktop. Instead we list the accounts that have actually signed in to this
// machine before, which is what a welcome screen is for and what XP effectively
// showed. That list lives under ProfileList in the registry.
#include "Win32Common.h"

#include "xplogin/Interfaces.h"
#include "xplogin/Logging.h"
#include "xplogin/StringUtil.h"
#include "xplogin/Win32Factories.h"

#include <shlwapi.h>

#include <vector>

namespace xplogin {
namespace {

const wchar_t* const kProfileListKey =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList";
const wchar_t* const kSpecialAccountsKey =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\\SpecialAccounts"
    L"\\UserList";

// Resolves an account name to its SID string, and tells us whether the account
// is local or came from a domain.
bool LookupAccount(const std::wstring& qualifiedName, std::wstring* sidText,
                   AccountSource* source) {
    DWORD sidSize = 0;
    DWORD domainSize = 0;
    SID_NAME_USE use = SidTypeUnknown;

    ::LookupAccountNameW(nullptr, qualifiedName.c_str(), nullptr, &sidSize, nullptr,
                         &domainSize, &use);
    if (sidSize == 0) {
        return false;
    }

    std::vector<BYTE> sid(sidSize);
    std::vector<wchar_t> domain(domainSize + 1, L'\0');
    if (!::LookupAccountNameW(nullptr, qualifiedName.c_str(), sid.data(), &sidSize,
                              domain.data(), &domainSize, &use)) {
        return false;
    }
    if (use != SidTypeUser) {
        return false;
    }

    if (sidText) {
        *sidText = win32::SidToString(sid.data());
    }
    if (source) {
        *source = AccountSource::Local;
    }
    return true;
}

// Turns "S-1-5-21-...-1001" back into DOMAIN\user.
bool ResolveSid(const std::wstring& sidText, std::wstring* domain,
                std::wstring* username, AccountSource* source) {
    PSID sid = nullptr;
    if (!::ConvertStringSidToSidW(sidText.c_str(), &sid)) {
        return false;
    }

    DWORD nameSize = 0;
    DWORD domainSize = 0;
    SID_NAME_USE use = SidTypeUnknown;
    ::LookupAccountSidW(nullptr, sid, nullptr, &nameSize, nullptr, &domainSize, &use);
    if (nameSize == 0) {
        ::LocalFree(sid);
        return false;
    }

    std::vector<wchar_t> nameBuffer(nameSize + 1, L'\0');
    std::vector<wchar_t> domainBuffer(domainSize + 1, L'\0');
    const BOOL ok = ::LookupAccountSidW(nullptr, sid, nameBuffer.data(), &nameSize,
                                        domainBuffer.data(), &domainSize, &use);
    ::LocalFree(sid);
    if (!ok || use != SidTypeUser) {
        return false;
    }

    if (username) *username = nameBuffer.data();
    if (domain) *domain = domainBuffer.data();

    if (source) {
        // AzureAD\ and MicrosoftAccount\ are how Windows 10 names cloud
        // identities; everything else with a non-machine domain is AD.
        const std::wstring domainName = domainBuffer.data();
        if (EqualsNoCase(domainName, L"AzureAD") ||
            EqualsNoCase(domainName, L"MicrosoftAccount")) {
            *source = AccountSource::MicrosoftAccount;
        } else {
            *source = AccountSource::Domain;
        }
    }
    return true;
}

std::wstring GetMachineNameW() {
    wchar_t buffer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = ARRAYSIZE(buffer);
    if (!::GetComputerNameW(buffer, &size)) {
        return std::wstring();
    }
    return std::wstring(buffer, size);
}

class Win32UserEnumerator : public IUserEnumerator {
public:
    std::vector<UserAccount> EnumerateLocalUsers() override {
        std::vector<UserAccount> accounts;

        DWORD entriesRead = 0;
        DWORD totalEntries = 0;
        // NetUserEnum's resume handle is LPDWORD, not the DWORD_PTR that
        // several other Net* enumerators take. On x64 the difference is 32
        // bits against 64 and the call does not compile at all.
        DWORD resumeHandle = 0;
        const std::wstring machine = MachineName();

        do {
            win32::NetApiBuffer<USER_INFO_20> buffer;
            const NET_API_STATUS status = ::NetUserEnum(
                nullptr, 20, FILTER_NORMAL_ACCOUNT,
                reinterpret_cast<LPBYTE*>(buffer.Receive()), MAX_PREFERRED_LENGTH,
                &entriesRead, &totalEntries, &resumeHandle);

            if (status != NERR_Success && status != ERROR_MORE_DATA) {
                XPLOG_ERROR("NetUserEnum failed: %lu", static_cast<unsigned long>(status));
                break;
            }

            const USER_INFO_20* entries = buffer.Get();
            for (DWORD i = 0; i < entriesRead; ++i) {
                UserAccount account;
                account.username = entries[i].usri20_name ? entries[i].usri20_name : L"";
                if (account.username.empty()) {
                    continue;
                }
                account.displayName = entries[i].usri20_full_name
                                          ? entries[i].usri20_full_name
                                          : account.username;
                if (account.displayName.empty()) {
                    account.displayName = account.username;
                }
                account.domain = machine;
                account.source = AccountSource::Local;

                const DWORD flags = entries[i].usri20_flags;
                account.disabled = (flags & UF_ACCOUNTDISABLE) != 0;
                account.lockedOut = (flags & UF_LOCKOUT) != 0;
                account.blankPassword = (flags & UF_PASSWD_NOTREQD) != 0;

                // UF_DONT_EXPIRE_PASSWD aside, LSA is the authority on
                // expiry; this only drives the tile's status line.
                AccountSource source = AccountSource::Local;
                LookupAccount(machine + L"\\" + account.username, &account.sid,
                              &source);

                accounts.push_back(std::move(account));
            }

            if (status != ERROR_MORE_DATA) {
                break;
            }
        } while (resumeHandle != 0);

        XPLOG_DEBUG("enumerated %d local accounts", static_cast<int>(accounts.size()));
        return accounts;
    }

    std::vector<UserAccount> EnumerateDomainUsers() override {
        std::vector<UserAccount> accounts;
        if (!IsDomainJoined()) {
            return accounts;
        }

        HKEY profileList = nullptr;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kProfileListKey, 0,
                            KEY_ENUMERATE_SUB_KEYS | KEY_WOW64_64KEY,
                            &profileList) != ERROR_SUCCESS) {
            return accounts;
        }

        const std::wstring machine = MachineName();
        for (DWORD index = 0;; ++index) {
            wchar_t sidText[256] = {};
            DWORD nameLength = ARRAYSIZE(sidText);
            const LONG status = ::RegEnumKeyExW(profileList, index, sidText,
                                                &nameLength, nullptr, nullptr,
                                                nullptr, nullptr);
            if (status == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (status != ERROR_SUCCESS) {
                continue;
            }

            // Well-known SIDs (SYSTEM, LOCAL SERVICE, ...) are short; real user
            // profiles are S-1-5-21-<3 subauthorities>-<rid>.
            if (::StrCmpNIW(sidText, L"S-1-5-21-", 9) != 0) {
                continue;
            }

            std::wstring domain;
            std::wstring username;
            AccountSource source = AccountSource::Domain;
            if (!ResolveSid(sidText, &domain, &username, &source)) {
                continue;
            }
            // Local accounts already came from NetUserEnum.
            if (EqualsNoCase(domain, machine)) {
                continue;
            }

            UserAccount account;
            account.username = username;
            account.displayName = username;
            account.domain = domain;
            account.sid = sidText;
            account.source = source;
            accounts.push_back(std::move(account));
        }

        ::RegCloseKey(profileList);
        XPLOG_DEBUG("enumerated %d domain profiles", static_cast<int>(accounts.size()));
        return accounts;
    }

    std::vector<std::wstring> HiddenAccountNames() override {
        std::vector<std::wstring> hidden;

        HKEY key = nullptr;
        if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSpecialAccountsKey, 0,
                            KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key) !=
            ERROR_SUCCESS) {
            return hidden;
        }

        for (DWORD index = 0;; ++index) {
            wchar_t valueName[256] = {};
            DWORD nameLength = ARRAYSIZE(valueName);
            DWORD type = 0;
            DWORD data = 0;
            DWORD dataSize = sizeof(data);

            const LONG status = ::RegEnumValueW(
                key, index, valueName, &nameLength, nullptr, &type,
                reinterpret_cast<LPBYTE>(&data), &dataSize);
            if (status == ERROR_NO_MORE_ITEMS) {
                break;
            }
            if (status != ERROR_SUCCESS || type != REG_DWORD) {
                continue;
            }
            // A value of 0 means "hide this account from the welcome screen".
            if (data == 0) {
                hidden.emplace_back(valueName);
            }
        }

        ::RegCloseKey(key);
        return hidden;
    }

    std::wstring MachineName() override {
        if (machineName_.empty()) {
            machineName_ = GetMachineNameW();
        }
        return machineName_;
    }

    bool IsDomainJoined() override {
        LPWSTR domain = nullptr;
        NETSETUP_JOIN_STATUS joinStatus = NetSetupUnknownStatus;
        if (::NetGetJoinInformation(nullptr, &domain, &joinStatus) != NERR_Success) {
            return false;
        }
        const bool joined = joinStatus == NetSetupDomainName;
        if (domain) {
            ::NetApiBufferFree(domain);
        }
        return joined;
    }

private:
    std::wstring machineName_;
};

} // namespace

std::shared_ptr<IUserEnumerator> MakeWin32UserEnumerator() {
    return std::make_shared<Win32UserEnumerator>();
}

} // namespace xplogin
