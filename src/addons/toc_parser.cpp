#include "addons/toc_parser.hpp"
#include <fstream>
#include <algorithm>
#include <cctype>

namespace wowee::addons {

std::string TocFile::getTitle() const {
    auto it = directives.find("Title");
    return (it != directives.end()) ? it->second : addonName;
}
bool TocFile::isLoadOnDemand() const {
    auto it = directives.find("LoadOnDemand");
    return (it != directives.end()) && it->second == "1";
}

/// The spelling FrameXML uses for a Blizzard addon, given the one on disk.
///
/// An addon's name is its folder's name, and this install's folders are lower
/// case - the extraction flattened them. Loading copes: every lookup here folds
/// case before matching. Announcing does not, because the announcement goes to
/// Lua and Lua's == is case-sensitive:
///
///     if ( name == "Blizzard_GlyphUI" and IsAddOnLoaded("Blizzard_TalentUI") ...
///
/// That is GlyphFrame_OnEvent, and inside it is the only place the glyph panel
/// is ever parented to the talent frame, sized to it, and its close button
/// raised above it. Fired as "blizzard_glyphui" the test simply failed, so the
/// panel stayed a child of UIParent in the top-left corner of the screen with
/// no close button on it - which is exactly what it looked like.
///
/// Every addon that does its setup on ADDON_LOADED and names itself has the
/// same problem, so this is fixed once, here, where the name is decided.
///
/// The spellings are FrameXML's own, taken from the files that compare against
/// them rather than guessed: Blizzard_BarberShopUI capitalises the S and
/// Blizzard_GMChatUI capitalises both letters of GM, and a guess would have had
/// either wrong.
static std::string canonicalAddonName(const std::string& onDisk) {
    static const char* kKnown[] = {
        "Blizzard_AchievementUI",   "Blizzard_ArenaUI",
        "Blizzard_AuctionUI",       "Blizzard_BarberShopUI",
        "Blizzard_BattlefieldMinimap", "Blizzard_BindingUI",
        "Blizzard_Calendar",        "Blizzard_CombatLog",
        "Blizzard_CombatText",      "Blizzard_DebugTools",
        "Blizzard_GlyphUI",         "Blizzard_GMChatUI",
        "Blizzard_GMSurveyUI",      "Blizzard_GuildBankUI",
        "Blizzard_InspectUI",       "Blizzard_ItemSocketingUI",
        "Blizzard_MacroUI",         "Blizzard_RaidUI",
        "Blizzard_TalentUI",        "Blizzard_TimeManager",
        "Blizzard_TokenUI",         "Blizzard_TradeSkillUI",
        "Blizzard_TrainerUI",
    };
    auto fold = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    };
    const std::string key = fold(onDisk);
    for (const char* known : kKnown) {
        if (fold(known) == key) return known;
    }
    // Anything else keeps the name it has. A third-party addon's folder is
    // already spelled the way its own files expect.
    return onDisk;
}

