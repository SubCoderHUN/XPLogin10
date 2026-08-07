#include "xplogin/UserDirectory.h"

#include "xplogin/StringUtil.h"

#include <algorithm>

namespace xplogin {
namespace {

// Accounts Windows creates for itself. None of these ever belong on a tile.
const wchar_t* const kSystemAccounts[] = {
    L"DefaultAccount",
    L"WDAGUtilityAccount",
    L"defaultuser0",
    L"HomeGroupUser$",
    L"SYSTEM",
    L"LOCAL SERVICE",
    L"NETWORK SERVICE",
};

bool ContainsNoCase(const std::vector<std::wstring>& list, const std::wstring& value) {
    for (const std::wstring& item : list) {
        if (EqualsNoCase(item, value)) {
            return true;
        }
    }
    return false;
}

} // namespace

UserDirectory::UserDirectory(std::shared_ptr<IUserEnumerator> enumerator,
                             UserDirectoryOptions options)
    : enumerator_(std::move(enumerator)), options_(std::move(options)) {}

bool UserDirectory::IsSystemAccount(const std::wstring& username) {
    for (const wchar_t* name : kSystemAccounts) {
        if (EqualsNoCase(username, name)) {
            return true;
        }
    }
    // Machine accounts and most service accounts end in '$'.
    return !username.empty() && username.back() == L'$';
}

void UserDirectory::SortAccounts(std::vector<UserAccount>& accounts) {
    std::stable_sort(accounts.begin(), accounts.end(),
                     [](const UserAccount& a, const UserAccount& b) {
                         if (a.CanLogOn() != b.CanLogOn()) {
                             return a.CanLogOn();
                         }
                         const std::wstring& na =
                             a.displayName.empty() ? a.username : a.displayName;
                         const std::wstring& nb =
                             b.displayName.empty() ? b.username : b.displayName;
                         int cmp = CompareNoCase(na, nb);
                         if (cmp != 0) {
                             return cmp < 0;
                         }
                         return CompareNoCase(a.domain, b.domain) < 0;
                     });
}

AccountSource ClassifyAccountDomain(const std::wstring& domain,
                                    const std::wstring& machineName) {
    if (domain.empty() || EqualsNoCase(domain, machineName)) {
        return AccountSource::Local;
    }
    if (EqualsNoCase(domain, L"MicrosoftAccount") ||
        EqualsNoCase(domain, L"AzureAD")) {
        return AccountSource::MicrosoftAccount;
    }
    return AccountSource::Domain;
}

std::vector<UserAccount> UserDirectory::ApplyVisibilityRules(
    std::vector<UserAccount> input,
    const std::vector<std::wstring>& hiddenNames,
    const UserDirectoryOptions& options) {
    std::vector<UserAccount> visible;
    visible.reserve(input.size());

    size_t enabledNonAdmin = 0;
    for (const UserAccount& a : input) {
        if (a.CanLogOn() && !EqualsNoCase(a.username, L"Administrator") &&
            !EqualsNoCase(a.username, L"Guest") && !IsSystemAccount(a.username)) {
            ++enabledNonAdmin;
        }
    }

    for (UserAccount& account : input) {
        if (account.username.empty()) {
            continue;
        }
        if (IsSystemAccount(account.username)) {
            continue;
        }
        if (account.hidden) {
            continue;
        }
        if (ContainsNoCase(hiddenNames, account.username)) {
            continue;
        }
        if (ContainsNoCase(options.extraHiddenNames, account.username)) {
            continue;
        }
        if (!options.showDisabledAccounts && account.disabled) {
            continue;
        }
        if (!options.showDomainAccounts && account.source == AccountSource::Domain) {
            continue;
        }
        if (!options.showMicrosoftAccounts &&
            account.source == AccountSource::MicrosoftAccount) {
            continue;
        }
        if (!options.showGuestAccount && EqualsNoCase(account.username, L"Guest")) {
            continue;
        }
        // XP kept the built-in Administrator off the welcome screen, unless it
        // was the last usable way in.
        if (!options.showBuiltInAdministrator &&
            EqualsNoCase(account.username, L"Administrator") && enabledNonAdmin > 0) {
            continue;
        }

        if (account.displayName.empty()) {
            account.displayName = account.username;
        }
        visible.push_back(account);
    }

    // Deduplicate: the same SID can turn up from both enumerators, and a domain
    // account can shadow a local one with the same name.
    std::vector<UserAccount> deduped;
    deduped.reserve(visible.size());
    for (const UserAccount& account : visible) {
        bool duplicate = false;
        for (const UserAccount& existing : deduped) {
            const bool sameSid = !account.sid.empty() && !existing.sid.empty() &&
                                 EqualsNoCase(account.sid, existing.sid);
            const bool sameName = EqualsNoCase(account.username, existing.username) &&
                                  EqualsNoCase(account.domain, existing.domain);
            if (sameSid || sameName) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            deduped.push_back(account);
        }
    }

    SortAccounts(deduped);
    if (options.maxAccounts > 0 && deduped.size() > options.maxAccounts) {
        deduped.resize(options.maxAccounts);
    }
    return deduped;
}

void UserDirectory::Refresh() {
    users_.clear();
    if (!enumerator_) {
        return;
    }

    machineName_ = enumerator_->MachineName();

    std::vector<UserAccount> all = enumerator_->EnumerateLocalUsers();
    for (UserAccount& a : all) {
        if (a.domain.empty()) {
            a.domain = machineName_;
        }
        if (a.source == AccountSource::Unknown) {
            a.source = AccountSource::Local;
        }
    }

    if (options_.showDomainAccounts) {
        std::vector<UserAccount> domainUsers = enumerator_->EnumerateDomainUsers();
        for (UserAccount& a : domainUsers) {
            if (a.source == AccountSource::Unknown) {
                a.source = AccountSource::Domain;
            }
        }
        all.insert(all.end(), domainUsers.begin(), domainUsers.end());
    }

    users_ = ApplyVisibilityRules(std::move(all), enumerator_->HiddenAccountNames(),
                                 options_);
}

int UserDirectory::IndexOfUser(const std::wstring& qualifiedOrBareName) const {
    std::wstring domain;
    std::wstring name;
    if (!SplitQualifiedName(qualifiedOrBareName, &domain, &name)) {
        return -1;
    }

    for (size_t i = 0; i < users_.size(); ++i) {
        if (!EqualsNoCase(users_[i].username, name)) {
            continue;
        }
        if (domain.empty() || EqualsNoCase(users_[i].domain, domain)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int UserDirectory::IndexOfSid(const std::wstring& sid) const {
    if (sid.empty()) {
        return -1;
    }
    for (size_t i = 0; i < users_.size(); ++i) {
        if (EqualsNoCase(users_[i].sid, sid)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const UserAccount* UserDirectory::At(int index) const {
    if (index < 0 || index >= static_cast<int>(users_.size())) {
        return nullptr;
    }
    return &users_[static_cast<size_t>(index)];
}

} // namespace xplogin
