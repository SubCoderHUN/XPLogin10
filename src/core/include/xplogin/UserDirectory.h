// XPLogin10 - builds the tile list shown on the welcome screen.
//
// Merges local SAM accounts with domain accounts, applies the same visibility
// rules XP used (SpecialAccounts\UserList, hide built-ins, hide disabled), and
// produces a stable ordering so tiles do not jump around between refreshes.
#pragma once

#include "xplogin/Interfaces.h"
#include "xplogin/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace xplogin {

struct UserDirectoryOptions {
    bool showDisabledAccounts = false;
    bool showDomainAccounts   = true;
    bool showBuiltInAdministrator = false; // XP hid it unless it was the only account
    bool showGuestAccount     = false;
    bool showMicrosoftAccounts = true;

    // One account, no password: sign in without asking, and show XP's welcome
    // message for the whole of it rather than an account list nobody is meant
    // to click. On by default because it is what both XP and Windows 10 do
    // unprompted - turning it off gives a one-tile screen that waits.
    bool autoLogonBlankPassword = true;

    // Hard cap; the welcome screen scrolls beyond this.
    size_t maxAccounts = 64;

    // Extra names to hide, from config (case insensitive).
    std::vector<std::wstring> extraHiddenNames;
};

// Which kind of account the domain half of a qualified name describes.
//
// Windows hands the sign-in screen names like "WINBOX\\Bill",
// "CONTOSO\\alice" and "MicrosoftAccount\\someone@example.com", and the third
// is neither of the other two. A Microsoft account has a local profile and a
// local SID, so it is not a domain account and must not be hidden by a setting
// about domain accounts - on a machine set up with a Microsoft account and
// nothing else, that would hide the only way in.
//
// AzureAD is the work-account spelling of the same idea and is classified with
// it, because everything this project does with the answer is the same for
// both: show the tile, and hand LSA the qualified name unchanged.
AccountSource ClassifyAccountDomain(const std::wstring& domain,
                                    const std::wstring& machineName);

class UserDirectory {
public:
    UserDirectory(std::shared_ptr<IUserEnumerator> enumerator,
                  UserDirectoryOptions options = {});

    // Re-queries the OS. Safe to call on every SetUsageScenario / refresh.
    void Refresh();

    const std::vector<UserAccount>& Users() const { return users_; }
    size_t Count() const { return users_.size(); }

    // -1 when not found. Matching is case insensitive and accepts either the
    // bare name or DOMAIN\name.
    int IndexOfUser(const std::wstring& qualifiedOrBareName) const;
    int IndexOfSid(const std::wstring& sid) const;

    const UserAccount* At(int index) const;

    // The machine name used to qualify local accounts.
    const std::wstring& MachineName() const { return machineName_; }

    // Exposed for testing: applies the visibility rules to an arbitrary list.
    static std::vector<UserAccount> ApplyVisibilityRules(
        std::vector<UserAccount> input,
        const std::vector<std::wstring>& hiddenNames,
        const UserDirectoryOptions& options);

    // Stable XP-like ordering: enabled before disabled, then case-insensitive
    // by display name, then by domain to break exact ties.
    static void SortAccounts(std::vector<UserAccount>& accounts);

    // True for names Windows always keeps off the welcome screen.
    static bool IsSystemAccount(const std::wstring& username);

private:
    std::shared_ptr<IUserEnumerator> enumerator_;
    UserDirectoryOptions             options_;
    std::vector<UserAccount>         users_;
    std::wstring                     machineName_;
};

} // namespace xplogin
