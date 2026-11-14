#pragma once

/**
 * original_client_login.hpp - the account name and the realm list address the
 * original client would show, read from its own files.
 *
 * In drop-in mode (core/dropin.hpp) this client sits in a WoW 3.3.5a folder
 * whose player has already logged in from it: Wow.exe saved the account name
 * in WTF\Config.wtf and reads its realm list server from realmlist.wtf. The
 * login screen starts from those, so the first start of wowee.exe asks for no
 * more than the original would. Read only - nothing here writes to the
 * original client's files, and its password is never anywhere in them.
 *
 * How Wow.exe 12340 reads them (addresses in that binary):
 *
 *  - A .wtf file is run by 0x00766530: the name as given, relative to the
 *    client's folder, else WTF\<name>. Its text (0x00766400; a UTF-8 BOM is
 *    skipped) is split into lines on CR and LF, and only a line whose first
 *    four characters are "SET " in any case is run, as the console command
 *    "set" (0x007681f0). Everything else - comments, blank lines, other
 *    commands - is passed over.
 *  - "set" takes two tokens with the separators " ,;\t\"\r\n" (0x0076f1e0):
 *    the cvar's name and its value. A token may be quoted, and may then hold
 *    the separators; a quote ends it. Whatever follows the value is ignored.
 *    Cvar names match without regard to case, so "set realmlist" sets
 *    realmList.
 *  - Startup (0x004067f0) runs Config.wtf first, then registers "locale"
 *    (default "****", which becomes "enUS") and settles it in 0x00402d50: the
 *    first locale, from the configured one on through the table at 0x00ad3028
 *    (deDE enGB enUS esES frFR koKR zhCN zhTW enCN enTW esMX ruRU, wrapping
 *    round; an unknown name starts at enUS), whose Data\<loc>\locale-<loc>.MPQ
 *    exists.
 *  - GetSavedAccountName (0x004dd5d0) answers the "accountName" cvar
 *    (registered at 0x00401b60), which the login screen's "remember account
 *    name" fills and clears through SetSavedAccountName (0x004dbec0).
 *  - Then 0x00405dd0 calls 0x006b0bf0 with that locale, which registers
 *    realmList (default "us.logon.worldofwarcraft.com:3724") and runs
 *    data\<locale>\realmlist.wtf, and only when that cannot be opened,
 *    realmlist.wtf in the client's folder - each also under WTF\. A realmList
 *    set by Config.wtf therefore stands only when the file run after it does
 *    not set one too. Data\realmlist.wtf is not read at all.
 */

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wowee {
namespace core {

/// Every "SET name value" a .wtf file runs, in order, as Wow.exe reads it.
std::vector<std::pair<std::string, std::string>> parseWtfSetLines(std::string_view text);

/// The value the last "SET `cvar`" in `text` leaves (cvar name in any case).
std::optional<std::string> findWtfSet(std::string_view text, std::string_view cvar);

/// A realmList value split into a host and a port ("host" or "host:port";
/// 3724 when there is no port or it is not a number from 1 to 65535).
struct RealmAddress {
    std::string host;
    int port = 3724;
};
std::optional<RealmAddress> splitRealmList(std::string_view value);

/// The locale Wow.exe settles on (0x00402d50): `configured` when its
/// locale-<loc>.MPQ is installed, else the next installed one in the
/// client's table order; `configured` itself ("enUS" for "" or "****") when
/// none is.
std::string settleClientLocale(const std::string& configured,
                               const std::vector<std::string>& installed);

/// What the original client in `clientRoot` would show at its login screen.
struct OriginalClientLogin {
    /// The locale whose realmlist.wtf was looked for.
    std::string locale;
    /// The saved account name ("" when "remember account name" was off).
    std::string accountName;
    /// realmList as the client would end up with it; "" when no file sets it.
    std::string realmList;
    /// The file that set realmList ("" when none did).
    std::string realmListFrom;
};

/// Reads Config.wtf and the realmlist.wtf the client in `clientRoot` would run.
OriginalClientLogin readOriginalClientLogin(const std::filesystem::path& clientRoot);

} // namespace core
} // namespace wowee