static bool startsWithNoCase(const std::string& s, const char* prefix) {
    for (size_t i = 0; prefix[i]; ++i) {
        if (i >= s.size() || std::tolower(static_cast<unsigned char>(s[i])) !=
                                 std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    }
    return true;
}

/// Every directive whose name begins with one of `prefixes`, split at spaces and
/// commas; directives taken in name order.
static std::vector<std::string> namesOf(const std::unordered_map<std::string, std::string>& directives,
                                        std::initializer_list<const char*> prefixes) {
    std::vector<std::string> keys;
    for (const auto& [key, value] : directives) {
        (void)value;
        for (const char* prefix : prefixes) {
            if (!startsWithNoCase(key, prefix)) continue;
            keys.push_back(key);
            break;
        }
    }
    std::sort(keys.begin(), keys.end());
    std::vector<std::string> names;
    for (const auto& key : keys) {
        const std::string& value = directives.at(key);
        size_t pos = 0;
        while (pos < value.size()) {
            const size_t start = value.find_first_not_of(" ,\t", pos);
            if (start == std::string::npos) break;
            const size_t end = value.find_first_of(" ,\t", start);
            names.push_back(value.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) break;
            pos = end;
        }
    }
    return names;
}

std::vector<std::string> TocFile::getRequiredDependencies() const { return namesOf(directives, {"RequiredDep", "Dep"}); }
std::vector<std::string> TocFile::getOptionalDependencies() const { return namesOf(directives, {"OptionalDep"}); }
std::vector<std::string> TocFile::getLoadWith() const { return namesOf(directives, {"LoadWith"}); }
std::vector<std::string> TocFile::getLoadManagers() const { return namesOf(directives, {"LoadManager"}); }

std::pair<std::string, std::string> tocDirective(const std::string& text, const std::string& locale) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos) return {};
    const auto trim = [](std::string s) {
        const size_t start = s.find_first_not_of(" \t");
        const size_t end = s.find_last_not_of(" \t");
        return start == std::string::npos ? std::string() : s.substr(start, end - start + 1);
    };
    std::string key = trim(text.substr(0, colon));
    const std::string value = trim(text.substr(colon + 1));
    const size_t dash = key.rfind('-');
    bool localised = false;
    for (const char* name : {"Title", "Notes", "Author", "Version", "X-"})
        if (startsWithNoCase(key, name)) localised = true;
    if (localised && dash != std::string::npos) {
        const std::string suffix = key.substr(dash + 1);
        for (const char* known : {"enUS", "koKR", "frFR", "deDE", "zhCN", "zhTW", "esES", "esMX", "ruRU"}) {
            if (!startsWithNoCase(suffix, known)) continue;
            if (!startsWithNoCase(locale, known)) return {};
            key = trim(key.substr(0, dash));
            break;
        }
    }
    return {key, value};
}

static std::vector<std::string> parseVarList(const std::string& val) {
    std::vector<std::string> result;
    size_t pos = 0;
    while (pos <= val.size()) {
        size_t comma = val.find(',', pos);
        std::string name = (comma != std::string::npos) ? val.substr(pos, comma - pos) : val.substr(pos);
        size_t start = name.find_first_not_of(" \t");
        size_t end = name.find_last_not_of(" \t");
        if (start != std::string::npos)
            result.push_back(name.substr(start, end - start + 1));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return result;
}

std::vector<std::string> TocFile::getSavedVariables() const {
    auto it = directives.find("SavedVariables");
    return (it != directives.end()) ? parseVarList(it->second) : std::vector<std::string>{};
}

std::vector<std::string> TocFile::getSavedVariablesPerCharacter() const {
    auto it = directives.find("SavedVariablesPerCharacter");
    return (it != directives.end()) ? parseVarList(it->second) : std::vector<std::string>{};
}

std::optional<TocFile> parseTocFile(const std::string& tocPath, const std::string& locale) {
    std::ifstream f(tocPath);
    if (!f.is_open()) return std::nullopt;

    TocFile toc;
    toc.basePath = tocPath;
    // Strip filename to get directory
    size_t lastSlash = tocPath.find_last_of("/\\");
    if (lastSlash != std::string::npos) {
        toc.basePath = tocPath.substr(0, lastSlash);
        toc.addonName = tocPath.substr(lastSlash + 1);
    }
    // Strip .toc extension from addon name
    size_t dotPos = toc.addonName.rfind(".toc");
    if (dotPos != std::string::npos) toc.addonName.resize(dotPos);
    toc.addonName = canonicalAddonName(toc.addonName);

    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        // 0x005f86a0: without its byte order mark.
        if (first && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        first = false;
        // Strip trailing CR (Windows line endings)
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Skip empty lines
        if (line.empty()) continue;

        // ## directives
        if (line.size() >= 3 && line[0] == '#' && line[1] == '#') {
            auto [key, value] = tocDirective(line.substr(2), locale);
            if (!key.empty()) toc.directives[key] = value;
            continue;
        }

        // Single # comment
        if (line[0] == '#') continue;

        // Whitespace-only line
        size_t firstNonSpace = line.find_first_not_of(" \t");
        if (firstNonSpace == std::string::npos) continue;

        // File entry - normalize backslashes to forward slashes
        std::string filename = line.substr(firstNonSpace);
        size_t lastNonSpace = filename.find_last_not_of(" \t");
        if (lastNonSpace != std::string::npos) filename.resize(lastNonSpace + 1);
        std::replace(filename.begin(), filename.end(), '\\', '/');
        toc.files.push_back(std::move(filename));
    }

    return toc;
}

} // namespace wowee::addons
