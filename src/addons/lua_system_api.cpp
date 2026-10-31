// lua_system_api.cpp - System, time, sound, locale, map, addons, instances, and utilities Lua API bindings.
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#include <array>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ranges>
#include <set>
#include <unordered_set>
#include <utility>
#include <vector>
#include "game/chat_handler.hpp"
#include "game/inventory_handler.hpp"
#include "game/group_defines.hpp"
#include "game/battleground_flag.hpp"
#include "game/minimap_tracking.hpp"
#include "game/known_titles.hpp"
#include "game/movement_handler.hpp"   // the client-control table, for GetBattlefieldVehicleInfo's seventh value
#include "core/open_url.hpp"
#include "core/version.hpp"
#include "core/config_paths.hpp"
#include "ui/settings_schema.hpp"
#include "ui/settings_i18n.hpp"
#include "imgui.h"
#include "game/opcode_table.hpp"
#include "network/packet.hpp"
#include "addons/lua_api_helpers.hpp"
#include "addons/lua_api_registrations.hpp"
#include "addons/chat_color_table.hpp"
#include "addons/lua_taint.hpp"
#include "addons/chat_window_background.hpp"
#include "addons/client_cvar_registry.hpp"
#include "ui/display_modes.hpp"
#include "ui/fullscreen_scale.hpp"
#include "ui/widget_tree.hpp"
#include "rendering/camera_controller.hpp"
#include "pipeline/asset_manager.hpp"
#include "addons/lua_engine.hpp"
#include "game/bg_score_defs.hpp"
#include "game/calendar_month.hpp"
#include "game/calendar_data.hpp"
#include "game/lfg_update.hpp"
#include "game/pet_action.hpp"
#include "audio/activity_sound_manager.hpp"
#include "audio/ambient_sound_manager.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "audio/combat_sound_manager.hpp"
#include "audio/footstep_manager.hpp"
#include "audio/mount_sound_manager.hpp"
#include "audio/movement_sound_manager.hpp"
#include "audio/music_manager.hpp"
#include "audio/npc_voice_manager.hpp"
#include "audio/player_voice_manager.hpp"
#include "audio/ui_sound_manager.hpp"
#include "core/app_clock.hpp"
#include "core/window.hpp"

#include <SDL3/SDL.h>
#include "game/expansion_profile.hpp"
#include "core/coordinates.hpp"
#include "rendering/minimap_zoom.hpp"
#include "rendering/minimap_blips.hpp"
#include "rendering/world_map/coordinate_projection.hpp"
#include "core/local_time.hpp"

namespace wowee::addons {

// CombatLog_Object_IsA(unitFlags, mask) - does a combat log unit match a filter.
//
// The flags are four exclusive categories packed together (affiliation,
// reaction, control, unit type) plus a set of non-exclusive special bits, and a
// filter names every value it accepts within a category. COMBATLOG_FILTER_MINE
// is AFFILIATION_MINE + REACTION_FRIENDLY + CONTROL_PLAYER + TYPE_PLAYER +
// TYPE_OBJECT, and a player only ever carries one of those two type bits - so
// the obvious (flags & mask) == mask never matches anything, and the whole
// combat log filters itself empty.
//
// A category the mask says nothing about is not a constraint.
/// The month the calendar is looking at, as (month, year).
///
/// State, because it is state in WoW as well: CalendarSetMonth moves it by an
/// offset and CalendarGetMonth reads it back. Recomputing it from the clock on
/// every call would answer correctly once and make the month buttons do
/// nothing. Seeded from the local clock the first time it is asked, which is
/// the month the calendar opens on.
static std::pair<int, int>& calendarViewedMonthState() {
    static std::pair<int, int> viewed{0, 0};
    if (viewed.first == 0) {
        const std::time_t now = std::time(nullptr);
        const std::tm t = core::localTime(now);
        viewed = {t.tm_mon + 1, t.tm_year + 1900};
    }
    return viewed;
}

static std::pair<int, int> calendarViewedMonth() {
    return calendarViewedMonthState();
}

static void calendarSetViewedMonth(int month, int year) {
    if (month < 1 || month > 12) return;
    calendarViewedMonthState() = {month, year};
}

/// One end of the range the calendar offers, as weekday, month, day, year -
/// the order the interface unpacks it in.
static int pushCalendarBoundDate(lua_State* L, int monthOffset) {
    // From today's month rather than the viewed one: a bound that moved as the
    // player paged would let them page for ever.
    const std::time_t now = std::time(nullptr);
    const std::tm t = core::localTime(now);
    const int baseMonth = t.tm_mon + 1;
    const int baseYear  = t.tm_year + 1900;
    const auto info =
        wowee::game::calendarMonthAt(baseMonth, baseYear, monthOffset);
    // The first of that month at the near end, its last day at the far end, so
    // the whole bounding month is inside the range rather than half of it.
    const int day = (monthOffset < 0) ? 1 : info.numDays;
    lua_pushnumber(L, wowee::game::weekdayOf(info.month, day, info.year));
    lua_pushnumber(L, info.month);
    lua_pushnumber(L, day);
    lua_pushnumber(L, info.year);
    return 4;
}

/// The rows on the day an interface call is asking about.
///
/// Every day-indexed calendar getter takes (monthOffset, day) and indexes the
/// same list, so resolving it lives here once - three getters computing "which
/// month is offset -1 of the viewed one" separately is how they would drift
/// apart on the first month that ends on a Sunday.
static std::vector<wowee::game::CalendarDayEntry> calendarDayRows(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return {};
    const int monthOffset = static_cast<int>(luaL_optnumber(L, 1, 0));
    const int day = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (day < 1) return {};
    const auto viewed = calendarViewedMonth();
    const auto info = wowee::game::calendarMonthAt(viewed.first, viewed.second,
                                                   monthOffset);
    if (day > info.numDays) return {};
    return wowee::game::calendarEntriesForDay(gh->getCalendarData(), info.month,
                                              day, info.year);
}

/// The event being built up before it is sent.
///
/// State, because the interface builds one over several calls and commits it
/// with another: CalendarNewEvent starts it, CalendarEventSetTitle and the
/// rest fill it in, CalendarAddEvent sends it. Nothing is carried between
/// those calls except this.
static wowee::game::CalendarEventDraft& calendarDraft() {
    static wowee::game::CalendarEventDraft draft;
    return draft;
}

/// CalendarEventSetLocked and its three siblings: the open event's flag,
/// where the player may (GameHandler::calendarSetOpenEventFlag), and the
/// draft's to match, which is what an edit sends.
static int calendarSetOpenEventFlag(lua_State* L, uint32_t flag, bool on) {
    auto* gh = getGameHandler(L);
    if (gh && gh->calendarSetOpenEventFlag(flag, on)) {
        if (on) calendarDraft().flags |= flag;
        else calendarDraft().flags &= ~flag;
    }
    return 0;
}

/// The row a right-click menu is about: which month, which day, which entry.
///
/// State because the menu needs it to be. UIDropDownMenu builds its list in
/// one call and runs the click in another, and nothing is carried between the
/// two - so the row is named once by CalendarContextSelectEvent and every verb
/// on the menu reads it back.
struct CalendarContextRow { int monthOffset = 0; int day = 0; int index = 0; };

static CalendarContextRow& calendarContextRow() {
    static CalendarContextRow row;
    return row;
}

/// The rows of a day, for a caller that already has the handler and the day.
static std::vector<wowee::game::CalendarDayEntry> calendarRowsFor(
        wowee::game::GameHandler& gh, int monthOffset, int day) {
    if (day < 1) return {};
    const auto viewed = calendarViewedMonth();
    const auto info = wowee::game::calendarMonthAt(viewed.first, viewed.second,
                                                   monthOffset);
    if (day > info.numDays) return {};
    return wowee::game::calendarEntriesForDay(gh.getCalendarData(), info.month,
                                              day, info.year);
}

/// Which of the six kinds of row this event is, as the string the interface
/// compares against. Guild events carry a flag; everything else the player can
/// see on their own calendar is theirs.
static const char* calendarTypeName(const wowee::game::CalendarEvent& ev) {
    // CALENDAR_EVENTTYPE_* from the server: 4 is the guild announcement, and
    // the guild flag distinguishes an event from an announcement.
    constexpr uint32_t kGuildEventFlag = 0x0400;
    if (ev.type == 4) return "GUILD_ANNOUNCEMENT";
    if (ev.flags & kGuildEventFlag) return "GUILD_EVENT";
    return "PLAYER";
}

/// Answer the invitation on the menu's row, if that row has one.
///
/// The invite id comes from the invite list the calendar arrived with - the
/// server wants both it and the event id, and an event the player was never
/// invited to has no invite to answer. Silent when there is none rather than
/// sending a request the server would refuse.
static int calendarRespondToContextInvite(lua_State* L, uint32_t status) {
    auto* gh = getGameHandler(L);
    const auto& row = calendarContextRow();
    if (!gh || row.day < 1) return 0;
    const auto rows = calendarRowsFor(*gh, row.monthOffset, row.day);
    if (row.index < 1 || static_cast<size_t>(row.index) > rows.size()) return 0;
    const auto& entry = rows[static_cast<size_t>(row.index) - 1];
    if (entry.kind != wowee::game::CalendarEntryKind::Event) return 0;
    const auto& cal = gh->getCalendarData();
    const uint64_t eventId = cal.events[entry.index].eventId;
    for (const auto& invite : cal.invites) {
        if (invite.eventId != eventId) continue;
        gh->respondToCalendarInvite(eventId, invite.inviteId, status);
        return 0;
    }
    return 0;
}

/// The event a context verb names: three numbers name a row of a day, else
/// the row the menu is about (0x00c207e8). False with neither - the verb's
/// usage error; true with an event id of 0 where the row is not an event.
static bool calendarNamedOrContextEvent(lua_State* L, uint64_t& eventId) {
    eventId = 0;
    CalendarContextRow row = calendarContextRow();
    if (lua_isnumber(L, 1) && lua_isnumber(L, 2) && lua_isnumber(L, 3)) {
        row = {.monthOffset = static_cast<int>(std::lround(lua_tonumber(L, 1))),
               .day = static_cast<int>(std::lround(lua_tonumber(L, 2))),
               .index = static_cast<int>(std::lround(lua_tonumber(L, 3)))};
    } else if (row.day < 1 || row.index < 1) {
        return false;
    }
    auto* gh = getGameHandler(L);
    if (!gh) return true;
    const auto rows = calendarRowsFor(*gh, row.monthOffset, row.day);
    if (row.index < 1 || static_cast<size_t>(row.index) > rows.size()) return true;
    const auto& entry = rows[static_cast<size_t>(row.index) - 1];
    if (entry.kind == wowee::game::CalendarEntryKind::Event)
        eventId = gh->getCalendarData().events[entry.index].eventId;
    return true;
}

/// A day's record as the calendar keeps it (0x005b9260): its row, the
/// event behind it where it is one, the event's flags (+0x98) and its
/// invite flags and status (+0x9c, +0xa8).
struct CalendarRecord {
    wowee::game::CalendarDayEntry entry;
    const wowee::game::CalendarEvent* event = nullptr;
    uint32_t flags = 0;
    wowee::game::CalendarInviteState invite;
};

/// The record three numbers name - a row of a day of the viewed month,
/// counted from its offset - or, without them, the one the menu is about
/// (0x00c207e8). False with neither, the caller's usage error; `out` empty
/// where no record answers.
static bool calendarRecordArg(lua_State* L, std::optional<CalendarRecord>& out) {
    out.reset();
    CalendarContextRow row = calendarContextRow();
    if (lua_isnumber(L, 1) && lua_isnumber(L, 2) && lua_isnumber(L, 3)) {
        row = {.monthOffset = static_cast<int>(lua_tonumber(L, 1)),
               .day = static_cast<int>(std::lround(lua_tonumber(L, 2))),
               .index = static_cast<int>(std::lround(lua_tonumber(L, 3)))};
    } else if (row.day < 1 || row.index < 1) {
        return false;
    }
    auto* gh = getGameHandler(L);
    if (!gh) return true;
    const auto rows = calendarRowsFor(*gh, row.monthOffset, row.day);
    if (row.index < 1 || static_cast<size_t>(row.index) > rows.size()) return true;
    CalendarRecord rec;
    rec.entry = rows[static_cast<size_t>(row.index) - 1];
    const auto& cal = gh->getCalendarData();
    namespace cf = wowee::game::calendar_flag;
    switch (rec.entry.kind) {
        case wowee::game::CalendarEntryKind::Holiday: rec.flags = cf::kHoliday; break;
        case wowee::game::CalendarEntryKind::RaidLockout: rec.flags = cf::kRaidLockout; break;
        case wowee::game::CalendarEntryKind::Event: {
            rec.event = &cal.events[rec.entry.index];
            rec.flags = rec.event->flags;
            const auto now = gh->getGameDate();
            rec.invite = wowee::game::calendarInviteState(cal, *rec.event, gh->getPlayerGuid(), now ? &*now : nullptr);
            if (!rec.invite.kept) return true;
            break;
        }
    }
    out = rec;
    return true;
}

/// 0x005b8080: "CREATOR" for invite flags 4, "MODERATOR" for 2.
static const char* calendarModeratorText(uint32_t inviteFlags) {
    if ((inviteFlags & 4u) != 0) return "CREATOR";
    return (inviteFlags & 2u) != 0 ? "MODERATOR" : "";
}

/// 0x005b80b0's invite type: 2 for a guild event's (flags 0x400) where its
/// invite flags hold 8, else 1.
static int calendarInviteType(const CalendarRecord& rec) {
    return (rec.flags & wowee::game::calendar_flag::kGuildEvent) != 0 && (rec.invite.flags & 8u) != 0 ? 2 : 1;
}

/// 0x005b8040: a holiday's (flags 8) day of a run of two or more - its
/// first "START", its last "END", the rest "ONGOING" - else "".
static const char* calendarSequenceType(uint32_t flags, uint32_t index, uint32_t count) {
    if ((flags & wowee::game::calendar_flag::kHoliday) == 0 || count < 2) return "";
    if (index == 0) return "START";
    return index == count - 1 ? "END" : "ONGOING";
}

/// A global string of the interface's, or its key where it has none -
/// what the client's string lookup (0x00819d40) answers.
static void pushGlobalText(lua_State* L, const char* key) {
    lua_getglobal(L, key);
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, key);
    }
}

/// The player's own answer to an event, from the invite list that came with
/// it. Zero is CALENDAR_INVITESTATUS_INVITED, which is what an event carries
/// while it is unanswered.
static double calendarInviteStatusFor(const wowee::game::CalendarData& cal,
                                      uint64_t eventId) {
    for (const auto& invite : cal.invites) {
        if (invite.eventId == eventId) return static_cast<double>(invite.status);
    }
    return 0;
}

/// CombatLog_Object_IsA(unitFlags, mask) (0x0074d600): 1 when the flags
/// have one of the mask's special bits, or something of it in each of the
/// four categories - nil otherwise, not false.
static int lua_CombatLog_Object_IsA(lua_State* L) {
    const auto flags = static_cast<uint32_t>(std::llround(lua_tonumber(L, 1)));
    const auto mask = static_cast<uint32_t>(std::llround(lua_tonumber(L, 2)));
    if (!wowee::game::combat_log::maskComplete(flags & mask)) return luaReturnNil(L);
    lua_pushnumber(L, 1);
    return 1;
}

/// PlaySound(sound) (0x009858b0): a SoundEntries row on the SFX channel
/// (0x004c8fc0), named by its id (a number, or a string that reads as one) or
/// by its Name (0x004ccaa0). Anything else is the usage error. Returns
/// nothing.
static int lua_PlaySound(lua_State* L) {
    if (!lua_isnumber(L, 1) && !lua_isstring(L, 1)) return luaL_error(L, "Usage: PlaySound(\"sound\")");

    // Silent while the interface is building itself, before anything else -
    // including the log below, so a line here means a sound that was played.
    // See LuaEngine::setUiSoundsSuppressed and AddonManager::loadAllAddons.
    if (LuaEngine::uiSoundsSuppressed()) return 0;

    // Which sound the interface asked for, and who asked.
    //
    // A sound reported as playing loudly on every world entry turned out to be
    // igMainMenuOpen - uEscapeScreenOpen.wav - played seven times inside
    // thirty milliseconds, which stacks into one hit. Seven callers or one
    // caller seven times is the question a byte count cannot answer, and there
    // are seven separate PlaySound("igMainMenuOpen") sites in the interface.
    // The traceback names it in one line.
    if (core::Logger::getInstance().shouldLog(core::LogLevel::INFO)) {
        const char* asked = lua_isstring(L, 1) ? lua_tostring(L, 1) : "<id>";
        // From the C side rather than through debug.traceback: this Lua is
        // sandboxed and has no debug table, so the traceback came back empty
        // and named nothing. lua_getstack walks the same frames without it.
        std::string where;
        lua_Debug ar;
        for (int level = 1; level <= 3 && lua_getstack(L, level, &ar); ++level) {
            if (!lua_getinfo(L, "Sl", &ar)) break;
            if (!where.empty()) where += " <- ";
            where += ar.short_src;
            where += ":" + std::to_string(ar.currentline);
        }
        LOG_INFO("PlaySound: ", asked, " <- ", where);
    }

    auto* svc = getLuaServices(L);
    auto* ac = svc ? svc->audioCoordinator : nullptr;
    if (!ac) return 0;
    auto* sfx = ac->getUiSoundManager();
    if (!sfx) return 0;

    // Accept numeric sound ID or string name
    std::string sound;
    if (lua_isnumber(L, 1)) {
        const auto id = static_cast<int32_t>(lua_tonumber(L, 1));
        // A row that asked for one instance at a time and is still playing is
        // answered with silence by the client, so Refused stops here too - only
        // a row this install cannot play at all falls through to a stand-in.
        if (id > 0 && sfx->playById(static_cast<uint32_t>(id)) != audio::UiSoundManager::UiPlayResult::Unavailable)
            return 0;
        // Without the row: the ids that have a stand-in here.
        switch (id) {
            case 856: case 1115: sfx->playButtonClick(); return 0; // igMainMenuOption
            case 840: sfx->playQuestActivate(); return 0;          // igQuestListOpen
            case 841: sfx->playQuestComplete(); return 0;           // igQuestListComplete
            case 862: sfx->playBagOpen(); return 0;                // igBackPackOpen
            case 863: sfx->playBagClose(); return 0;               // igBackPackClose
            case 867: sfx->playError(); return 0;                  // igPlayerInvite
            case 888: sfx->playLevelUp(); return 0;                // LEVELUPSOUND
            default: return 0;
        }
    } else {
        const char* name = luaL_optstring(L, 1, "");
        // The real sound first. These names are SoundEntries.dbc rows - the
        // interface is naming one, and the row names the files - so looking it
        // up plays what WoW plays rather than the nearest thing this client had
        // already loaded, and it covers all sixty-eight rather than the
        // forty-four below.
        //
        // The table below stays as the fallback: an install without the sound,
        // or without the dbc, still gets something for the names it can be
        // approximated for.
        // As above: the client answers a refused one-at-a-time ask with silence,
        // so only an unresolvable name reaches the hand-mapped fallback.
        if (sfx->playByName(name) != audio::UiSoundManager::UiPlayResult::Unavailable) return 0;

        sound = name;
        for (char& c : sound) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        // FrameXML asks for eighty-nine names across the files that load -
        // sixty-eight when this was written - and nine of them were answered,
        // so nearly every panel this branch has handed over opened, closed and
        // was clicked in silence. The sound manager already had most of these
        // under its own names; only the mapping was missing.
        //
        // Mapped where the client has the sound the name asks for, and left
        // alone where it does not. Substituting something that merely exists
        // is worse than silence: a wrong sound is a wrong sound forever, while
        // a missing one is a gap someone can still hear.
        struct Mapping { const char* name; void (audio::UiSoundManager::*play)(); };
        static const Mapping kMappings[] = {
            // A click is a click, and these are all buttons.
            {"IGMAINMENUOPTION",            &audio::UiSoundManager::playButtonClick},
            {"IGMAINMENUOPTIONCHECKBOXON",  &audio::UiSoundManager::playButtonClick},
            {"IGMAINMENUOPTIONCHECKBOXOFF", &audio::UiSoundManager::playButtonClick},
            {"IGPLAYERINVITEACCEPTED",      &audio::UiSoundManager::playButtonClick},
            {"IGCHARACTERINFOTAB",          &audio::UiSoundManager::playButtonClick},
            {"UCHATSCROLLBUTTON",           &audio::UiSoundManager::playButtonClick},
            {"IGCHATSCROLLUP",              &audio::UiSoundManager::playButtonClick},
            {"IGCHATSCROLLDOWN",            &audio::UiSoundManager::playButtonClick},
            {"IGCHATBOTTOM",                &audio::UiSoundManager::playButtonClick},
            {"IGMAINMENUOPEN",              &audio::UiSoundManager::playMenuButtonClick},
            {"IGMAINMENUCLOSE",             &audio::UiSoundManager::playMenuButtonClick},
            {"IGMAINMENUCONTINUE",          &audio::UiSoundManager::playMenuButtonClick},
            {"GSTITLEOPTIONOK",             &audio::UiSoundManager::playButtonClick},
            {"GSTITLEOPTIONEXIT",           &audio::UiSoundManager::playButtonClick},
            // The panels, each of which has its own pair here already.
            {"IGCHARACTERINFOOPEN",         &audio::UiSoundManager::playCharacterSheetOpen},
            {"IGCHARACTERINFOCLOSE",        &audio::UiSoundManager::playCharacterSheetClose},
            {"TALENTSCREENOPEN",            &audio::UiSoundManager::playCharacterSheetOpen},
            {"TALENTSCREENCLOSE",           &audio::UiSoundManager::playCharacterSheetClose},
            // The guild vault, which was not silent: GuildVaultOpen and
            // GuildVaultClose are rows in SoundEntries.dbc, so playByName above
            // has been finding and playing them all along.
            //
            // What was missing is the fallback this table is for. Its two
            // samples were preloaded from a fixed path and had no method to
            // reach them, so an install with the wavs but no SoundEntries.dbc -
            // the case the table exists to cover - had them in memory and no
            // way to ask for them.
            {"GUILDVAULTOPEN",              &audio::UiSoundManager::playGuildBankOpen},
            {"GUILDVAULTCLOSE",             &audio::UiSoundManager::playGuildBankClose},
            {"IGBACKPACKOPEN",              &audio::UiSoundManager::playBagOpen},
            {"IGBACKPACKCLOSE",             &audio::UiSoundManager::playBagClose},
            {"KEYRINGOPEN",                 &audio::UiSoundManager::playBagOpen},
            {"KEYRINGCLOSE",                &audio::UiSoundManager::playBagClose},
            {"IGQUESTLOGOPEN",              &audio::UiSoundManager::playQuestLogOpen},
            {"IGQUESTLOGCLOSE",             &audio::UiSoundManager::playQuestLogClose},
            {"IGQUESTLISTOPEN",             &audio::UiSoundManager::playQuestActivate},
            {"IGQUESTLISTCOMPLETE",         &audio::UiSoundManager::playQuestComplete},
            {"IGQUESTLOGABANDONQUEST",      &audio::UiSoundManager::playQuestFailed},
            {"IGQUESTCANCEL",               &audio::UiSoundManager::playQuestFailed},
            {"WRITEQUEST",                  &audio::UiSoundManager::playQuestUpdate},
            {"IGSPELLBOOKOPEN",             &audio::UiSoundManager::playPickupBook},
            {"IGSPELLBOOKCLOSE",            &audio::UiSoundManager::playPickupBook},
            {"IGABILITYOPEN",               &audio::UiSoundManager::playPickupBook},
            {"IGABILITYCLOSE",              &audio::UiSoundManager::playPickupBook},
            {"IGABILIITYPAGETURN",          &audio::UiSoundManager::playPickupBook},
            {"AUCTIONWINDOWOPEN",           &audio::UiSoundManager::playAuctionHouseOpen},
            {"AUCTIONWINDOWCLOSE",          &audio::UiSoundManager::playAuctionHouseClose},
            // The rest, each with an exact counterpart.
            {"LEVELUPSOUND",                &audio::UiSoundManager::playLevelUp},
            {"MAPPING",                     &audio::UiSoundManager::playMinimapPing},
            {"TELLMESSAGE",                 &audio::UiSoundManager::playWhisperReceived},
            {"IGCHARACTERNPCSELECT",        &audio::UiSoundManager::playTargetSelect},
            {"IGCREATUREAGGROSELECT",       &audio::UiSoundManager::playTargetSelect},
            {"IGCREATURENEUTRALSELECT",     &audio::UiSoundManager::playTargetSelect},
            {"INTERFACESOUND_LOSTTARGETUNIT", &audio::UiSoundManager::playTargetDeselect},
            {"IGBACKPACKCOINSELECT",        &audio::UiSoundManager::playLootCoinSmall},
            {"IGBACKPACKCOINOK",            &audio::UiSoundManager::playLootCoinSmall},
            {"LOOTWINDOWOPENEMPTY",         &audio::UiSoundManager::playError},
            {"LFG_DENIED",                  &audio::UiSoundManager::playError},
            {"LFG_REWARDS",                 &audio::UiSoundManager::playQuestComplete},
        };
        bool mapped = false;
        for (const Mapping& m : kMappings) {
            if (sound == m.name) { (sfx->*m.play)(); mapped = true; break; }
        }
        // Said once per name, because a sound that resolves to nothing is the
        // one gap in this file that leaves no trace at all: the row is missing
        // from the dbc and the name is not in the table above, so the call
        // returns having done nothing and the panel is silent.
        //
        // Silence is the right answer - substituting a sound that merely
        // exists would be wrong forever, where a missing one can still be
        // heard - but it should be possible to find out which name it was
        // without guessing from a quiet button. The interface asks for eighty
        // nine of these across the files that load, and the table below covers
        // the ones the client has an equivalent for.
        if (!mapped) {
            static std::set<std::string> saidAlready;
            if (saidAlready.insert(sound).second) {
                LOG_WARNING("PlaySound: '", name, "' names no row this install "
                            "has and nothing here stands in for it, so it is "
                            "silent");
            }
        }
    }
    return 0;
}

/// PlaySoundFile(file) (0x009859b0): the file once on the SCRIPTSOUND
/// channel (0x004c9110(file, 4)). A second argument is not read. Returns 1.
///
/// One caller in the interface, on the login screen, and a great many in
/// addons: it is how an addon ships a sound of its own.
static int lua_PlaySoundFile(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: PlaySoundFile(\"soundfile\")");
    auto* svc = getLuaServices(L);
    auto* ac = svc ? svc->audioCoordinator : nullptr;
    auto* sfx = ac ? ac->getUiSoundManager() : nullptr;
    const char* path = lua_tostring(L, 1);
    if (sfx && path && *path) sfx->playFile(path);
    lua_pushnumber(L, 1);
    return 1;
}

/// PlayMusic(file) (0x00985950): the file looping on the SCRIPTMUSIC channel
/// (0x004c9110(file, 5)), and the zone music held off until StopMusic.
/// Returns 1.
static int lua_PlayMusic(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: PlayMusic(\"musicfile\")");
    auto* svc = getLuaServices(L);
    auto* ac = svc ? svc->audioCoordinator : nullptr;
    const char* path = lua_tostring(L, 1);
    if (ac && path) ac->playScriptMusic(path);
    lua_pushnumber(L, 1);
    return 1;
}

/// StopMusic() (0x00985a10 -> 0x004c8680): PlayMusic's file falls over half
/// a second and the zone music may start again. Returns nothing.
static int lua_StopMusic(lua_State* L) {
    auto* svc = getLuaServices(L);
    if (auto* ac = svc ? svc->audioCoordinator : nullptr) ac->stopScriptMusic();
    return 0;
}

/// GetPlayerMapPosition(unit) → where that unit is on the map now showing,
/// as a fraction across it.
///
/// Two faults, both silent. It answered raw world coordinates, and
/// worldmapframe.lua multiplies what it gets by WorldMapDetailFrame's width -
/// so the player arrow was placed some thousands of pixels off the parchment
/// rather than on it. And it ignored the unit it was asked about, so the four
/// party markers and the forty raid ones all described the player.
///
/// Zero and zero is the answer for "not on this map": every caller tests
/// `if ( x == 0 and y == 0 )` and hides its marker, which is what should happen
/// for someone in another zone.
static int lua_GetPlayerMapPosition(lua_State* L) {
    auto* gh = getGameHandler(L);
    auto* svc = getLuaServices(L);
    const char* uid = luaL_optstring(L, 1, "player");
    if (!gh || !svc || !svc->mapUVForWorldPos || !uid) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 2;
    }
    float wx = 0.0f, wy = 0.0f, wz = 0.0f;
    std::string unit(uid);
    toLowerInPlace(unit);
    if (unit == "player") {
        const auto& mi = gh->getMovementInfo();
        wx = mi.x; wy = mi.y; wz = mi.z;
    } else if (const uint64_t guid = resolveUnitGuid(gh, unit)) {
        // Through the entity when it is in range, and the group list when it is
        // not - a raid member across the zone is exactly who this is asked
        // about, and they have no entity here.
        if (auto e = gh->getEntityManager().getEntity(guid)) {
            wx = e->getX(); wy = e->getY(); wz = e->getZ();
        } else if (const auto* m = findPartyMember(gh, guid)) {
            // The group list carries a coarse position - SMSG_PARTY_MEMBER_STATS
            // truncates it to a pair of int16 yards - which is ample for a dot
            // on a zone map and is the only position there is for someone out
            // of range.
            wx = static_cast<float>(m->posX);
            wy = static_cast<float>(m->posY);
        }
    }
    float u = 0.0f, v = 0.0f;
    if ((wx == 0.0f && wy == 0.0f) || !svc->mapUVForWorldPos(wx, wy, wz, u, v)) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        return 2;
    }
    lua_pushnumber(L, u);
    lua_pushnumber(L, v);
    return 2;
}

// GetPlayerFacing() → radians (0 = north, increasing counter-clockwise)
static int lua_GetPlayerFacing(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) {
        // The movement info holds the canonical yaw; Lua is given the
        // server orientation, as the original client gives it.
        lua_pushnumber(L, core::coords::canonicalToPlayerFacing(
                              gh->getMovementInfo().orientation));
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

// GetCVar(name) → value string (stub for most, real for a few)
/// CVars the player or the interface has actually set, which win over the
/// defaults below.
///
/// SetCVar was a no-op, so every option the interface changed reverted the
/// instant it was read back: ticking a box in the interface options did
/// nothing, and any code that writes a CVar and then reads it to confirm - of
/// which FrameXML has a fair amount - saw its own write disappear.
static std::unordered_map<std::string, std::string>& cvarStore() {
    static std::unordered_map<std::string, std::string> store;
    return store;
}

/// The file as storedCVarValue last read it, for the CVars the store does not
/// hold: one never set is not in it, and asking for one read the whole file
/// every time - twice a frame for the nameplates alone, ~0.9 ms on Windows.
/// Dropped whenever this process loads or writes the file.
struct CVarFileSnapshot {
    std::unordered_map<std::string, std::string> values;
    bool valid = false;
};
static CVarFileSnapshot& cvarFileSnapshot() {
    static CVarFileSnapshot snapshot;
    return snapshot;
}

/// What each CVar held when this session read its file, kept apart from the
/// store because the store moves and this does not.
///
/// It is the client's "startup value": the string its cvar object holds at
/// +0x4c, which `cvar_reset` (0x00767840) puts back and `cvarlist` (0x00767220)
/// prints in parentheses. The client takes it from Config.wtf as each CVar is
/// registered and never refreshes it, so resetting the same CVar twice lands on
/// the same value however many times it was changed in between - and a name that
/// Config.wtf did not mention has no startup value at all, which is why the
/// reset falls back to the registered default rather than to nothing.
static std::unordered_map<std::string, std::string>& cvarStartupValues() {
    static std::unordered_map<std::string, std::string> values;
    return values;
}

/// Where the CVars live between runs.
///
/// Its own file rather than settings.cfg, which this client's own panel writes
/// whole from its own fields: a CVar written into that file would be dropped
/// the next time that panel saved.
static std::string cvarStorePath() {
    return core::getConfigRoot() + "/cvars.cfg";
}

/// Read the stored CVars back. Called once, before the interface loads,
/// because a panel reads its checkbox out of the CVar as it is built and
/// anything arriving later leaves the box disagreeing with the setting.
///
/// Storing them in memory alone made every interface option last exactly one
/// session. That is not a small thing: the equipment manager is off until its
/// box is ticked, the box writes equipmentManager, and the paperdoll reads that
/// on VARIABLES_LOADED - so it came back off on every login, and so did every
/// other option the player had set.
/// Whether loadStoredCVars has run in this process. A write before it - the
/// login screen's graphics sheet, a preset chosen before the interface is up -
/// would otherwise save a store holding only that one value over the file.
static bool& cvarStoreLoaded() {
    static bool loaded = false;
    return loaded;
}

static void loadStoredCVars() {
    cvarStoreLoaded() = true;
    cvarFileSnapshot().valid = false;
    std::ifstream in(cvarStorePath());
    if (!in.is_open()) return;
    std::string line;
    size_t loaded = 0;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string key = line.substr(0, eq);
        toLowerInPlace(key);
        cvarStore()[key] = line.substr(eq + 1);
        // The same string twice, for two questions that are not the same: what
        // the CVar is now, and what it was when the session started.
        cvarStartupValues()[key] = line.substr(eq + 1);
        ++loaded;
    }
    // Mouse Look Speed was stored in this client's own units until now: the
    // number written was the degrees-per-movement the camera was handed, so a
    // config from before the two were separated says 0.2 where the CVar means
    // 180. Read back as it stands it multiplies by 0.00125 again and the view
    // stops turning, so anything under the shipped slider's own minimum of 90
    // can only be the old scale and is converted - which keeps the rate that
    // config had rather than replacing it with this build's default. Not saved
    // from here: the conversion is the same every time it runs over that line,
    // and the next CVar change writes the store anyway.
    if (auto it = cvarStore().find("camerayawmovespeed"); it != cvarStore().end()) {
        const float stored = std::atof(it->second.c_str());
        if (stored > 0.0f && stored < 90.0f) {
            const std::string moved = ui::settingNumberText(stored / 0.00125);
            it->second = moved;
            cvarStartupValues()["camerayawmovespeed"] = moved;
        }
    }
    LOG_INFO("CVars: loaded ", loaded, " from ", cvarStorePath());
}

/// Write them back. Called on every change rather than at shutdown: the file is
/// a few hundred bytes, and a setting that survives only a clean exit is not
/// one a player can rely on.
static void saveStoredCVars() {
    const std::string path = cvarStorePath();
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path);
    if (!out.is_open()) {
        LOG_WARNING("Could not save CVars to ", path);
        return;
    }
    // Sorted, so a diff of the file reads as a change of settings rather than
    // as the hash order moving underneath it.
    std::vector<const std::pair<const std::string, std::string>*> rows;
    rows.reserve(cvarStore().size());
    for (const auto& kv : cvarStore()) rows.push_back(&kv);
    std::sort(rows.begin(), rows.end(),
              [](const auto* a, const auto* b) { return a->first < b->first; });
    for (const auto* kv : rows) {
        // A value with a newline in it would come back as two broken lines.
        if (kv->second.find('\n') != std::string::npos) continue;
        out << kv->first << '=' << kv->second << '\n';
    }
    cvarFileSnapshot().valid = false;
}

/// Tutorials the player has already been shown.
///
/// The stock client keeps these in account data, which this client is not
/// sent, so they ride in the CVar file beside everything else that has to
/// survive a login - one row, the ids comma separated. An in-memory set would
/// look finished and re-show every tutorial on the next login, which is the
/// shape this repository keeps producing.
///
/// TutorialFrame_NewTutorial refuses to queue a tutorial that is already
/// flagged, so an answer of "no" here means every tutorial fires again on
/// every login, and an unanswered call raises inside TutorialFrame_Update.
static const char* kTutorialCVar = "wowee_tutorialsFlagged";

static bool tutorialFlagged(int id) {
    const std::string& all = cvarStore()[kTutorialCVar];
    const std::string needle = std::to_string(id);
    size_t at = 0;
    while ((at = all.find(needle, at)) != std::string::npos) {
        const bool startOk = (at == 0) || all[at - 1] == ',';
        const size_t end = at + needle.size();
        const bool endOk = (end == all.size()) || all[end] == ',';
        if (startOk && endOk) return true;
        at = end;
    }
    return false;
}

/// Every flagged tutorial, in id order.
///
/// Stored in the order they were seen, which is not the order they are browsed
/// in: the next and previous buttons walk the ids, so the list is sorted here
/// rather than kept sorted, and a file hand-edited into any order still reads.
static std::vector<int> tutorialIds() {
    std::vector<int> ids;
    const std::string& all = cvarStore()[kTutorialCVar];
    size_t at = 0;
    while (at < all.size()) {
        const size_t comma = all.find(',', at);
        const std::string piece = all.substr(
            at, comma == std::string::npos ? std::string::npos : comma - at);
        if (!piece.empty()) {
            try { ids.push_back(std::stoi(piece)); } catch (...) {}
        }
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

static void flagTutorial(int id) {
    if (id <= 0 || tutorialFlagged(id)) return;
    std::string& all = cvarStore()[kTutorialCVar];
    if (!all.empty()) all += ',';
    all += std::to_string(id);
    saveStoredCVars();
}

/// A sound CVar's value, or the stock client's default for it.
///
/// The defaults matter as much as the store does. Sound_MasterVolumeUp reads
/// the CVar, runs it through tonumber and adds a step - with nothing stored and
/// no default it reads nil, the `if (volume)` guard below it fails, and the
/// volume keys do nothing at all rather than anything visible.
static float soundCVar(const char* key, float fallback) {
    if (auto it = cvarStore().find(key); it != cvarStore().end()) {
        try {
            return std::stof(it->second);
        } catch (const std::exception&) {
            return fallback;
        }
    }
    return fallback;
}

/// Push the sound CVars at the audio system, which is what makes them settings
/// rather than a record of what was clicked.
///
/// Every channel is recomputed from the store rather than from the one CVar
/// that changed, so enable and volume compose and the order they arrive in does
/// not matter - Sound_ToggleSound writes EnableSFX and EnableAmbience one after
/// the other, and the interface's options panel writes a volume and an enable
/// together on apply.
///
/// This client splits sound finer than the interface does: it has separate
/// volumes for combat, spells, movement, footsteps and the rest, where FrameXML
/// has one "sound effects". Turning SFX off and on again therefore levels those
/// channels rather than restoring them. That is the retail behaviour - there is
/// no finer control there to restore - and the client's own settings panel
/// re-applies its sliders whenever it is used, so neither owner is stuck with
/// the other's answer.
static void applySoundCVars(lua_State* L) {
    auto* svc = getLuaServices(L);
    auto* ac = svc ? svc->audioCoordinator : nullptr;
    if (!ac) return;

    // The sliders first, then the switches on top. Each switch is applied by
    // zeroing a channel and zeroing has no inverse, so without this the "on"
    // case wrote nothing at all: turning sound effects off and on again left
    // all nine effect channels silent until the client's own settings window
    // was touched. Recomputing from the store is what the note above says this
    // does, and now does.
    if (svc->reapplyAudioVolumes) svc->reapplyAudioVolumes();

    // Master, music and ambience are the client's own settings now, reached
    // through SetCVar above - this function must not write them too, or the two
    // fight and the last writer wins.
    //
    // What is left here are the three enable switches, which the client's panel
    // has no equivalent for. They multiply whatever the settings applied.
    // Music off is the MUSIC category muted and the music stopped, the slots
    // picking nothing until it is on again (0x004d1590, 0x004c88b0).
    // How many sounds are heard at once (0x0087c710 sets FMOD up with
    // Sound_NumChannels real voices); the rest play on silent.
    audio::AudioEngine::instance().setMaxRealVoices(
        static_cast<int>(soundCVar("sound_numchannels", static_cast<float>(audio::voice_priority::kDefaultRealVoices))));
    const bool musicOn = soundCVar("sound_enablemusic", 1.0f) != 0.0f;
    if (!musicOn) {
        if (auto* music = ac->getMusicManager()) music->setVolume(0);
    }
    ac->setMusicEnabled(musicOn);
    ac->setZoneMusicNoDelay(soundCVar("sound_zonemusicnodelay", 0.0f) != 0.0f);

    const bool ambOn = soundCVar("sound_enableambience", 1.0f) != 0.0f;
    if (!ambOn) {
        if (auto* ambient = ac->getAmbientSoundManager()) ambient->setVolumeScale(0.0f);
    }

    // The character's own voice lines for a refused action - "I can't do that
    // yet", "Not enough rage". PlayerVoiceManager has carried an enabled_ flag
    // all along and playError honours it; nothing ever set it, so the switch in
    // the Sound panel was a checkbox that moved and changed nothing.
    //
    // Not folded into the sfx switch below: this is the one people actually
    // reach for, and turning it off should leave every other effect alone.
    if (auto* voice = ac->getPlayerVoiceManager()) {
        voice->setEnabled(soundCVar("sound_enableerrorspeech", 1.0f) != 0.0f);
    }

    // The volume itself is a client setting now, reached through SetCVar, and
    // applied by applyAudioVolumes as one scale over the seven effect volumes
    // this client keeps separately. Only the switch is left here, and it zeroes
    // them on top of whatever that applied.
    const bool sfxOn = soundCVar("sound_enablesfx", 1.0f) != 0.0f;
    if (!sfxOn) {
        if (auto* m = ac->getUiSoundManager())        m->setVolumeScale(0.0f);
        if (auto* m = ac->getCombatSoundManager())    m->setVolumeScale(0.0f);
        if (auto* m = ac->getMovementSoundManager())  m->setVolumeScale(0.0f);
        if (auto* m = ac->getFootstepManager())       m->setVolumeScale(0.0f);
        if (auto* m = ac->getActivitySoundManager())  m->setVolumeScale(0.0f);
        if (auto* m = ac->getMountSoundManager())     m->setVolumeScale(0.0f);
        if (auto* m = ac->getNpcVoiceManager())       m->setVolumeScale(0.0f);
        if (auto* m = ac->getPlayerVoiceManager())    m->setVolumeScale(0.0f);
        // The Effects channel: a doodad's fire or a spell's sound, which
        // went on through the switch (0x004c6a40 refuses every SFX play).
        audio::AudioEngine::instance().setEffectsVolume(0.0f);
    }
}


/// The value a CVar has when nobody has set it.
///
/// Split out so GetCVarDefault can answer it. That was aliased straight to
/// GetCVar, which answers the *current* value - and every options panel
/// captures control.defaultValue from it as the panel loads, so the Defaults
/// button restored each control to whatever it had been when the panel was
/// opened. Nothing looked broken: the button worked, it just always agreed
/// with wherever the controls already were.

/// The eight camera views' cvars (registered by 0x005fd910):
/// "camera" + Distance, Pitch or Yaw + the view's suffix, and the defaults
/// the client registers them with (0x00ad1ba8); pitch and yaw in degrees.
constexpr const char* kCameraViewSuffix[8] = {"", "A", "B", "C", "D", "E", "Com", "Barber Shop"};
constexpr const char* kCameraViewPart[3] = {"Distance", "Pitch", "Yaw"};
constexpr const char* kCameraViewDefault[8][3] = {
    {"0.0", "0.0", "0.0"},   {"0.0", "0.0", "0.0"},    {"5.55", "10.0", "0.0"},
    {"5.55", "20.0", "0.0"}, {"13.88", "30.0", "0.0"}, {"13.88", "10.0", "0.0"},
    {"0.0", "0.0", "0.0"},   {"5.0", "10.0", "0.0"},
};

static std::string cameraViewCVar(int view, int part) {
    return std::string("camera") + kCameraViewPart[part] + kCameraViewSuffix[view];
}

/// A camera view cvar's registered default, or null for any other name.
static const char* cameraViewCVarDefault(const std::string& lowerName) {
    for (int view = 0; view < 8; ++view) {
        for (int part = 0; part < 3; ++part) {
            std::string name = cameraViewCVar(view, part);
            toLowerInPlace(name);
            if (name == lowerName) return kCameraViewDefault[view][part];
        }
    }
    return nullptr;
}

/// The CVars RegisterCVar made, by lower-case name, with the default each was
/// registered with ("0" when none was given, 0x0050fea0).
static std::unordered_map<std::string, std::string>& scriptRegisteredCVars() {
    static std::unordered_map<std::string, std::string> registered;
    return registered;
}

/// Client CVars RegisterCVar was called on again. Registering an existing CVar
/// rewrites its storage bits with the ones given (0x00767fc0 keeps all but
/// 0x30 and ors the new ones in), and RegisterCVar gives none - so such a CVar
/// is no longer kept on the server.
static std::unordered_set<std::string>& reregisteredClientCVars() {
    static std::unordered_set<std::string> names;
    return names;
}

/// `stated`, when given, comes back false for a name this chain does not name.
/// The "0" at the end of it is a stand-in for an unanswered CVar rather than a
/// value the client registered the CVar with, and the CVar listing has to tell
/// the two apart: it prints `(default "0")` only where the client actually holds
/// a default string on the object (+0x40 null means print nothing).
static void pushCvarDefault(lua_State* L, const std::string& n,
                            bool* stated = nullptr) {
    if (stated) *stated = true;
    // A CVar an addon registered has the default it was registered with.
    if (auto it = scriptRegisteredCVars().find(n); it != scriptRegisteredCVars().end()) {
        lua_pushstring(L, it->second.c_str());
        return;
    }
    // The camera's views, and which one it is on and how it moves to another
    // (0x005fd910: cameraView "2", cameraViewBlendStyle "1").
    if (const char* view = cameraViewCVarDefault(n)) { lua_pushstring(L, view); return; }
    if (n == "cameraview") { lua_pushstring(L, "2"); return; }
    if (n == "cameraviewblendstyle") { lua_pushstring(L, "1"); return; }
    // The frame caps as the client registers them (maxFPS "200", maxFPSBk
    // "30") - the defaults the frame loop reads them with.
    if (n == "maxfps") { lua_pushstring(L, "200"); return; }
    if (n == "maxfpsbk") { lua_pushstring(L, "30"); return; }
    // And the others the client reads where it uses them: no core mask, the
    // footprints on (00780000.c:10440 registers "1"), and a pet's and an
    // emote's sounds on - which the sound_enable rule below answers too, said
    // here by name so the two sides can be checked against each other.
    if (n == "processaffinitymask") { lua_pushstring(L, "0"); return; }
    if (n == "showfootprints") { lua_pushstring(L, "1"); return; }
    if (n == "sound_enableemotesounds" || n == "sound_enablepetsounds") { lua_pushstring(L, "1"); return; }
    // Return sensible defaults for commonly queried CVars
    // The sound ones read back as on and at full, which is what this client
    // starts as. Volume up/down step from whatever is read here, so answering
    // nothing leaves those keys inert rather than merely at a default.
    // Off, as the real client has it: alt-tabbing away silences the game.
    // Ahead of the sound_enable prefix below, which would otherwise answer it
    // "1" - that rule swallows every name it starts with, so a default of any
    // other value has to be stated before it rather than after.
    if (n == "sound_enablesoundwhengameisinbg") lua_pushstring(L, "0");
    else if (n.rfind("sound_enable", 0) == 0) lua_pushstring(L, "1");
    // Master at full, but the client registers its other three scales at
    // different levels (0x004d0000): music 0.4 and ambience 0.6 against master
    // and effects at 1.0. Answering full for all of them put the Sound panel's
    // Music and Ambience sliders at their top on a fresh profile and made the
    // zone's music as loud as the effects over it, which is what the original
    // does not do. The settings behind those two rows are the same numbers as
    // percentages; see the Sound page of the schema.
    else if (n == "sound_mastervolume") lua_pushstring(L, "1");
    else if (n == "sound_musicvolume") lua_pushstring(L, "0.4");
    else if (n == "sound_ambiencevolume") lua_pushstring(L, "0.6");
    // Both at their maximum, because this client has no lower setting to be
    // at. miniaudio mixes every voice it is given at the device's own rate:
    // there is no channel cap to raise and no quality tier to pick. Falling
    // through to the "0" at the end of this chain parked both sliders at the
    // far left of the Sound panel - 32 channels and Low quality - which read
    // as a client running at its worst and was not a setting at all.
    //
    // The controls are taken off the panels; see kRemovedControlsLua.
    // On by default (0x004d1600 registers it "1"): a spell effect's model
    // sound on the active player plays at the listener (M2EventSounds).
    else if (n == "sound_listeneratcharacter") lua_pushstring(L, "1");
    // On (0x00747860 registers it "1"): every unit's footfalls play
    // (0x00747240).
    else if (n == "footstepsounds") lua_pushstring(L, "1");
    // The client registers both (0x007f3a10, 0x0078e400): violenceLevel at
    // its locale's limit, 2 here, which shows a spell's first SpellVisual
    // (0x007fa290); spellEffectLevel ("Spell Detail") at 9, which drops all
    // of a kit's falling models (0x007fb5b0).
    else if (n == "violencelevel") lua_pushstring(L, "2");
    else if (n == "spelleffectlevel") lua_pushstring(L, "9");
    // On (0x0051d9b0): UnitPower and the cast read the predicted power.
    else if (n == "predictedpower") lua_pushstring(L, "1");
    // 0x004d0000's registration: "32" (0x009e21dc), the real voices the
    // sound engine is set up with (audio/voice_priority.hpp).
    else if (n == "sound_numchannels") lua_pushstring(L, "32");
    // As registered ("1"); this client mixes every voice it is given, so which
    // quality tier the original would have asked its backend for changes
    // nothing here - it is answered because cvarlist prints it.
    else if (n == "sound_outputquality") lua_pushstring(L, "1");
    else if (n == "uiscale") lua_pushstring(L, "1");
    // The tracking menu's own entry, by name; none (0x0057e070).
    else if (n == "minimaptrackedinfo") lua_pushstring(L, "");
    // Off, as registered (0x00520000 "0", setter FUN_005237e0): the scale the
    // interface uses is the one its own code picks for the resolution, and the
    // "Use UI Scale" checkbox is what makes uiscale override that.
    else if (n == "useuiscale") lua_pushstring(L, "0");
    else if (n == "screenwidth" || n == "gxresolution") {
        auto* svc = getLuaServices(L);
        auto* win = svc ? svc->window : nullptr;
        lua_pushstring(L, std::to_string(win ? win->getWidth() : 1920).c_str());
    } else if (n == "screenheight" || n == "gxfullscreenresolution") {
        auto* svc = getLuaServices(L);
        auto* win = svc ? svc->window : nullptr;
        lua_pushstring(L, std::to_string(win ? win->getHeight() : 1080).c_str());
    // Both nameplate switches start off (0x00511xxx); V and Shift-V turn them
    // on.
    } else if (n == "nameplateshowfriends") lua_pushstring(L, "0");
    else if (n == "nameplateshowenemies") lua_pushstring(L, "0");
    else if (n == "sound_enablesfx") lua_pushstring(L, "1");
    else if (n == "sound_enableerrorspeech") lua_pushstring(L, "1");
    // On with two processors or more (0x004d1600 registers it by 0x0086b240).
    else if (n == "sound_enabledspeffects") lua_pushstring(L, "1");
    // One, not zero: the slider is a multiple of the original client's limit
    // and zero is not a position on it. Answering zero pinned it at minimum.
    else if (n == "cameradistancemaxfactor") lua_pushstring(L, "1");
    else if (n == "weatherdensity") lua_pushstring(L, "2");
    else if (n == "particledensity") lua_pushstring(L, "1");
    else if (n == "environmentdetail") lua_pushstring(L, "1");
    // The top of the slider: this client has always drawn at 16x, so anything
    // less would be a quality setting the player never asked for.
    else if (n == "texturefilteringmode") lua_pushstring(L, "5");
    else if (n == "groundeffectdist") lua_pushstring(L, "140");
    // Level 3 is 4096, which is the shadow map this client drew before the
    // setting existed. Answering the size it has always used keeps a player who
    // never touches this from being quietly downgraded by it appearing.
    else if (n == "extshadowquality") lua_pushstring(L, "3");
    // Clicking open ground clears the target, which is the real client's
    // behaviour and this one's. Without saying so it fell to the generic zero,
    // and zero here means sticky targeting - so the checkbox would have shown
    // itself ticked while the client went on clearing, the panel and the game
    // disagreeing about the same switch.
    else if (n == "deselectonclick") lua_pushstring(L, "1");
    // "Unit shadow LOD": 1, the blob shadows under the units, as the client
    // registers it (0x007e4a40); 0 turns them off.
    else if (n == "shadowlod") lua_pushstring(L, "1");
    // On, which is what this client has always done and what the real one
    // defaults to. Unset it fell to zero, so the checkbox would have shown the
    // numbers switched off while they were drawn.
    else if (n == "enablecombattext") lua_pushstring(L, "1");
    // Damage and healing on, which is what this client drew before either was
    // read; unset they fell to zero, which would have shown the boxes unticked
    // while the numbers were on screen. Dodges, parries and misses are the one
    // of the three the client registers off (0x00510000), so a fresh profile
    // does not say why a swing missed until the player asks it to.
    else if (n == "fctdamage" || n == "fcthealing") lua_pushstring(L, "1");
    else if (n == "fctdodgeparrymiss") lua_pushstring(L, "0");
    // Names over players on both sides are on; over creatures and critters
    // off (0x007e6150).
    else if (n == "unitnameenemyplayername" || n == "unitnamefriendlyplayername")
        lua_pushstring(L, "1");
    else if (n == "unitnamenpc" || n == "unitnamenoncombatcreaturename")
        lua_pushstring(L, "0");
    // Locked, as the client registers it (0x00510000 "1"). Interface Options
    // declares 0 for the same name in its uvarInfo table, but that table never
    // writes the CVar on load - InterfaceOptionsFrame_LoadUVars only calls
    // SetCVar when the player has already changed the global - so the engine's
    // registration is what a fresh account runs on, and a player who wants the
    // bars free unlocks them.
    else if (n == "lockactionbars") lua_pushstring(L, "1");
    // Off, as registered. The level is this client's own line to draw and it
    // has always drawn it; the row stays in the schema, and the original's
    // fresh profile has the checkbox unticked.
    else if (n == "showitemlevel") lua_pushstring(L, "0");
    // Off: trades arrive as they always have unless the player says otherwise.
    else if (n == "blocktrades") lua_pushstring(L, "0");
    // On, which is what the aura icons have always drawn.
    else if (n == "buffdurations") lua_pushstring(L, "1");
    // Off, which is the behaviour there has been: changing target leaves the
    // swing running and it follows to whoever is selected next.
    else if (n == "stopautoattackontargetchange") lua_pushstring(L, "0");
    // On, which is what has always been printed.
    else if (n == "showlootspam") lua_pushstring(L, "1");
    // On, as the real client has it: coming back and talking takes the flag
    // off rather than leaving it for the player to notice.
    else if (n == "autoclearafk") lua_pushstring(L, "1");
    // Off, as registered (0x00510000): a guild member arriving or leaving is
    // announced only to the player who asked for it.
    else if (n == "guildmembernotify") lua_pushstring(L, "0");
    // Off, as the real client has it: attacking an ally does nothing unless
    // the player has asked for it to mean assist.
    else if (n == "assistattack") lua_pushstring(L, "0");
    // On, as registered (0x00510000 "1"): losing the top of the threat list
    // makes a noise unless the player has turned that off.
    else if (n == "threatplaysounds") lua_pushstring(L, "1");
    // Two that were relying on the generic zero rather than saying so. Both
    // want off, so the behaviour was right - but by coincidence, and the rule
    // this file keeps proving is that a value nobody registers is a value
    // nobody has checked. Stated, they agree by construction.
    else if (n == "autodismountflying") lua_pushstring(L, "0");
    // On, as the client registers it (0x0051d9b0: autoDismount "1").
    else if (n == "autodismount") lua_pushstring(L, "1");
    // On, as the client registers them (0x0051d9b0: autoStand and
    // autoUnshift "1"), which 0x00739650 reads.
    else if (n == "autostand") lua_pushstring(L, "1");
    else if (n == "autounshift") lua_pushstring(L, "1");
    else if (n == "colorblindmode") lua_pushstring(L, "0");
    // On, as registered (0x00510000 "1" - "Whether you should be protected
    // against accidentally double-clicking an aura"). The client's own version
    // of that protection is the half-second guard on the Attack button, so a
    // fresh profile has the guard up and the row unticks it.
    else if (n == "secureabilitytoggle") lua_pushstring(L, "1");
    // A quest whose progress changes starts being watched (autoQuestWatch "1"),
    // but quest titles on the world map are not coloured by difficulty until the
    // player says so (mapQuestDifficulty registered "0", 0x00510000).
    else if (n == "mapquestdifficulty") lua_pushstring(L, "0");
    else if (n == "autoquestprogress") lua_pushstring(L, "1");
    else if (n == "consolidatebuffs") lua_pushstring(L, "0");
    else if (n == "watchframewidth") lua_pushstring(L, "0");
    // Nameplates and names for totems: plates on both sides, names on
    // neither (0x00511xxx, 0x007e6150).
    else if (n == "nameplateshowenemytotems") lua_pushstring(L, "1");
    else if (n == "nameplateshowfriendlytotems") lua_pushstring(L, "1");
    else if (n == "unitnameenemytotemname") lua_pushstring(L, "0");
    else if (n == "unitnamefriendlytotemname") lua_pushstring(L, "0");
    // Reaction, not class, is what a world-space bar is for here; the setting
    // is offered and honoured, but green stays the default.
    else if (n == "showclasscolorinnameplate") lua_pushstring(L, "0");
    // The Combat Text panel's own filters, each as 0x00510000 registers it:
    // spell mechanics on the player's own target registered "0" as are the
    // same events on anyone else, so the line saying a swing was absorbed or
    // reflected is opt-in at both ends.
    else if (n == "combatdamage") lua_pushstring(L, "1");
    else if (n == "combathealing") lua_pushstring(L, "1");
    else if (n == "combatlogperiodicspells") lua_pushstring(L, "1");
    else if (n == "petmeleedamage") lua_pushstring(L, "1");
    else if (n == "fctspellmechanics") lua_pushstring(L, "0");
    else if (n == "fctspellmechanicsother") lua_pushstring(L, "0");
    // Camera Following Style and Camera Following Speed (degrees a second),
    // at the client's own defaults as wow.exe registers them: "4"
    // (PTR_DAT_00ad1c2c) and "180" (0x00a1e7a8). Stored and answered back
    // like any other CVar; nothing here reads either.
    else if (n == "camerasmoothstyle") lua_pushstring(L, "4");
    else if (n == "camerayawsmoothspeed") lua_pushstring(L, "180");
    // The same group's other rates, and the window a view blend is held to.
    // These are not left for the generic stand-in because this client does
    // read them: SetView's blend times itself by distance /
    // cameraDistanceSmoothSpeed and pitch / cameraPitchSmoothSpeed and then
    // clamps to cameraSmoothTimeMin and cameraSmoothTimeMax (0x00603330 ->
    // 0x00600e00, 0x00601190), and MoveView{In,Out,Up,Down}Start move at
    // cameraDistanceMoveSpeed yards and cameraPitchMoveSpeed degrees a second
    // (0x005ff080 ff.). Each of those reads names the same fallback the client
    // registers, so the answer has to be that number or GetCVar shows one
    // value while the camera moves at another.
    else if (n == "cameradistancesmoothspeed") lua_pushstring(L, "8.33");
    else if (n == "camerapitchsmoothspeed") lua_pushstring(L, "45");
    else if (n == "camerasmoothtimemin") lua_pushstring(L, "0.1");
    else if (n == "camerasmoothtimemax") lua_pushstring(L, "2.0");
    else if (n == "cameradistancemovespeed") lua_pushstring(L, "8.33");
    else if (n == "camerapitchmovespeed") lua_pushstring(L, "90");
    // Whether a view the player has moved still blends (Controls > "Camera
    // Following Style"'s second style). Registered "0" like the rest of the
    // group, and read by refreshViewSmoothing, so it is said here for the same
    // reason as the rates above - the read and the answer are one number.
    else if (n == "cameracustomviewsmoothing") lua_pushstring(L, "0");
    // Titles on player names, as the real client shows them.
    else if (n == "unitnameplayerpvptitle") lua_pushstring(L, "1");
    // Cast bars over enemy nameplates are on, and so are bubbles over the
    // party's heads (0x00510000 registers chatBubblesParty "1" beside
    // chatBubbles "1"). The row unticks them for a player who does not want
    // their raid's conversation floating over the boss.
    else if (n == "showvkeycastbar") lua_pushstring(L, "1");
    else if (n == "chatbubblesparty") lua_pushstring(L, "1");
    // Clicking anywhere in a chat window starts typing, on, as 0x00510000
    // registers it. Nothing in the C++ reads the name: the shipped FrameXML
    // does, and only while chatStyle is "im" - the always-visible chat box - so
    // this is one of the rows whose whole effect is in the interface's own
    // click handling. Answered because the Chat page has the row on the CVar,
    // and a row showing "off" over a game that behaves as "on" is the kind of
    // disagreement this file keeps having to fix.
    else if (n == "wholechatwindowclickable") lua_pushstring(L, "1");
    // Pets and guardians plated on both sides; pets named, guardians not
    // (0x00511xxx, 0x007e6150).
    else if (n == "nameplateshowenemypets") lua_pushstring(L, "1");
    else if (n == "nameplateshowfriendlypets") lua_pushstring(L, "1");
    else if (n == "nameplateshowenemyguardians") lua_pushstring(L, "1");
    else if (n == "nameplateshowfriendlyguardians") lua_pushstring(L, "1");
    else if (n == "unitnameenemypetname") lua_pushstring(L, "1");
    else if (n == "unitnamefriendlypetname") lua_pushstring(L, "1");
    else if (n == "unitnameenemyguardianname") lua_pushstring(L, "0");
    else if (n == "unitnamefriendlyguardianname") lua_pushstring(L, "0");
    // Guild names over players are shown; your own name over your own head is
    // not, both as the real client has them.
    else if (n == "unitnameplayerguild") lua_pushstring(L, "1");
    else if (n == "unitnameown") lua_pushstring(L, "0");
    // Plates may overlap; "0" is the old never-overlapping placement
    // (0x00511xxx registers it "1").
    else if (n == "nameplateallowoverlap") lua_pushstring(L, "1");
    // Both on, as registered (0x00510000): spam is filtered and so is mature
    // language, each behind a checkbox worded as a switch-off.
    else if (n == "spamfilter") lua_pushstring(L, "1");
    else if (n == "profanityfilter") lua_pushstring(L, "1");
    // Off, as it ships: a zone track stops at its end and the next one starts
    // when the zone asks for it.
    else if (n == "sound_zonemusicnodelay") lua_pushstring(L, "0");
    // Mouse Look Speed, counted the way the client counts it. It registers 180,
    // and the view turns 0.00125 degrees per movement of the mouse for each
    // degree of that number (0x006020b0 - which first divides the two deltas by
    // a factor pair built from the resolution's own ratio, 0x0047c020 and
    // 0x0047bf90 reading 0x0050f7c0, which sscanf's gxResolution). This client
    // keeps one sensitivity number, measured in degrees per movement, so the
    // kClientCVars entry below carries the 0.00125 between the two and the
    // setting behind it starts at 180 * 0.00125 = 0.225.
    //
    // Answering the setting's own number here instead - which is what this line
    // used to do - made the Mouse Look Speed slider offer this client's scale
    // rather than its own, and left MoveViewLeft/Right reading the same name as
    // degrees a second: a player who had ever touched the slider got a view that
    // turned 0.2 degrees a second.
    else if (n == "camerayawmovespeed") lua_pushstring(L, "180");
    // Joined by default: 0x00510000 registers it "1" and gives it a setter
    // (0x00512750), because the channel is how a guild finds its next member -
    // a player in no guild is in it from the first login and leaves by unticking
    // the row, which is what that setter is for.
    else if (n == "guildrecruitmentchannel") lua_pushstring(L, "1");
    // Combat log entries are kept 300 seconds unless set (0x0074d9e0).
    else if (n == "combatlogretentiontime") lua_pushstring(L, "300");

    // The uvarInfo table's own defaults, for the CVars it names that nothing
    // here answered. An unanswered CVar is not "off": GetCVar gives back
    // nothing, which is neither "0" nor "1", so InterfaceOptionsFrame_LoadUVars
    // compares it against the default, finds them unequal, and takes the arm
    // meant for a value the player has changed - on the very first login,
    // before anyone has touched the panel.
    //
    // The interface's uvarInfo table names these CVars and gives each a
    // default of its own, and for several of them that default is not what the
    // client registers (lockActionBars, lootUnderMouse, questFadingDisable,
    // displayWorldPVPObjectives...). The engine's registration is the one a
    // fresh profile runs on: InterfaceOptionsFrame_LoadUVars compares
    // GetCVar against the table's default and only calls SetCVar when the
    // player has already changed the global, so on the first login nothing in
    // FrameXML writes any of these. What follows is therefore the registered
    // string, not the table's, and where the two differ the table is answered
    // by that comparison rather than by GetCVar.
    else if (n == "alwaysshowactionbars") lua_pushstring(L, "0");
    else if (n == "autoquestwatch") lua_pushstring(L, "1");
    else if (n == "combattextfloatmode") lua_pushstring(L, "1");
    else if (n == "displayworldpvpobjectives") lua_pushstring(L, "1");
    else if (n == "fctauras") lua_pushstring(L, "0");
    else if (n == "fctcombatstate") lua_pushstring(L, "0");
    else if (n == "fctcombopoints") lua_pushstring(L, "0");
    else if (n == "fctdamagereduction") lua_pushstring(L, "0");
    else if (n == "fctenergygains") lua_pushstring(L, "0");
    else if (n == "fctfriendlyhealers") lua_pushstring(L, "0");
    else if (n == "fcthonorgains") lua_pushstring(L, "0");
    else if (n == "fctlowmanahealth") lua_pushstring(L, "1");
    else if (n == "fctperiodicenergygains") lua_pushstring(L, "0");
    else if (n == "fctreactives") lua_pushstring(L, "0");
    else if (n == "fctrepchanges") lua_pushstring(L, "0");
    else if (n == "hidepartyinraid") lua_pushstring(L, "0");
    else if (n == "lootundermouse") lua_pushstring(L, "1");
    else if (n == "questfadingdisable") lua_pushstring(L, "1");
    else if (n == "removechatdelay") lua_pushstring(L, "0");
    else if (n == "showarenaenemyframes") lua_pushstring(L, "1");
    else if (n == "showarenaenemycastbar") lua_pushstring(L, "1");
    else if (n == "showarenaenemypets") lua_pushstring(L, "1");
    else if (n == "showpartybackground") lua_pushstring(L, "0");
    else if (n == "showpartypets") lua_pushstring(L, "1");
    else if (n == "showtargetoftarget") lua_pushstring(L, "0");
    else if (n == "targetoftargetmode") lua_pushstring(L, "5");
    else if (n == "sound_enablemusic") lua_pushstring(L, "1");
    else if (n == "chatbubbles") lua_pushstring(L, "1");
    // Off, which is what a stock client has and what interfaceoptionsframe.lua
    // itself declares as the default. Only reached before the handler exists;
    // once it does, the branch above answers from the setting itself.
    else if (n == "autolootdefault") lua_pushstring(L, "0");
    // On, as it is for a fresh account. The XP bar and the unit frames put
    // their whole tooltip behind this one: GameTooltip_AddNewbieTip is called
    // with noNormalText set, so with tips off it does nothing at all and
    // hovering the experience bar says nothing.
    else if (n == "shownewbietips") lua_pushstring(L, "1");
    // Off, which is what the real client ships and is a decision here rather
    // than an accident. GearManagerDialog_OnEvent shows GearManagerToggleButton
    // only `if ( GetCVarBool("equipmentManager") )`, and that button is the only
    // way onto the character sheet's gear manager - so this looks at first like
    // an unanswered CVar hiding a feature this client implements in full.
    //
    // It is not hidden, it is opt-in, exactly as in 3.3.5: Interface >
    // Features > Use Equipment Manager is a real panel here, its checkbox
    // writes this CVar, the store answers ahead of these defaults, and the
    // button appears on the next VARIABLES_LOADED. Turning it on by default
    // would be this client deciding something the real one leaves to the
    // player.
    //
    // Answered rather than left silent so the reasoning is on record: reading
    // as off by accident and reading as off on purpose look identical from
    // Lua and are not the same thing to whoever reads this next.
    else if (n == "equipmentmanager") lua_pushstring(L, "0");
    // Off, as 0x00510000 registers it - "Show only Debuffs that the player can
    // dispel. Only applies to raids." Interface Options declares 1 for it in
    // uvarInfo, but that table never writes the CVar on load (see the note
    // above its own block), so the registration decides what a fresh account
    // runs: every debuff is listed until a raider ticks the box.
    //
    // Answered only now that it drives something. UnitDebuff took the "RAID"
    // filter FrameXML passes and ignored it, so turning this on before would
    // have claimed a filter that does not filter - which is worse than the
    // wrong default, because it reads as working.
    else if (n == "showdispeldebuffs") lua_pushstring(L, "0");
    // Off, also as declared. It asks the same "RAID" filter of *buffs* - only
    // the ones this character could cast - and that half is not implemented,
    // so this stays where the real client leaves it rather than being turned
    // on into a filter that would not filter.
    else if (n == "showcastablebuffs") lua_pushstring(L, "0");
    // The numbers on a unit frame's bars. Off, as 0x00510000 registers each of
    // the four: a stock client writes them only while the mouse is over the
    // bar, and the four Status Text checkboxes are what write them permanently.
    // They were answered "1" here for a while, which showed the numbers from
    // the first login - the same kind of switch that previewTalents and
    // showKeyring had, and the same reason to leave it to the player.
    // The unit frames each ask about their own, not about "statusText" - the
    // player frame's bars carry cvar = "playerStatusText" - so answering only
    // the general one leaves every bar's numbers hidden, correct text and all.
    else if (n == "statustext" || n == "playerstatustext" ||
             n == "targetstatustext" || n == "petstatustext" ||
             n == "partystatustext") {
        lua_pushstring(L, "0");
    }
    else if (n == "statustextpercentage") lua_pushstring(L, "0");
    // The game's own red edge, on: 0x00510000 registers "1". Nothing in the C++
    // reads this one - it is the shipped FrameXML that does. playerframe.lua
    // calls CombatFeedback_StartFullscreenStatus() as PLAYER_REGEN_DISABLED
    // goes out and StopFullscreenStatus as PLAYER_REGEN_ENABLED does, and those
    // two are combatfeedback.lua calling LowHealthFrame_StartFlashing(0.7, 0.7)
    // and LowHealthFrame_StopFlashing() on WorldFrame's own LowHealthFrame. So
    // with this answered "0" - which the generic stand-in did - a player stood
    // in combat at half health with no edge at all, and the interface's own
    // checkbox for it had never been touched. This client's low-health pulse is
    // a separate setting and stays as it was; the game's flash is not it.
    else if (n == "screenedgeflash") lua_pushstring(L, "1");
    // Which stat category each column of the character sheet shows. These are
    // not preferences with a sensible fallback - UpdatePaperdollStats compares
    // the value against five names and fills the column from whichever matches,
    // so an unrecognised one matches nothing and every row is left blank. That
    // is what "0" gave it, and it is why the character sheet showed two empty
    // panels below the model with no error anywhere to say why: the code ran to
    // completion and simply had nothing to write.
    //
    // The pair registered as the empty string (0x00510000), which paperdollframe
    // notices on load and replaces - `if ( GetCVar("playerStatLeftDropdown") ==
    // "" ... )` then SetCVar to base stats and melee combat. Answering the two
    // names here skips that write and lands on the same sheet the original ends
    // up with; the original's Defaults button would put "" back and FrameXML
    // would write the names again.
    else if (n == "playerstatleftdropdown")  lua_pushstring(L, "PLAYERSTAT_BASE_STATS");
    else if (n == "playerstatrightdropdown") lua_pushstring(L, "PLAYERSTAT_MELEE_COMBAT");
    // Whether a conversation opens in its own window or in the chat frame -
    // registered "popout" (0x00510000 gives the string straight to the
    // registrar, with "popout" and "inline" named in its help text). "0"
    // behaved as "inline", because the only test is against "popout", so this
    // is the first time the name is answered at all.
    else if (n == "conversationmode") lua_pushstring(L, "popout");
    // Who last spoke to you as a GM, and empty means nobody has.
    //
    // uiparent.lua does `if ( lastTalkedToGM ~= "" )` at login and, when that
    // passes, loads Blizzard_GMChatUI and *shows* it with a "your last session"
    // line. Falling through to "0" made that test pass every time, so the GM
    // chat window opened on every login for a player no GM had ever contacted.
    //
    // The empty string is not a placeholder here - it is the value the client
    // stores until a GM actually writes.
    else if (n == "lasttalkedtogm") lua_pushstring(L, "");
    // Off, as WotLK registers it (0x00510000, with the setter LAB_005127d0).
    // The staging flow behind it is all built - AddPreviewTalentPoints stages
    // against the real max rank, GetGroupPreviewTalentPointsSpent totals the
    // staging map, LearnPreviewTalents sends one request per rank,
    // ResetGroupPreviewTalentPoints clears it and fires the event the frame
    // redraws on, and GetTalentInfo answers previewRank - so the feature is
    // there for the player who ticks the row; the original leaves the ticking
    // to them, on the talent sheet's own "Talent Preview" checkbox.
    else if (n == "previewtalents") lua_pushstring(L, "0");
    // On, as registered (chatMouseScroll "1"). The chat frame only calls
    // EnableMouseWheel(true) inside that test, so "0" was not a quiet
    // preference - it was the wheel doing nothing over chat.
    else if (n == "chatmousescroll") lua_pushstring(L, "1");
    // Off, as registered ("0", 0x00510000): the keyring button beside the bags
    // is opt-in behind the Filters dropdown, both in the original and here.
    else if (n == "showkeyring")     lua_pushstring(L, "0");
    // The quest tracker's filter, and it is not a preference - it is a bitmask
    // fed to bit.band. watchframe.lua starts it at 0 on load and then, on
    // VARIABLES_LOADED, overwrites it with tonumber(GetCVar("trackerFilter")).
    // An unanswered CVar makes that nil, and bit.band(nil, x) raises: the
    // tracker worked until the login event that is supposed to configure it,
    // then went down on its next update, every session.
    //
    // Seven, the default the client registers it with (0x00510000 area,
    // "trackerFilter" -> 0x00a01c0c "7"): achievements, completed quests and
    // quests from other zones. With the last bit clear watchframe.lua keeps
    // only the quests CURRENT_MAP_QUESTS holds, which WatchFrame_GetCurrentMapQuests
    // builds from QuestMapUpdateAllQuests and QuestPOIGetQuestIDByVisibleIndex -
    // the client's POI icon list, not the log's zone headers.
    else if (n == "trackerfilter") lua_pushstring(L, "7");
    // The trainer's service type filter (0x00bd09e4): available and
    // unavailable shown, used hidden.
    else if (n == "servicetypefilter") lua_pushstring(L, "3");
    // Quest POI marks of completed quests fanned out by this much
    // (0x005e71a0: "POIShiftComplete", 0x009f3548 "0.6").
    else if (n == "poishiftcomplete") lua_pushstring(L, "0.6");
    // Manual, which is WATCHFRAME_SORT_MANUAL. Only ever compared with ==, so
    // nil was survivable here - it is answered for the same reason its
    // neighbour is, and because the sort menu's ticks read it.
    else if (n == "trackersorting") lua_pushstring(L, "0");
    // The narrow tracker, which is what a stock client has.
    // WatchFrame_SetWidth tests `width == "0"` and takes the wide branch for
    // anything else, so nil quietly chose the wide one.
    else if (n == "watchframewidth") lua_pushstring(L, "0");
    // Opaque. WorldMapFrame_SetOpacity computes 0.5 + (1.0 - opacity) * 0.5,
    // which raises on a nil - and WorldMap_ToggleSizeDown calls it, so putting
    // the map into windowed mode took it down. Zero is opaque here: the
    // arithmetic reads the value as how transparent to be.
    else if (n == "worldmapopacity") lua_pushstring(L, "0");
    // Half, as the arena frames have it. The same arithmetic-on-nil shape,
    // reached only if the arena addon loads, which is why it is a default
    // rather than a fix.
    else if (n == "partybackgroundopacity") lua_pushstring(L, "0.5");
    // Full volume and sound on, which is what a fresh client has. These are
    // read as numbers by the sound options, where zero reads as silence
    // rather than as "unset".
    else if (n == "sound_mastervolume")   lua_pushstring(L, "1");
    else if (n == "sound_enableallsound") lua_pushstring(L, "1");
    // On, as a stock client has it. ActionButton_SetTooltip branches on this:
    // with it off the tooltip is anchored to the right of the button itself, so
    // an action bar tooltip appeared at the bottom of the screen across the
    // icons. On, it goes through GameTooltip_SetDefaultAnchor to the
    // bottom-right corner, clear of the bar, which is where WoW puts it.
    else if (n == "ubertooltips") lua_pushstring(L, "1");
    // The target's cast bar, which a stock client shows. targetframe.lua tests
    // this as `if ( GetCVar("showTargetCastbar") == "0" )` and sets
    // showCastbar false - and an unrecognised name answers exactly "0" here,
    // so that branch was taken every time and the target frame is one of the
    // elements FrameXML draws by default. Nothing about it looked broken: the
    // bar was switched off by a setting nobody had touched.
    else if (n == "showtargetcastbar") lua_pushstring(L, "1");
    // Quest markers on the world map, on in a stock WotLK client. The
    // objectives checkbox reads this for its initial state, so "0" started it
    // unchecked and the markers off.
    else if (n == "questpoi") lua_pushstring(L, "1");
    // The aggro warning on the unit frames. Three is "always", which is what
    // the Display panel lists first and what a stock WotLK client ships with;
    // one is "in an instance", two "in a party", zero never. Falling through
    // to the "0" below would have left the indicator off with the option
    // reading Never, which is a preference nobody chose.
    else if (n == "threatwarning") lua_pushstring(L, "3");
    // Only reached with no client behind the call, because GetCVar answers this
    // one from the "Chat box always visible" setting. A word, not a number: the
    // social options panel branches on it and raises on anything it does not
    // recognise, so the blanket "0" below took its whole update down.
    //
    // "im" keeps the box on screen - ChatEdit_SetDeactivated hides it only for
    // "classic", and otherwise leaves it at a third alpha with its text
    // cleared, which is a box you can click into. That is the whole of what
    // this switch does, and it is why the setting is phrased as visibility
    // rather than as a style.
    else if (n == "chatstyle") lua_pushstring(L, "im");
    // "none", which is the word this one is switched off with - and the blanket
    // default below is a number, which is not off but a format string.
    //
    // This is not a preference that read wrong. Timestamps are held as the
    // strftime format to print, and the interface tests the *word* "none" for
    // off: anything else is truthy and gets printed. So the fallback "0" became
    // CHAT_TIMESTAMP_FORMAT = "0", strftime copied that digit through as a
    // literal, and every line of chat in the game arrived with a 0 stuck to the
    // front of it.
    //
    // Worth remembering when adding a CVar here: the fallback answers "0" for
    // everything unlisted, which is right for a flag and wrong for any setting
    // whose value is a word.
    else if (n == "showtimestamps") lua_pushstring(L, "none");
    // The selection circle under the target and the highlight circle under the
    // mouseover unit, both registered on (0x00740000 "ObjectSelectionCircle"
    // &DAT_009e1464 = "1"; 0x00510000 "unitHighlights" "1") and both drawn by
    // this client regardless: GameScreen's target circle is unconditional
    // (0x00743ec0, "while ObjectSelectionCircle is on - its default"), and the
    // unit highlight follows the mouseover whenever the interface is shown,
    // which here is always. Falling through to "0" left either control reading
    // off while the circle was on screen.
    else if (n == "objectselectioncircle") lua_pushstring(L, "1");
    else if (n == "unithighlights") lua_pushstring(L, "1");
    // Helpful spells self-cast when nothing friendly is targeted, as the client
    // registers it (0x00510000 "autoSelfCast" "1") and as this client does:
    // GameHandler::autoSelfCast_ starts true and GetCVar answers "1" from it,
    // so only GetCVarDefault was left on "0" - the value the Combat panel's
    // Defaults button would have restored, turning off a feature that is on.
    else if (n == "autoselfcast") lua_pushstring(L, "1");
    // Offline guild members are listed, as the client registers it (0x00510000
    // "guildShowOffline" "1") and as this client's roster shows them
    // (guildShowOffline() starts true, and the roster it is given holds
    // everyone); the default answered "0" against a list that shows them.
    else if (n == "guildshowoffline") lua_pushstring(L, "1");
    // Nothing stated this name: the stand-in, and the one answer here that is not
    // something the client registered the CVar with.
    else { lua_pushstring(L, "0"); if (stated) *stated = false; }
}


/// The CVars whose value is a client setting.
///
/// FrameXML's Video and Interface panels are bound to CVar names. The values
/// behind these names are settings this client already had - they simply had
/// never been introduced, so the panels wrote to a store nothing read and the
/// controls did nothing.
///
/// A row here, a key in Application's bridge, and the control works. The
/// alternative - which the entries below this table still are - is a getter and
/// a setter on LuaServices, a lambda in Application, and a branch in each of
/// GetCVar and SetCVar, four places per option.
struct ClientCVarBinding {
    const char* cvar;      ///< lower case, as both sides fold it
    const char* setting;   ///< the key Application's bridge answers to
    /// What the setting reads when the CVar reads one, where the two are not
    /// counting the same thing.
    ///
    /// Almost always they are: a volume is 0 to 1 on both sides, a checkbox is
    /// 0 or 1. Ground clutter is not. Blizzard's Ground Density slider counts
    /// doodads and runs 16 to 64; this client's setting is a proportion and
    /// runs 0 to 1.5. Handing one straight to the other put 64 into a field
    /// that stops at 1.5, so every position of that slider - all seven of them
    /// - came out as the most clutter the client will draw, and stayed there:
    /// the value written to the config was clamped back down on the next load,
    /// which made it permanent.
    double scale = 1.0;
};

// NOLINTBEGIN(modernize-use-designated-initializers) - read as text by
// tools/settings_without_a_control.py and tools/cvar_slider_range_fit.py,
// which match {"cvar", "setting"} to find which settings a Blizzard slider
// drives. Naming the fields makes both of them see an empty table.
constexpr ClientCVarBinding kClientCVars[] = {
    {"farclip",              "viewdistance"},
    // Mouse Sensitivity. Its shipped range is 0.5 to 1.5, a multiplier around
    // 1.0 for the pointer, and this client's sensitivity is an amount that runs
    // 0.05 to 1.0 - so passed through unchanged the slowest setting that slider
    // offered was two and a half times this client's default. The pointer's own
    // speed is not something this client keeps, so the slider drives the look
    // rate directly and is drawn on that rate's scale instead (kCVarRanges);
    // Mouse Look Speed reads the same setting in its own units below.
    {"mousespeed",           "mousespeed"},
    // Mouse Look Speed, which is the setting above counted the way the client
    // counts it: it turns the view cameraYawMoveSpeed * 0.00125 degrees per
    // movement of the mouse (0x006020b0), so its shipped 90..270 slider is
    // 0.1125..0.3375 of this client's degrees. Scaled through the table rather
    // than converted at one call site, so GetCVar, the shipped slider, the
    // console and the setting are one number in each of its two units.
    {"camerayawmovespeed",   "mousespeed",  0.00125},
    {"showclock",            "minimapclock"},
    {"nameplateshowfriends", "friendlyplates"},
    // Not gxWindow: it is the fullscreen row the other way round, which a scale
    // cannot say, so GetCVar and SetCVar carry it by hand.
    // 64 doodads is the most Blizzard's slider asks for and 150 percent the
    // most this client draws, so one of theirs is 150/64 of ours. It was
    // 1.5/64 while the setting travelled as a fraction; the setting is the
    // percentage the panel shows now, and this is the one place that knows.
    {"groundeffectdensity",  "groundclutter", 150.0 / 64.0},
    // The rest of what the game's own Effects panel used to drive. That
    // panel is retired and these are rows in the schema now, but the cvars
    // stay bound to them: an addon or a macro writing particleDensity still
    // reaches the setting the panel shows, rather than a second store that
    // quietly disagrees with it.
    //
    // The scales are the difference between a fraction and a percentage.
    // Blizzard's particle and environment sliders run 0.1-1 and 0.5-1.5;
    // these settings are percentages, so one of theirs is a hundred of ours.
    {"groundeffectdist",     "groundclutterdistance"},
    {"particledensity",      "particledensity", 100.0},
    {"weatherdensity",       "weatherdetail"},
    {"environmentdetail",    "environmentdetail", 100.0},
    // Six levels on their slider and five here: theirs ends on 16x twice,
    // every card stopping there, so the top two both land on this one's 4.
    {"texturefilteringmode", "texturefiltering"},
    // Their slider is 0 to 1 and this setting is a percentage.
    {"sound_sfxvolume",      "effectsvolume", 100.0},
    // The switches off the game's own Sound panel, now rows in the schema.
    // Bound rather than converted because applySoundCVars reads the store:
    // the setting writes the cvar, the panel asks for a re-apply, and the
    // one piece of code that knows what each switch silences stays where it
    // is. Not master, mute or the effects volume - those three are bridged
    // through getAudioSetting already, and a second route would have the two
    // fight over the same value.
    {"sound_enablesoundwhengameisinbg", "soundinbackground"},
    {"sound_enablemusic",    "enablemusic"},
    {"sound_zonemusicnodelay", "loopmusic"},
    {"sound_enableambience", "enableambience"},
    {"sound_enableerrorspeech", "errorspeech"},
    {"sound_enablesfx",      "enablesoundeffects"},
    // Three that were each written out four times over - a getter and a setter
    // on LuaServices, a lambda in Application, and a branch in each of GetCVar
    // and SetCVar - because the client had no key for them when they were
    // added. It has now, so they are rows like the rest.
    {"gxvsync",              "vsync"},
    {"mouseinvertpitch",     "invertmouse"},
    // The client keeps this one and pushes it at the game handler each frame,
    // so writing the setting is what makes the checkbox stick across a session
    // as well as within one. The branch it replaces wrote the handler directly
    // and never saved.
    {"autolootdefault",      "autoloot"},
    // The client's own cvar of that name (0x0078e1a0), read at start-up.
    {"specular",             "specular"},
    // How far back the camera may be pulled. The row counts yards; the CVar
    // the game's slider wrote counts multiples of the 22 the original client
    // gives, so a 1 here is 22 there and the slider's 2 is 44.
    {"cameradistancemaxfactor", "cameramaxdistance", 22.0},
    // Read by the client's own attack handling, not by FrameXML's buttons.
    {"secureabilitytoggle",  "secureabilitytoggle"},
    // The bubbles are drawn by this client, so both boxes are its own.
    {"chatbubbles",          "chatbubbles"},
    {"chatbubblesparty",     "chatbubblesparty"},
    // Both owned by the client and turned by a key or a menu as well as by
    // the row - which is why each has a field, and why the field is what
    // the CVar answers from.
    {"nameplateshowenemies", "enemyplates"},
    {"rotateminimap",        "minimaprotate"},
};
// NOLINTEND(modernize-use-designated-initializers)

/// What another CVar currently reads as, for the settings that only mean
/// something in pairs. The store first, then the default, which is the same
/// order GetCVar answers in.
std::string cvarValueOr(lua_State* L, const char* name, const char* fallback) {
    (void)L;
    if (auto it = cvarStore().find(name); it != cvarStore().end()) return it->second;
    return fallback;
}

/// Set while a CVar is being applied to the setting it drives.
///
/// The store already holds the value in that direction, and echoing it back
/// rewrites it with whatever survives the trip: ground clutter is kept as a
/// whole percent, so a Ground Density of 24 came back as 23.893333 and the
/// store no longer said what the player picked - nor any position that slider
/// can be put in.
static bool g_applyingCVarToSetting = false;

/// Set while the stored CVars are being replayed into client state at startup.
///
/// That replay is for the CVars the interface owns and the client follows. A
/// CVar the client owns runs the other way, and replaying one writes a stale
/// echo over the setting it is supposed to be reading: the store keeps whatever
/// the options panel last wrote, which for chatStyle is a value from before the
/// setting existed. Left alone it turned "Chat box always visible" off at every
/// launch, on the player's behalf.
static bool g_replayingStoredCVars = false;

const ClientCVarBinding* findClientCVar(const std::string& lowerName) {
    for (const auto& b : kClientCVars) {
        if (lowerName == b.cvar) return &b;
    }
    return nullptr;
}

/// RegisterForSave("variable") (0x0052b470) and
/// RegisterForSavePerCharacter("variable") (0x0052b4e0): the global kept in
/// the account's or the character's SavedVariables.lua. Blizzard's scripts
/// only - a tainted caller is refused.
static int registerForSave(lua_State* L, bool perCharacter) {
    if (taint::current() != 0) {
        return luaL_error(L, perCharacter
            ? "RegisterForSavePerCharacter() is only available to Blizzard scripts"
            : "RegisterForSave() is only available to Blizzard scripts");
    }
    if (!lua_isstring(L, 1)) {
        return luaL_error(L, perCharacter ? "Usage: RegisterForSavePerCharacter(\"variable\")"
                                          : "Usage: RegisterForSave(\"variable\")");
    }
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
    auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!engine) return 0;
    const std::string name = lua_tostring(L, 1);
    auto& list = engine->registeredForSave(perCharacter);
    if (std::find(list.begin(), list.end(), name) == list.end()) list.push_back(name);
    return 0;
}
static int lua_RegisterForSave(lua_State* L) { return registerForSave(L, false); }
static int lua_RegisterForSavePerCharacter(lua_State* L) { return registerForSave(L, true); }

static int lua_GetCVar(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    // Folded to lower case, because the client's CVar names are not
    // case-sensitive and the interface does not spell them consistently.
    // uidropdownmenu.lua asks for "uiscale" where everything else says
    // "uiScale"; an exact match answered "0" for it, tonumber("0") is 0, and
    // every dropdown menu in the interface opened at SetScale(0) - laid out,
    // drawn, and invisible.
    std::string n(name);
    toLowerInPlace(n);
    // Asked of the client before the store, for the settings it owns outright.
    // Enemy nameplates and the minimap's rotation used to be here too - the V
    // key and the minimap's menu turn them without SetCVar - and are rows
    // bound in kClientCVars now, which answers from the row the same way.
    if (n == "gxwindow") {
        // The schema's fullscreen row the other way round: 1 is windowed. The
        // Windowed box on the Resolution page is retired for that row, and
        // RestartGx reads this on Okay - so it has to be the window's live
        // state, not a store nothing writes any more.
        if (auto* svc = getLuaServices(L); svc && svc->getFullscreen) {
            lua_pushstring(L, svc->getFullscreen() ? "0" : "1");
            return 1;
        }
    } else if (n == "sound_mastervolume" || n == "sound_musicvolume" ||
               n == "sound_ambiencevolume" || n == "sound_enableallsound") {
        // FrameXML's Sound options are bound to these, and the client owns the
        // values. Answering from the CVar store would report whatever the panel
        // last wrote and drift from what is actually playing - the same fault
        // the nameplate and minimap entries above exist for.
        if (auto* svc = getLuaServices(L); svc && svc->getAudioSetting) {
            const bool isSwitch = (n == "sound_enableallsound");
            const char* key = n == "sound_mastervolume"   ? "master"
                            : n == "sound_musicvolume"    ? "music"
                            : n == "sound_ambiencevolume" ? "ambient"
                                                          : "enableall";
            const float v = svc->getAudioSetting(key);
            // A switch answers "1" or "0" and nothing else. The options panels
            // compare the string exactly - `if ( value == "1" )` - so a float
            // formatted as "1.000000" reads as off, and the box unticks itself
            // the moment the panel is opened again.
            if (isSwitch) {
                lua_pushstring(L, v != 0.0f ? "1" : "0");
            } else {
                lua_pushstring(L, ui::settingNumberText(v).c_str());
            }
            return 1;
        }
    } else if (n == "chatstyle") {
        // Above the store, like the nameplate and minimap rows: the client owns
        // this one now, under "Chat box always visible", and the store holds
        // whatever the social options panel last wrote.
        //
        // It writes on load as well as on a change - so a config saved before
        // that setting existed already says chatstyle=classic, and answering
        // from the store meant the new setting was read once at the default and
        // never again. The box stayed hidden with the box ticked.
        if (auto* svc = getLuaServices(L); svc && svc->getClientSetting) {
            const std::string visible = svc->getClientSetting("chatboxvisible");
            if (!visible.empty()) {
                lua_pushstring(L, visible == "0" ? "classic" : "im");
                return 1;
            }
        }
    } else if (n == "autoselfcast") {
        if (auto* gh = getGameHandler(L)) {
            lua_pushstring(L, gh->isAutoSelfCast() ? "1" : "0");
            return 1;
        }
    } else if (n == "showtimestamps") {
        // A stored "0" here was never chosen. It is what the old default
        // answered, and the options panel writes back whatever it read, so a
        // config saved while that default was live has the fault baked into
        // it - and fixing only the default would leave those players with the
        // 0 in front of every line for good. The word for off is "none"; a
        // digit is a strftime format that prints itself.
        if (auto it = cvarStore().find(n); it != cvarStore().end() &&
            (it->second.empty() || it->second == "0")) {
            lua_pushstring(L, "none");
            return 1;
        }
    }
    if (n == "uiscale") {
        // From the tree, above the store. The tree clamps to the range the
        // slider offers, so a stored value would report back what was asked for
        // rather than what was applied - which is how a control comes to show a
        // number the interface is not using.
        if (auto* tree = getWidgetTree(L)) {
            lua_pushstring(L, ui::settingNumberText(tree->userScale()).c_str());
            return 1;
        }
    }
    if (const auto* binding = findClientCVar(n)) {
        if (auto* svc = getLuaServices(L); svc && svc->getClientSetting) {
            const std::string v = svc->getClientSetting(binding->setting);
            if (!v.empty()) {
                // Back into the units the slider is built in. Left alone when
                // the two count the same thing, so a "1" stays "1" rather than
                // becoming "1.000000" on its way through a double.
                if (binding->scale != 1.0) {
                    lua_pushstring(L, ui::settingNumberText(std::atof(v.c_str()) /
                                                           binding->scale).c_str());
                } else {
                    lua_pushstring(L, v.c_str());
                }
                return 1;
            }
        }
    }
    if (auto it = cvarStore().find(n); it != cvarStore().end()) {
        lua_pushstring(L, it->second.c_str());
        return 1;
    }
    pushCvarDefault(L, n);
    return 1;
}

/// GetCVarDefault(name) → what it would be if nobody had set it.
///
/// Not the same question as GetCVar, and it used to be answered by the same
/// function. The options panels capture control.defaultValue from this as they
/// load and the Defaults button writes it back, so aliasing the two made that
/// button restore each control to the value it already had.
static int lua_GetCVarDefault(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    std::string n(name);
    toLowerInPlace(n);
    pushCvarDefault(L, n);
    return 1;
}

/// GetCVarBool(name) → the setting as a boolean.
///
/// FrameXML branches on this, and one of those branches decides how a unit
/// frame's health bar keeps itself current: predictedHealth sends it down an
/// OnUpdate poll instead of registering UNIT_HEALTH. Unimplemented, the call
/// answered nil through the fallback and took the event branch by luck - which
/// is the branch that works here, but only until the fallback is off, when the
/// same call errors instead.
static int lua_GetCVarBool(lua_State* L) {
    lua_pushvalue(L, 1);
    lua_GetCVar(L);
    const char* v = lua_tostring(L, -1);
    const bool on = v && *v && std::string(v) != "0";
    lua_pop(L, 1);
    lua_pushboolean(L, on);
    return 1;
}

/// The registration flags of the CVar by that lower-case name (0x00767460 and
/// the CVar's +0x1c), or none when no CVar has that name.
///
/// Registered are the client's own, the ones RegisterCVar made, and the ones
/// in the saved file or set since - the client registers every name it reads
/// from Config.wtf (0x007681f0), with no flags.
static std::optional<uint32_t> registeredCVarFlags(const std::string& lower) {
    if (auto flags = clientCVarFlags(lower)) {
        if (reregisteredClientCVars().count(lower)) {
            return *flags & ~(kCVarStoredWithAccount | kCVarStoredWithCharacter);
        }
        return flags;
    }
    if (scriptRegisteredCVars().count(lower) || cvarStore().count(lower)) return 0u;
    return std::nullopt;
}

// RegisterCVar("cvar" [, default]) (0x0050ff30 -> 0x0050fea0): a CVar of the
// addon's own, with no flags, defaulting to the value given or to "0". One
// that exists keeps its value and default and loses its server storage.
static int lua_RegisterCVar(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: RegisterCVar(\"cvar\" [, default])");
    std::string n = lua_tostring(L, 1);
    if (n.empty()) return 0;
    toLowerInPlace(n);
    if (clientCVarFlags(n)) {
        reregisteredClientCVars().insert(n);
        return 0;
    }
    if (scriptRegisteredCVars().count(n)) return 0;
    scriptRegisteredCVars()[n] = lua_isstring(L, 2) ? lua_tostring(L, 2) : "0";
    return 0;
}

// GetCVarInfo("cvar") → value, defaultValue, storedWithAccount, storedWithCharacter
// (0x0050ff50): the two flags 1 or nil; all four nil for a CVar that is not
// there, a hidden one included.
static int lua_GetCVarInfo(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: GetCVarInfo(\"cvar\")");
    std::string n = lua_tostring(L, 1);
    toLowerInPlace(n);
    const auto flags = registeredCVarFlags(n);
    if (!flags || (*flags & kCVarHidden)) {
        for (int i = 0; i < 4; ++i) lua_pushnil(L);
        return 4;
    }
    lua_pushvalue(L, 1);
    lua_GetCVar(L);
    lua_remove(L, -2);
    lua_pushvalue(L, 1);
    lua_GetCVarDefault(L);
    lua_remove(L, -2);
    if (*flags & kCVarStoredWithAccount) lua_pushnumber(L, 1); else lua_pushnil(L);
    if (*flags & kCVarStoredWithCharacter) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 4;
}

/// GetCVarAbsoluteMin and GetCVarAbsoluteMax (0x00514f60, 0x00515010): only
/// extShadowQuality has bounds, the lowest level and the highest the renderer
/// takes; every other CVar answers nil, and a name that is no CVar raises.
static int cvarAbsoluteBound(lua_State* L, bool max, const char* usage) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "%s", usage);
    std::string n = lua_tostring(L, 1);
    toLowerInPlace(n);
    const auto flags = registeredCVarFlags(n);
    if (!flags || (*flags & kCVarHidden)) {
        return luaL_error(L, "Couldn't find CVar named '%s'", lua_tostring(L, 1));
    }
    if (n != "extshadowquality") {
        lua_pushnil(L);
        return 1;
    }
    // The client's device answers 5 (0x00873f50); this renderer's levels stop
    // at 4 (Renderer::initialize clamps the stored level to 0-4), and a level
    // above that would be one nothing draws.
    lua_pushnumber(L, max ? 4 : 0);
    return 1;
}
static int lua_GetCVarAbsoluteMin(lua_State* L) {
    return cvarAbsoluteBound(L, false, "Usage: GetCVarAbsoluteMin(\"cvar\")");
}
static int lua_GetCVarAbsoluteMax(lua_State* L) {
    return cvarAbsoluteBound(L, true, "Usage: GetCVarAbsoluteMax(\"cvar\")");
}

/// The payload this client announces a CVar change with, for the changes *it*
/// makes - its own settings window, which is not one of FrameXML's panels.
///
/// A change the interface makes needs none of this: SetCVar's third argument is
/// the announcement and the binding forwards it (see lua_SetCVar), so a panel
/// that declares `event = "SHOW_BUFF_DURATION_TEXT"` gets heard as that. What is
/// written here is what this window announces instead, and it is only ever as
/// right as the pairs below: FrameXML's settings tables say what a CVar is
/// announced as, in two places -
///
///     ["REMOVE_CHAT_DELAY"] = { default = "0", cvar = "removeChatDelay",
///                               event = "REMOVE_CHAT_DELAY_TEXT" }
///
/// in interfaceoptionsframe.lua, and self.cvar beside self.cvarLabel in the
/// status-text bars' OnLoad. Framing this as a name-to-label mapping the binary
/// was assumed to hold was what made it wrong: the transform below answers
/// BUFF_DURATIONS for buffDurations, and every listener compares
/// SHOW_BUFF_DURATION_TEXT, so a change made in this window reached the CVar and
/// never the frames listening for it. tools/framexml_cvar_label_check.py reads
/// those declarations and reports where this table disagrees with them.
///
/// The transform is the fallback for the CVars that have no declared pair, where
/// the only evidence of what the interface expects is the comparison itself:
/// `arg1 == "DISPLAY_FREE_BAG_SLOTS"` has no settings table entry beside it, and
/// a window-driven change to displayFreeBagSlots is the only way that setting
/// ever moves here.
static std::string cvarLabelFor(const std::string& name) {
    // The pairs FrameXML declares, which the transform below would get wrong.
    static const std::pair<const char*, const char*> kNamed[] = {
        // The status-text bars, which set both halves in their OnLoad.
        {"playerstatustext",       "STATUS_TEXT_PLAYER"},
        {"partystatustext",        "STATUS_TEXT_PARTY"},
        {"petstatustext",          "STATUS_TEXT_PET"},
        {"targetstatustext",       "STATUS_TEXT_TARGET"},
        // Compared in TextStatusBar_OnEvent with no declaration beside it; the
        // percent text has no checkbox of its own, which is why.
        {"statustextpercentage",   "STATUS_TEXT_PERCENT"},
        // interfaceoptionsframe.lua's table, which is where the panels that use
        // the old SetBinding-style rows say what each CVar is announced as.
        {"alwaysshowactionbars",   "ALWAYS_SHOW_MULTIBARS_TEXT"},
        {"autolootdefault",        "AUTO_LOOT_DEFAULT_TEXT"},
        {"autoquestwatch",         "AUTO_QUEST_WATCH_TEXT"},
        {"buffdurations",          "SHOW_BUFF_DURATION_TEXT"},
        {"displayworldpvpobjectives", "WORLD_PVP_OBJECTIVES_DISPLAY"},
        {"enablecombattext",       "SHOW_COMBAT_TEXT_TEXT"},
        {"fctauras",               "COMBAT_TEXT_SHOW_AURAS_TEXT"},
        {"fctcombatstate",         "COMBAT_TEXT_SHOW_COMBAT_STATE_TEXT"},
        {"fctcombopoints",         "COMBAT_TEXT_SHOW_COMBO_POINTS_TEXT"},
        {"fctdamagereduction",     "COMBAT_TEXT_SHOW_RESISTANCES_TEXT"},
        {"fctdodgeparrymiss",      "COMBAT_TEXT_SHOW_DODGE_PARRY_MISS_TEXT"},
        {"fctenergygains",         "COMBAT_TEXT_SHOW_ENERGIZE_TEXT"},
        {"fctfriendlyhealers",     "COMBAT_TEXT_SHOW_FRIENDLY_NAMES_TEXT"},
        {"fcthonorgains",          "COMBAT_TEXT_SHOW_HONOR_GAINED_TEXT"},
        {"fctlowmanahealth",       "COMBAT_TEXT_SHOW_LOW_HEALTH_MANA_TEXT"},
        {"fctperiodicenergygains", "COMBAT_TEXT_SHOW_PERIODIC_ENERGIZE_TEXT"},
        {"fctreactives",           "COMBAT_TEXT_SHOW_REACTIVES_TEXT"},
        {"fctrepchanges",          "COMBAT_TEXT_SHOW_REPUTATION_TEXT"},
        {"hidepartyinraid",        "HIDE_PARTY_INTERFACE_TEXT"},
        {"lockactionbars",         "LOCK_ACTIONBAR_TEXT"},
        {"lootundermouse",         "LOOT_UNDER_MOUSE_TEXT"},
        {"questfadingdisable",     "SHOW_QUEST_FADING_TEXT"},
        {"removechatdelay",        "REMOVE_CHAT_DELAY_TEXT"},
        {"showarenaenemycastbar",  "SHOW_ARENA_ENEMY_CASTBAR_TEXT"},
        {"showarenaenemyframes",   "SHOW_ARENA_ENEMY_FRAMES_TEXT"},
        {"showarenaenemypets",     "SHOW_ARENA_ENEMY_PETS_TEXT"},
        {"showcastablebuffs",      "SHOW_CASTABLE_BUFFS_TEXT"},
        {"showcastabledebuffs",    "SHOW_CASTABLE_DEBUFFS_TEXT"},
        {"showdispeldebuffs",      "SHOW_DISPELLABLE_DEBUFFS_TEXT"},
        {"shownewbietips",         "SHOW_NEWBIE_TIPS_TEXT"},
        {"showpartybackground",    "SHOW_PARTY_BACKGROUND_TEXT"},
        {"showpartypets",          "SHOW_PARTY_PETS_TEXT"},
        {"showtargetoftarget",     "SHOW_TARGET_OF_TARGET_TEXT"},
        {"targetoftargetmode",     "SHOW_TARGET_OF_TARGET_STATE"},
        // Not a label at all: the one writer of chatStyle announces it as its
        // own name (interfaceoptionspanels.lua:962), and floatingchatframe's
        // Click-anywhere button compares exactly that.
        {"chatstyle",              "chatStyle"},
        // Compared by that same Click-anywhere button, and declared nowhere:
        // the social panel row for this CVar names its text and no event, so
        // the original client never announces it and the button only ever
        // updates from the chatStyle change beside it. This window drives the
        // CVar (settings_schema.cpp), and the comparison says what the frame is
        // waiting for, so that is what this announces - under the transform it
        // would be WHOLE_CHAT_WINDOW_CLICKABLE, which nothing reads.
        {"wholechatwindowclickable", "CHAT_WHOLE_WINDOW_CLICKABLE"},
    };
    std::string key(name);
    toLowerInPlace(key);
    for (const auto& [cvar, label] : kNamed) {
        if (key == cvar) return label;
    }
    // showTargetCastbar → SHOW_TARGET_CASTBAR, displayFreeBagSlots →
    // DISPLAY_FREE_BAG_SLOTS, xpBarText → XP_BAR_TEXT.
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (i > 0 && std::isupper(c) && !std::isupper(static_cast<unsigned char>(name[i - 1]))) {
            out += '_';
        }
        out += static_cast<char>(std::toupper(c));
    }
    return out;
}

// SetCVar(name, value [, scriptCVar])
/// Everything a CVar does besides being remembered.
///
/// Split out of lua_SetCVar so the same work can be done for the values
/// restored from disk. loadStoredCVars fills the map directly - it has to, it
/// runs before any of these services exist - so without this every setting
/// here was saved on exit and never applied again. The panel read the stored
/// value back and showed it correctly while the client ran on the default,
/// which is the shape of "I set it, it looks set, and nothing happened".
///
/// CVAR_UPDATE is deliberately left behind in lua_SetCVar: that event says
/// somebody changed a setting, which is not what restoring one is.
static void applyCVarSideEffects(lua_State* L, const std::string& key,
                                 const std::string& value) {
    // A sound CVar is a setting, not a note. Without this the interface's
    // volume keys and its Sound options both wrote to a map nobody read, so
    // turning music off left it playing.
    // The interface's own scale, which is the widget tree's to keep - not a
    // client setting, and not the ImGui window scale beside it in the settings
    // window. Those are two different interfaces that happen to share a word.
    // The power UnitPower and the cast read (0x0071c2e0).
    if (key == "predictedpower") {
        if (auto* gh = getGameHandler(L)) gh->setPredictedPowerEnabled(value != "0");
    }
    if (key == "uiscale") {
        if (auto* tree = getWidgetTree(L)) {
            // Only when the switch beside it is on. Otherwise the stored value
            // is kept for when it is turned back on, but not applied.
            const std::string useIt = cvarValueOr(L, "useuiscale", "1");
            if (useIt != "0") {
                tree->setUserScale(static_cast<float>(std::atof(value.c_str())));
            }
        }
    }
    // The minimap's zoom, put back where the player left it.
    //
    // Not a setting anyone sets in a panel: it is where the +/- buttons and the
    // wheel over the ring were left, and the real client remembers that across
    // a session. Minimap:SetZoom writes it here on the way past; this is the
    // way back, and it runs after FrameXML is up, which is the first moment
    // there is a Minimap to put it on.
    //
    // Straight onto the widget rather than through Minimap:SetZoom, so it does
    // not write back what it just read. MINIMAP_UPDATE_ZOOM is what the real
    // client fires when the zoom changes under the interface, and it is what
    // greys the button that can no longer go any further - without it a map
    // restored at full zoom offers a zoom-in that does nothing.
    // The inside step waits on the widget for the map to first go indoors.
    if (key == "wowee_minimapinsidezoom") {
        if (auto* tree = getWidgetTree(L)) {
            if (auto* mm = tree->findByName("Minimap")) {
                mm->savedInsideZoom = rendering::minimap_zoom::clampLevel(std::atoi(value.c_str()));
            }
        }
    }
    if (key == "wowee_minimapzoom") {
        if (auto* tree = getWidgetTree(L)) {
            if (auto* mm = tree->findByName("Minimap")) {
                const int z = std::atoi(value.c_str());
                mm->zoomLevel = rendering::minimap_zoom::clampLevel(z);
                lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
                auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
                lua_pop(L, 1);
                if (engine) engine->fireEvent("MINIMAP_UPDATE_ZOOM", {});
            }
        }
    }
    // Unticking it puts the interface back to the size the screen's own height
    // gives, which is what the tick means: use a scale of mine rather than the
    // default. It did nothing at all before - the box moved and the interface
    // stayed exactly as it was, at whatever scale had been set.
    // Camera Following Style, whose one meaningful distinction here is its
    // first option: "Never adjust camera" against the three that do. The
    // client has the same switch already, reached from its own panel, so this
    // is routed into that setting rather than to the camera directly - two
    // controls over one value, which is what setSettingValue exists for, and
    // it persists on the way past.
    // Chat Style, which is this client's "Chat box always visible" seen from
    // the interface's own Social panel. Routed into the setting so the two
    // controls cannot disagree - and so the read above, which answers from the
    // setting, sees what the panel chose.
    if (key == "chatstyle" && !g_replayingStoredCVars) {
        if (auto* svc = getLuaServices(L); svc && svc->setClientSetting) {
            svc->setClientSetting("chatboxvisible", value == "classic" ? "0" : "1");
        }
    }
    // Windowed mode, which is the fullscreen row the other way round. Applied
    // to the row at once rather than left for RestartGx: with the Resolution
    // page's box retired, an addon's write is the only kind there is.
    if (key == "gxwindow" && !g_replayingStoredCVars) {
        if (auto* svc = getLuaServices(L); svc && svc->setClientSetting) {
            svc->setClientSetting("fullscreen", value == "0" ? "1" : "0");
        }
    }
    // Mouse Look Speed needs nothing here: it is bound to the mouse sensitivity
    // setting in kClientCVars with the client's own 0.00125 between the two
    // units, and that table converts in both directions. Handled here it passed
    // its number through unchanged, which only stayed safe because the shipped
    // slider was redrawn on this client's scale - on its own 90 to 270 it would
    // have handed the camera 90 degrees per movement of the mouse.
    // Pitch, for the writes that name only it - the console's SetCVar, an addon.
    // The shipped slider never writes this name on its own (it writes it as half
    // its own value, and the yaw row above takes that share through the
    // setting), so this is the whole of what such a write does: the client turns
    // 1/600 of a degree per movement of the mouse per unit of it (0x006020b0).
    // Touching the look slider afterwards takes the pitch back to its share, as
    // moving that slider in the original writes its half of the pair too.
    if (key == "camerapitchmovespeed") {
        if (auto* svc = getLuaServices(L); svc && svc->getCameraController) {
            if (auto* cc = svc->getCameraController()) {
                cc->setMousePitchSensitivity(
                    static_cast<float>(std::atof(value.c_str())) * 0.0016666667f);
            }
        }
    }
    // Loop Music. The checkbox says Loop Music and the CVar behind it says
    // ZoneMusicNoDelay: the zone's next track follows without the silence
    // between them (0x004c87d0). A track still plays once.
    if (key == "sound_zonemusicnodelay") {
        if (auto* svc = getLuaServices(L); svc && svc->setZoneMusicNoDelay) {
            svc->setZoneMusicNoDelay(value != "0");
        }
    }
    // Ground Clutter Radius, in yards and used as it stands.
    if (key == "groundeffectdist") {
        if (auto* svc = getLuaServices(L); svc && svc->setGroundDetailDistance) {
            svc->setGroundDetailDistance(static_cast<float>(std::atof(value.c_str())));
        }
    }

    // Texture Filtering, offered as levels 0 to 5 rather than as a number of
    // samples: each step doubles, and the last two are both the 16x that is
    // every desktop GPU's maximum. The panel marks this one gameRestart, so
    // what it changes is the textures loaded from here on.
    if (key == "texturefilteringmode") {
        if (auto* svc = getLuaServices(L); svc && svc->setAnisotropyLimit) {
            const int level = std::clamp(std::atoi(value.c_str()), 0, 5);
            svc->setAnisotropyLimit(static_cast<float>(std::min(1 << level, 16)));
        }
    }

    // Environment Detail, offered as 0.5 to 1.5 and passed on as it stands.
    if (key == "environmentdetail") {
        if (auto* svc = getLuaServices(L); svc && svc->setEnvironmentDetail) {
            svc->setEnvironmentDetail(static_cast<float>(std::atof(value.c_str())));
        }
    }

    // Particle Density, which the panel already offers as a fraction - 0.1 to
    // 1 - so it needs no conversion, only passing on.
    if (key == "particledensity") {
        if (auto* svc = getLuaServices(L); svc && svc->setParticleDensity) {
            svc->setParticleDensity(static_cast<float>(std::atof(value.c_str())));
        }
    }

    // weatherDensity, 0 to 3, as the client takes it (0x00784040).
    if (key == "weatherdensity") {
        if (auto* svc = getLuaServices(L); svc && svc->setWeatherDensity) {
            svc->setWeatherDensity(static_cast<float>(std::atof(value.c_str())));
        }
    }
    if (key == "useuiscale") {
        if (auto* tree = getWidgetTree(L)) {
            if (value == "0") {
                tree->setUserScale(1.0f);
            } else {
                tree->setUserScale(
                    static_cast<float>(std::atof(cvarValueOr(L, "uiscale", "1").c_str())));
            }
        }
    }
    // The table first: these are settings the client owns and the panels drive.
    if (const auto* binding = findClientCVar(key)) {
        if (auto* svc = getLuaServices(L); svc && svc->setClientSetting) {
            // The store is the source here, so what the setting does with the
            // value must not come back and overwrite it.
            g_applyingCVarToSetting = true;
            if (binding->scale != 1.0) {
                svc->setClientSetting(binding->setting,
                                      ui::settingNumberText(std::atof(value.c_str()) *
                                                            binding->scale));
            } else {
                svc->setClientSetting(binding->setting, value);
            }
            g_applyingCVarToSetting = false;
        }
    }
    // The four the client owns go to its settings, which then apply and save.
    // Both systems used to write the same AudioEngine - the CVar store from
    // here and the settings panel from its own sliders - and whichever ran last
    // won. A client left muted by its own setting came back silent however the
    // interface's Sound panel was set, because the next thing to touch the
    // settings slammed the master volume back to zero.
    if (key == "sound_mastervolume" || key == "sound_musicvolume" ||
        key == "sound_ambiencevolume" || key == "sound_enableallsound") {
        if (auto* svc = getLuaServices(L); svc && svc->setAudioSetting) {
            const char* which = key == "sound_mastervolume"   ? "master"
                              : key == "sound_musicvolume"    ? "music"
                              : key == "sound_ambiencevolume" ? "ambient"
                                                              : "enableall";
            svc->setAudioSetting(which, static_cast<float>(std::atof(value.c_str())));
            return;
        }
        applySoundCVars(L);
    }
    else if (key.rfind("sound_", 0) == 0) applySoundCVars(L);
    // The one other CVar this client acts on. "0" is the only false value a
    // CVar carries - and it arrives as a string, which in Lua would be true.
    else if (key == "autoselfcast") {
        if (auto* gh = getGameHandler(L)) gh->setAutoSelfCast(value != "0");
    }
    // A modified click, kept in the store so it survives a restart. The Lua
    // table the click handlers read is written here as well, both when the
    // store is replayed at start-up - after Bindings.xml has put the defaults
    // in it - and when an addon writes the CVar itself.
    else if (key.rfind("modifiedclick_", 0) == 0) {
        std::string action = key.substr(14);
        for (char& c : action) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        lua_getglobal(L, "__WoweeModifiedClick");
        if (lua_istable(L, -1)) {
            lua_pushstring(L, value.c_str());
            lua_setfield(L, -2, action.c_str());
        }
        lua_pop(L, 1);
    }
}

/// Apply what was loaded from disk, once the services behind it exist.
///
/// Called after the interface is up rather than at load: the store is filled
/// before any renderer, camera or audio manager is wired, and every branch
/// above checks its service and would quietly do nothing that early - which is
/// indistinguishable from the fault this exists to fix.
std::string storedCVarValue(const std::string& key, const std::string& fallback) {
    // The store first, for a call made after the interface is up; the file
    // otherwise, which is the case this exists for - read once and kept
    // (cvarFileSnapshot) until this process loads or writes it again.
    std::string wanted = key;
    toLowerInPlace(wanted);
    if (auto it = cvarStore().find(wanted); it != cvarStore().end()) return it->second;

    CVarFileSnapshot& snapshot = cvarFileSnapshot();
    if (!snapshot.valid) {
        snapshot.values.clear();
        snapshot.valid = true;
        std::ifstream in(cvarStorePath());
        std::string line;
        while (in.is_open() && std::getline(in, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            std::string k = line.substr(0, eq);
            toLowerInPlace(k);
            // The first row for a key, as reading line by line answered.
            snapshot.values.emplace(std::move(k), line.substr(eq + 1));
        }
    }
    if (auto it = snapshot.values.find(wanted); it != snapshot.values.end()) return it->second;
    return fallback;
}

void setStoredCVar(const std::string& key, const std::string& value) {
    if (!cvarStoreLoaded()) loadStoredCVars();
    std::string k = key;
    toLowerInPlace(k);
    const auto existing = cvarStore().find(k);
    if (existing != cvarStore().end() && existing->second == value) return;
    cvarStore()[k] = value;
    saveStoredCVars();
}

void noteClientSettingChanged(const std::string& settingKey, const std::string& value) {
    if (g_applyingCVarToSetting) return;
    for (const auto& binding : kClientCVars) {
        if (settingKey != binding.setting) continue;
        if (!cvarStoreLoaded()) loadStoredCVars();
        // Back into the CVar's own units, the same conversion GetCVar makes.
        const std::string text =
            binding.scale != 1.0
                ? ui::settingNumberText(std::atof(value.c_str()) / binding.scale)
                : value;
        auto it = cvarStore().find(binding.cvar);
        if (it != cvarStore().end() && it->second == text) return;
        cvarStore()[binding.cvar] = text;
        saveStoredCVars();
        return;
    }
}

void applySoundCVarSideEffects(lua_State* L) { applySoundCVars(L); }

void applyStoredCVarSideEffects(lua_State* L) {
    g_replayingStoredCVars = true;
    for (const auto& [key, value] : cvarStore()) {
        applyCVarSideEffects(L, key, value);
    }
    g_replayingStoredCVars = false;
    LOG_INFO("CVars: applied ", cvarStore().size(), " stored values");
}

/// What GetCVar answers for an already lower-cased name.
///
/// The listing and the two console setters read through this rather than out of
/// the store, because the store is only one of the places an answer comes from:
/// the interface's own scale, the client settings behind the kClientCVars
/// bindings and the switches the game handler keeps each answer for themselves.
/// A listing that read the map would print values the panels are not using,
/// which is the disagreement this file keeps having to chase down.
static std::string cVarAnswer(lua_State* L, const std::string& lower) {
    lua_pushstring(L, lower.c_str());
    lua_GetCVar(L);
    const char* v = lua_tostring(L, -1);
    std::string out = v ? v : "";
    lua_pop(L, 1);
    return out;
}

/// The default string the client holds on the CVar object (+0x40), or false when
/// it states none for the name.
static bool cVarStatedDefault(lua_State* L, const std::string& lower, std::string& out) {
    bool stated = false;
    pushCvarDefault(L, lower, &stated);
    const char* v = lua_tostring(L, -1);
    const bool answered = stated && v != nullptr;
    if (answered) out = v;
    lua_pop(L, 1);
    return answered;
}

/// The console commands' write: the store, the file, and back into whatever
/// reads the CVar - and nothing else. No CVAR_UPDATE, because the client's
/// reset path assigns the CVar's own string (0x0047cf80) and raises no event;
/// see the note on resetStoredCVar in the header.
static void writeCVarFromConsole(lua_State* L, const std::string& lower,
                                 const std::string& target) {
    // And before any of it, for a CVar registered read-only - the `>> 2 & 1` test
    // both setters make on the registration flags (kCVarReadOnly). Bit 8, which
    // the interface's SetCVar treats as read-only as well, is not part of that
    // test, so it is not part of this one either.
    if (auto flags = clientCVarFlags(lower); flags && (*flags & kCVarReadOnly)) return;
    // 0x00766990 and 0x00766a20 both stop short when the value the CVar already
    // has is the one they were about to write, and compare case-insensitively
    // doing it (0x0076e780 is the A-Z fold - not the case-sensitive compare
    // 0x00767220 uses to decide which parentheticals to print).
    std::string current = cVarAnswer(L, lower);
    std::string wanted = target;
    toLowerInPlace(current);
    toLowerInPlace(wanted);
    if (current == wanted) return;
    cvarStore()[lower] = target;
    saveStoredCVars();
    applyCVarSideEffects(L, lower, target);
}

void setCVarFromConsole(lua_State* L, const std::string& name, const std::string& value) {
    if (!L || name.empty()) return;
    // The client's lookup of the name is the case-insensitive one (0x0055f4d0),
    // and a lookup that fails is not an error here: 0x007681f0 goes on to
    // register the name with no flags and assign the value anyway. The store is
    // the same kind of list, so writing the key is the registration.
    std::string lower = name;
    toLowerInPlace(lower);
    writeCVarFromConsole(L, lower, value);
}

std::vector<CVarConsoleRow> cVarConsoleRows(lua_State* L) {
    std::vector<CVarConsoleRow> rows;
    if (!L) return rows;
    // The client walks one linked list of every CVar object it owns, so the
    // listing is every name in it: the client's own, the ones RegisterCVar made,
    // and every name a config line or a SetCVar mentioned - the client registers
    // those as it reads them too (0x007681f0), with no flags. Hidden CVars
    // included: the list is the client's own inventory, not what an addon may
    // ask about, so GetCVarInfo's reticence about a hidden name does not reach
    // it.
    std::vector<std::pair<std::string, std::string>> names;  // spelling, key
    std::unordered_set<std::string> seen;
    const auto add = [&](std::string spelling, std::string key) {
        if (!seen.insert(key).second) return;
        names.emplace_back(std::move(spelling), std::move(key));
    };
    for (std::string_view registered : clientCVarNames()) {
        std::string key(registered);
        toLowerInPlace(key);
        add(std::string(registered), std::move(key));   // the client's own spelling
    }
    for (const auto& [key, value] : scriptRegisteredCVars()) add(key, key);
    for (const auto& [key, value] : cvarStore()) add(key, key);
    // Alphabetically, which is this table's order rather than the client's
    // registration order; see clientCVarNames.
    std::sort(names.begin(), names.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    rows.reserve(names.size());
    for (const auto& [spelling, key] : names) {
        CVarConsoleRow row;
        row.name = spelling;
        row.value = cVarAnswer(L, key);
        // Both halves are left empty when they are not to be printed: no default
        // string on the object at all (this client states one per name it has
        // looked at, and the stand-in at the end of that chain is not one), or
        // the same string the CVar already has. That compare is case-sensitive in
        // the client, unlike the setters' - so a value differing only in case is
        // listed as changed, and no write is made for it.
        std::string def;
        if (cVarStatedDefault(L, key, def) && def != row.value) row.defaultValue = def;
        if (auto it = cvarStartupValues().find(key);
            it != cvarStartupValues().end() && it->second != row.value) {
            row.resetValue = it->second;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

bool cVarConsoleHas(const std::string& name) {
    std::string key(name);
    toLowerInPlace(key);
    // The client's lookup (0x0055f4d0) is a hash table keyed on the lower-cased
    // name compared case-insensitively, and it answers for a hidden CVar as
    // readily as for any other - unlike SetCVar's own lookup, which hides those.
    return clientCVarFlags(key).has_value() || scriptRegisteredCVars().count(key) ||
           cvarStore().count(key);
}

void resetStoredCVar(lua_State* L, const std::string& name) {
    if (!L) return;
    std::string key(name);
    toLowerInPlace(key);
    // +0x4c then +0x40: the startup value, or the registered default for a name
    // this session's file did not mention. Neither is not "set it to nothing" -
    // 0x00766990 returns without writing at all.
    if (auto it = cvarStartupValues().find(key); it != cvarStartupValues().end()) {
        writeCVarFromConsole(L, key, it->second);
        return;
    }
    std::string def;
    if (cVarStatedDefault(L, key, def)) writeCVarFromConsole(L, key, def);
}

void restoreCVarDefault(lua_State* L, const std::string& name) {
    if (!L) return;
    std::string key(name);
    toLowerInPlace(key);
    // The mirror: 0x00766a20 reads +0x40 first and +0x4c only when there is no
    // registered default to put back.
    std::string def;
    if (cVarStatedDefault(L, key, def)) {
        writeCVarFromConsole(L, key, def);
        return;
    }
    if (auto it = cvarStartupValues().find(key); it != cvarStartupValues().end()) {
        writeCVarFromConsole(L, key, it->second);
    }
}

/// Report the controls kRemovedControlsLua could not find.
///
/// An entry there naming nothing is a bug in the list rather than a state of
/// the interface - nine of them named CVars where frames were wanted and were
/// skipped in silence, which is how the list came to be half decorative.
static int lua_WoweeReportMissingFixedControls(lua_State* L) {
    const char* names = lua_isstring(L, 1) ? lua_tostring(L, 1) : nullptr;
    if (names && *names) {
        LOG_WARNING("Fixed controls: no such frame(s): ", names);
    }
    return 0;
}

static int lua_SetCVar(lua_State* L) {
    const char* name = lua_isstring(L, 1) ? lua_tostring(L, 1) : nullptr;
    if (!name) return 0;
    // A number is as valid as a string here and FrameXML passes both.
    std::string value;
    if (lua_isstring(L, 2) || lua_isnumber(L, 2)) {
        value = lua_tostring(L, 2);
    } else if (lua_isboolean(L, 2)) {
        value = lua_toboolean(L, 2) ? "1" : "0";
    } else {
        // nil is how an unticked box reports itself, and these are written
        // straight through: SetCVar("questPOI", self:GetChecked()). Storing
        // the empty string for it would make GetCVar answer something that is
        // neither "0" nor a number, so tonumber(GetCVar(...)) - which the
        // options panels do - would give nil where it wanted a zero.
        value = "0";
    }
    // The same folding as the read side, or a value written as "uiScale"
    // would be invisible to a read of "uiscale".
    std::string key(name);
    toLowerInPlace(key);
    const auto existing = cvarStore().find(key);
    const bool changed = (existing == cvarStore().end() || existing->second != value);
    cvarStore()[key] = value;
    // Only on a real change. A slider drag calls SetCVar on every frame it
    // moves, and most of those calls set the value it already has.
    if (changed) saveStoredCVars();
    applyCVarSideEffects(L, key, value);
    // What is announced is the caller's *third argument*, not a spelling of the
    // name. SetCVar's binding in the binary (FUN_00514c10, and the fire at
    // 0x00514cff) tests that argument for being a string or a number
    // (FUN_0084df60 - the argument slot's type against 3 and 4) and raises
    // CVAR_UPDATE only when it is, with that argument itself: the interface's
    // panels pass the label their own settings table declares, and a SetCVar
    // called without one says nothing to anyone. Inferring a label instead was
    // guessing at a payload the caller had already chosen: SetCVar("chatStyle",
    // v, "chatStyle") - the only writer floatingchatframe's Click-anywhere
    // button answers to - arrived as CHAT_STYLE and matched nothing, and the
    // thirty-plus pairs FrameXML spells out (buffDurations is announced as
    // SHOW_BUFF_DURATION_TEXT) arrived as BUFF_DURATIONS and matched nothing
    // either. lua_isstring answers true for a number, which is the binary's
    // rule: both types announce.
    //
    // The second event argument is this client's rather than the binary's. That
    // fire pushes one string under a "%s%s" format (0x009e1ad0), so the `value`
    // every FrameXML handler unpacks - TextStatusBar_OnEvent branches on it -
    // reads past the argument list. Sending the value is the answer the
    // handlers were written for; matching what the stack happened to hold is
    // not something worth reproducing.
    //
    // Through the engine in the registry, which is where the event puts itself;
    // the event tables are its business rather than this file's.
    if (!lua_isstring(L, 3)) return 0;
    const std::string payload = lua_tostring(L, 3);
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
    auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (engine) engine->fireEvent("CVAR_UPDATE", {payload, value});
    return 0;
}


static int lua_GetNumAddOns(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_addon_count");
    return 1;
}

/// Pushes the addon registry table and resolves argument `arg` to a 1-based
/// index into it.
///
/// FrameXML passes either an index or a name to every addon query, so both
/// GetAddOnInfo and GetAddOnMetadata had to accept both, and each carried its
/// own copy of the search.
///
/// On success the table is left on the stack and the caller indexes it. On
/// failure the stack is left exactly as it was found and the answer is 0, so
/// the caller only has to push its own nil. Getting that asymmetry wrong is
/// how a binding leaks a table per call and the interpreter grows all session.
///
/// Names are matched exactly. FrameXML's own addon list passes back the name
/// it was given, so a case difference here would be one it introduced itself.
int pushAddonRegistryAndIndex(lua_State* L, int arg) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_addon_info");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        return 0;
    }

    int idx = 0;
    if (lua_isnumber(L, arg)) {
        idx = static_cast<int>(lua_tonumber(L, arg));
    } else if (lua_isstring(L, arg)) {
        const char* wanted = lua_tostring(L, arg);
        const int count = static_cast<int>(lua_objlen(L, -1));
        for (int i = 1; i <= count; i++) {
            lua_rawgeti(L, -1, i);
            lua_getfield(L, -1, "name");
            const char* name = lua_tostring(L, -1);
            lua_pop(L, 1);
            if (name && strcmp(name, wanted) == 0) { idx = i; lua_pop(L, 1); break; }
            lua_pop(L, 1);
        }
    }

    if (idx < 1) {
        lua_pop(L, 1);
        return 0;
    }
    return idx;
}

static int lua_GetAddOnInfo(lua_State* L) {
    // Accept index (1-based) or addon name
    const int idx = pushAddonRegistryAndIndex(L, 1);
    if (idx < 1) { lua_pushnil(L); return 1; }

    lua_rawgeti(L, -1, idx);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); lua_pushnil(L); return 1; }

    // name, title, notes, url, loadable, reason, security, newVersion.
    //
    // Five were returned and the fourth was not url, so everything from there
    // shifted: the loadable flag landed in url's place, the security string in
    // loadable's - truthy, so addons read as loadable by accident - and the
    // last three came back nil. addonlist.lua reads security seventh.
    lua_getfield(L, -1, "name");
    lua_getfield(L, -2, "title");
    lua_getfield(L, -3, "notes");

    // The registry table and the entry are still underneath, and `return 8`
    // takes the top eight of the stack whatever they are. Leaving them there
    // shifted the answer by one: the entry table arrived as the name, the name
    // as the title, and loadable came back nil - falsy - so every addon read
    // as unloadable while the security string landed in newVersion. The pop
    // this replaces removed the last value pushed rather than either of them.
    lua_remove(L, -4);              // the addon info entry
    lua_remove(L, -4);              // the registry table

    lua_pushnil(L);                 // 4: url - not in a 3.3.5 manifest
    lua_pushboolean(L, 1);          // 5: loadable
    lua_pushnil(L);                 // 6: reason it is not, and it is
    lua_pushstring(L, "INSECURE");  // 7: security
    lua_pushnil(L);                 // 8: newVersion
    return 8;
}

/// The addon entry an IsAddOnLoadOnDemand or GetAddOnDependencies argument
/// names, left on the stack, as 0x00511a60 and 0x005179b0 resolve it: an index
/// out of range raises, a name nothing has answers false. Usage raised for
/// neither.
static bool pushAddonEntryForQuery(lua_State* L, const char* usage) {
    if (lua_type(L, 1) != LUA_TNUMBER && !lua_isstring(L, 1)) {
        luaL_error(L, "%s", usage);
        return false;
    }
    if (lua_type(L, 1) == LUA_TNUMBER) {
        lua_getfield(L, LUA_REGISTRYINDEX, "wowee_addon_count");
        const int count = static_cast<int>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        const int64_t index = std::llround(lua_tonumber(L, 1));
        if (index < 1 || index > count) {
            luaL_error(L, "AddOn index must be in the range of 1 to %d", count);
            return false;
        }
    }
    const int idx = pushAddonRegistryAndIndex(L, 1);
    if (idx < 1) return false;
    lua_rawgeti(L, -1, idx);
    lua_remove(L, -2);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return false; }
    return true;
}

// IsAddOnLoadOnDemand(index or "name") (0x00511a60): 1 for an addon whose
// TOC says LoadOnDemand, nil otherwise.
static int lua_IsAddOnLoadOnDemand(lua_State* L) {
    bool onDemand = false;
    if (pushAddonEntryForQuery(L, "Usage: IsAddOnLoadOnDemand(index or \"name\")")) {
        lua_getfield(L, -1, "loadOnDemand");
        onDemand = lua_toboolean(L, -1) != 0;
        lua_pop(L, 2);
    }
    if (onDemand) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 1;
}

// GetAddOnDependencies(index or "name") (0x005179b0): the addons it requires,
// one value each.
static int lua_GetAddOnDependencies(lua_State* L) {
    if (!pushAddonEntryForQuery(L, "Usage: GetAddOnDependencies(index or \"name\")")) return 0;
    lua_getfield(L, -1, "dependencies");
    lua_remove(L, -2);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
    const int n = static_cast<int>(lua_objlen(L, -1));
    if (!lua_checkstack(L, n + 1)) { lua_pop(L, 1); return 0; }
    for (int i = 1; i <= n; ++i) lua_rawgeti(L, -i, i);
    lua_remove(L, -n - 1);
    return n;
}

// GetAddOnMetadata(addonNameOrIndex, key) → value
static int lua_GetAddOnMetadata(lua_State* L) {
    const int idx = pushAddonRegistryAndIndex(L, 1);
    if (idx < 1) { lua_pushnil(L); return 1; }

    const char* key = luaL_checkstring(L, 2);
    lua_rawgeti(L, -1, idx);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); lua_pushnil(L); return 1; }
    lua_getfield(L, -1, "metadata");
    if (!lua_istable(L, -1)) { lua_pop(L, 3); lua_pushnil(L); return 1; }
    lua_getfield(L, -1, key);
    return 1;
}

// UnitBuff(unitId, index) / UnitDebuff(unitId, index)
// Returns: name, rank, icon, count, debuffType, duration, expirationTime, caster, isStealable, shouldConsolidate, spellId

static int lua_GetLocale(lua_State* L) {
    auto* svc = getLuaServices(L);
    auto* profile = svc && svc->expansionRegistry
        ? svc->expansionRegistry->getActive() : nullptr;
    lua_pushstring(L, profile ? profile->locale.c_str() : "enUS");
    return 1;
}

static int lua_GetBuildInfo(lua_State* L) {
    auto* svc = getLuaServices(L);
    auto* profile = svc && svc->expansionRegistry
        ? svc->expansionRegistry->getActive() : nullptr;
    if (!profile) {
        lua_pushstring(L, "3.3.5a");
        lua_pushnumber(L, 12340);
        lua_pushstring(L, "");
        lua_pushnumber(L, 30300);
        return 4;
    }

    const std::string version = profile->versionString();
    uint32_t tocVersion = 11200;
    if (profile->majorVersion == 2) tocVersion = 20400;
    else if (profile->majorVersion >= 3) tocVersion = 30300;

    lua_pushstring(L, version.c_str());
    lua_pushnumber(L, profile->build);
    lua_pushstring(L, "");
    lua_pushnumber(L, tocVersion);
    return 4;
}

static int lua_GetCurrentMapAreaID(lua_State* L) {
    // One past the WorldMapArea id, which is what 3.3.5 answers here. The
    // interface's own arithmetic says so and is the authority: it takes this
    // minus one and hands the result to SetMapByID, and treats a negative
    // result as "no zone, ask which continent instead". So zero means a
    // continent is showing, and every other value is an id plus one.
    //
    // The physical map id was answered instead - 0 for Eastern Kingdoms, 571
    // for Northrend - which is a different number in a different space. It
    // made the Wintergrasp check (area 502) never true, and the windowed-size
    // toggle reopen the map somewhere else.
    if (auto* svc = getLuaServices(L)) {
        if (svc->getMapWorldAreaId) {
            const uint32_t wma = svc->getMapWorldAreaId();
            lua_pushnumber(L, wma == 0 ? 0 : static_cast<lua_Number>(wma + 1));
            return 1;
        }
    }
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->getCurrentMapId() : 0);
    return 1;
}

// The zone's name from its id, the way the texts were answered before the
// client's own texts were worked out here, for where they are not known.
static std::string zoneNameFromId(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return {};
    // The terrain under the player first, and the server's zone only as a
    // fallback: the server's reaches us on SMSG_INIT_WORLD_STATES alone, sent
    // when the server notices a zone change and not otherwise.
    uint32_t zoneId = 0;
    if (auto* svc = getLuaServices(L)) {
        if (svc->getLiveZoneId) zoneId = svc->getLiveZoneId();
    }
    if (zoneId == 0) zoneId = gh->getWorldStateZoneId();
    return zoneId != 0 ? gh->getWhoAreaName(zoneId) : std::string();
}

bool flyableHere(lua_State* L, uint64_t unit) {
    auto* gh = getGameHandler(L);
    auto* svc = getLuaServices(L);
    if (!gh || !svc || !svc->getZoneTexts) return false;
    const auto t = svc->getZoneTexts();
    if (!t.known) return false;
    // The subzone's row where it has one, the zone's otherwise.
    const uint32_t area = (t.subzoneId != 0 && !gh->getAreaName(t.subzoneId).empty())
        ? t.subzoneId : t.zoneId;
    const uint32_t flags = gh->getAreaFlags(area);
    if ((flags & 0x400u) == 0 || (flags & 0x20000000u) != 0) return false;
    if (unit == 0) return true;
    const auto e = gh->getEntityManager().getEntity(unit);
    constexpr uint32_t kNorthrend = 571;
    constexpr uint32_t kColdWeatherFlying = 54197;
    if (e && e->getType() == game::ObjectType::PLAYER && gh->getCurrentMapId() == kNorthrend &&
        !(gh->getSpellHandler() && gh->getSpellHandler()->hasKnownSpell(kColdWeatherFlying))) {
        return false;
    }
    return true;
}

/// IsFlyableArea() (0x00612260) → 1 where the player may fly, else nil.
static int lua_IsFlyableArea(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint64_t player = gh && gh->getEntityManager().getEntity(gh->getPlayerGuid())
        ? gh->getPlayerGuid() : 0;
    if (flyableHere(L, player)) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

static LuaServices::ZoneTexts zoneTexts(lua_State* L) {
    if (auto* svc = getLuaServices(L)) {
        if (svc->getZoneTexts) return svc->getZoneTexts();
    }
    return {};
}

// GetZoneText (0x00515570), GetRealZoneText (0x005155a0), GetSubZoneText
// (0x005155d0) and GetMinimapZoneText (0x00515600): the four texts 0x005204c0
// keeps, "" for none. Four different answers - in Dalaran the zone is
// Dalaran and the subzone the district or shop the player is in, where all
// four used to be the zone of the chunk under the city, Crystalsong Forest.
static int lua_GetZoneText(lua_State* L) {
    const auto t = zoneTexts(L);
    lua_pushstring(L, t.known ? t.zone.c_str() : zoneNameFromId(L).c_str());
    return 1;
}

static int lua_GetRealZoneText(lua_State* L) {
    const auto t = zoneTexts(L);
    lua_pushstring(L, t.known ? t.realZone.c_str() : zoneNameFromId(L).c_str());
    return 1;
}

static int lua_GetSubZoneText(lua_State* L) {
    const auto t = zoneTexts(L);
    lua_pushstring(L, t.known ? t.subzone.c_str() : "");
    return 1;
}

static int lua_GetMinimapZoneText(lua_State* L) {
    const auto t = zoneTexts(L);
    lua_pushstring(L, t.known ? t.minimap.c_str() : zoneNameFromId(L).c_str());
    return 1;
}

// SetupFullscreenScale(frame) - the scale of a full-screen frame.
//
// The client's own, and it was a no-op here: frame:SetScale(min(1, aspect *
// 0.75)) for a frame that sits outside UIParent's scale, worked out for this
// client's root by fullscreenFrameScale. WorldMapFrame_OnShow is the caller
// that matters - the full-size map takes the whole height of the screen,
// whatever the interface scale.
static int lua_SetupFullscreenScale(lua_State* L) {
    if (!lua_istable(L, 1)) return luaL_error(L, "Usage: SetupFullscreenScale(frame)");
    auto* tree = getWidgetTree(L);
    const auto* root = tree ? tree->get(tree->rootId()) : nullptr;
    if (!root) return 0;
    // Through the frame's own SetScale, so the change goes wherever any other
    // scale change goes.
    lua_getfield(L, 1, "SetScale");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return 0; }
    lua_pushvalue(L, 1);
    lua_pushnumber(L, ui::fullscreenFrameScale(root->rectW, root->rectH));
    if (lua_pcall(L, 2, 0, 0) != 0) {
        LOG_WARNING("SetupFullscreenScale: ", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    return 0;
}

// --- World Map Navigation API ---
// --- World Map Navigation API ---

// Map ID → continent mapping
static int mapIdToContinent(uint32_t mapId) {
    switch (mapId) {
        case 0:   return 2; // Eastern Kingdoms
        case 1:   return 1; // Kalimdor
        case 530: return 3; // Outland
        case 571: return 4; // Northrend
        default:  return 0; // Instance or unknown
    }
}

// Internal tracked map state (which continent/zone the map UI is viewing)
static int s_mapContinent = 0;
static int s_mapZone = 0;

/// The map view changed, so say so.
///
/// Fired unconditionally, including when the view was already what it is being
/// set to. That is not laziness - watchframe.lua calls SetMapToCurrentZone
/// purely for the side effect, and says so beside the call: "forces WatchFrame
/// event via the WORLD_MAP_UPDATE event, needed to restore the POIs in the
/// tracker to the current zone". Firing only on a change would drop exactly the
/// case that line exists for.
static void fireWorldMapUpdate(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
    auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (engine) engine->fireEvent("WORLD_MAP_UPDATE", {});
}

// SetMapToCurrentZone() - sets map view to the player's current zone
static int lua_SetMapToCurrentZone(lua_State* L) {
    // Called every time the map is shown. It set two statics that nothing
    // reads any more and never told the map, so a map left on another zone
    // stayed there when reopened.
    if (auto* svc = getLuaServices(L)) {
        if (svc->showPlayerMapZone) svc->showPlayerMapZone();
    }
    auto* gh = getGameHandler(L);
    if (gh) {
        s_mapContinent = mapIdToContinent(gh->getCurrentMapId());
        s_mapZone = static_cast<int>(gh->getWorldStateZoneId());
    }
    fireWorldMapUpdate(L);
    return 0;
}

// GetCurrentMapContinent() → continentId (1=Kalimdor, 2=EK, 3=Outland, 4=Northrend)
static int lua_GetCurrentMapContinent(lua_State* L) {
    // What the map is showing, asked of the map. This kept a static of its
    // own instead, set from the player's position on first use and from
    // SetMapZoom after that, and the map itself was never consulted or told -
    // so the two dropdowns and the zoom-out button moved a number no one drew
    // from while the map stayed where it was.
    if (auto* svc = getLuaServices(L)) {
        if (svc->getMapContinentIndex) {
            const int shown = svc->getMapContinentIndex();
            // Said once per disagreement, because this one is asked from
            // WorldMapFrame_OnUpdate and a line per frame is not a report.
            //
            // Zero from the map means WORLD or COSMIC level, and both
            // dropdowns answer it with UIDropDownMenu_ClearAll - so a pick
            // that moved the number here but did not move the map leaves the
            // dropdown blank, which reads as the dropdown not working.
            static int lastReported = -1;
            if (shown == 0 && s_mapContinent != 0 && lastReported != s_mapContinent) {
                lastReported = s_mapContinent;
                LOG_WARNING("GetCurrentMapContinent: asked for continent ",
                            s_mapContinent, " and the map is showing none, so"
                            " both dropdowns clear themselves");
            } else if (shown != 0) {
                lastReported = -1;
            }
            lua_pushnumber(L, shown);
            return 1;
        }
    }
    if (s_mapContinent == 0) {
        auto* gh = getGameHandler(L);
        if (gh) s_mapContinent = mapIdToContinent(gh->getCurrentMapId());
    }
    lua_pushnumber(L, s_mapContinent);
    return 1;
}

// GetCurrentMapZone() → zoneId
static int lua_GetCurrentMapZone(lua_State* L) {
    // A row in the zone dropdown, not an area id. The fallback below answered
    // the area id - a number in the thousands where the dropdown wanted a
    // position in a list of a few dozen - so the selected row was never the
    // one being shown. SetMapZoom, meanwhile, wrote a row number into the same
    // static, and the two meanings sat in one variable.
    if (auto* svc = getLuaServices(L)) {
        if (svc->getMapZoneIndex) {
            lua_pushnumber(L, svc->getMapZoneIndex());
            return 1;
        }
    }
    if (s_mapZone == 0) {
        auto* gh = getGameHandler(L);
        if (gh) s_mapZone = static_cast<int>(gh->getWorldStateZoneId());
    }
    lua_pushnumber(L, s_mapZone);
    return 1;
}

// SetMapZoom(continent [, zone]) - sets map view to continent/zone
static int lua_SetMapZoom(lua_State* L) {
    s_mapContinent = static_cast<int>(luaL_checknumber(L, 1));
    s_mapZone = static_cast<int>(luaL_optnumber(L, 2, 0));
    // Tell the map, which this never did. Every route out of a zone map runs
    // through here - the zone dropdown, the continent dropdown, and four of
    // the zoom-out button's six branches - so none of them changed what was
    // drawn.
    // At warning when the map turns the pair down, because every symptom of
    // that is a non-event: the dropdown keeps the row it was clicked on, the
    // map stays where it was, and nothing is written down. An unknown
    // continent and a zone row past the end of that continent's list are the
    // two ways in, and they are the two the dropdowns can produce.
    if (auto* svc = getLuaServices(L)) {
        if (!svc->setMapByIndex) {
            LOG_WARNING("SetMapZoom(", s_mapContinent, ", ", s_mapZone,
                        "): no map to tell - the view number moved and nothing"
                        " drew from it");
        } else if (!svc->setMapByIndex(s_mapContinent, s_mapZone)) {
            LOG_WARNING("SetMapZoom(", s_mapContinent, ", ", s_mapZone,
                        "): the map refused the pair, so it stays where it was");
        }
    }
    fireWorldMapUpdate(L);
    return 0;
}

// GetMapContinents() → "Kalimdor", "Eastern Kingdoms", ...
static int lua_GetMapContinents(lua_State* L) {
    if (auto* svc = getLuaServices(L)) {
        if (svc->getMapContinentNames) {
            const auto names = svc->getMapContinentNames();
            if (!names.empty() &&
                lua_checkstack(L, static_cast<int>(names.size()))) {
                for (const auto& n : names) lua_pushstring(L, n.c_str());
                return static_cast<int>(names.size());
            }
        }
    }
    lua_pushstring(L, "Kalimdor");
    lua_pushstring(L, "Eastern Kingdoms");
    lua_pushstring(L, "Outland");
    lua_pushstring(L, "Northrend");
    return 4;
}

// GetMapZones(continent) → the zones on that continent, in dropdown order
//
// Four made-up names per continent before this - "a minimal representative
// set", which is a list nobody's zone is on. The row picked from it was then
// handed to SetMapZoom as the zone to show, so the dropdown offered four
// zones out of dozens and choosing one of them did nothing anyway.
static int lua_GetMapZones(lua_State* L) {
    const int cont = static_cast<int>(luaL_checknumber(L, 1));
    auto* zonesSvc = getLuaServices(L);
    if (!zonesSvc || !zonesSvc->getMapZoneNames) {
        LOG_WARNING("GetMapZones(", cont, "): no map to ask, so the zone"
                    " dropdown is built empty");
    }
    if (auto* svc = zonesSvc) {
        if (svc->getMapZoneNames) {
            const auto names = svc->getMapZoneNames(cont);
            // Lua promises a C function twenty free slots and no more. Four
            // hard-coded names never came near it; a continent's real zone
            // list is dozens, and pushing them unasked writes past the stack.
            if (!names.empty() &&
                !lua_checkstack(L, static_cast<int>(names.size()))) {
                LOG_WARNING("GetMapZones(", cont, "): no room on the Lua stack"
                            " for ", names.size(), " zones, so the dropdown is"
                            " built empty");
                return 0;
            }
            // An empty list is a blank zone dropdown and no other trace. The
            // continent may be one this client has no zone data for, or the
            // map data may not be loaded yet - either way the dropdown that
            // opens is the same empty one.
            if (names.empty()) {
                LOG_WARNING("GetMapZones(", cont, "): the map lists no zones on"
                            " that continent, so the zone dropdown is empty");
            }
            for (const auto& n : names) lua_pushstring(L, n.c_str());
            return static_cast<int>(names.size());
        }
    }
    return 0;
}

// GetNumMapLandmarks() → how many pins the map being shown has
static int lua_GetNumMapLandmarks(lua_State* L) {
    auto* svc = getLuaServices(L);
    lua_pushnumber(L, (svc && svc->getMapLandmarks)
        ? static_cast<lua_Number>(svc->getMapLandmarks().size()) : 0);
    return 1;
}

/// GetMapLandmarkInfo(i) → name, description, textureIndex, x, y, mapLinkID
///
/// The area POIs on whatever the map is showing. Answered zero of them while
/// the client had them loaded from AreaPOI.dbc and was drawing them on its own
/// map, so FrameXML's map came up with no pins on it at all.
///
/// The pins FrameXML draws here are the same rows the reference draws - AreaPOI
/// - but it reaches them its own way, and the way says what is missing.  Its
/// list is rebuilt inside the routine that *changes* the shown map (0x00546310,
/// which fills 0x00be8ee0 entries of id, x, y and two kind dwords, stride 0x14)
/// from a copy of the AreaPOI rows: 0x00ad3118 records at 0x00ad312c, stride
/// 0x58 - twenty-two dwords, which is why the stride is not the file's 216-byte
/// record size: the client's copy stops after the map link and has the localised
/// string blocks folded in.  Field for field the reads land on AreaPOI's own
/// columns: +0x40 the flags, +0x44 the area id, +0x48 the name, +0x4c the
/// description, +0x50 a world state, +0x54 the map link, and the nine Icon
/// columns from +0x8.
///
/// What it does with them: 0x005445f0 projects the row into the map's space and
/// one that lands outside is not in the list; the area id has to be at least 1
/// and resolve through the index at 0x00ad3154 to a row whose +0x28 is negative
/// or whose +0xc answers at 0x00546150; and a row naming a world state is in the
/// list only while that state is satisfied (0x00548d10), the same rule the
/// minimap already follows (minimap_blips.hpp, areaPoiIcon).  Read back out at
/// 0x00545a30 the icon comes from that state's value minus one, clamped to the
/// nine - and in this install's AreaPOI.dbc every row reads zero for the state
/// column, of all 738 rows, so the reference takes Icon[0] like this does.  The
/// flag word answers bit 0x200 as the seventh value.
///
/// Alongside them the reference keeps one record per map built from the player's
/// own position, and a second kind of entry - marked in the entry's third dword,
/// and the only kind ClickLandmark walks - answered from a table it looks an id
/// up in, with a fixed textureIndex of 6, no description, and no link.  That
/// table (0x00ad4f0c rows, 0x00ad4f20, stride 0x44) is still unnamed: its rows
/// carry a map's id second and up to four area ids third to sixth.
static int lua_GetMapLandmarkInfo(lua_State* L) {
    auto* svc = getLuaServices(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!svc || !svc->getMapLandmarks || index < 1) return luaReturnNil(L);
    const auto marks = svc->getMapLandmarks();
    if (index > static_cast<int>(marks.size())) return luaReturnNil(L);
    const auto& m = marks[static_cast<size_t>(index) - 1];

    lua_pushstring(L, m.name.c_str());          // 1: name
    lua_pushstring(L, m.description.c_str());   // 2: description
    // Which cell of the POI icon atlas to draw. AreaPOI's icon type is that
    // index in the client's own drawing too.
    lua_pushnumber(L, m.icon);                  // 3: textureIndex
    lua_pushnumber(L, m.x);                     // 4: x across the map
    lua_pushnumber(L, m.y);                     // 5: y down the map
    // A landmark that leads to another map - AreaPOI's map link column, +0x54
    // of the client's record, and a per-row number in the file (its first rows
    // carry 1, 12, 26, 40, 49, 63 there).  Still nil, deliberately: answering
    // it sends worldmapframe.lua's WorldMapPOI_OnClick down its ClickLandmark
    // branch instead of the plain map click, and ClickLandmark does nothing
    // here yet - the table it walks to turn a link into a map is the unnamed
    // one above.  So the link waits for the verb.
    lua_pushnil(L);                             // 6: mapLinkID
    // Whether the battlefield minimap shows it too. Seven values, not six:
    // blizzard_battlefieldminimap.lua gates every pin on this one, so a false
    // there hides it on the smaller map - and the flag is AreaPOI's own, bit
    // 0x200 of the word the client reads at +0x40.  This install's file has the
    // bit set for 681 of its 738 rows and clear for 57, so the always-true
    // answer here was not a safe default but a different list: 57 pins the
    // reference does not draw on that window were drawn here.
    lua_pushboolean(L, (m.flags & rendering::minimap_blips::kPoiOnBattleMap) != 0
        ? 1 : 0);                               // 7: showInBattleMap
    return 7;
}

/// GetNumMapOverlays() / GetMapOverlayInfo(i)
///
/// The explored-area art: one entry per overlay, each a texture prefix and the
/// pixel rectangle it occupies on the zone map. The interface slices it into
/// 256-pixel tiles named prefix1, prefix2 and so on, which is the same
/// arrangement the client's own map renderer reads.
///
/// The count answered zero, so the loop that builds them never ran once and
/// the map was drawn with nothing on it but its background.
static int lua_GetNumMapOverlays(lua_State* L) {
    auto* svc = getLuaServices(L);
    lua_pushnumber(L, (svc && svc->getMapOverlays)
        ? static_cast<lua_Number>(svc->getMapOverlays().size()) : 0);
    return 1;
}

static int lua_GetMapOverlayInfo(lua_State* L) {
    auto* svc = getLuaServices(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!svc || !svc->getMapOverlays || index < 1) return luaReturnNil(L);
    const auto overlays = svc->getMapOverlays();
    if (index > static_cast<int>(overlays.size())) return luaReturnNil(L);
    const auto& o = overlays[static_cast<size_t>(index) - 1];

    lua_pushstring(L, o.texture.c_str());   // 1: texture prefix
    lua_pushnumber(L, o.width);             // 2: width in pixels
    lua_pushnumber(L, o.height);            // 3: height in pixels
    lua_pushnumber(L, o.offsetX);           // 4: x offset on the zone map
    lua_pushnumber(L, o.offsetY);           // 5: y offset
    // The "map point" pair, which WorldMapFrame_Update reads and never uses.
    lua_pushnumber(L, 0);                   // 6
    lua_pushnumber(L, 0);                   // 7
    return 7;
}

/// GetTrackingTexture() → the icon for what the minimap is tracking, or nil.
///
/// Nothing is tracked here: tracking is a spell effect this client does not
/// model, and GetNumTrackingTypes already answers none. Said explicitly
/// because the missing-API fallback answers with an object, and an object is
/// not nil - MiniMapTrackingIcon:SetTexture(GetTrackingTexture()) would then
/// be handed a table where a path belongs and the button would show the
/// tracking icon for a tracking type that does not exist.
// ── Minimap tracking ───────────────────────────────────────────────────────
//
// The tracking menu is not a fixed list. It is whatever tracking the player
// has learned, which is the known spells applying aura 44 (creatures) or 45
// (resources) - Spell.dbc's EffectApplyAuraName, and the only thing in the
// spell data that tells a tracking spell from any other buff. The effect id
// beside it says an aura is applied but never which one.
//
// All three of these used to be stubs: no texture, no types, and no binding
// at all for the count. GetNumTrackingTypes being absent was the worst of
// them, because a missing global answers nil and the initialiser opens with
// `for id = 1, count`, which raises on nil and took the whole menu down.

/// Aura ids that make a spell a tracking spell.
static constexpr uint32_t kAuraTrackCreatures = 44;
static constexpr uint32_t kAuraTrackResources = 45;

/// The player's tracking spells, ordered by name.
///
/// Sorted rather than left in the known-spell set's own order, which is a hash
/// order: the menu would otherwise list the same spells differently from one
/// session to the next.
static std::vector<uint32_t> trackingSpells(game::GameHandler* gh) {
    std::vector<uint32_t> out;
    if (!gh) return out;
    for (uint32_t sid : gh->getKnownSpells()) {
        // Asking for the name is what fills the cache; the entry is not there
        // to be read until something has.
        gh->getSpellName(sid);
        auto it = gh->spellNameCacheRef().find(sid);
        if (it == gh->spellNameCacheRef().end()) continue;
        for (uint32_t aura : it->second.effectAuraIds) {
            if (aura == kAuraTrackCreatures || aura == kAuraTrackResources) {
                out.push_back(sid);
                break;
            }
        }
    }
    std::sort(out.begin(), out.end(), [gh](uint32_t a, uint32_t b) {
        return gh->getSpellName(a) < gh->getSpellName(b);
    });
    return out;
}

/// Whether that tracking is the one currently running.
static bool trackingActive(game::GameHandler* gh, uint32_t spellId) {
    if (!gh) return false;
    for (const auto& aura : gh->getPlayerAuras()) {
        if (aura.spellId == spellId) return true;
    }
    return false;
}

// The menu's own entries after the spells (0x00a11c50, offered by class by
// 0x0057eb00): the town services, the mailbox and low-level quests. 3.x's;
// the earlier menus are the spells alone.
static std::vector<int> otherTrackingEntries(lua_State* L, game::GameHandler* gh) {
    if (!gh || interfaceVersion(L) < 30000) return {};
    return game::minimap_tracking::offered(gh->getPlayerClass());
}

/// The chosen entry, read back from minimapTrackedInfo the first time it is
/// asked for (0x0057e070 keeps it there by name).
static int otherTrackingActive(lua_State* L, game::GameHandler* gh) {
    static bool restored = false;
    if (gh && !restored) {
        restored = true;
        const int stored = game::minimap_tracking::byName(storedCVarValue("minimapTrackedInfo", "").c_str());
        const auto offered = otherTrackingEntries(L, gh);
        if (stored >= 0 && std::find(offered.begin(), offered.end(), stored) != offered.end()) {
            gh->setMinimapOtherTracking(stored);
        }
    }
    return gh ? gh->minimapOtherTracking() : -1;
}

/// Choose an entry, or none (0x0057e070): kept by name, and the menu told.
static void setOtherTracking(game::GameHandler* gh, int index) {
    if (!gh) return;
    gh->setMinimapOtherTracking(index);
    setStoredCVar("minimapTrackedInfo",
                  index >= 0 ? game::minimap_tracking::kOther[static_cast<size_t>(index)].name : "");
    gh->fireAddonEvent("MINIMAP_UPDATE_TRACKING", {});
}

static std::string otherTrackingTexture(int index) {
    return std::string("Interface\\Minimap\\Tracking\\") +
           game::minimap_tracking::kOther[static_cast<size_t>(index)].icon;
}

/// GetTrackingTexture() → the icon on the minimap button.
///
/// The button's own art is empty in the XML and comes entirely from here, so
/// answering nil left a blank square on the minimap. Nothing tracked is not
/// nothing to draw: it is the magnifying glass, which is what a stock client
/// shows and what makes the button look like something to click. The menu's
/// own entry, when one is chosen, before a spell's (0x0057f4f0).
static int lua_GetTrackingTexture(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int other = otherTrackingActive(L, gh);
    if (other >= 0) {
        lua_pushstring(L, otherTrackingTexture(other).c_str());
        return 1;
    }
    for (uint32_t sid : trackingSpells(gh)) {
        if (!trackingActive(gh, sid)) continue;
        const std::string icon = gh->getSpellIconPath(sid);
        if (!icon.empty()) { lua_pushstring(L, icon.c_str()); return 1; }
    }
    // Nothing tracked, and what a stock client shows for that is not the same
    // picture in every expansion. 3.x has a button whose art is empty in the
    // XML and a magnifying glass to fill it with. 1.12 has neither: its
    // Minimap.xml hides the tracking frame outright when this answers nil, and
    // the 1.12 archives carry no Interface\Minimap\Tracking folder at all -
    // interface.MPQ, patch.MPQ and patch-2.MPQ hold nothing under that path. So
    // on turtle the answer named a file that does not exist, and the frame was
    // shown with an empty icon in it rather than hidden.
    //
    // Only 1.12 changes. The tracking dropdown and its None entry are 2.x's and
    // nothing here has been checked against a 2.4.3 tree, so every other
    // interface keeps the answer it already had.
    if (interfaceVersion(L) < 20000) return luaReturnNil(L);
    lua_pushstring(L, "Interface\\Minimap\\Tracking\\None");
    return 1;
}

/// GetNumTrackingTypes() → the spells the player knows, then the menu's own
/// entries the class is offered.
static int lua_GetNumTrackingTypes(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, static_cast<lua_Number>(trackingSpells(gh).size() + otherTrackingEntries(L, gh).size()));
    return 1;
}

/// GetTrackingInfo(index) → name, texture, active, category.
///
/// "spell" for the category, because these are spell icons and the menu uses
/// that to crop the icon's border - the same trim the action bar gives them.
/// The menu's own entries after them are "other" (0x0057f100): their
/// GlobalStrings name and their picture under Interface\Minimap\Tracking.
///
/// Four values and not five. The spell id would have been useful to
/// GameTooltip:SetTrackingSpell and belongs in none of them: 4.x defines a
/// fifth of its own - whether the entry nests under the one above it - and a
/// number in that position would be read as that. __WoweeActiveTrackingSpell
/// carries it instead, under a name no interface will ever call.
static int lua_GetTrackingInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    const auto spells = trackingSpells(gh);
    if (index >= 1 && index <= static_cast<int>(spells.size())) {
        const uint32_t sid = spells[static_cast<size_t>(index - 1)];
        lua_pushstring(L, gh->getSpellName(sid).c_str());
        lua_pushstring(L, gh->getSpellIconPath(sid).c_str());
        lua_pushboolean(L, trackingActive(gh, sid) ? 1 : 0);
        lua_pushstring(L, "spell");
        return 4;
    }
    const auto others = otherTrackingEntries(L, gh);
    const int o = index - 1 - static_cast<int>(spells.size());
    if (o < 0 || o >= static_cast<int>(others.size())) return 0;
    const int entry = others[static_cast<size_t>(o)];
    const char* key = game::minimap_tracking::kOther[static_cast<size_t>(entry)].name;
    lua_getglobal(L, key);
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, key);
    }
    lua_pushstring(L, otherTrackingTexture(entry).c_str());
    lua_pushboolean(L, otherTrackingActive(L, gh) == entry ? 1 : 0);
    lua_pushstring(L, "other");
    return 4;
}

/// __WoweeActiveTrackingSpell() → the spell id of the tracking now running.
///
/// This client's own and not an interface function: GameTooltip:SetTrackingSpell
/// needs the id to render the spell's own tooltip, and every real API that
/// could have carried it is a shape some expansion has already defined.
static int lua_ActiveTrackingSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    for (uint32_t sid : trackingSpells(gh)) {
        if (!trackingActive(gh, sid)) continue;
        lua_pushnumber(L, static_cast<lua_Number>(sid));
        return 1;
    }
    return luaReturnNil(L);
}

/// SetTracking(index), as 0x0057f380 does it: a spell is cast - casting is
/// how tracking is turned on, there is no separate message. One of the menu's
/// own entries takes the place of any tracking spell running, whose aura is
/// cancelled. Nil, the menu's "None", cancels both.
static int lua_SetTracking(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const auto spells = trackingSpells(gh);
    const auto cancelSpellTracking = [&]() {
        for (uint32_t sid : spells) {
            if (trackingActive(gh, sid)) gh->cancelAura(sid);
        }
    };
    if (!lua_isnumber(L, 1)) {
        cancelSpellTracking();
        setOtherTracking(gh, -1);
        return 0;
    }
    const int index = static_cast<int>(lua_tonumber(L, 1));
    if (index >= 1 && index <= static_cast<int>(spells.size())) {
        // 0x0057f380 -> 0x0080da40: a tracking spell's cast asks the taint
        // check like any other.
        const uint32_t spell = spells[static_cast<size_t>(index - 1)];
        if (spellCastAllowed(L, gh, spell)) gh->castSpell(spell, 0);
        return 0;
    }
    const auto others = otherTrackingEntries(L, gh);
    const int o = index - 1 - static_cast<int>(spells.size());
    if (o < 0 || o >= static_cast<int>(others.size())) return 0;
    cancelSpellTracking();
    setOtherTracking(gh, others[static_cast<size_t>(o)]);
    return 0;
}


static int lua_GetGameTime(lua_State* L) {
    // Returns server game time as hours, minutes
    auto* gh = getGameHandler(L);
    float gt = gh ? gh->getGameTime() : -1.0f;
    if (gt < 0.0f) {
        // The server has not said yet. Answer with the local clock rather than
        // a negative hour, which is what the interface's own clock would then
        // print - and it is the same clock the sky falls back to, so the two
        // agree until the server's time arrives.
        const std::time_t now = std::time(nullptr);
        const std::tm lt = core::localTime(now);
        gt = static_cast<float>(lt.tm_hour) + static_cast<float>(lt.tm_min) / 60.0f;
    }
    const int hours = static_cast<int>(gt) % 24;
    const int mins = static_cast<int>((gt - static_cast<int>(gt)) * 60.0f);
    lua_pushnumber(L, hours);
    lua_pushnumber(L, mins);
    return 2;
}


static int lua_IsInInstance(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushboolean(L, 0); lua_pushstring(L, "none"); return 2; }
    bool inInstance = gh->isInInstance();
    lua_pushboolean(L, inInstance);
    lua_pushstring(L, inInstance ? "party" : "none");  // simplified: "none", "party", "raid", "pvp", "arena"
    return 2;
}

/// GetInstanceInfo() (0x0051a8c0): name, instanceType, difficultyIndex,
/// difficultyName, maxPlayers, playerDifficulty, isDynamicInstance - the
/// instance's own difficulty (SMSG_INSTANCE_DIFFICULTY), counted from one, and
/// its MapDifficulty.dbc row's name and size. A map Map.dbc has no row for
/// answers nil, "none", 0, "", 0, 0, nil.
static int lua_GetInstanceInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto info = gh ? gh->getInstanceInfo() : game::GameHandler::InstanceInfo{};
    if (!info.known) {
        lua_pushnil(L);
        lua_pushstring(L, "none");
        lua_pushnumber(L, 0);
        lua_pushstring(L, "");
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnil(L);
        return 7;
    }
    lua_pushstring(L, info.name.c_str());
    lua_pushstring(L, info.type.c_str());
    lua_pushnumber(L, info.difficulty + 1);
    lua_pushstring(L, info.difficultyName.c_str());
    lua_pushnumber(L, info.maxPlayers);
    lua_pushnumber(L, info.playerDifficulty);
    lua_pushboolean(L, info.dynamic ? 1 : 0);
    return 7;
}

static int lua_GetInstanceDifficulty(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? (gh->getInstanceDifficulty() + 1) : 1);
    return 1;
}

static int lua_strsplit(lua_State* L) {
    const char* delim = luaL_checkstring(L, 1);
    const char* str = luaL_checkstring(L, 2);
    if (!delim[0]) { lua_pushstring(L, str); return 1; }
    int count = 0;
    std::string s(str);
    // One value per field, and the field count follows from the string rather
    // than from anything this client chose. Lua guarantees only a small slack
    // above the arguments, and pushing past the top corrupts the heap rather
    // than raising - so the room is asked for before any of it is used.
    // A chat line of commas is all it takes.
    const size_t fields = static_cast<size_t>(
        std::count(s.begin(), s.end(), delim[0])) + 1;
    if (!lua_checkstack(L, static_cast<int>(fields) + 1)) {
        lua_pushstring(L, str);
        return 1;
    }
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t found = s.find(delim[0], pos);
        if (found == std::string::npos) {
            lua_pushstring(L, s.substr(pos).c_str());
            count++;
            break;
        }
        lua_pushstring(L, s.substr(pos, found - pos).c_str());
        count++;
        pos = found + 1;
    }
    return count;
}

// strtrim(str) - remove leading/trailing whitespace
static int lua_strtrim(lua_State* L) {
    const char* str = luaL_checkstring(L, 1);
    std::string s(str);
    size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    lua_pushstring(L, (start == std::string::npos) ? "" : s.substr(start, end - start + 1).c_str());
    return 1;
}

/// strlenutf8(s) - the number of characters, where string.len counts bytes.
///
/// Unbound, it answered nil through the fallback, and both callers do
/// arithmetic on the result rather than checking it: autocomplete.lua and
/// chatframe.lua compute an offset as
/// `GetUTF8CursorPosition() - strlenutf8(command) - 1`. A nil there raises, so
/// typing a slash command or a player name took chat autocomplete down.
static int lua_strlenutf8(lua_State* L) {
    size_t len = 0;
    const char* s = luaL_optlstring(L, 1, "", &len);
    int chars = 0;
    for (size_t i = 0; i < len; ++i) {
        // A continuation byte is 10xxxxxx; every other byte opens a character.
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++chars;
    }
    lua_pushnumber(L, chars);
    return 1;
}

/// strconcat(...) - every argument joined, as one string (0x00816d80: a
/// lua_concat of the whole stack). With none it is the empty string.
static int lua_strconcat(lua_State* L) {
    lua_concat(L, lua_gettop(L));
    return 1;
}

/// strreplace(s, find, replace [, max]) -> result, count (0x00816c40).
///
/// A plain substring rather than a pattern, replaced up to `max` times (0 or
/// none is every one), into the client's 4096-byte buffer: what does not fit
/// is cut. No replacement answers `s` itself. The client's loop never ends on
/// an empty `find` with no limit; this one stops once the buffer is full, or
/// at once where an empty replacement would never fill it.
static int lua_strreplace(lua_State* L) {
    size_t sLen = 0, findLen = 0, replLen = 0;
    const char* s = luaL_checklstring(L, 1, &sLen);
    const char* find = luaL_checklstring(L, 2, &findLen);
    const char* repl = luaL_checklstring(L, 3, &replLen);
    const lua_Integer limit = luaL_optinteger(L, 4, 0);
    constexpr size_t kBuffer = 0x1000;
    std::string out;
    size_t room = kBuffer;
    size_t pos = 0;
    int count = 0;
    for (; limit == 0 || count < limit; ++count) {
        const char* hit = std::strstr(s + pos, find);
        if (!hit) break;
        if (findLen == 0 && (room == 0 || replLen == 0)) break;
        const size_t lead = std::min(static_cast<size_t>(hit - (s + pos)), room);
        out.append(s + pos, lead);
        room -= lead;
        const size_t put = std::min(replLen, room);
        out.append(repl, put);
        room -= put;
        pos = static_cast<size_t>(hit - s) + findLen;
    }
    if (count < 1) {
        lua_pushlstring(L, s, sLen);
    } else {
        out.append(s + pos, std::min(sLen - pos, room));
        lua_pushlstring(L, out.data(), out.size());
    }
    lua_pushnumber(L, count);
    return 2;
}

/// difftime(t1 [, t2]) - t1 - t2 in seconds, t2 defaulting to 0 (0x008178a0).
/// Both are taken as whole seconds first.
static int lua_wow_difftime(lua_State* L) {
    const auto t2 = static_cast<std::time_t>(static_cast<int32_t>(luaL_optnumber(L, 2, 0)));
    const auto t1 = static_cast<std::time_t>(static_cast<int32_t>(luaL_checknumber(L, 1)));
    lua_pushnumber(L, std::difftime(t1, t2));
    return 1;
}

/// debugprofilestart() / debugprofilestop() (0x00817350, 0x00817370): the
/// performance counter kept, and the milliseconds since. Until a start the
/// counter's own zero is the mark, as the client's static starts out.
static std::chrono::steady_clock::duration& debugProfileMark() {
    static std::chrono::steady_clock::duration mark{};
    return mark;
}

static int lua_debugprofilestart(lua_State* L) {
    (void)L;
    debugProfileMark() = std::chrono::steady_clock::now().time_since_epoch();
    return 0;
}

static int lua_debugprofilestop(lua_State* L) {
    const auto since = std::chrono::steady_clock::now().time_since_epoch() - debugProfileMark();
    lua_pushnumber(L, std::chrono::duration<double, std::milli>(since).count());
    return 1;
}

/// The one function the shipped client registers under every name whose
/// body was compiled out of it (0x008e5250): it answers nothing. The debug
/// family beside these - debugload, debuginfo, debugprint, debugdump,
/// debugbreak, debughook, debugtimestamp - and the developer toggles
/// Bindings.xml and the world map still name.
static int lua_compiledOutNothing(lua_State* L) {
    (void)L;
    return 0;
}

// wipe(table) - clear all entries from a table
static int lua_wipe(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    // Remove all integer keys
    int len = static_cast<int>(lua_objlen(L, 1));
    for (int i = len; i >= 1; i--) {
        lua_pushnil(L);
        lua_rawseti(L, 1, i);
    }
    // Remove all string keys
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        lua_pop(L, 1);       // pop value
        lua_pushvalue(L, -1); // copy key
        lua_pushnil(L);
        lua_rawset(L, 1);    // table[key] = nil
    }
    lua_pushvalue(L, 1);
    return 1;
}

// date(format) - safe date function (os.date was removed)
/// date(format, time) - the clock, as WoW exposes it.
///
/// Both shapes FrameXML uses: "*t" for a table of parts, a strftime string
/// otherwise, and an optional timestamp. Formatting "*t" as a strftime string
/// yields the literal "*t", which is what BetterDate then indexed for an hour.
static int lua_wow_date(lua_State* L) {
    const char* fmt = luaL_optstring(L, 1, "%c");
    const std::time_t when = lua_isnumber(L, 2)
        ? static_cast<std::time_t>(lua_tonumber(L, 2))
        : std::time(nullptr);

    std::tm parts{};
    parts = core::localTime(when);

    if (std::strcmp(fmt, "*t") == 0 || std::strcmp(fmt, "!*t") == 0) {
        lua_newtable(L);
        auto set = [&](const char* key, int value) {
            lua_pushinteger(L, value);
            lua_setfield(L, -2, key);
        };
        set("year", parts.tm_year + 1900);
        set("month", parts.tm_mon + 1);
        set("day", parts.tm_mday);
        set("hour", parts.tm_hour);
        set("min", parts.tm_min);
        set("sec", parts.tm_sec);
        set("wday", parts.tm_wday + 1);
        set("yday", parts.tm_yday + 1);
        lua_pushboolean(L, parts.tm_isdst > 0);
        lua_setfield(L, -2, "isdst");
        return 1;
    }

    char out[256];
    const size_t n = std::strftime(out, sizeof(out), fmt, &parts);
    lua_pushlstring(L, out, n);
    return 1;
}

// time() - current unix timestamp
static int lua_wow_time(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(time(nullptr)));
    return 1;
}

// GetTime() - returns elapsed seconds since engine start (shared epoch)
static int lua_wow_gettime(lua_State* L) {
    lua_pushnumber(L, luaGetTimeNow());
    return 1;
}

// Names FrameXML reaches for that this client has no state behind yet.
//
// Found by running the load with the missing-API fallback off, which is the
// only way to see them: with it on they answer and the gap is invisible.
// IsThreatWarningEnabled alone was asked 58 times in one load.
//
// Answering falsely is the point. Each returns what the feature being absent
// looks like - no threat warnings, no runes, nobody to pass loot to - so the
// caller takes the branch it would take on a client where that feature is off,
// rather than dividing by a nil.
static int lua_ReturnFalse(lua_State* L) { lua_pushboolean(L, 0); return 1; }
static int lua_ReturnTrue(lua_State* L)  { lua_pushboolean(L, 1); return 1; }
static int lua_ReturnNil(lua_State* L)   { lua_pushnil(L); return 1; }
static int lua_ReturnZero(lua_State* L)  { lua_pushnumber(L, 0.0); return 1; }

/// Which row the skill list has selected, which is UI state rather than
/// anything the game knows. Same shape as selectedFriend and selectedIgnore.
static int& selectedSkill() { static int v = 0; return v; }
static int lua_ReturnNothing(lua_State*) { return 0; }

/// Which channel the channel-list panel has highlighted. Panel state with no
/// counterpart in the game, kept here so the getter and setter agree.
static int& selectedDisplayChannel() { static int selected = 0; return selected; }

/// Which of the four extra action bars are switched on: bottom-left,
/// bottom-right, right and the second right, in that order.
///
/// The real client mirrors these from the server's account data. Here they are
/// a preference with nowhere else to live, so they are kept beside the pair of
/// calls that read and write them - the setter used to accept them and forget,
/// which left the getter with nothing to answer.
static std::array<bool, 4>& actionBarToggles() {
    static std::array<bool, 4> shown{};
    return shown;
}

/// SetEuropeanNumbers' switch (0x00d413b8), which the client's formatting reads.
static bool& europeanNumbers() {
    static bool on = false;
    return on;
}

/// SetLayoutMode's value (frame manager +0xd08).
static int32_t& layoutMode() {
    static int32_t mode = 0;
    return mode;
}


/// A cooldown that is not running: start and duration both zero. Two values,
/// because the caller adds them together on the next line -
/// local start, duration = GetSummonFriendCooldown(); start + duration - and
/// one of them missing is arithmetic on nil.
/// The resolutions this client offers, as "WIDTHxHEIGHT" strings, and which of
/// them is current. One entry - the window as it actually is - because this
/// client does not enumerate modes.
///
/// UpdateMenuBarTop reads them together and immediately divides:
///   string.match((({GetScreenResolutions()})[GetCurrentResolution()] or ""),
///                "(%d+).-(%d+)")
/// then tonumber(width) / tonumber(height). An empty list makes both nil.
// GetScreenResolutions() → every mode, as "WxH", one return value each.
//
// It used to answer the window's current size and nothing else, so the video
// panel's dropdown had a single row and there was nothing to pick. The list is
// the one this client's own settings panel offers, shared rather than written
// twice, because the dropdown carries a *position* in it: it hands that index
// straight back to SetScreenResolution.
static int lua_GetScreenResolutions(lua_State* L) {
    for (int i = 0; i < ui::kNumDisplayResolutions; ++i)
        lua_pushstring(L, ui::displayResolutionLabel(i).c_str());
    return ui::kNumDisplayResolutions;
}

// GetCurrentResolution() → which of those the window is, counted from one.
static int lua_GetCurrentResolution(lua_State* L) {
    auto* svc = getLuaServices(L);
    const int idx = (svc && svc->getResolutionIndex) ? svc->getResolutionIndex() : 0;
    lua_pushnumber(L, idx + 1);
    return 1;
}

// SetScreenResolution(index) - the dropdown's row, counted from one.
//
// A no-op before this, which is why the comment beside RestoreVideoResolutionDefaults
// could say nothing above it was settable. Both records are moved together, so
// this client's own options do not show the size it had before.
/// The four anti-aliasing modes, in the shape the video panel reads them.
///
/// VideoOptionsResolutionPanel_GetMultisampleFormats walks its varargs in
/// threes - colour bits, depth bits, sample count - and formats each into
/// MULTISAMPLING_FORMAT_STRING. The colour and depth numbers are this client's
/// swapchain and depth buffer, which do not change with the mode; only the
/// sample count does, and those are the same four the client's own
/// Anti-aliasing setting offers.
static int lua_GetMultisampleFormats(lua_State* L) {
    static constexpr int kSamples[] = {1, 2, 4, 8};
    for (int samples : kSamples) {
        lua_pushnumber(L, 32);        // colour bits
        lua_pushnumber(L, 24);        // depth bits
        lua_pushnumber(L, samples);
    }
    return 3 * static_cast<int>(sizeof(kSamples) / sizeof(kSamples[0]));
}

/// The row of the list above that is in force, counted from one.
static int lua_GetCurrentMultisampleFormat(lua_State* L) {
    auto* svc = getLuaServices(L);
    const int row = (svc && svc->getAntiAliasingIndex) ? svc->getAntiAliasingIndex() : 0;
    lua_pushnumber(L, std::clamp(row, 0, 3) + 1);
    return 1;
}

/// SetMultisampleFormat(row) - the dropdown's row, counted from one.
static int lua_SetMultisampleFormat(lua_State* L) {
    auto* svc = getLuaServices(L);
    const int row = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (svc && svc->setAntiAliasingIndex && row >= 1) svc->setAntiAliasingIndex(row - 1);
    return 0;
}

/// GetRefreshRates() - one zero, which is this API's own word for "none".
///
/// Not nothing. VideoOptionsResolutionPanel_GetRefreshRates tests for exactly
/// one argument equal to zero and, finding it, disables the dropdown and greys
/// its label and text - which is what the real client does in windowed mode,
/// where the desktop owns the refresh rate. Returning nothing instead runs its
/// loop zero times, and the control was left blank, enabled and clickable with
/// nothing behind it.
///
/// The general rule that a list-returning function says "none" by returning
/// nothing still holds; this one is a caller that defined its own sentinel and
/// checks for it first.
/// The sound output drivers the Sound panel can offer, which is one.
///
/// AudioOptionsSoundPanelHardwareDropDown_Initialize loops from 0 to num-1 and
/// adds a button per driver. With num zero it added none, and an empty
/// dropdown does not draw empty - UIDropDownMenu_Refresh walks the shared
/// DropDownList1 buttons, so it reads whichever list was built last and takes
/// its text. The same shape that put a screen resolution in the Multisampling
/// box put someone else's answer in Game Sound Output.
///
/// This client opens whichever playback device the system offers and does not
/// switch between them, so the list is that one device by name - the truth
/// rather than a stub, and the shape GetScreenResolutions already takes.
static int lua_Sound_GetNumOutputDrivers(lua_State* L) {
    lua_pushnumber(L, audio::AudioEngine::instance().isInitialized() ? 1 : 0);
    return 1;
}

/// The driver at that index, counted from zero as the panel counts it.
static int lua_Sound_GetOutputDriverNameByIndex(lua_State* L) {
    const int index = static_cast<int>(luaL_optnumber(L, 1, -1));
    std::string name = audio::AudioEngine::instance().getOutputDeviceName();
    if (index != 0 || name.empty()) { lua_pushnil(L); return 1; }
    lua_pushstring(L, name.c_str());
    return 1;
}

static int lua_GetRefreshRates(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

static int lua_SetScreenResolution(lua_State* L) {
    auto* svc = getLuaServices(L);
    const int row = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (svc && svc->setResolutionIndex && row >= 1) svc->setResolutionIndex(row - 1);
    return 0;
}

/// The battleground position list carries two kinds of entry, and the packet
/// says which by the block it arrived in.
///
/// AzerothCore's writer sends `m_numPlayerPositions` first - a count it always
/// writes as zero, with a commented-out loop beside it - and then the flag
/// carriers. So group 0 is the team positions, which no AzerothCore realm
/// sends, and group 1 is the carriers. The parser has recorded which all along
/// and the first version of these accessors ignored it, which would have drawn
/// flag carriers as ordinary party dots.
static const game::BgPlayerPosition* bgEntryAt(game::GameHandler* gh,
                                               int group, int index) {
    if (!gh || index < 1) return nullptr;
    int seen = 0;
    for (const auto& p : gh->getBgPlayerPositions()) {
        if (p.group != group) continue;
        if (++seen == index) return &p;
    }
    return nullptr;
}

static int bgCount(game::GameHandler* gh, int group) {
    if (!gh) return 0;
    int n = 0;
    for (const auto& p : gh->getBgPlayerPositions()) if (p.group == group) ++n;
    return n;
}

/// GetBattlefieldPosition(index) → where a team mate is, as a fraction across
/// the map, and their name.
///
/// Origin and no name past the end, because WorldMapFrame_Update's loop is
/// bounded by MAX_RAID_MEMBERS rather than by how many are present and hides
/// each frame whose position comes back as zero. On an AzerothCore realm that
/// is every one of them: the server writes the count and never the entries.
static int lua_GetBattlefieldPosition(lua_State* L) {
    auto* gh = getGameHandler(L);
    auto* svc = getLuaServices(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    float u = 0.0f, v = 0.0f;
    if (const auto* p = bgEntryAt(gh, 0, index)) {
        if (svc && svc->mapUVForWorldPos &&
            svc->mapUVForWorldPos(p->wowX, p->wowY, 0.0f, u, v)) {
            lua_pushnumber(L, u);
            lua_pushnumber(L, v);
            lua_pushstring(L, gh->lookupName(p->guid).c_str());
            return 3;
        }
    }
    lua_pushnumber(L, 0.0);
    lua_pushnumber(L, 0.0);
    lua_pushstring(L, "");
    return 3;
}

/// GetBattlefieldFlagPosition(index) → where a carried flag is, and which flag.
///
/// The third value names a texture under Interface\\WorldStateFrame, picked
/// as 0x0054d010 picks it (game::bg_flag): by the carrier's own team where it
/// is in sight, else by whether it is in the player's raid - so one carrier
/// alone is drawn with its flag as two are. The client keeps the carriers
/// last first (0x0054b3f0), and an index past them has no texture. The place
/// is the carrier's own while it is in sight (0x0054dcc0 -> 0x005444f0),
/// else the one the server's positions packet gave.
static int lua_GetBattlefieldFlagPosition(lua_State* L) {
    auto* gh = getGameHandler(L);
    auto* svc = getLuaServices(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    float u = 0.0f, v = 0.0f;
    const auto* p = bgEntryAt(gh, 1, index);
    const char* flag = nullptr;
    std::optional<glm::vec2> carrierInSight;
    if (p) {
        std::optional<uint8_t> raceInSight;
        if (auto carrier = gh->getEntityManager().getEntity(p->guid);
            carrier && carrier->getType() == game::ObjectType::PLAYER) {
            raceInSight = static_cast<uint8_t>(
                carrier->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0)) & 0xFF);
            carrierInSight = glm::vec2(carrier->getX(), carrier->getY());
        }
        bool inRaid = false;
        const auto& group = gh->getPartyData();
        if (game::groupIsRaid(group.groupType)) {
            for (const auto& m : group.members) inRaid = inRaid || m.guid == p->guid;
        }
        flag = game::bg_flag::texture(
            game::bg_flag::carrierTeam(raceInSight, gh->getPlayerRace(), inRaid), gh->getCurrentMapId());
    }
    if (p && flag && svc && svc->mapUVForWorldPos) {
        // The carrier's own place while it is in sight, else the packet's.
        if (const auto uv = game::bg_flag::mapPosition(
                carrierInSight, glm::vec2(p->wowX, p->wowY),
                [svc](float x, float y, float& mu, float& mv) {
                    return svc->mapUVForWorldPos(x, y, 0.0f, mu, mv);
                })) {
            u = uv->x;
            v = uv->y;
            lua_pushnumber(L, u);
            lua_pushnumber(L, v);
            lua_pushstring(L, flag);
            return 3;
        }
    }
    lua_pushnumber(L, 0.0);
    lua_pushnumber(L, 0.0);
    lua_pushstring(L, "");
    return 3;
}

/// GetNumBattlefieldVehicles() / GetBattlefieldVehicleInfo(index)
///
/// The vehicles the world map and the battleground minimap draw their pins for.
/// Both loops ask for a count, then walk it, and hide whatever answers no
/// position - so "no vehicles" is an answer those windows act on, and the
/// answer they had here for years.
///
/// The client's list is unit-side and is never sent: see
/// game::vehicle::kVehicleFlagShownOnMap for how a guid gets onto it and off
/// again. Asking the units in view says the same set here; the client keeps its
/// entries in the order the units came onto it, while these come back by guid,
/// which is stable between frames. Both callers draw every entry the count
/// admits rather than holding one entry to one slot, so nothing that is drawn
/// moves because the order changed. The client stops at 40 (0x0054aba0); the
/// same stop is kept here.
static std::vector<uint64_t> battlefieldVehicleGuids(game::GameHandler* gh) {
    std::vector<uint64_t> out;
    if (!gh) return out;
    const auto* tables = gh->vehicleRoster().tables();
    if (!tables) return out;
    for (const auto& [guid, entity] : gh->getEntityManager().getEntities()) {
        const uint32_t vehicleId = gh->vehicleRoster().vehicleId(guid);
        if (vehicleId == 0) continue;
        const auto* row = tables->vehicle(vehicleId);
        if (!row || (row->flags & game::vehicle::kVehicleFlagShownOnMap) == 0) continue;
        out.push_back(guid);
    }
    std::ranges::sort(out);
    if (out.size() > 40) out.resize(40);
    return out;
}

static int lua_GetNumBattlefieldVehicles(lua_State* L) {
    lua_pushnumber(L, static_cast<double>(battlefieldVehicleGuids(getGameHandler(L)).size()));
    return 1;
}

/// Eight values, in the client's order (0x0054c4d0): the place on the map now
/// showing, the unit's name, whether it is driven, what kind of vehicle it is,
/// which way it faces, whether the local player is the one driving it, and
/// whether it is still about.
///
/// No position is no answer at all - the client looks both coordinates up
/// before it pushes anything and returns zero values when either fails, and
/// worldmapframe.lua tests `if ( vehicleX and … )` and hides the pin.
///
/// The three that are not straight reads of the unit:
///
///   4 (isPossessed) picks the "Occupied" half of the texture pair. The client
///     reads a bit of its own there - the byte at +0xd7 of the unit block that
///     also carries the creator guid, read five times over in the decompile and
///     written nowhere it shows - so the seats are asked instead: anyone sitting
///     in one of them counts as driving it. An airship nobody drives stays
///     unoccupied, which is the case the two maps show most.
///
///   7 (isPlayer) is the client's "+0xa30 0x400", whether the client controls
///     that unit (0x00714ac0) - which is how a vehicle the player is standing
///     in is not drawn a second time on top of the player's own marker. It is
///     not a check of the guid's type, and WoWee has the same bit already:
///     MovementHandler's client-control table, which SMSG_CLIENT_CONTROL_UPDATE
///     fills.
///
///   6 is the facing. worldmapframe.lua hands it straight to SetRotation, so it
///     is given in the same system the player arrow's facing is given in -
///     GetPlayerFacing's converted one - and not raw.
static int lua_GetBattlefieldVehicleInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    auto* svc = getLuaServices(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || !svc || index < 1) return luaReturnNil(L);
    const auto guids = battlefieldVehicleGuids(gh);
    if (index > static_cast<int>(guids.size())) return luaReturnNil(L);
    const uint64_t guid = guids[static_cast<size_t>(index) - 1];
    auto entity = gh->getEntityManager().getEntity(guid);
    // A vehicle is a unit, and its name and health are the unit's.
    if (!entity || !entity->isUnit()) return luaReturnNil(L);
    const auto* unit = static_cast<const game::Unit*>(entity.get());

    float u = 0.0f, v = 0.0f;
    if (!svc->mapUVForWorldPos ||
        !svc->mapUVForWorldPos(entity->getX(), entity->getY(), entity->getZ(), u, v)) {
        return luaReturnNil(L);
    }

    const uint32_t vehicleId = gh->vehicleRoster().vehicleId(guid);
    const auto* row = gh->vehicleRoster().tables() ? gh->vehicleRoster().tables()->vehicle(vehicleId)
                                                   : nullptr;
    bool occupied = false;
    for (uint32_t seat = 0; seat < game::vehicle::kSeatCount && !occupied; ++seat) {
        occupied = gh->vehicleRoster().occupant(guid, static_cast<uint8_t>(seat)) != 0;
    }
    auto* mh = gh->getMovementHandler();

    lua_pushnumber(L, u);                                   // 1: x across the map
    lua_pushnumber(L, v);                                   // 2: y down the map
    lua_pushstring(L, unit->getName().c_str());             // 3: unitName
    lua_pushboolean(L, occupied ? 1 : 0);                   // 4: isPossessed
    if (row && row->uiLocomotionType < game::vehicle::kUiLocomotionNames.size()) {
        lua_pushstring(L, game::vehicle::kUiLocomotionNames[row->uiLocomotionType]);
    } else {
        lua_pushnil(L);                                     // 5: vehicleType
    }
    lua_pushnumber(L, core::coords::canonicalToPlayerFacing(entity->getOrientation()));
                                                            // 6: orientation
    lua_pushboolean(L, mh && mh->unitHasClientControl(guid) ? 1 : 0);  // 7: isPlayer
    // The client asks its dead-or-ghost vtable slot (+0x128) and inverts it;
    // no health is how that reads here, the same test UnitIsDeadOrGhost makes.
    lua_pushboolean(L, unit->getHealth() != 0 ? 1 : 0);     // 8: isAlive
    return 8;
}

// ---- Chat window settings ----
//
// The three getters here answered with fixed numbers and every setter was
// missing, so the interface could read a chat window's settings and never save
// one. Moving a window, resizing it, recolouring it or closing it all went
// through a setter that did nothing, and the next FCF_LoadChatSettings read
// back the same defaults it read the first time.
//
// This is entirely client-side - WoW keeps it in the config, not on the server
// - so a store here is the whole feature rather than a stand-in for one.
struct ChatWindowSettings {
    std::string name;
    float fontSize = 14.0f;
    // Black at a quarter alpha, which is what the interface seeds a fresh
    // window with: DEFAULT_CHATFRAME_COLOR is {0,0,0} and
    // DEFAULT_CHATFRAME_ALPHA is 0.25, both in floatingchatframe.lua.
    //
    // These were white at full alpha. FCF_LoadChatSettings hands whatever is
    // here to FCF_SetWindowColor and FCF_SetWindowAlpha for every window that
    // has no saved settings of its own, so the chat opened behind an opaque
    // white panel with the text on it. Installs that ran such a build saved
    // that white, and loadInterfaceState repairs it on the way back in.
    float r = kChatBackgroundDefault[0], g = kChatBackgroundDefault[1];
    float b = kChatBackgroundDefault[2], alpha = kChatBackgroundDefault[3];
    bool shown = false;
    bool locked = false;
    int  docked = 0;            // 0 = not docked; otherwise its place on the dock
    bool uninteractable = false;
    // Saved geometry. Absent until something saves it, which is not the same as
    // zero: FCF_RestorePositionAndDimensions only restores what it is given,
    // and a zero width is a window with no width.
    bool hasPosition = false;
    std::string point = "TOPLEFT";
    float xOffset = 0.0f, yOffset = 0.0f;
    bool hasDimensions = false;
    float width = 0.0f, height = 0.0f;
    /// Which of ChatTypeGroup's names this window shows, and which chat
    /// channels it carries. Both are read once per login and are the whole of
    /// what a chat window listens to: ChatFrame_RegisterForMessages walks the
    /// first list and calls RegisterEvent for every event in each group, and
    /// nothing else in FrameXML registers a chat frame for a CHAT_MSG_ event.
    std::vector<std::string> messageGroups;
    std::vector<std::pair<std::string, int>> channels;
};

/// Every group name ChatTypeGroup defines in 3.3.5, which is what the General
/// window shows by default - say and yell through to loot, experience and the
/// error line. Names FrameXML does not know are skipped by the reader rather
/// than raising, so the cost of listing one too many is nothing and the cost of
/// missing one is that kind of message never appearing.
inline constexpr const char* kDefaultChatGroups[] = {
    "SYSTEM", "SAY", "EMOTE", "YELL", "WHISPER", "PARTY", "PARTY_LEADER",
    "RAID", "RAID_LEADER", "RAID_WARNING", "BATTLEGROUND",
    "BATTLEGROUND_LEADER", "GUILD", "OFFICER", "MONSTER_SAY", "MONSTER_YELL",
    "MONSTER_EMOTE", "MONSTER_WHISPER", "MONSTER_BOSS_EMOTE",
    "MONSTER_BOSS_WHISPER", "ERRORS", "AFK", "DND", "IGNORED", "BG_HORDE",
    "BG_ALLIANCE", "BG_NEUTRAL", "COMBAT_XP_GAIN", "COMBAT_HONOR_GAIN",
    "COMBAT_FACTION_CHANGE", "SKILL", "LOOT", "MONEY", "OPENING",
    "TRADESKILLS", "PET_INFO", "COMBAT_MISC_INFO", "ACHIEVEMENT",
    "GUILD_ACHIEVEMENT", "CHANNEL", "TARGETICONS",
};

/// NUM_CHAT_WINDOWS in 3.3.5. Indices are one-based from Lua.
static constexpr int kNumChatWindows = 10;

static std::array<ChatWindowSettings, kNumChatWindows>& chatWindows() {
    static std::array<ChatWindowSettings, kNumChatWindows> windows = [] {
        std::array<ChatWindowSettings, kNumChatWindows> w{};
        // WoW's default layout docks General and the combat log and leaves the
        // rest neither shown nor docked. Which window is being asked about
        // matters: docked is that window's place on the dock, and FCF_DockFrame
        // asserts that whatever claims position one is the dock's primary, so
        // answering one for every window claimed each was first.
        w[0].shown = true; w[0].docked = 1;
        w[1].shown = true; w[1].docked = 2;
        // General carries everything; the combat log window is driven by
        // Blizzard_CombatLog and registers its own events, so its list is
        // empty here exactly as it is in the real client.
        for (const char* g : kDefaultChatGroups) w[0].messageGroups.emplace_back(g);
        // The channels the default layout carries are filled in when they are
        // first asked for (settleWindowChannels): they are ChatChannels.dbc's
        // shortcuts and ids, in the client's language, and the file is not
        // read yet when this runs.
        return w;
    }();
    return windows;
}

/// Where the chat layout and the action-bar toggles live between runs.
///
/// The CVars got their own file first and these two stores were left in memory,
/// which is the same defect twice: a player who moves a chat window, renames a
/// tab, sends whispers to their own window or turns on the right-hand action
/// bars finds all of it back to default on the next login. Nothing about that
/// looks like a bug - it looks like the setting never took.
///
/// One file for both, because they are the same kind of thing: interface state
/// the real client keeps in account data this one has no equivalent of.
static std::string interfaceStatePath() {
    return core::getConfigRoot() + "/interface_state.cfg";
}

static void saveInterfaceState();

/// Make a window's channel list what the frame can match lines against.
///
/// FrameXML never adds a channel by itself - ChatFrame_RegisterForChannels
/// reads this list and nothing else fills it - so the first window starts
/// with the zone channels the client joins by itself, by shortcut and
/// ChatChannels.dbc id (0x00508320 hands them to 0x00501a70). The frame finds
/// a zone channel's lines by that id, since the line names it "Allgemein -
/// Dalaran" and the list "Allgemein"; a channel of the player's own it finds
/// by name. An empty first window is the defaults, not a setting: the
/// interface's reset empties it and expects the client to put them back.
///
/// A layout saved by builds that joined the channels by English name holds
/// "General" with no id, which matches nothing on a client in any other
/// language. Each is put back as its row's shortcut and id.
static void settleWindowChannels(lua_State* L, ChatWindowSettings& w, bool first) {
    auto* gh = getGameHandler(L);
    auto* ch = gh ? gh->getChatHandler() : nullptr;
    if (!ch || ch->channelRows().empty()) return;
    bool changed = false;
    if (first && w.channels.empty()) {
        for (const auto& c : ch->defaultWindowChannels()) w.channels.push_back(c);
        changed = !w.channels.empty();
    }
    for (auto& c : w.channels) {
        if (c.second != 0) continue;
        if (const auto* r = game::chat_channels::rowForLegacyName(ch->channelRows(), c.first)) {
            c = {r->shortcut, static_cast<int>(r->id)};
            changed = true;
        }
    }
    if (changed) saveInterfaceState();
}

/// The window a one-based index names, or nullptr if it names none.
static ChatWindowSettings* chatWindow(lua_State* L, int argIndex) {
    const int id = static_cast<int>(luaL_optnumber(L, argIndex, 0));
    if (id < 1 || id > kNumChatWindows) return nullptr;
    return &chatWindows()[static_cast<size_t>(id - 1)];
}

/// Write the chat layout and the bar toggles out.
///
/// Sorted and one key per line, the same shape as the CVar file beside it, so
/// the two read alike and a diff shows a settings change rather than hash order
/// moving. A name with a newline in it is dropped rather than written, since it
/// would come back as two broken lines.
static void saveInterfaceState() {
    const std::string path = interfaceStatePath();
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path);
    if (!out.is_open()) {
        LOG_WARNING("Could not save interface state to ", path);
        return;
    }
    const auto& bars = actionBarToggles();
    for (size_t i = 0; i < bars.size(); ++i)
        out << "bar." << (i + 1) << "=" << (bars[i] ? 1 : 0) << "\n";
    const auto& windows = chatWindows();
    for (size_t i = 0; i < windows.size(); ++i) {
        const auto& w = windows[i];
        const std::string k = "chat." + std::to_string(i + 1) + ".";
        if (w.name.find('\n') == std::string::npos)
            out << k << "name=" << w.name << "\n";
        out << k << "shown="   << (w.shown ? 1 : 0) << "\n";
        out << k << "locked="  << (w.locked ? 1 : 0) << "\n";
        out << k << "docked="  << w.docked << "\n";
        out << k << "uninteractable=" << (w.uninteractable ? 1 : 0) << "\n";
        out << k << "fontSize=" << w.fontSize << "\n";
        out << k << "colour=" << w.r << "," << w.g << "," << w.b << "," << w.alpha << "\n";
        // Position and dimensions only once something has saved them. Absent is
        // not zero: FCF_RestorePositionAndDimensions restores what it is given,
        // and a zero width is a window with no width.
        if (w.hasPosition)
            out << k << "position=" << w.point << "," << w.xOffset << "," << w.yOffset << "\n";
        if (w.hasDimensions)
            out << k << "size=" << w.width << "," << w.height << "\n";
        // Written even when empty, because empty is a real setting: a window
        // whose groups were all removed must come back empty rather than
        // falling to the defaults and refilling itself on the next login.
        std::string groups;
        for (const auto& g : w.messageGroups) {
            if (g.find_first_of(",\n") != std::string::npos) continue;
            if (!groups.empty()) groups += ",";
            groups += g;
        }
        out << k << "groups=" << groups << "\n";
        std::string chans;
        for (const auto& c : w.channels) {
            if (c.first.find_first_of(",:\n") != std::string::npos) continue;
            if (!chans.empty()) chans += ",";
            chans += c.first + ":" + std::to_string(c.second);
        }
        out << k << "channels=" << chans << "\n";
    }
}

/// Read them back, before the interface loads: a chat window is built from
/// these as it is created, and a value arriving later leaves the window
/// disagreeing with the setting.
static void loadInterfaceState() {
    std::ifstream in(interfaceStatePath());
    if (!in.is_open()) return;
    auto& bars = actionBarToggles();
    auto& windows = chatWindows();
    std::string line;
    size_t loaded = 0;
    bool repaired = false;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        auto num = [&value] {
            try { return std::stof(value); } catch (const std::exception&) { return 0.0f; }
        };
        if (key.rfind("bar.", 0) == 0) {
            const int idx = std::atoi(key.c_str() + 4);
            if (idx >= 1 && idx <= static_cast<int>(bars.size()))
                bars[static_cast<size_t>(idx - 1)] = (value == "1");
            ++loaded;
            continue;
        }
        if (key.rfind("chat.", 0) != 0) continue;
        const size_t dot = key.find('.', 5);
        if (dot == std::string::npos) continue;
        const int idx = std::atoi(key.substr(5, dot - 5).c_str());
        if (idx < 1 || idx > kNumChatWindows) continue;
        auto& w = windows[static_cast<size_t>(idx - 1)];
        const std::string field = key.substr(dot + 1);
        if      (field == "name")     w.name = value;
        else if (field == "shown")    w.shown = (value == "1");
        else if (field == "locked")   w.locked = (value == "1");
        else if (field == "docked")   w.docked = std::atoi(value.c_str());
        else if (field == "uninteractable") w.uninteractable = (value == "1");
        else if (field == "fontSize") {
            // Only a height text can be read at. num() answers 0 for anything
            // it cannot parse, and a zero here reaches SetFont as the size of
            // every line in the window. The interface offers 12 through 18.
            const float size = num();
            if (std::isfinite(size) && size >= 6.0f && size <= 32.0f) w.fontSize = size;
        }
        else if (field == "colour") {
            // Read into locals and only kept if all four are numbers, for the
            // same reason position and size below do it: this file is written
            // back out verbatim, so a nan taken in here is a nan saved again
            // every session after. On a miss the window keeps the fresh-window
            // colour the struct already holds.
            float cr = 0.0f, cg = 0.0f, cb = 0.0f, ca = 0.0f;
            if (std::sscanf(value.c_str(), "%f,%f,%f,%f", &cr, &cg, &cb, &ca) == 4 &&
                std::isfinite(cr) && std::isfinite(cg) && std::isfinite(cb) &&
                std::isfinite(ca)) {
                // A white panel is the one this client wrote before it had the
                // interface's own default, not one anybody picked, and at full
                // alpha it covers the chat with the text lost in it.
                if (repairChatBackground(cr, cg, cb, ca)) {
                    LOG_WARNING("Chat window ", idx,
                                " had a white background saved, which hides the "
                                "text; restoring the default.");
                    repaired = true;
                }
                w.r = cr;
                w.g = cg;
                w.b = cb;
                w.alpha = ca;
            }
        } else if (field == "position") {
            char pt[32] = {0};
            float px = 0.0f, py = 0.0f;
            // Read into locals and only kept if they are numbers. A file
            // written before this was refused on the way out still holds a
            // nan, and taking it back would restore the frame that swallows
            // every hit test - see lua_SetChatWindowSavedPosition.
            if (std::sscanf(value.c_str(), "%31[^,],%f,%f", pt, &px, &py) == 3 &&
                std::isfinite(px) && std::isfinite(py)) {
                w.point = pt;
                w.xOffset = px;
                w.yOffset = py;
                w.hasPosition = true;
            }
        } else if (field == "size") {
            float pw = 0.0f, ph = 0.0f;
            if (std::sscanf(value.c_str(), "%f,%f", &pw, &ph) == 2 &&
                std::isfinite(pw) && std::isfinite(ph)) {
                w.width = pw;
                w.height = ph;
                w.hasDimensions = true;
            }
        } else if (field == "groups") {
            w.messageGroups.clear();
            for (size_t at = 0; at <= value.size();) {
                const size_t comma = value.find(',', at);
                const std::string one = value.substr(at, comma - at);
                if (!one.empty()) w.messageGroups.push_back(one);
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        } else if (field == "channels") {
            w.channels.clear();
            for (size_t at = 0; at <= value.size();) {
                const size_t comma = value.find(',', at);
                const std::string one = value.substr(at, comma - at);
                const size_t colon = one.rfind(':');
                if (colon != std::string::npos && colon > 0) {
                    w.channels.emplace_back(one.substr(0, colon),
                                            std::atoi(one.c_str() + colon + 1));
                }
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        } else continue;
        ++loaded;
    }
    LOG_INFO("Interface state: loaded ", loaded, " value(s) from ",
             interfaceStatePath());
    // Written back at once rather than left for whatever saves next, so the
    // repair happens once and a player reading the file sees what the client
    // is actually using.
    if (repaired) saveInterfaceState();
}

/// A chat window's saved settings. FCF_SetWindowAlpha takes the alpha from
/// here and remembers it as oldAlpha, which the fade handlers then hand to
/// max() on every mouse-over - so a missing alpha is not a cosmetic gap, it is
/// an error every time the cursor crosses the frame.
static int lua_GetChatWindowInfo(lua_State* L) {
    const ChatWindowSettings* w = chatWindow(L, 1);
    if (!w) w = &chatWindows()[0];

    lua_pushstring(L, w->name.c_str());
    lua_pushnumber(L, w->fontSize);
    lua_pushnumber(L, w->r);
    lua_pushnumber(L, w->g);
    lua_pushnumber(L, w->b);
    lua_pushnumber(L, w->alpha);
    // Numbers and nil, not booleans. docked is a dock position, not a flag -
    // FCF_LoadChatSettings hands it straight to FCF_DockFrame as the index to
    // insert at, and that compares it against a count. A boolean there is a
    // comparison between a boolean and a number, which is an error rather than
    // a wrong answer.
    if (w->shown) lua_pushnumber(L, 1.0); else lua_pushnil(L);
    if (w->locked) lua_pushnumber(L, 1.0); else lua_pushnil(L);
    if (w->docked > 0) lua_pushnumber(L, w->docked); else lua_pushnil(L);
    if (w->uninteractable) lua_pushnumber(L, 1.0); else lua_pushnil(L);
    return 10;
}

// GetChatWindowSavedPosition(index) → point, xOffset, yOffset
static int lua_GetChatWindowSavedPosition(lua_State* L) {
    const ChatWindowSettings* w = chatWindow(L, 1);
    if (!w || !w->hasPosition) return luaReturnNil(L);
    lua_pushstring(L, w->point.c_str());
    lua_pushnumber(L, w->xOffset);
    lua_pushnumber(L, w->yOffset);
    return 3;
}

// GetChatWindowSavedDimensions(index) → width, height
static int lua_GetChatWindowSavedDimensions(lua_State* L) {
    const ChatWindowSettings* w = chatWindow(L, 1);
    if (!w || !w->hasDimensions) return luaReturnNil(L);
    lua_pushnumber(L, w->width);
    lua_pushnumber(L, w->height);
    return 2;
}

static int lua_SetChatWindowName(lua_State* L) {
    if (auto* w = chatWindow(L, 1)) w->name = luaL_optstring(L, 2, "");
    saveInterfaceState();
    return 0;
}
static int lua_SetChatWindowSize(lua_State* L) {
    if (auto* w = chatWindow(L, 1)) w->fontSize = static_cast<float>(luaL_optnumber(L, 2, 14.0));
    saveInterfaceState();
    return 0;
}
// A missing component keeps what the window already has rather than falling to
// a number of this function's choosing. FCF_SetWindowColor passes on whatever
// FCF_GetChatWindowInfo answered, and that is nil for a window the interface
// has no settings for - so a default of 1 here reads "the interface said
// nothing" as "the interface said white", and saves it. The white background
// this client shipped with is what that looks like once it is on disk.
static int lua_SetChatWindowColor(lua_State* L) {
    if (auto* w = chatWindow(L, 1)) {
        w->r = static_cast<float>(luaL_optnumber(L, 2, w->r));
        w->g = static_cast<float>(luaL_optnumber(L, 3, w->g));
        w->b = static_cast<float>(luaL_optnumber(L, 4, w->b));
    }
    saveInterfaceState();
    return 0;
}
static int lua_SetChatWindowAlpha(lua_State* L) {
    if (auto* w = chatWindow(L, 1))
        w->alpha = static_cast<float>(luaL_optnumber(L, 2, w->alpha));
    saveInterfaceState();
    return 0;
}
static int lua_SetChatWindowShown(lua_State* L) {
    if (auto* w = chatWindow(L, 1)) w->shown = lua_toboolean(L, 2) != 0;
    saveInterfaceState();
    return 0;
}
static int lua_SetChatWindowLocked(lua_State* L) {
    if (auto* w = chatWindow(L, 1)) w->locked = lua_toboolean(L, 2) != 0;
    saveInterfaceState();
    return 0;
}
/// The dock position, which is a number and not a flag - see GetChatWindowInfo.
/// A false or nil means undocked, which is zero here rather than a missing key.
static int lua_SetChatWindowDocked(lua_State* L) {
    if (auto* w = chatWindow(L, 1)) {
        if (lua_isnumber(L, 2)) w->docked = static_cast<int>(lua_tonumber(L, 2));
        else w->docked = lua_toboolean(L, 2) ? 1 : 0;
    }
    saveInterfaceState();
    return 0;
}
static int lua_SetChatWindowUninteractable(lua_State* L) {
    if (auto* w = chatWindow(L, 1)) w->uninteractable = lua_toboolean(L, 2) != 0;
    saveInterfaceState();
    return 0;
}
static int lua_SetChatWindowSavedPosition(lua_State* L) {
    auto* w = chatWindow(L, 1);
    if (!w) return 0;
    // Called with nil to forget the position, which is how a window that has
    // been re-docked stops being restored to where it floated.
    if (lua_isnoneornil(L, 2)) { w->hasPosition = false; saveInterfaceState(); return 0; }
    const float x = static_cast<float>(luaL_optnumber(L, 3, 0.0));
    const float y = static_cast<float>(luaL_optnumber(L, 4, 0.0));
    // A position is saved as a fraction of the screen, so anything that made
    // the screen zero for a frame makes this nan - and a nan written out is
    // permanent, because it is read back on every login and put straight into
    // the frame's rect. From there it is not a misplaced window: a rect with a
    // nan in it matches every hit test, since every comparison against a nan
    // is false, and one mouse-enabled frame answering every hit test stops the
    // camera turning for good. Refused rather than stored.
    if (!std::isfinite(x) || !std::isfinite(y)) {
        LOG_WARNING("Refusing a chat window position that is not a number: ",
                    x, ",", y);
        return 0;
    }
    w->point   = luaL_optstring(L, 2, "TOPLEFT");
    w->xOffset = x;
    w->yOffset = y;
    w->hasPosition = true;
    saveInterfaceState();
    return 0;
}
static int lua_SetChatWindowSavedDimensions(lua_State* L) {
    auto* w = chatWindow(L, 1);
    if (!w) return 0;
    if (lua_isnoneornil(L, 2)) { w->hasDimensions = false; saveInterfaceState(); return 0; }
    const float wd = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    const float ht = static_cast<float>(luaL_optnumber(L, 3, 0.0));
    if (!std::isfinite(wd) || !std::isfinite(ht)) {
        LOG_WARNING("Refusing chat window dimensions that are not numbers: ",
                    wd, "x", ht);
        return 0;
    }
    w->width  = wd;
    w->height = ht;
    w->hasDimensions = true;
    saveInterfaceState();
    return 0;
}

/// ResetChatWindows() - back to the layout a new character starts with.
///
/// Announced, because the settings changing is not something the windows can
/// see. Every floating chat frame answers UPDATE_FLOATING_CHAT_WINDOWS by
/// re-reading its own settings, which is exactly what has just changed
/// underneath it; without the event the store is reset and the windows stay
/// where they were until something else happens to reload them.
static int lua_ResetChatWindows(lua_State* L) {
    auto& windows = chatWindows();
    windows = std::array<ChatWindowSettings, kNumChatWindows>{};
    windows[0].shown = true; windows[0].docked = 1;
    windows[1].shown = true; windows[1].docked = 2;

    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
    auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (engine) engine->fireEvent("UPDATE_FLOATING_CHAT_WINDOWS", {});
    saveInterfaceState();
    return 0;
}

/// SetChatColorNameByClass(chatType, on) (0x004fedc0): the type's class
/// flag in the chat colour table, and UPDATE_CHAT_COLOR_NAME_BY_CLASS when a
/// type of that name exists.
static int lua_SetChatColorNameByClass(lua_State* L) {
    if (!lua_isstring(L, 1)) {
        return luaL_error(L, "Usage: SetChatColorNameByClass(chatType, colorNameByClass)");
    }
    const std::string type = lua_tostring(L, 1);
    const bool on = lua_toboolean(L, 2) != 0;
    if (!chatColorTable().setNameByClass(type, on)) return 0;
    if (auto* gh = getGameHandler(L)) {
        gh->fireAddonEvent("UPDATE_CHAT_COLOR_NAME_BY_CLASS", {type, game::eventBool(on)});
    }
    return 0;
}

/// ResetChatColors() (0x004fed10): every chat type back to its colour. The
/// class flags stay, and nothing is said - the interface asks again.
static int lua_ResetChatColors(lua_State* L) {
    (void)L;
    chatColorTable().resetColors();
    return 0;
}

/// Names FrameXML reached for and did not find, harvested from a run rather
/// than guessed at: the fallback records each once and reports the list at
/// shutdown. Each answers with what the feature being absent looks like, so
/// the caller takes the branch it would take on a client where that feature is
/// switched off.
static int lua_GetMapInfo(lua_State* L) {
    // mapFileName, textureHeight, textureWidth. WorldMapFrame builds every
    // detail tile's path out of the first: "Interface\\WorldMap\\"..name
    // .."\\"..name..i. Answered as an empty string, that came out as
    // "Interface\\WorldMap\\\\1" and the map was drawn with no background
    // at all - and because "" is true in Lua, the interface's own fallback to
    // the world map never ran either. Nil is what it wants when there is no
    // zone map, which is how a continent reaches that fallback.
    auto* svc = getLuaServices(L);
    const std::string name = (svc && svc->getMapFileName) ? svc->getMapFileName()
                                                          : std::string();
    if (name.empty()) return luaReturnNil(L);

    lua_pushstring(L, name.c_str());
    // The detail frame is 1002 by 668 and sliced into 256-pixel tiles, which
    // is fixed for every zone map of this era. Only the name is read back out
    // of here by the interface; these two are shadowed before they are used.
    lua_pushnumber(L, 668.0);
    lua_pushnumber(L, 1002.0);
    return 3;
}

/// The expansion this client speaks. Two is Wrath, which is what the wire
/// format and the DBC layouts here assume.
/// The expansion, counted from zero: 0 vanilla, 1 TBC, 2 Wrath. That is the
/// numbering LFGDungeons.dbc's expansion column uses and the one
/// MAX_PLAYER_LEVEL_TABLE is keyed by - [0]=60, [1]=70, [2]=80.
static int expansionLevelZeroBased(lua_State* L) {
    auto* svc = getLuaServices(L);
    auto* reg = svc ? svc->expansionRegistry : nullptr;
    auto* prof = reg ? reg->getActive() : nullptr;
    if (!prof) return 2;
    if (prof->id == "wotlk") return 2;
    if (prof->id == "tbc") return 1;
    return 0;   // classic and turtle
}

static int lua_GetExpansionLevel(lua_State* L) {
    lua_pushnumber(L, expansionLevelZeroBased(L));
    return 1;
}

/// Normal, which is the difficulty a fresh group is on.
static int lua_ReturnOne(lua_State* L) {
    lua_pushnumber(L, 1.0);
    return 1;
}

// The always-up battleground score lines, in the order FrameXML asks for them.
// The server sends world states as bare key/value pairs, so the labels come
// from the shared table this client's own heads-up display also reads.
struct WorldStateLine {
    std::string text;
};

static std::vector<WorldStateLine> worldStateLines(game::GameHandler* gh) {
    std::vector<WorldStateLine> out;
    if (!gh) return out;
    const game::BgScoreDef* def = game::findBgScoreDef(gh->getWorldStateMapId());
    if (!def) return out;

    auto alliance = gh->getWorldState(def->allianceKey);
    auto horde    = gh->getWorldState(def->hordeKey);
    if (!alliance && !horde) return out;

    uint32_t maxScore = def->hardcodedMax;
    if (def->maxKey != 0) {
        if (auto mv = gh->getWorldState(def->maxKey)) maxScore = *mv;
    }
    const bool showMax = maxScore > 0 && def->unit && def->unit[0] != '\0';

    char buf[96];
    for (int side = 0; side < 2; ++side) {
        const char* label = side == 0 ? "Alliance" : "Horde";
        const uint32_t score = (side == 0 ? alliance : horde).value_or(0);
        if (showMax) snprintf(buf, sizeof(buf), "%s: %u/%u", label, score, maxScore);
        else         snprintf(buf, sizeof(buf), "%s: %u", label, score);
        out.push_back({buf});
    }
    return out;
}

// GetNumWorldStateUI() - how many always-up lines there are to draw: the
// WorldStateUI.dbc rows for the place and the player's phases (0x00548720);
// the battleground's two scores where this install has no such file.
static int lua_GetNumWorldStateUI(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (const auto lines = gh ? gh->worldStateUiLines() : std::nullopt) {
        lua_pushnumber(L, static_cast<double>(lines->size()));
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(worldStateLines(gh).size()));
    return 1;
}

/// GetWorldStateUIInfo from the file's row (0x00548d40): its type, the
/// state its variable holds (1 where it names none), the text with the
/// world states put in, the icons, tooltips and extended UI, and the
/// extended UI's three states.
static int pushWorldStateUiRow(lua_State* L, game::GameHandler& gh, const game::world_state_ui::Row& row) {
    const auto state = [&gh](uint32_t id) { return gh.getWorldState(id).value_or(0u); };
    lua_pushnumber(L, row.type);
    lua_pushnumber(L, row.stateVariable == 0 ? 1.0 : static_cast<double>(state(row.stateVariable)));
    lua_pushstring(L, game::world_state_ui::text(row.text, state, gh.worldStateNow()).c_str());
    lua_pushstring(L, row.icon.c_str());
    lua_pushstring(L, row.dynamicIcon.c_str());
    lua_pushstring(L, row.tooltip.c_str());
    lua_pushstring(L, row.dynamicTooltip.c_str());
    lua_pushstring(L, row.extendedUI.c_str());
    for (uint32_t variable : row.extendedUIState) lua_pushnumber(L, static_cast<double>(state(variable)));
    return 11;
}

// GetWorldStateUIInfo(index) → uiType, state, text, icon, dynamicIcon, tooltip,
// dynamicTooltip, extendedUI, extendedUIState1, extendedUIState2, extendedUIState3
//
// uiType 0 keeps worldstateframe.lua out of its world-PvP branch, which is the
// one gated on a CVar and on IsSubZonePVPPOI. state 1 means "show, no flash".
static int lua_GetWorldStateUIInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (const auto rows = gh ? gh->worldStateUiLines() : std::nullopt) {
        if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetWorldStateUIInfo(index)");
        const auto at = static_cast<int64_t>(std::llround(lua_tonumber(L, 1))) - 1;
        if (at < 0 || at >= static_cast<int64_t>(rows->size())) {
            lua_pushnumber(L, 0);
            return 1;
        }
        return pushWorldStateUiRow(L, *gh, *(*rows)[static_cast<size_t>(at)]);
    }
    const int index = static_cast<int>(luaL_optinteger(L, 1, 0));
    const auto lines = worldStateLines(getGameHandler(L));
    if (index < 1 || index > static_cast<int>(lines.size())) return 0;

    lua_pushinteger(L, 0);                              // uiType
    lua_pushinteger(L, 1);                              // state
    lua_pushstring(L, lines[index - 1].text.c_str());   // text
    lua_pushstring(L, "");                              // icon
    lua_pushstring(L, "");                              // dynamicIcon
    lua_pushstring(L, "");                              // tooltip
    lua_pushstring(L, "");                              // dynamicTooltip
    lua_pushstring(L, "");                              // extendedUI
    lua_pushinteger(L, 0);                              // extendedUIState1
    lua_pushinteger(L, 0);                              // extendedUIState2
    lua_pushinteger(L, 0);                              // extendedUIState3
    return 11;
}

static int lua_GetDefaultLanguage(lua_State* L) {
    auto* gh = getGameHandler(L);
    static const std::set<uint8_t> kHordeRaces = {2, 5, 6, 8, 10};
    const bool horde = gh && kHordeRaces.count(gh->getPlayerRace()) > 0;
    // Named from Languages.dbc, as every chat line's language is. ChatFrame
    // prints "[language]" in front of any line whose language differs from
    // this, and an English "Common" beside a German "Gemeinsprache" from the
    // file differed on every line - so a localized client headed everything
    // the player's own faction said, and their custom emotes, with it.
    // Orcish is language 1 and Common 7; the English names when the file
    // has neither.
    const std::string& fromFile = gh ? gh->getLanguageName(horde ? 1u : 7u) : std::string();
    lua_pushstring(L, !fromFile.empty() ? fromFile.c_str() : (horde ? "Orcish" : "Common"));
    return 1;
}

/// No enchant on either hand: four values per hand, and MainMenuBar reads the
/// expiry as a number.
/// GetWeaponEnchantInfo() → per hand: hasEnchant, expiration, charges.
///
/// It answered no for both hands unconditionally, so TemporaryEnchantFrame
/// took its early exit and hid itself - a sharpening stone or an oil showed
/// nothing at all. The buff bar is handed over, so this client's own weapon
/// enchant display beside it is suppressed and this was the only one left.
///
/// The enchant is tracked per equipped item; its remaining time is not, and
/// the frame reads expiration only to write a countdown under an icon it has
/// already decided to show. Zero there costs the countdown, not the icon.
static int lua_GetWeaponEnchantInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const game::EquipSlot kHands[2] = {game::EquipSlot::MAIN_HAND,
                                       game::EquipSlot::OFF_HAND};
    for (const game::EquipSlot hand : kHands) {
        bool enchanted = false;
        if (gh) {
            const uint64_t guid = gh->getEquipSlotGuid(static_cast<int>(hand));
            if (guid != 0) enchanted = gh->getItemEnchantIds(guid).second != 0;
        }
        lua_pushboolean(L, enchanted ? 1 : 0);   // hasEnchant
        lua_pushnumber(L, 0.0);                  // expiration, not tracked
        lua_pushnumber(L, 0.0);                  // charges
    }
    return 6;
}

/// Which modifier keys are held. Answered from the real keyboard rather than
/// falsely, because a shift-click means something different from a click and
/// FrameXML asks these on every button press.
static int lua_IsShiftKeyDown(lua_State* L) {
    lua_pushboolean(L, (SDL_GetModState() & SDL_KMOD_SHIFT) != 0);
    return 1;
}
static int lua_IsControlKeyDown(lua_State* L) {
    lua_pushboolean(L, (SDL_GetModState() & SDL_KMOD_CTRL) != 0);
    return 1;
}
static int lua_IsAltKeyDown(lua_State* L) {
    lua_pushboolean(L, (SDL_GetModState() & SDL_KMOD_ALT) != 0);
    return 1;
}
static int lua_IsModifierKeyDown(lua_State* L) {
    const SDL_Keymod m = SDL_GetModState();
    lua_pushboolean(L, (m & (SDL_KMOD_SHIFT | SDL_KMOD_CTRL | SDL_KMOD_ALT)) != 0);
    return 1;
}

/// Whether an addon is loaded. Real rather than false: the registry holds the
/// addons that were enabled and loaded this session, and FrameXML asks before
/// deciding whether a feature exists - answering no where the answer is yes
/// hides an addon from the interface that is meant to work with it.
static int lua_IsAddOnLoaded(lua_State* L) {
    const char* wanted = lua_isstring(L, 1) ? lua_tostring(L, 1) : nullptr;
    if (!wanted) { lua_pushboolean(L, 0); return 1; }

    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_addon_info");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_pushboolean(L, 0); return 1; }
    const int count = static_cast<int>(lua_objlen(L, -1));
    bool found = false;
    for (int i = 1; i <= count && !found; ++i) {
        lua_rawgeti(L, -1, i);
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "loadOnDemand");
            const bool lod = lua_toboolean(L, -1) != 0;
            lua_pop(L, 1);
            lua_getfield(L, -1, "name");
            const char* name = lua_tostring(L, -1);
            // Being listed means loaded only for addons that load at startup.
            // A load-on-demand one is listed from the start and loaded later,
            // so its state is the manager's to answer, below.
            found = !lod && name && std::strcmp(name, wanted) == 0;
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    // A load-on-demand addon is not in the list handed to the VM at startup -
    // it is not loaded then - so its loaded state is asked of the manager.
    if (!found) {
        auto* svc = getLuaServices(L);
        if (svc && svc->isAddOnLoaded) found = svc->isAddOnLoaded(wanted);
    }
    lua_pushboolean(L, found ? 1 : 0);
    return 1;
}

/// GetTime() → seconds since the client started, as a float.
///
/// The interface's shared reference for anything timed: a cooldown records
/// GetTime() + duration and something else compares against it later. It was
/// never implemented, so every one of those comparisons was against nil -
/// including the ones in this client's own bootstrap.
/// LoadAddOn(name) → loaded, reason.
///
/// The reason is not optional when loaded is false: UIParentLoadAddOn builds
/// an error message out of _G["ADDON_" .. reason], so nil there is a
/// concatenation against nothing. These are Blizzard's own load-on-demand
/// panels - the talent frame and its like - which this client does not ship,
/// and MISSING is the reason string for exactly that.
/// LoadAddOn(name) → loaded, reason
///
/// How the interface reaches half its own panels: the talent tree, the
/// achievement window, the macro editor, the key bindings, the trade skill and
/// glyph frames are all load-on-demand addons that FrameXML asks for the first
/// time one is opened. This answered "MISSING" unconditionally, so none of them
/// ever appeared - and an addon that ships an optional module got the same.
static int lua_LoadAddOn(lua_State* L) {
    const char* name = lua_isstring(L, 1) ? lua_tostring(L, 1) : nullptr;
    auto* svc = getLuaServices(L);
    if (!name || !svc || !svc->loadAddOn) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "MISSING");
        return 2;
    }
    // Blizzard_Calendar loads. It was refused for a long time, and the note
    // that stood here is worth keeping in outline: the minimap's date button
    // calls Calendar_LoadUI, whose CalendarFrame_OnShow calls OpenCalendar, so
    // loading the addon with its verbs missing raised on one click. Saying no
    // made the button do nothing, which was the truth.
    //
    // What changed is that there is a calendar behind it now - the packet is
    // parsed into CalendarData, and the month grid, the day lists and the
    // holidays are answered from it. The write side is still missing, and a
    // missing global is a callable stand-in rather than a raise, so creating
    // an event does nothing rather than breaking the window it is in.
    std::string reason;
    const bool ok = svc->loadAddOn(name, reason);
    lua_pushboolean(L, ok ? 1 : 0);
    if (ok) { lua_pushnil(L); return 2; }
    // Only a reason the interface has a string for. UIParentLoadAddOn does
    //
    //     message(format(ADDON_LOAD_FAILED, name, _G["ADDON_"..reason]))
    //
    // so a token globalstrings does not define makes that lookup nil and
    // format raise - the report of a failed load failing, inside whichever
    // panel was being opened. Every load-on-demand panel opens this way.
    //
    // The list is globalstrings' own ADDON_* names, which is where the far
    // side reads them from. Anything else is answered as CORRUPT, which is
    // what the real client says when an addon is there and will not run.
    static const char* kKnown[] = {
        "BANNED", "CORRUPT", "DEMAND_LOADED", "DEP_BANNED", "DEP_CORRUPT",
        "DEP_DEMAND_LOADED", "DEP_DISABLED", "DEP_INCOMPATIBLE",
        "DEP_INSECURE", "DEP_INTERFACE_VERSION", "DEP_MISSING", "DISABLED",
        "INCOMPATIBLE", "INSECURE", "INTERFACE_VERSION", "MISSING",
    };
    bool known = false;
    for (const char* k : kKnown) {
        if (reason == k) { known = true; break; }
    }
    lua_pushstring(L, known ? reason.c_str() : "CORRUPT");
    return 2;
}

static int lua_ReturnNoCooldown(lua_State* L) {
    lua_pushnumber(L, 0.0);
    lua_pushnumber(L, 0.0);
    return 2;
}

/// Alliance or Horde for the player, and the same for anyone else until this
/// client tracks other units' factions. Returns the English tag and the
/// localised name, which is the pair FrameXML expects.
static int lua_UnitFactionGroup(lua_State* L) {
    // Whose faction. This answered the player's for every unit, and the target
    // frame asks it about the *target* to pick the PvP badge - so an Alliance
    // player saw an Alliance badge over a Horde target, and the party frames
    // did the same for anyone in the group.
    std::string uid(luaL_optstring(L, 1, "player"));
    toLowerInPlace(uid);
    // Orc, Undead, Tauren, Troll, Blood Elf.
    static const std::set<uint8_t> kHordeRaces = {2, 5, 6, 8, 10};
    const uint8_t race = unitRaceOf(getGameHandler(L), uid);
    // Nothing rather than a guess when the race is unknown: FrameXML compares
    // the answer against the player's and hides the badge when it has neither,
    // which is better than a badge of the wrong side.
    if (race == 0) return 0;
    const bool horde = kHordeRaces.count(race) > 0;
    lua_pushstring(L, horde ? "Horde" : "Alliance");
    lua_pushstring(L, horde ? "Horde" : "Alliance");
    return 2;
}

// RunScript(body) → compiles and runs Lua, the way /script does (0x00510b30)
//
// Under the taint every /run carries (PTR_DAT_00ac804c), whoever calls it, so
// what it does is refused as MACRO_ACTION_FORBIDDEN rather than run as the
// interface's own code. A compile failure is reported the same way a runtime
// one is, through the error handler (0x00819210), because to whoever typed it
// they are the same mistake.
static int lua_RunScript(lua_State* L) {
    if (!lua_isstring(L, 1)) return 0;
    const char* body = lua_tostring(L, 1);
    if (!body || !*body) return 0;
    taint::execute(L, body, body, taint::macro());
    return 0;
}

// IsMouseButtonDown(button) → whether it is held right now
//
// Named as WoW names them - "LeftButton", "RightButton", "MiddleButton" - and
// answers for any button when asked for none, which is what a bare call means.
static int lua_IsMouseButtonDown(lua_State* L) {
    const char* which = luaL_optstring(L, 1, nullptr);
    bool down = false;
    if (!which || !*which) {
        down = ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
               ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
               ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    } else {
        std::string name(which);
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name == "leftbutton" || name == "left")        down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        else if (name == "rightbutton" || name == "right") down = ImGui::IsMouseDown(ImGuiMouseButton_Right);
        else if (name == "middlebutton" || name == "middle") down = ImGui::IsMouseDown(ImGuiMouseButton_Middle);
    }
    lua_pushboolean(L, down ? 1 : 0);
    return 1;
}

// Screenshot() → saves one where the client's own binding puts it
static int lua_Screenshot(lua_State* L) {
    auto* svc = getLuaServices(L);
    if (svc && svc->takeScreenshot) svc->takeScreenshot();
    return 0;
}

// ToggleAutoRun(), TurnLeftStart() / TurnLeftStop(), TurnRightStart() /
// TurnRightStop() - the movement bindings, acting as their keys do.
static int lua_ToggleAutoRun(lua_State* L) {
    if (!taint::allowed(L, 0)) return 0;   // 0x005fc190
    auto* svc = getLuaServices(L);
    if (svc && svc->toggleAutoRun) svc->toggleAutoRun();
    return 0;
}
// ToggleRun() - the walk/run binding (0x005faae0): flips the mover's walk mode.
//
// It first asks 0x005191c0(0), the protected-function check that ~50 client
// functions share (see taint::allowed): kind 0, like every movement binding,
// is refused outright to insecure code and uses up the hardware event.
static int lua_ToggleRun(lua_State* L) {
    if (!taint::allowed(L, 0)) return 0;
    if (auto* gh = getGameHandler(L)) gh->toggleRun();
    return 0;
}
// CameraZoomIn(yards) / CameraZoomOut(yards) - the camera's distance from the
// player, as the zoom bindings move it.
template <int Sign>
static int lua_CameraZoom(lua_State* L) {
    auto* svc = getLuaServices(L);
    const float yards = static_cast<float>(luaL_optnumber(L, 1, 1.0));
    if (svc && svc->cameraZoom) svc->cameraZoom(Sign * yards);
    return 0;
}
// ---- The camera's saved views ----
//
// Eight views on the camera (+0xb8), read from their cvars when it is made
// (0x005fd910) and kept: SaveView writes both, ResetView only the view. The
// bindings reach the five third-person ones, 1-5; which one is current is
// cameraView (+0xb4).
struct CameraViews {
    bool loaded = false;
    std::array<rendering::CameraController::ViewPose, 8> views{};
    int current = 2;
};

static float storedCVarFloat(const std::string& name, const char* fallback) {
    return static_cast<float>(std::atof(storedCVarValue(name, fallback).c_str()));
}

static CameraViews& cameraViews() {
    static CameraViews views;
    if (!views.loaded) {
        views.loaded = true;
        for (int i = 0; i < 8; ++i) {
            views.views[static_cast<size_t>(i)] = {
                .distance = storedCVarFloat(cameraViewCVar(i, 0), kCameraViewDefault[i][0]),
                .pitch = storedCVarFloat(cameraViewCVar(i, 1), kCameraViewDefault[i][1]),
                .yaw = storedCVarFloat(cameraViewCVar(i, 2), kCameraViewDefault[i][2])};
        }
        views.current = static_cast<int>(storedCVarFloat("cameraView", "2"));
    }
    return views;
}

static rendering::CameraController* luaCamera(lua_State* L) {
    auto* svc = getLuaServices(L);
    return svc && svc->getCameraController ? svc->getCameraController() : nullptr;
}

/// Whether the follow smoothing runs on the view the camera is on (0x005fff40):
/// with cameraCustomViewSmoothing, or while the view is still its default -
/// distance, pitch and yaw each within a thousandth of it (0x005ff8e0).
static void refreshViewSmoothing(rendering::CameraController& cc) {
    const auto& views = cameraViews();
    bool allowed = storedCVarFloat("cameraCustomViewSmoothing", "0") != 0.0f;
    const int i = views.current;
    if (!allowed && i >= 0 && i < 8) {
        const auto& v = views.views[static_cast<size_t>(i)];
        const float parts[3] = {v.distance, v.pitch, v.yaw};
        allowed = true;
        for (int k = 0; k < 3; ++k) {
            const float def = static_cast<float>(std::atof(kCameraViewDefault[i][k]));
            // Pitch and yaw are kept in radians by the client, distance in yards.
            const float scale = k == 0 ? 1.0f : 0.017453292f;
            if (std::fabs((parts[k] - def) * scale) >= 0.001f) allowed = false;
        }
    }
    cc.setViewAllowsFollowSmoothing(allowed);
}

/// 0x00603330 from the bindings: view `index` taken by cameraViewBlendStyle -
/// 1 eased, 2 at once, anything else not at all - the current one again at
/// once while the player has not moved it since. A new one is kept in
/// cameraView.
static void setCameraView(rendering::CameraController& cc, int index) {
    auto& views = cameraViews();
    int style = static_cast<int>(storedCVarFloat("cameraViewBlendStyle", "1"));
    if (views.current == index && !cc.viewMovedByPlayer() && style != 0) style = 2;
    if (views.current != index) {
        views.current = index;
        setStoredCVar("cameraView", std::to_string(index));
    }
    const auto& to = views.views[static_cast<size_t>(index)];
    refreshViewSmoothing(cc);
    if (style == 2) {
        cc.goToViewPose(to, 0.0f);
        return;
    }
    if (style != 1) return;
    // Each part that moves takes its distance over its smoothing speed
    // (cameraDistanceSmoothSpeed yards, cameraPitchSmoothSpeed and
    // cameraYawSmoothSpeed degrees a second: 0x00600e00, 0x00601190,
    // 0x00601410); all take the longest, held to cameraSmoothTimeMin and
    // cameraSmoothTimeMax. Measured from where the camera is now: 0x00603330
    // first resets each part's end to its present value, so a view set part
    // way through another blend is timed from the middle of it.
    const auto from = cc.presentViewPose();
    const float dDistance = std::fabs(to.distance - from.distance);
    const float dPitch = std::fabs(to.pitch - from.pitch);
    const float dYaw = std::fabs(std::remainder(to.yaw - from.yaw, 360.0f));
    constexpr float kRadian = 57.29578f;
    float seconds = 0.0f;
    if (dDistance >= 0.001f)
        seconds = std::max(seconds, dDistance / storedCVarFloat("cameraDistanceSmoothSpeed", "8.33"));
    if (dPitch / kRadian >= 0.001f)
        seconds = std::max(seconds, dPitch / storedCVarFloat("cameraPitchSmoothSpeed", "45"));
    if (dYaw / kRadian >= 0.001f)
        seconds = std::max(seconds, dYaw / storedCVarFloat("cameraYawSmoothSpeed", "180"));
    seconds = std::max(seconds, storedCVarFloat("cameraSmoothTimeMin", "0.1"));
    seconds = std::min(seconds, storedCVarFloat("cameraSmoothTimeMax", "2.0"));
    cc.goToViewPose(to, seconds);
}

/// The view number the bindings pass, 1-5, or a usage error.
static int cameraViewArg(lua_State* L, const char* usage) {
    if (!lua_isnumber(L, 1)) luaL_error(L, "%s", usage);  // does not return
    return static_cast<int>(lua_tonumber(L, 1));
}

// SetView(index) (0x006039b0).
static int lua_SetView(lua_State* L) {
    const int index = cameraViewArg(L, "Usage: SetView(viewModeIndex)");
    auto* cc = luaCamera(L);
    if (cc && index >= 1 && index <= 5) setCameraView(*cc, index);
    return 0;
}

// SaveView(index) (0x005ff260 -> 0x005fe440): where the camera is going kept
// as that view, and in its cvars.
static int lua_SaveView(lua_State* L) {
    const int index = cameraViewArg(L, "Usage: SaveView(viewModeIndex)");
    auto* cc = luaCamera(L);
    if (!cc || index < 1 || index > 5) return 0;
    const auto pose = cc->currentViewPose();
    cameraViews().views[static_cast<size_t>(index)] = pose;
    const float parts[3] = {pose.distance, pose.pitch, pose.yaw};
    for (int part = 0; part < 3; ++part) {
        char text[40];
        std::snprintf(text, sizeof(text), "%f", static_cast<double>(parts[part]));
        setStoredCVar(cameraViewCVar(index, part), text);
    }
    refreshViewSmoothing(*cc);
    return 0;
}

// ResetView(index) (0x00604c80 -> 0x006048a0): that view back to its default
// - the view only, the cvars keep what was saved - and taken again if it is
// the one the camera is on.
static int lua_ResetView(lua_State* L) {
    const int index = cameraViewArg(L, "Usage: ResetView(viewModeIndex)");
    auto* cc = luaCamera(L);
    if (!cc || index < 1 || index > 5) return 0;
    auto& views = cameraViews();
    views.views[static_cast<size_t>(index)] = {
        .distance = static_cast<float>(std::atof(kCameraViewDefault[index][0])),
        .pitch = static_cast<float>(std::atof(kCameraViewDefault[index][1])),
        .yaw = static_cast<float>(std::atof(kCameraViewDefault[index][2]))};
    if (views.current == index) setCameraView(*cc, index);
    refreshViewSmoothing(*cc);
    return 0;
}

// NextView() / PrevView() (0x00604ce0, 0x00604d10): the next view up to 5,
// the one before down to 1.
template <int Step>
static int lua_StepView(lua_State* L) {
    auto* cc = luaCamera(L);
    if (!cc) return 0;
    const int current = cameraViews().current;
    if (Step > 0 ? current < 5 : current > 1) setCameraView(*cc, current + Step);
    return 0;
}

/// A CVar's value as a number, through GetCVar as the interface reads it.
static double cvarNumber(lua_State* L, const char* name) {
    lua_getglobal(L, "GetCVar");
    lua_pushstring(L, name);
    lua_call(L, 1, 1);
    const double v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0.0;
    lua_pop(L, 1);
    return v;
}
static void setCVarText(lua_State* L, const char* name, const std::string& value) {
    lua_getglobal(L, "SetCVar");
    lua_pushstring(L, name);
    lua_pushstring(L, value.c_str());
    lua_call(L, 2, 0);
}

// The graphics settings' own calls (0x00510200 ff.): GetFarclip and
// GetTexLodBias read farclip and texLodBias, GetBaseMip one less than
// baseMip; the setters write them back - farclip and texLodBias as "%f",
// baseMip as the whole number given. Water detail is no setting: Get 0,
// Set only checks its argument (0x00608560, 0x005101d0).
static int lua_GetFarclip(lua_State* L) {
    lua_pushnumber(L, cvarNumber(L, "farclip"));
    return 1;
}
static int lua_SetFarclip(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: SetFarclip(value)");
    char text[32];
    std::snprintf(text, sizeof(text), "%f", lua_tonumber(L, 1));
    setCVarText(L, "farclip", text);
    return 0;
}
static int lua_GetTexLodBias(lua_State* L) {
    lua_pushnumber(L, cvarNumber(L, "texLodBias"));
    return 1;
}
static int lua_SetTexLodBias(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: SetTexLodBias(value)");
    char text[32];
    std::snprintf(text, sizeof(text), "%f", lua_tonumber(L, 1));
    setCVarText(L, "texLodBias", text);
    return 0;
}
static int lua_GetBaseMip(lua_State* L) {
    lua_pushnumber(L, 1.0 - static_cast<double>(static_cast<float>(cvarNumber(L, "baseMip"))));
    return 1;
}
static int lua_SetBaseMip(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: SetBaseMip(value)");
    setCVarText(L, "baseMip", std::to_string(static_cast<int>(lua_tonumber(L, 1))));
    return 0;
}
static int lua_GetWaterDetail(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}
static int lua_SetWaterDetail(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: SetWaterDetail(value)");
    return 0;
}

// MoveView{In,Out,Left,Right,Up,Down}Start([speed]) and Stop (0x005ff080
// ff., 0x005ff000): the camera's view moves that way at speed (1 by default)
// times cameraDistanceMoveSpeed (yards, 8.33) for in and out,
// cameraYawMoveSpeed (degrees, 180) for left and right, and
// cameraPitchMoveSpeed (90) for up and down, until stopped.
template <int Direction>
static int lua_MoveViewStart(lua_State* L) {
    const float speed = lua_isnumber(L, 1) ? static_cast<float>(lua_tonumber(L, 1)) : 1.0f;
    static constexpr const char* kRates[3][2] = {
        {"cameraDistanceMoveSpeed", "8.33"}, {"cameraYawMoveSpeed", "180"}, {"cameraPitchMoveSpeed", "90"}};
    const auto& r = kRates[Direction / 2];
    if (auto* cc = luaCamera(L)) cc->startViewMove(Direction, speed, storedCVarFloat(r[0], r[1]));
    return 0;
}
template <int Direction>
static int lua_MoveViewStop(lua_State* L) {
    if (auto* cc = luaCamera(L)) cc->stopViewMove(Direction);
    return 0;
}

// WoweeSetCamera(distance, pitch, yaw): the camera as /gps reports it (its
// "Camera:" line, ui::client_gps), at once - for scripts that put a report's
// view back. Not the client's.
static int lua_WoweeSetCamera(lua_State* L) {
    const float distance = static_cast<float>(luaL_checknumber(L, 1));
    const float pitch = static_cast<float>(luaL_checknumber(L, 2));
    const float yaw = static_cast<float>(luaL_checknumber(L, 3));
    if (auto* cc = luaCamera(L)) cc->goToViewPose({distance, pitch, yaw}, 0.0f);
    return 0;
}

// FlipCameraYaw(degrees) (0x005ff2c0).
static int lua_FlipCameraYaw(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: FlipCameraYaw(degrees)");
    if (auto* cc = luaCamera(L)) cc->flipYaw(static_cast<float>(lua_tonumber(L, 1)));
    return 0;
}

// JumpOrAscendStart() (0x005fbf80): holds ascend (0x2000) until AscendStop,
// and jumps.
static int lua_JumpOrAscendStart(lua_State* L) {
    if (!taint::allowed(L, 0)) return 0;   // after the player is found
    auto* svc = getLuaServices(L);
    if (svc && svc->setBindingControl)
        svc->setBindingControl(rendering::CameraController::kControlAscend, true);
    if (svc && svc->pressJump) svc->pressJump();
    return 0;
}
// The held movement bindings (0x005fc0a0-0x005fc560): each Start sets its
// input-control bit and each Stop clears it - MoveForward 0x10, MoveBackward
// 0x20, StrafeLeft 0x40, StrafeRight 0x80; AscendStop 0x2000, DescendStop
// 0x4000.
//
// Every one asks 0x005191c0(0) first, and does nothing for insecure code -
// all but the mouselook pair (0x005fcc10, 0x005fc890), which ask nothing.
template <uint32_t Bit, bool Held, bool Asks = true>
static int lua_BindingControl(lua_State* L) {
    if (Asks && !taint::allowed(L, 0)) return 0;
    auto* svc = getLuaServices(L);
    if (svc && svc->setBindingControl) svc->setBindingControl(Bit, Held);
    return 0;
}
// SitStandOrDescendStart() (0x0051b1d0): holds descend (0x005fc0f0), and out
// of flight sits or stands, as a press of X.
static int lua_SitStandOrDescendStart(lua_State* L) {
    lua_BindingControl<rendering::CameraController::kControlDescend, true>(L);
    if (auto* cc = luaCamera(L)) cc->pressBindingSitStand();
    return 0;
}
// MouselookStart() (0x005fcc10) / MouselookStop() (0x005fc890) hold and
// release the right button's control, 0x1. MoveAndSteerStart() (0x005fc780)
// holds both buttons' controls, 0x2 and 0x1, which is a run forward steered by
// the mouse; MoveAndSteerStop() (0x005fc830) lets go of both. PitchUp/Down
// Start/Stop hold 0x400 / 0x800 (0x005fc8e0, 0x005fc920, 0x005fc570,
// 0x005fc5c0).
using CC = rendering::CameraController;
constexpr uint32_t kMoveAndSteer = CC::kControlLeftMouse | CC::kControlMouselook;
// IsMouselooking() (0x005f9dd0): 1 while 0x2000001 is held, else nil.
static int lua_IsMouselooking(lua_State* L) {
    auto* cc = luaCamera(L);
    if (cc && cc->isMouselooking()) lua_pushnumber(L, 1.0);
    else lua_pushnil(L);
    return 1;
}
template <int Direction, bool Held>
static int lua_BindingTurn(lua_State* L) {
    if (!taint::allowed(L, 0)) return 0;   // 0x005fc320-0x005fc3f0, kind 0
    auto* svc = getLuaServices(L);
    if (svc && svc->setBindingTurn) svc->setBindingTurn(Direction, Held);
    return 0;
}

// HasLFGRestrictions() → whether the group the player is in is *restricted*
//
// Not "am I in a finder group", which is what this answered until the roster's
// own restriction bit was read. The client has two getters side by side and the
// FrameXML uses both: 0x0052c310 answers from 0xbd19a4, the finder dungeon the
// roster carried, and that is IsPartyLFG (below, and used by `minimap.lua:242`
// and `uiparent.lua:3585, :3587`); 0x0052c350 pushes the boolean of 0xbd19a8,
// which is this one. The second byte is written only by the roster handler -
// 0x006d8870:6994 hands it the type byte's bit 2 - and zeroed whenever the party
// is emptied (0x0052d6e0:10375), so it says what the *server* says this group may
// not do, which is not the same as being a finder group at all: a normal group the
// server restricts answers here and not at IsPartyLFG.
//
// Answering it from the finder state cost half of uiparent/unitpopup. That pair
// gates the leave-disband and convert-to-raid buttons, the whisper-guild and
// unit-dissmiss lines and the raid-convert line on this one name
// (`uiparent.lua:3562`, `unitpopup.lua:599, :603, :631, :635, :663, :1060,
// :1068, :1096`, `raidframe.lua:70`, `partymemberframe.lua:190`,
// `playerframe.lua:64`, `targetframe.lua:118`, `chatframe.lua:2880`), and every
// one of them saw "restricted" for a plain finder party and hid what it gates.
static int lua_HasLFGRestrictions(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->getPartyData().lfgRestricted ? 1 : 0);
    return 1;
}

// GetLFGProposal() → proposalExists, typeID, id, name, texture, role,
//                    hasResponded, totalEncounters, completedEncounters,
//                    numMembers, isLeader, isHoliday
//
// Twelve values, which is what the client returns (0x00554170, `return 0xc`).
// The count matters twice over: lfdframe reads the eighth and the twelfth to
// themselves with select(), and a return short of those is an error there and
// not an answer of "none".  The client answers nothing at all when no proposal
// is pending - so every caller has to test the first value before it touches
// another - but each caller in the interface does exactly that, which is why
// the twelve nils here are only a stand-in for a call FrameXML never makes.
//
// The tenth is WoWee's own: the client pushes the constant 5.0 (0x00554170:3510)
// and lets GetLFGProposalMember answer nil for the empty seats of a five-seat
// offer.  The real count is the same answer for a full party and a shorter loop
// for anything else.
static int lua_GetLFGProposal(lua_State* L) {
    auto* gh = getGameHandler(L);
    const bool pending = gh && gh->getLfgState() == game::LfgState::Proposal;
    if (!pending) { for (int i = 0; i < 12; ++i) lua_pushnil(L); return 12; }

    // Nils here are not survivable, which only showed once the dialog could
    // actually open. LFDDungeonReadyPopup_Update builds a path with
    // "UI-LFG-BACKGROUND-"..texture, and concatenating nil raises; numMembers
    // goes into `for i = numMembers+1, ...`, where nil is arithmetic on
    // nothing; completedEncounters is compared with > 0. The dialog was
    // unreachable before, so none of it had ever run.
    const uint32_t dungeonId = gh->getLfgDungeonId();
    const game::LfgDungeon* dungeon = nullptr;
    for (const auto& d : gh->getLfgDungeons()) {
        if (d.id == dungeonId) { dungeon = &d; break; }
    }

    // The role the player has in the offered party, which is what the dialog
    // names and icons: the client scans the proposal's own member rows for the
    // one flagged self and reads that row's mask (0x00554170:3470-3484).  Not
    // the local role selection, which is what this read until now - the two
    // differ for a pre-made party, whose members are offered the role each of
    // them walks in with rather than what each had ticked in the queue.  The
    // selection stays as the fallback for a proposal that carries no self row,
    // which is the case the client's scan leaves a leftover in.
    const auto& members = gh->getLfgProposalMembers();
    const auto self = std::find_if(members.begin(), members.end(),
                                   [](const game::LfgProposalMember& m) { return m.isSelf; });
    const uint32_t roles = self != members.end() ? self->role
                                                 : gh->getLfgOfferedRoles();
    // The same mask's bit 0 is the group leader, which is why the client tests
    // 2/4/8 for the three role names and 1 on its own for its eleventh value
    // (0x00554170:3480, :3511).  lfdframe.lua:541 draws the leader mark with it.
    const bool isLeader = (roles & 0x01) != 0;
    const char* role = (roles & 0x02) ? "TANK"
                     : (roles & 0x04) ? "HEALER"
                                      : "DAMAGER";

    lua_pushboolean(L, 1);                                       // 1: proposalExists
    lua_pushnumber(L, dungeon ? dungeon->typeId : 1);            // 2: typeID
    // The dungeon, not the proposal: LFDDungeonReadyPopup keeps this as
    // dungeonID and looks the dungeon up by it.
    lua_pushnumber(L, dungeonId);                                // 3: id
    lua_pushstring(L, dungeon ? dungeon->name.c_str()
                              : gh->getCurrentLfgDungeonName().c_str());  // 4: name
    // TextureFilename from LFGDungeons.dbc, which is exactly the suffix that
    // path is built from.
    lua_pushstring(L, dungeon ? dungeon->texture.c_str() : "");  // 5: texture
    lua_pushstring(L, role);                                     // 6: role
    lua_pushboolean(L, 0);                                       // 7: hasResponded
    // Bosses there are and bosses done. The client derives both rather than
    // reading them: it walks DungeonEncounter.dbc for the offered dungeon's map
    // and difficulty and counts the rows against the mask the proposal carries
    // (0x00554170:3483-3500, the same walk countLfgEncounters makes for the raid
    // browser). Two kinds of row skip that walk - a "Random" row answers nought
    // bosses, and one with flag bit 2 set, which is the four holiday rows and
    // nothing else, answers exactly one - and both spell "done" as "the mask is
    // not empty", so a holiday offer reads as cleared or not rather than partly
    // cleared. Nothing is answered for either number without an LFGDungeons row
    // to walk from, which is what the client's own early return leaves the pair
    // as: nought, since the values are pushed as numbers and not nils.
    //
    // These two are what the ready dialog draws its "Bosses Killed: %d of %d"
    // line from (lfdframe.lua:478,534) and what bounds the boss list its tooltip
    // builds, so while they were both nought the tooltip was skipped by its own
    // `if ( numBosses == 0 … )` and GetLFGProposalEncounter never ran.
    uint32_t totalEncounters = 0, doneEncounters = 0;
    const uint32_t doneMask = gh->getLfgProposalEncountersDone();
    if (dungeon) {
        if (dungeon->typeId == static_cast<uint32_t>(game::LfgTypeId::Random)) {
            doneEncounters = doneMask != 0 ? 1u : 0u;
        } else if ((dungeon->flags & 0x04) != 0) {
            totalEncounters = 1;
            doneEncounters  = doneMask != 0 ? 1u : 0u;
        } else {
            const auto counts = game::countLfgEncounters(gh->getDungeonEncounters(),
                                                         dungeon->mapId, dungeon->difficulty,
                                                         doneMask);
            totalEncounters = counts.total;
            doneEncounters  = counts.complete;
        }
    }
    lua_pushnumber(L, totalEncounters);                          // 8: totalEncounters
    lua_pushnumber(L, doneEncounters);                           // 9: completedEncounters
    // The real count now that the roster is parsed - each row is a role icon
    // and a tick, both of which GetLFGProposalMember can answer.
    lua_pushnumber(L, static_cast<double>(gh->getLfgProposalMembers().size()));  // 10

    lua_pushboolean(L, isLeader ? 1 : 0);                        // 11: isLeader
    // Whether this is one of the four seasonal bosses. The client answers it off
    // flag bit 2 of the dungeon's own row (0x00554170:3505); lfdframe.lua reads
    // it as the twelfth value and drops the boss tooltip for those offers,
    // whose single "boss" is not a boss at all.
    lua_pushboolean(L, dungeon && (dungeon->flags & 0x04) != 0);  // 12: isHoliday
    return 12;
}

// GetLFGInfoServer() → inParty, joined, queued, noPartialClear, achievements,
//                      lfgComment, slotCount
static int lua_GetLFGInfoServer(lua_State* L) {
    auto* gh = getGameHandler(L);
    const bool queued = gh && gh->isLfgQueued();
    // Answers, not blanks. LFRFrame_OnEvent does
    // `LFRQueueFrameComment:SetText(lfgComment)` and then `strtrim(lfgComment)`
    // whenever it is told the queue was joined, and nil there raised inside
    // strtrim - once for every LFG_UPDATE, which the server prompts every few
    // seconds while queued. The whole handler died with it each time, so the
    // raid browser's comment box was never updated and the log filled up.
    //
    // This client keeps no listing comment: an empty string is what it has,
    // and it is the value the interface tests for anyway.
    const bool inParty = gh && gh->isInGroup();
    lua_pushboolean(L, inParty ? 1 : 0);   // 1: inParty
    lua_pushboolean(L, queued ? 1 : 0);    // 2: joined
    lua_pushboolean(L, queued ? 1 : 0);    // 3: queued
    lua_pushboolean(L, 0);                 // 4: noPartialClear
    lua_pushboolean(L, 0);                 // 5: achievements
    lua_pushstring(L, "");                 // 6: lfgComment
    lua_pushnumber(L, queued ? 1 : 0);     // 7: slotCount
    return 7;
}

// GetLFGRoleUpdate() → roleCheckInProgress, slots, members
//
// The last two were nil, and nil is not a harmless blank here.
// LFDRoleCheckPopupDescription_OnEnter opens with `if ( slots <= 1 )`, which
// compares nil to a number and raises - hovering the role-check popup took the
// file down with it. LFDRoleCheckPopup_Update branches on `slots == 1` too, so
// every check also described itself as being for multiple dungeons.
//
// Both come off SMSG_LFG_ROLE_CHECK_UPDATE, which carries the dungeon list and
// the group size behind the count the handler used to stop at.
static int lua_GetLFGRoleUpdate(lua_State* L) {
    auto* gh = getGameHandler(L);
    const bool checking = gh && gh->getLfgState() == game::LfgState::RoleCheck;
    if (checking) lua_pushboolean(L, 1); else lua_pushnil(L);
    lua_pushnumber(L, gh ? static_cast<double>(gh->getLfgRoleCheckDungeons().size()) : 0.0);
    lua_pushnumber(L, gh ? gh->getLfgRoleCheckMembers() : 0);
    return 3;
}

// GetAllowLowLevelRaid() (0x0051d5a0): PLAYER_FLAGS 0x10000.
static int lua_GetAllowLowLevelRaid(lua_State* L) { return pushPlayerFlagBit(L, 0x10000); }

// GetTaxiBenchmarkMode() (0x0051d100): PLAYER_FLAGS 0x20000.
static int lua_GetTaxiBenchmarkMode(lua_State* L) { return pushPlayerFlagBit(L, 0x20000); }

/// A request of one opcode and one byte, the way the client's setters build
/// them.
static void sendByteRequest(game::GameHandler* gh, game::Opcode op, uint8_t value) {
    if (!gh || game::wireOpcode(op) == 0xFFFF) return;
    network::Packet packet(game::wireOpcode(op));
    packet.writeUInt8(value);
    gh->sendInterfaceRequest(packet);
}

// SetAllowLowLevelRaid([on]) (0x0051d600): with a player, the switch twice -
// CMSG_SET_ALLOW_LOW_LEVEL_RAID1 and _RAID2, a byte each.
static int lua_SetAllowLowLevelRaid(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!playerFlagsOf(gh)) return 0;
    const uint8_t on = luaClientSwitchArg(L, 1, true) ? 1 : 0;
    sendByteRequest(gh, game::Opcode::CMSG_SET_ALLOW_LOW_LEVEL_RAID1, on);
    sendByteRequest(gh, game::Opcode::CMSG_SET_ALLOW_LOW_LEVEL_RAID2, on);
    return 0;
}

// OpeningCinematic() (0x005166f0): CMSG_OPENING_CINEMATIC, which asks the
// server for the race's opening cinematic.
static int lua_OpeningCinematic(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh || game::wireOpcode(game::Opcode::CMSG_OPENING_CINEMATIC) == 0xFFFF) {
        LOG_WARNING("OpeningCinematic: no CMSG_OPENING_CINEMATIC to send");
        return 0;
    }
    network::Packet packet(game::wireOpcode(game::Opcode::CMSG_OPENING_CINEMATIC));
    gh->sendInterfaceRequest(packet);
    return 0;
}

// OpenTrainer() (0x00595040 -> 0x006d1f20): with a player, CMSG_TRAINER_LIST
// for the player's own guid - the trainer window asked of oneself, which a
// server answers only for a character it lets train itself.
static int lua_OpenTrainer(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!playerFlagsOf(gh) || game::wireOpcode(game::Opcode::CMSG_TRAINER_LIST) == 0xFFFF) {
        LOG_WARNING("OpenTrainer: no player to ask for, or no CMSG_TRAINER_LIST");
        return 0;
    }
    network::Packet packet(game::wireOpcode(game::Opcode::CMSG_TRAINER_LIST));
    packet.writeUInt64(gh->getPlayerGuid());
    gh->sendInterfaceRequest(packet);
    return 0;
}

/// A DBC this API reads, loaded, or null.
static std::shared_ptr<pipeline::DBCFile> loadedDBC(game::GameHandler* gh, const char* name) {
    auto* am = gh ? gh->services().assetManager : nullptr;
    if (!am || !am->isInitialized()) return nullptr;
    auto file = am->loadDBCOptional(name);
    return file && file->isLoaded() ? file : nullptr;
}

/// A localized string column, from whichever locale slot the client's file
/// fills (sixteen of them, then the flags).
static std::string localizedDBCString(const pipeline::DBCFile& dbc, uint32_t row, uint32_t first) {
    const uint32_t fields = dbc.getFieldCount();
    for (uint32_t i = 0; i < 16 && first + i < fields; ++i) {
        std::string text = dbc.getString(row, first + i);
        if (!text.empty()) return text;
    }
    return {};
}

// CanMapChangeDifficulty([mapID]) (0x00517d70): 1 when Map.dbc flags the map,
// the current one unless another is named, 0x100; nil otherwise.
static int lua_CanMapChangeDifficulty(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t mapId = lua_isnumber(L, 1) ? static_cast<uint32_t>(luaToClientInt(L, 1))
                                              : (gh ? gh->getCurrentMapId() : 0);
    auto maps = loadedDBC(gh, "Map.dbc");
    const int32_t row = maps && maps->getFieldCount() > 3 ? maps->findRecordById(mapId) : -1;
    constexpr uint32_t kMapFlagCanChangeDifficulty = 0x100;
    if (row >= 0 && (maps->getUInt32(static_cast<uint32_t>(row), 3) & kMapFlagCanChangeDifficulty))
        lua_pushnumber(L, 1);
    else
        lua_pushnil(L);
    return 1;
}

// GetGMTicketCategories() → id1, name1, id2, name2, ... (0x00516aa0): every
// row of GMTicketCategory.dbc.
static int lua_GetGMTicketCategories(lua_State* L) {
    auto categories = loadedDBC(getGameHandler(L), "GMTicketCategory.dbc");
    if (!categories || categories->getFieldCount() < 2) return 0;
    const uint32_t n = categories->getRecordCount();
    if (!lua_checkstack(L, static_cast<int>(n) * 2 + 1)) return 0;
    for (uint32_t r = 0; r < n; ++r) {
        lua_pushnumber(L, categories->getUInt32(r, 0));
        lua_pushstring(L, localizedDBCString(*categories, r, 1).c_str());
    }
    return static_cast<int>(n) * 2;
}

/// An Achievement.dbc row's flags and points (fields 41 and 39), for an
/// achievement that is not hidden (flag 2); none otherwise.
static std::optional<uint32_t> shownAchievementPoints(lua_State* L, uint32_t id) {
    auto achievements = loadedDBC(getGameHandler(L), "Achievement.dbc");
    if (!achievements || achievements->getFieldCount() <= 41) return std::nullopt;
    const int32_t row = achievements->findRecordById(id);
    if (row < 0) return std::nullopt;
    constexpr uint32_t kAchievementFlagHidden = 0x2;
    if (achievements->getUInt32(static_cast<uint32_t>(row), 41) & kAchievementFlagHidden) return std::nullopt;
    return achievements->getUInt32(static_cast<uint32_t>(row), 39);
}

// GetAchievementNumRewards(achievementID) (0x005b1970): 1 for one that is
// shown, nil for a hidden one or none.
static int lua_GetAchievementNumRewards(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetAchievementNumRewards(achievementID)");
    if (shownAchievementPoints(L, static_cast<uint32_t>(luaToClientInt(L, 1)))) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

// GetAchievementReward(achievementID, index) (0x005b1a00): its points, which
// are its one reward; nil for a hidden one or none.
static int lua_GetAchievementReward(lua_State* L) {
    if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2)) {
        // The client's own words, the other function's name and all (0x00a17638).
        return luaL_error(L, "Usage: GetAchievementNumRewards(achievementID, rewardIndex)");
    }
    if (const auto points = shownAchievementPoints(L, static_cast<uint32_t>(luaToClientInt(L, 1))))
        lua_pushnumber(L, *points);
    else
        lua_pushnil(L);
    return 1;
}

// GetNumPetitionItems() (0x005a1f70): how many charters the petition vendor
// offers, of the ten the client keeps.
static int lua_GetNumPetitionItems(lua_State* L) {
    auto* gh = getGameHandler(L);
    int count = 0;
    uint32_t itemId = 0, displayId = 0, cost = 0;
    while (gh && count < 10 && gh->getPetitionCharter(count, itemId, displayId, cost)) ++count;
    lua_pushnumber(L, count);
    return 1;
}

// GetLFGRoleUpdateMember(index) → chosen, level, name, className, leader,
// tank, healer, damage (0x00554750): a member of the role check, the four
// roles nil until they have chosen; the name and class nil and "" for one the
// name cache does not know yet.
static int lua_GetLFGRoleUpdateMember(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetLFGRoleUpdateMember(index)");
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(std::llround(lua_tonumber(L, 1)));
    if (!gh) return 0;
    const auto& members = gh->getLfgRoleCheckMemberList();
    if (index < 1 || index > static_cast<int>(members.size())) return 0;
    const auto& m = members[static_cast<size_t>(index) - 1];
    lua_pushboolean(L, m.chosen ? 1 : 0);
    lua_pushnumber(L, m.level);
    const std::string& name = gh->lookupName(m.guid);
    if (name.empty()) {
        lua_pushnil(L);
        lua_pushnil(L);
    } else {
        lua_pushstring(L, name.c_str());
        const uint8_t classId = gh->lookupPlayerClass(m.guid);
        lua_pushstring(L, luaClassToken(classId) ? kLuaClasses[classId] : "");
    }
    if (m.chosen) {
        for (uint32_t bit : {1u, 2u, 4u, 8u}) lua_pushboolean(L, (m.roles & bit) ? 1 : 0);
    } else {
        for (int i = 0; i < 4; ++i) lua_pushnil(L);
    }
    return 8;
}

static int lua_IsListedInLFR(lua_State* L) {
    // The listed state is the joined raid-browser list, and this client sets it
    // when the browser's dropdown asks (0x00beaa14, written by 0x00558ed0 and
    // cleared by 0x00558f90 and by the search update's false byte) - it is not
    // one of the queue states, and no packet on its own ever said "listed".
    // LfgState::RaidBrowser was the answer here and nothing ever set it, which
    // is why FrameXML's GetLFGMode could not return "listed" at all: its
    // precedence tests the queue first and this flag after.
    auto* gh = getGameHandler(L);
    const bool listed = gh != nullptr && gh->getLfgJoinedListId() != 0;
    lua_pushboolean(L, listed ? 1 : 0);
    return 1;
}

// IsPartyLFG() → 1 while the party has a dungeon finder dungeon, else nil
// (0x0052c310 reads 0xbd19a4).
static int lua_IsPartyLFG(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh && gh->getPartyData().lfgDungeonEntry != 0) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

// The two cooldowns that gate re-queuing. Neither is tracked, and both are read
// as `if ( expiration )` - so nil, because a zero would read as a live cooldown
// and park the queue frame behind a countdown that never ends.
static int lua_GetLFGDeserterExpiration(lua_State* L) { lua_pushnil(L); return 1; }
static int lua_GetLFGRandomCooldownExpiration(lua_State* L) { lua_pushnil(L); return 1; }

// RefreshLFGList() - the raid browser's refresh button.
static int lua_RefreshLFGList(lua_State* L) { (void)L; return 0; }

// GetTrackedAchievements() → the achievement ids being watched, as separate
// values. The watch frame counts them by return count.
static int lua_GetTrackedAchievements(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const auto& tracked = gh->getTrackedAchievements();
    // The watch frame shows ten, but nothing here enforces that, and Lua
    // guarantees twenty free slots.
    if (!tracked.empty() && !lua_checkstack(L, static_cast<int>(tracked.size()))) {
        return 0;
    }
    for (uint32_t id : tracked) lua_pushnumber(L, id);
    return static_cast<int>(tracked.size());
}

// IsPVPTimerRunning() - 0x0051bd60. Not the countdown: the test is the realm's
// PvP flag bit in PLAYER_FLAGS (0x40000) and nothing else, so a player who is
// flagged answers 1 even after their time is up and GetPVPTimer has started
// answering 999. Without a local player it is the usual nil, not false.
static int lua_IsPVPTimerRunning(lua_State* L) { return pushPlayerFlagBit(L, 0x00040000); }

// GetPVPTimer() - 0x0051bd00, which is 0x006de7a0's remaining time plus a second.
// The realm never sends the remaining time; the client works it out from a
// deadline it arms itself when the flag bit arrives (0x006de750, five minutes
// from the moment the flag is seen), which is what the PLAYER_FLAGS watch keeps
// in pvpFlagDeadlineMs_. So the answer is the full five minutes whenever the
// flag bit is clear, whatever is left of it while it runs, and 999 once the
// deadline has passed - and 0 with no local player, where the client skips the
// extra second because it never calls the countdown at all.
static int lua_GetPVPTimer(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto flags = playerFlagsOf(gh);
    double ms = 0;
    if (flags) {
        if ((*flags & 0x00040000) == 0) {
            ms = 300000;
        } else {
            const int64_t deadline = gh->pvpFlagDeadlineMs();
            const auto now = static_cast<int64_t>(core::appTimeSeconds() * 1000.0);
            ms = (deadline != 0 && now < deadline) ? static_cast<double>(deadline - now) : -1.0;
        }
        ms += 1000;
    }
    lua_pushnumber(L, ms);
    return 1;
}

// GetCurrentArenaSeason() → the season number, or NO_ARENA_SEASON.
//
// Zero is the right answer here rather than the usual trap: arenaframe compares
// it with == and ~= against NO_ARENA_SEASON, which is itself 0, so a nil would
// fail both tests instead of meaning "no season".
static int lua_GetCurrentArenaSeason(lua_State* L) { lua_pushnumber(L, 0); return 1; }

// GetPVPRankProgress() → how far through the current rank, 0..1.
// Fed straight to HonorFrameProgressBar:SetValue, which needs a number.
static int lua_GetPVPRankProgress(lua_State* L) { lua_pushnumber(L, 0); return 1; }

// ---- Arena team roster ----
//
// Which team an index names comes from the same list GetArenaTeam reads.
// ArenaTeamRoster(index) - ask the server for the roster.
static int lua_ArenaTeamRoster(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t teamId = arenaTeamIdAtIndex(gh, static_cast<int>(luaL_optnumber(L, 1, 0)));
    if (gh && teamId) gh->requestArenaTeamRoster(teamId);
    return 0;
}

// GetArenaTeamRosterInfo(teamIndex, memberIndex) →
//   name, rank, level, class, online, played, win, seasonPlayed, seasonWin, rating
//
// The six counts are numbers rather than nil: the panel subtracts them the line
// after - `loss = played - win` - so a nil raises there. Class stays nil, which
// the panel does test before using, and rank and level are zero because the
// roster carries neither.
static int lua_GetArenaTeamRosterInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t teamId = arenaTeamIdAtIndex(gh, static_cast<int>(luaL_optnumber(L, 1, 0)));
    const int member = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (!gh || teamId == 0 || member < 1) return luaReturnNil(L);
    const auto* roster = gh->getArenaTeamRoster(teamId);
    if (!roster || member > static_cast<int>(roster->members.size())) return luaReturnNil(L);
    const auto& m = roster->members[static_cast<size_t>(member) - 1];

    lua_pushstring(L, m.name.c_str());       // 1: name
    lua_pushnumber(L, 0);                    // 2: rank
    lua_pushnumber(L, 0);                    // 3: level
    lua_pushnil(L);                          // 4: class
    lua_pushboolean(L, m.online ? 1 : 0);    // 5: online
    lua_pushnumber(L, m.weekGames);          // 6: played
    lua_pushnumber(L, m.weekWins);           // 7: win
    lua_pushnumber(L, m.seasonGames);        // 8: seasonPlayed
    lua_pushnumber(L, m.seasonWins);         // 9: seasonWin
    lua_pushnumber(L, m.personalRating);     // 10: rating
    return 10;
}

// Which roster row is selected, and closing the roster. Both are the panel's
// own state - nothing is sent for either.
static int lua_GetArenaTeamRosterSelection(lua_State* L) { lua_pushnumber(L, 0); return 1; }
static int lua_SetArenaTeamRosterSelection(lua_State* L) { (void)L; return 0; }
static int lua_CloseArenaTeamRoster(lua_State* L) { (void)L; return 0; }

// Team captaincy is not reported by anything this client parses, so no team
// reads as the player's to run.
static int lua_IsArenaTeamCaptain(lua_State* L) { lua_pushboolean(L, 0); return 1; }

// Closing the battlemaster's window, which is a client-side dismissal.
static int lua_CloseBattlefield(lua_State* L) { (void)L; return 0; }

// CanExitVehicle(): 1 where the player's seat has Flags 0x2000000, else nil
// (0x005fb9c0).
static int lua_CanExitVehicle(lua_State* L) {
    if (playerVehicleSeatHasFlag(getGameHandler(L), 0x2000000)) lua_pushnumber(L, 1.0);
    else lua_pushnil(L);
    return 1;
}
// CanSwitchVehicleSeats(): 1 where the player's seat has Flags 0x4000000,
// else nil (0x005fba10).
static int lua_CanSwitchVehicleSeats(lua_State* L) {
    if (playerVehicleSeatHasFlag(getGameHandler(L), 0x4000000)) lua_pushnumber(L, 1.0);
    else lua_pushnil(L);
    return 1;
}
// VehiclePrevSeat() / VehicleNextSeat(): where the player's seat lets it
// switch (Flags 0x4000000), CMSG_REQUEST_VEHICLE_PREV_SEAT / NEXT_SEAT
// (0x005fb6d0 -> 0x0074c8b0, 0x005fb720 -> 0x0074c9a0).
static int lua_VehiclePrevSeat(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (playerVehicleSeatHasFlag(gh, 0x4000000)) gh->sendRequestVehicleSeatStep(false);
    return 0;
}
static int lua_VehicleNextSeat(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (playerVehicleSeatHasFlag(gh, 0x4000000)) gh->sendRequestVehicleSeatStep(true);
    return 0;
}
// IsUsingVehicleControls(): the seat's Flags 0x800000 (0x005fb970).
static int lua_IsUsingVehicleControls(lua_State* L) {
    if (playerVehicleSeatHasFlag(getGameHandler(L), 0x800000)) lua_pushnumber(L, 1.0);
    else lua_pushnil(L);
    return 1;
}
// The vehicle's aim, on the unit 0x005f9d20 names: the player, or the
// vehicle whose control seat it sits in.
//
// IsVehicleAimAngleAdjustable() / IsVehicleAimPowerAdjustable(): 1 where that
// unit's Vehicle.dbc row has Flags 0x400 / 0x800, else nil (0x005f9f70,
// 0x005f9fe0).
static int pushAimRowFlag(lua_State* L, uint32_t flag) {
    auto* gh = getGameHandler(L);
    const auto* row = gh ? gh->vehicleRowOf(gh->vehicleAimUnit()) : nullptr;
    if (row && (row->flags & flag)) lua_pushnumber(L, 1.0);
    else lua_pushnil(L);
    return 1;
}
static int lua_IsVehicleAimAngleAdjustable(lua_State* L) {
    return pushAimRowFlag(L, game::vehicle::kVehicleFlagAimAngle);
}
static int lua_IsVehicleAimPowerAdjustable(lua_State* L) {
    return pushAimRowFlag(L, game::vehicle::kVehicleFlagAimPower);
}
// VehicleAimGetAngle(): that unit's pitch (0x005f9e10).
static int lua_VehicleAimGetAngle(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->unitPitch(gh->vehicleAimUnit()) : 0.0);
    return 1;
}
// VehicleAimGetNormAngle(): the pitch as a share of its row's limits
// (0x005f9e60).
static int lua_VehicleAimGetNormAngle(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return luaReturnZero(L);
    const uint64_t unit = gh->vehicleAimUnit();
    lua_pushnumber(L, game::vehicle::normalizedAim(gh->vehicleRowOf(unit), gh->unitPitch(unit)));
    return 1;
}
// VehicleAimSetNormPower(p) / VehicleAimGetNormPower(): kept, 0..1
// (0x005f9f10, 0x005f9550).
static int lua_VehicleAimSetNormPower(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh && lua_isnumber(L, 1)) gh->setVehicleAimPower(static_cast<float>(lua_tonumber(L, 1)));
    return 0;
}
static int lua_VehicleAimGetNormPower(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? gh->vehicleAimPower() : 0.0);
    return 1;
}
// VehicleAimIncrement([d]) / VehicleAimDecrement([d]): the mover's pitch
// moved by d, 0.1 unless given (0x005fb770, 0x005fb7d0 -> 0x005fb4b0).
static int stepVehicleAim(lua_State* L, float sign) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const float d = lua_isnumber(L, 1) ? static_cast<float>(lua_tonumber(L, 1)) : 0.1f;
    // From the pitch a pitch over time is on its way to, else the mover's.
    gh->requestVehicleAim(gh->activeMover(), gh->vehicleAimBasePitch() + sign * d);
    return 0;
}
static int lua_VehicleAimIncrement(lua_State* L) { return stepVehicleAim(L, 1.0f); }
static int lua_VehicleAimDecrement(lua_State* L) { return stepVehicleAim(L, -1.0f); }
// VehicleAimRequestAngle(a): where the mover's row has Flags 0x400
// (0x005fb820).
static int lua_VehicleAimRequestAngle(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const uint64_t mover = gh->activeMover();
    const auto* row = gh->vehicleRowOf(mover);
    if (row && (row->flags & game::vehicle::kVehicleFlagAimAngle) && lua_isnumber(L, 1)) {
        gh->requestVehicleAim(mover, static_cast<float>(lua_tonumber(L, 1)));
    }
    return 0;
}
// VehicleAimRequestNormAngle(n): the aim unit's limits' share n, where its
// row has Flags 0x400 (0x005fb8c0).
static int lua_VehicleAimRequestNormAngle(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const uint64_t unit = gh->vehicleAimUnit();
    const auto* row = gh->vehicleRowOf(unit);
    if (row && (row->flags & game::vehicle::kVehicleFlagAimAngle) && lua_isnumber(L, 1)) {
        const auto limits = game::vehicle::aimLimits(row);
        const float n = static_cast<float>(lua_tonumber(L, 1));
        gh->requestVehicleAim(unit, limits.min + (limits.max - limits.min) * n);
    }
    return 0;
}

// HasKey() - whether the player carries a key ring at all. The keyring exists
// and holds keys, so the button that opens it is offered.
static int lua_HasKey(lua_State* L) { lua_pushboolean(L, 1); return 1; }

// GetArenaTeam(index) →
//   teamName, teamSize, teamRating, teamPlayed, teamWins, seasonTeamPlayed,
//   seasonTeamWins, playerPlayed, seasonPlayerPlayed, teamRank, playerRating,
//   backgroundR, backgroundG, backgroundB, emblem, emblemR, emblemG, emblemB,
//   border, borderR, borderG, borderB
//
// Twenty-two, because PVPTeam_Update unpacks every one of them on a single line
// and then feeds the colour components straight to SetVertexColor. Answering
// just the name - which is all the promote and kick confirmations need - left
// twenty-one nils behind it and took the team list down.
//
// The tabard is not tracked: SMSG_ARENA_TEAM_QUERY_RESPONSE carries the emblem
// style and colours and nothing here reads them. Those eight answer zero rather
// than nil, so the tabard draws black instead of raising mid-arithmetic.
static int lua_GetArenaTeam(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int idx = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || idx < 1) return luaReturnNil(L);
    const auto& teams = gh->getArenaTeamStats();
    if (idx > static_cast<int>(teams.size())) return luaReturnNil(L);
    const auto& t = teams[static_cast<size_t>(idx) - 1];

    lua_pushstring(L, t.teamName.c_str());  // 1: teamName
    lua_pushnumber(L, t.teamType);          // 2: teamSize (2, 3 or 5)
    lua_pushnumber(L, t.rating);            // 3: teamRating
    lua_pushnumber(L, t.weekGames);         // 4: teamPlayed
    lua_pushnumber(L, t.weekWins);          // 5: teamWins
    lua_pushnumber(L, t.seasonGames);       // 6: seasonTeamPlayed
    lua_pushnumber(L, t.seasonWins);        // 7: seasonTeamWins
    // Per-player totals come with the roster, not the team summary.
    lua_pushnumber(L, 0);                   // 8: playerPlayed
    lua_pushnumber(L, 0);                   // 9: seasonPlayerPlayed
    lua_pushnumber(L, t.rank);              // 10: teamRank
    lua_pushnumber(L, 0);                   // 11: playerRating
    for (int i = 0; i < 11; ++i) lua_pushnumber(L, 0);  // 12-22: tabard
    return 22;
}

// The daily-win bonus on a random or holiday battleground.
//
// → hasWin, winHonor, winArena, lossHonor, lossArena
//
// Zero rather than nil for the four amounts, and the difference matters: the
// frame writes `if (winHonor ~= 0)` and shows a reward line when that passes.
// nil passes it - nil is not zero - and the line appears with nothing in it.
// Zero says "no bonus" and the line is correctly skipped.
//
// The bonus itself is a per-character daily the server tracks and does not
// volunteer, so "none available" is the honest answer rather than a placeholder
// for one. Reachable, unlike most of this file's absences: the random
// battleground row is drawn from GetBattlegroundInfo, which answers for real.
static int lua_BattlegroundHonorBonusesNone(lua_State* L) {
    lua_pushboolean(L, 0);   // hasWin - the daily is not known to be waiting
    lua_pushnumber(L, 0);    // winHonor
    lua_pushnumber(L, 0);    // winArena
    lua_pushnumber(L, 0);    // lossHonor
    lua_pushnumber(L, 0);    // lossArena
    return 5;
}

/// GetRandomBGHonorCurrencyBonuses() → hasWin, winHonor, winArena, lossHonor,
/// lossArena - what the random battleground pays on top of the match itself.
///
/// The five zeroes above are what this answered, and the panel hides an amount
/// that is zero - so the random battleground's rewards box was drawn empty,
/// which is the whole reason a player picks that row.
///
/// The figures are the stock ones: thirty honour and twenty-five arena points
/// for the day's first win, five honour for a loss. They are the server's to
/// decide and it never sends them, so a realm that has changed its
/// Battleground.RewardWinner* settings will be described by this as though it
/// had not - which is a great deal closer than nothing at all, and is what the
/// real client shows, since it does not ask either.
///
/// hasWin is false because whether *today's* first win is still waiting is
/// per-character daily state the server keeps to itself. The panel does not
/// read it; it is the fifth return only because the API has five.
static int lua_GetRandomBGHonorCurrencyBonuses(lua_State* L) {
    lua_pushboolean(L, 0);    // hasWin
    lua_pushnumber(L, 30);    // winHonor
    lua_pushnumber(L, 25);    // winArena
    lua_pushnumber(L, 5);     // lossHonor
    lua_pushnumber(L, 0);     // lossArena
    return 5;
}

// GetBattlefieldInstanceInfo(index) → which numbered instance that row is.
//
// Zero, which the list reads as unnumbered - the same "first available" the
// selection means. The server numbers instances only for a client that asks to
// pick one, and nothing here does.
static int lua_GetBattlefieldInstanceInfo(lua_State* L) {
    // The instance number of the i-th battleground instance on offer, counting
    // from one. A flat zero before, so every row in the list read "Warsong
    // Gulch 0" - and the count beside it, GetNumBattlefields, was not bound at
    // all, so the list never got that far: it raised on a nil global.
    //
    // SMSG_BATTLEFIELD_LIST carries these and they were parsed into
    // AvailableBgInfo::instanceIds all along.
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || index < 1) { lua_pushnumber(L, 0); return 1; }
    const auto& bgs = gh->getAvailableBgs();
    if (bgs.empty()) { lua_pushnumber(L, 0); return 1; }
    const auto& ids = bgs.back().instanceIds;
    if (index > static_cast<int>(ids.size())) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, ids[static_cast<size_t>(index) - 1]);
    return 1;
}

// GetNumBattlefields() - how many instances of it there are to choose between.
static int lua_GetNumBattlefields(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); return 1; }
    const auto& bgs = gh->getAvailableBgs();
    lua_pushnumber(L, bgs.empty()
        ? 0 : static_cast<lua_Number>(bgs.back().instanceIds.size()));
    return 1;
}

// IsInLFGDungeon() - standing inside a dungeon the finder put you in.
//
// The state is already tracked: SocialHandler keeps an LfgState and InDungeon
// is one of its values. Nothing read it, so the minimap's dungeon button could
// not tell inside from outside and offered the wrong direction.
static int lua_IsInLFGDungeon(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->getLfgState() == game::LfgState::InDungeon ? 1 : 0);
    return 1;
}

// LFGTeleport(out) - out to the dungeon's entrance, or back in.
static int lua_LFGTeleport(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->lfgTeleport(lua_toboolean(L, 1) != 0);
    return 0;
}

// Whether the world the player is standing in is an arena.
//
// From BattlemasterList.dbc, which names each row's maps and which this client
// already reads for the queue list - the arena rows are loaded alongside the
// battleground ones and only the battlegrounds are kept in the queue list, so
// the arena maps were sitting there unasked.
//
// Both of these answered nil, which the battlefield frame and the arena frame
// each read as "not an arena". That is right until the player is in one.
static int lua_IsBattlefieldArena(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushboolean(L, gh && gh->isArenaMap(gh->getCurrentMapId()) ? 1 : 0);
    return 1;
}

// IsActiveBattlefieldArena() → isArena, isRegistered
//
// The second is whether the team is a registered one rather than a skirmish,
// which needs the arena team the server never mentions outside a match. Left
// nil, and the frame treats that as unregistered - which a skirmish is.
static int lua_IsActiveBattlefieldArena(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh && gh->isArenaMap(gh->getCurrentMapId())) lua_pushboolean(L, 1);
    else lua_pushnil(L);
    lua_pushnil(L);
    return 2;
}

// CanHearthAndResurrectFromArea() → 1 where the zone's AreaTable row carries
// 0x08000000, else nil (0x0054c810 reads the zone at 0x00bd080c) - the
// combined hearth-and-release button, which is Wintergrasp's.
static int lua_CanHearthAndResurrectFromArea(lua_State* L) {
    auto* gh = getGameHandler(L);
    auto* svc = getLuaServices(L);
    const uint32_t zoneId = svc && svc->getLiveZoneId ? svc->getLiveZoneId() : 0;
    if (gh && zoneId != 0 && (gh->getAreaFlags(zoneId) & 0x08000000u) != 0) {
        lua_pushnumber(L, 1);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// HearthAndResurrectFromArea() - asks for it, and answers nothing
// (0x0054c7a0: CMSG_HEARTH_AND_RESURRECT, empty).
static int lua_HearthAndResurrectFromArea(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->sendHearthAndResurrect();
    return 0;
}

// GetWorldPVPQueueStatus(i) → status, mapName, queueID
//
// "none", not nil. An empty world PvP queue is a status of its own, and the
// interface tests for it by name:
//
//     status, mapName, queueID = GetWorldPVPQueueStatus(i);
//     if ( status ~= "none" ) then numberQueues = numberQueues + 1; end
//
// nil is not "none", so this counted a queue that did not exist on every pass -
// and BattlefieldFrame_UpdateStatus ends by hiding the minimap's battlefield
// icon only when numberQueues reaches zero, which it now never did. The icon
// sat beside the minimap permanently, with no queue behind it and so nothing in
// its tooltip: a queue tracker that was always there and never tracked
// anything.
//
// There is no Wintergrasp queue in this client, so "none" is also the true
// answer. mapName and queueID stay nil, which is what an empty slot answers.
static int lua_GetWorldPVPQueueStatus(lua_State* L) {
    lua_pushstring(L, "none");
    lua_pushnil(L);
    lua_pushnil(L);
    return 3;
}

// LeaveBattlefield() - walk out of the battleground currently being played.
static int lua_LeaveBattlefield(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->leaveBattlefield();
    return 0;
}

// ---- World map player arrow and ping ----
//
// In the real client these manage a rotating 3D arrow model showing where the
// player is standing and which way they face, plus the ping that plays when a
// party member signals a spot on the map.
//
// This client already draws its own player marker, party dots and quest POIs
// over the map, so the arrow would be a second marker on top of the first.
// They are defined anyway because two of them are called from
// WorldMapFrame_OnLoad: without them that function raised on its fifth line,
// so the black separator never got its colour, the ping never initialised, and
// the WorldMapFrame_Update() call that ends OnLoad never ran at all.
//
// UpdateWorldMapArrowFrames is not among them: it was already registered
// elsewhere in this file, which is why it never appeared as missing.
static int lua_CreateWorldMapArrowFrame(lua_State* L) { (void)L; return 0; }
static int lua_ShowWorldMapArrowFrame(lua_State* L) { (void)L; return 0; }
static int lua_PositionWorldMapArrowFrame(lua_State* L) { (void)L; return 0; }
static int lua_InitWorldMapPing(lua_State* L) { (void)L; return 0; }

// The battlefield minimap's own copy of the arrow, for the same reason and
// with the same answer as the world map's above.
static int lua_CreateMiniWorldMapArrowFrame(lua_State* L) { (void)L; return 0; }
static int lua_PositionMiniWorldMapArrowFrame(lua_State* L) { (void)L; return 0; }
static int lua_ShowMiniWorldMapArrowFrame(lua_State* L) { (void)L; return 0; }

// ---- The stored combat log (UnitCombatLog_C.cpp) ----
//
// The entries the client keeps for combatLogRetentionTime, walked by the
// interface when it refilters: Blizzard_CombatLog_RefilterUpdate starts at
// the newest (CombatLogSetCurrentEntry(0)) and steps back through it with
// CombatLogAdvanceEntry(-1) while that answers 1.

static wowee::game::combat_log::Log* combatLogOf(lua_State* L) {
    auto* gh = getGameHandler(L);
    return gh ? &gh->combatLogFeed().log() : nullptr;
}

/// 1 when there is a current entry, nil when there is none.
static int pushHasCurrent(lua_State* L, bool has) {
    if (!has) return luaReturnNil(L);
    lua_pushnumber(L, 1);
    return 1;
}

/// CombatLogGetNumEntries([ignoreFilter]) (0x0074fa70).
static int lua_CombatLogGetNumEntries(lua_State* L) {
    auto* log = combatLogOf(L);
    lua_pushnumber(L, log ? log->numEntries(lua_toboolean(L, 1) != 0) : 0);
    return 1;
}

/// CombatLogGetCurrentEntry() (0x0074f2b0): the entry's arguments, as
/// COMBAT_LOG_EVENT has them, or nothing at all.
static int lua_CombatLogGetCurrentEntry(lua_State* L) {
    auto* gh = getGameHandler(L);
    const auto* entry = gh ? gh->combatLogFeed().log().current() : nullptr;
    if (!entry) return 0;
    const auto args = gh->combatLogFeed().argsOf(*entry);
    luaL_checkstack(L, static_cast<int>(args.size()), "CombatLogGetCurrentEntry");
    for (const auto& arg : args) {
        using Kind = wowee::game::combat_log::Arg::Kind;
        if (arg.kind == Kind::Number) lua_pushnumber(L, arg.number);
        else if (arg.kind == Kind::String) lua_pushlstring(L, arg.text.data(), arg.text.size());
        else lua_pushnil(L);
    }
    return static_cast<int>(args.size());
}

/// CombatLogSetCurrentEntry(index [,ignoreFilter]) (0x0074fae0): the
/// index-th from the oldest, or for 0 and below from the newest back.
static int lua_CombatLogSetCurrentEntry(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: CombatLogSetCurrentEntry(index [,ignoreFilter])");
    auto* log = combatLogOf(L);
    return pushHasCurrent(L, log && log->setCurrent(static_cast<int>(lua_tonumber(L, 1)), lua_toboolean(L, 2) != 0));
}

/// CombatLogAdvanceEntry(count [,ignoreFilter]) (0x0074fc20): on toward the
/// newest, or back for a negative count; off either end is no entry.
static int lua_CombatLogAdvanceEntry(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: CombatLogAdvanceEntry(count [,ignoreFilter])");
    auto* log = combatLogOf(L);
    return pushHasCurrent(L, log && log->advance(static_cast<int>(lua_tonumber(L, 1)), lua_toboolean(L, 2) != 0));
}

/// CombatLogAddFilter(events, srcMaskOrGuid, dstMaskOrGuid [,spell])
/// (0x0074ff70): events a list of names (nil is all); each side a mask,
/// which has to say something of every category, or a guid; the spell an id
/// or a name.
static int lua_CombatLogAddFilter(lua_State* L) {
    namespace cl = wowee::game::combat_log;
    cl::Filter filter;
    const char* events = lua_tostring(L, 1);
    filter.setEvents(events ? std::optional<std::string>(events) : std::nullopt);
    const auto side = [L](int idx, uint64_t& guid, uint32_t& mask, const char* which) {
        if (lua_type(L, idx) == LUA_TNUMBER) {
            mask = static_cast<uint32_t>(std::llround(lua_tonumber(L, idx)));
            if (!cl::maskComplete(mask)) {
                luaL_error(L, "CombatLogAddFilter(): incomplete filter for %s", which);
                return false;
            }
        } else if (lua_type(L, idx) == LUA_TSTRING) {
            guid = cl::parseGuid(lua_tostring(L, idx));
            mask = 0;
        } else {
            mask = 0xFFFFFFFFu;
        }
        return true;
    };
    if (!side(2, filter.srcGuid, filter.srcMask, "srcMask")) return 0;
    if (!side(3, filter.dstGuid, filter.dstMask, "dstMask")) return 0;
    if (lua_type(L, 4) == LUA_TNUMBER) filter.spellId = static_cast<uint32_t>(std::llround(lua_tonumber(L, 4)));
    else if (lua_type(L, 4) == LUA_TSTRING) filter.spellName = lua_tostring(L, 4);
    if (auto* log = combatLogOf(L)) log->addFilter(std::move(filter));
    return 0;
}

/// CombatLogResetFilter() (0x0074fa60).
static int lua_CombatLogResetFilter(lua_State* L) {
    if (auto* log = combatLogOf(L)) log->resetFilters();
    return 0;
}

/// CombatLogClearEntries() (0x00751120).
static int lua_CombatLogClearEntries(lua_State* L) {
    if (auto* log = combatLogOf(L)) log->clearEntries();
    return 0;
}

// CombatTextSetActiveUnit(unit) - which unit the floating combat text follows.
// It tells the client where to aim the events it already sends; the events do
// not change, so this is recorded by the caller and nothing is needed here.
static int lua_CombatTextSetActiveUnit(lua_State* L) { (void)L; return 0; }

// GetBattlefieldMapIconScale() → what to multiply the map's icon sizes by.
// One is the client's own default; the icons are sized in the frame itself,
// and every use here is a multiply, so a nil would take the arithmetic down.
static int lua_GetBattlefieldMapIconScale(lua_State* L) { lua_pushnumber(L, 1.0); return 1; }

// PlayerIsPVPInactive(unit) → whether a battleground member has gone idle and
// is about to be removed. The server reports this per-player in a battleground
// and nothing here parses it, so nobody reads as idle.
static int lua_PlayerIsPVPInactive(lua_State* L) { lua_pushboolean(L, 0); return 1; }

// ---- The battlemaster's battleground list ----
//
// GetBattlefieldInfo() → mapName, mapDescription, maxGroup, canEnter,
//                        isHoliday, isRandom
//
// About the battleground the battlemaster being spoken to offers, which is
// what the last SMSG_BATTLEFIELD_LIST described. The frame returns early on a
// nil name, so before this the list drew nothing at all.
//
// The description is nil: BattlemasterList.dbc carries no blurb, and the panel
// treats a missing one as "no description" rather than raising on it.
static int lua_GetBattlefieldInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return luaReturnNil(L);
    const auto& bgs = gh->getAvailableBgs();
    if (bgs.empty()) return luaReturnNil(L);
    const auto* info = gh->getBattlemasterInfo(bgs.back().bgTypeId);
    if (!info || info->name.empty()) return luaReturnNil(L);
    const auto& bg = bgs.back();
    lua_pushstring(L, info->name.c_str());   // 1: mapName
    lua_pushnil(L);                          // 2: mapDescription
    lua_pushnumber(L, info->maxGroupSize);   // 3: maxGroup
    // Three more, all of which the panel reads. Whether the player is inside
    // the level bracket the server sent, whether this is the week's call to
    // arms, and whether it is the random battleground rather than a named one
    // - type 32, which is what BattlefieldFrame keys the rewards block on.
    const uint32_t level = gh->getPlayerLevel();
    const bool inBracket = (bg.minLevel == 0 || level >= bg.minLevel) &&
                           (bg.maxLevel == 0 || level <= bg.maxLevel);
    constexpr uint32_t kRandomBattleground = 32;
    lua_pushboolean(L, inBracket ? 1 : 0);                        // 4: canEnter
    lua_pushboolean(L, bg.isHoliday ? 1 : 0);                     // 5: isHoliday
    lua_pushboolean(L, bg.bgTypeId == kRandomBattleground ? 1 : 0); // 6: isRandom
    return 6;
}

// GetNumBattlegroundTypes() → how many battlegrounds there are to queue for.
//
// This is the PvP frame's own list, not the battlemaster's offering, and it
// comes from BattlemasterList.dbc - which this client already loads. It was
// answering zero from the counting stub, so the list drew no rows, nothing was
// ever assigned frame.BGindex, and the click handler read that field as nil.
//
// Arenas are excluded: they share the table with battlegrounds and are told
// apart by the instance type, so offering them here would queue the player for
// the wrong thing.
static int lua_GetNumBattlegroundTypes(lua_State* L) {
    auto* gh = getGameHandler(L);
    lua_pushnumber(L, gh ? static_cast<double>(gh->getBattlegroundTypes().size()) : 0.0);
    return 1;
}

// GetBattlegroundInfo(index) → localizedName, canEnter, isHoliday, isRandom, id
//
// canEnter is the level range from the row, which is what the client itself
// knows; the server still has the final say when the queue request arrives.
// isHoliday would need the call-to-arms world state, which nothing here parses,
// and claiming a holiday that is not running is worse than not mentioning one.
static int lua_GetBattlegroundInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_checknumber(L, 1));
    if (!gh) return luaReturnNil(L);
    const auto& list = gh->getBattlegroundTypes();
    if (index < 1 || index > static_cast<int>(list.size())) return luaReturnNil(L);
    const auto& bg = list[static_cast<size_t>(index - 1)];

    // A row naming no level range - the random battleground is the only one -
    // is not a row saying nobody qualifies.
    const uint32_t level = gh->getPlayerLevel();
    const bool hasRange = bg.minLevel != 0 || bg.maxLevel != 0;
    const bool canEnter = !hasRange || (level >= bg.minLevel && level <= bg.maxLevel);

    lua_pushstring(L, bg.name.c_str());
    lua_pushboolean(L, canEnter ? 1 : 0);
    lua_pushboolean(L, 0);                       // isHoliday: world state not parsed
    lua_pushboolean(L, bg.mapCount > 1 ? 1 : 0); // several maps means a pool, i.e. random
    lua_pushnumber(L, bg.id);
    return 5;
}

/// The battleground the interface has selected, as a type id.
///
/// The panel knows its selection as a row number and tells this client about it
/// in one place only - RequestBattlegroundInstanceInfo, which it calls whenever
/// the selection moves and again when it opens - so that call is where the
/// choice is remembered.
///
/// JoinBattlefield used to queue for the last entry of the list of every
/// battleground instance info had ever been asked for. That list is keyed by
/// type and updated in place, so its last entry is whichever battleground was
/// looked at *first*: clicking Warsong Gulch, then Arathi Basin, then back to
/// Warsong Gulch and pressing Join queued for Arathi Basin.
static uint32_t& selectedBattlegroundTypeId() { static uint32_t typeId = 0; return typeId; }

// RequestBattlegroundInstanceInfo(index) - ask which instances are running.
//
// The reply is SMSG_BATTLEFIELD_LIST, which this client already handled and
// had no way to ask for. The index is a row in the list above, not a
// battleground id, so it is translated before it goes on the wire.
static int lua_RequestBattlegroundInstanceInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh) return 0;
    const auto& list = gh->getBattlegroundTypes();
    if (index < 1 || index > static_cast<int>(list.size())) return 0;
    selectedBattlegroundTypeId() = list[static_cast<size_t>(index - 1)].id;
    gh->requestBattlefieldList(selectedBattlegroundTypeId());
    return 0;
}

// Which instance of a battleground is picked in the list.
//
// Kept here because the pair has to round-trip: the frame highlights the row
// matching what it last set, so a getter that always answered zero left the
// first row highlighted whatever was clicked.
//
// Zero remains the default and still means "first available", which is what
// the server understands as no preference - the list opens on it, and nothing
// here queues for a specific instance regardless. This is the selection the
// interface is showing, not an instruction to the server.
static int& selectedBattlefield() { static int selected = 0; return selected; }

static int lua_GetSelectedBattlefield(lua_State* L) {
    lua_pushnumber(L, selectedBattlefield());
    return 1;
}

// SetSelectedBattlefield(index) - called unguarded from two places, so it has
// to exist before it has to do anything.
static int lua_SetSelectedBattlefield(lua_State* L) {
    selectedBattlefield() = static_cast<int>(luaL_optnumber(L, 1, 0));
    return 0;
}

// JoinBattlefield(index, asGroup, isArena) - queue for it.
//
// The index names an instance of the selected battleground, and zero means the
// first available one - which is what both join buttons pass.
static int lua_JoinBattlefield(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    const bool asGroup = lua_toboolean(L, 2) != 0;

    const auto& bgs = gh->getAvailableBgs();
    uint32_t bgTypeId = selectedBattlegroundTypeId();
    // Nothing selected can still happen - a queue asked for before the panel
    // has told anyone what it is showing - and one battleground on offer is
    // then not a guess.
    if (bgTypeId == 0 && bgs.size() == 1) bgTypeId = bgs.front().bgTypeId;
    if (bgTypeId == 0) {
        LOG_WARNING("JoinBattlefield: no battleground is selected, so there is "
                    "nothing to queue for - the panel names its choice through "
                    "RequestBattlegroundInstanceInfo and has not");
        return 0;
    }

    uint32_t instanceId = 0;
    for (const auto& bg : bgs) {
        if (bg.bgTypeId != bgTypeId) continue;
        if (index > 0 && index <= static_cast<int>(bg.instanceIds.size())) {
            instanceId = bg.instanceIds[static_cast<size_t>(index) - 1];
        }
        break;
    }

    // The battlemaster, when there is one to name. Queueing from the panel is
    // done standing anywhere, and the guid then belongs to whoever was last
    // spoken to - a vendor, a flight master - which is not a battlemaster and
    // is not what the real client sends. Nought is.
    const uint64_t battlemaster =
        gh->isGossipWindowOpen() ? gh->getCurrentGossip().npcGuid : 0;
    LOG_WARNING("CMSG_BATTLEMASTER_JOIN: bgTypeId=", bgTypeId, " instance=", instanceId,
             " asGroup=", asGroup, " battlemaster=0x", std::hex, battlemaster, std::dec);
    gh->joinBattlefield(battlemaster, bgTypeId, instanceId, asGroup);
    return 0;
}

// GetLFGCompletionReward() →
//   name, typeID, textureFilename, moneyBase, moneyVar, experienceBase,
//   experienceVar, numStrangers, numRewards
//
// What a finished dungeon finder run paid out, from SMSG_LFG_PLAYER_REWARD.
//
// The six counts answer zero rather than nil even with no reward to report,
// which is the opposite of the usual choice and for the opposite reason: the
// alert frame does not test them, it does arithmetic with them -
//
//     local moneyAmount = moneyBase + moneyVar * numStrangers
//
// so a nil raises there rather than reading as absent.
//
// The variable halves are always zero. They are the per-stranger bonus for
// filling a group with people from other realms, which 3.3.5a's server does not
// send and this realm could not have anyway; numStrangers is zero to match, so
// the arithmetic above lands on the base whichever way the frame reads it.
static int lua_GetLFGCompletionReward(lua_State* L) {
    auto* gh = getGameHandler(L);
    const game::LfgCompletionReward* r = gh ? &gh->getLfgCompletionReward() : nullptr;
    if (!r || !r->valid) {
        lua_pushnil(L);      // 1: name
        lua_pushnil(L);      // 2: typeID
        lua_pushnil(L);      // 3: textureFilename
        for (int i = 0; i < 6; ++i) lua_pushnumber(L, 0);
        return 9;
    }
    // The dungeon finished, not the random entry queued for: the toast names
    // the instance the player actually walked out of.
    const game::LfgDungeon* d = nullptr;
    for (const auto& e : gh->getLfgDungeons()) {
        if (e.id == r->dungeonId) { d = &e; break; }
    }
    // Pushed once each and in order, rather than an if/else per slot: two
    // pushes on one line read as two return values to anything counting them,
    // this file's own return-order sweep included.
    const std::string name = (d && !d->name.empty()) ? d->name : std::string();
    // Appended to "Interface\\LFGFrame\\LFGIcon-" by the frame, so it is the
    // bare suffix out of LFGDungeons.dbc and not a path.
    const std::string texture = (d && !d->texture.empty()) ? d->texture : std::string();
    name.empty() ? lua_pushnil(L) : lua_pushstring(L, name.c_str());          // 1
    d ? lua_pushinteger(L, static_cast<lua_Integer>(d->typeId)) : lua_pushnil(L);  // 2
    texture.empty() ? lua_pushnil(L) : lua_pushstring(L, texture.c_str());    // 3
    lua_pushnumber(L, static_cast<double>(r->money));  // 4: moneyBase
    lua_pushnumber(L, 0);                              // 5: moneyVar
    lua_pushnumber(L, static_cast<double>(r->xp));     // 6: experienceBase
    lua_pushnumber(L, 0);                              // 7: experienceVar
    lua_pushnumber(L, 0);                              // 8: numStrangers
    lua_pushnumber(L, static_cast<double>(r->items.size()));  // 9: numRewards
    return 9;
}

// GetLFGCompletionRewardItem(index) → texturePath, quantity
//
// One-based, and the index runs over the rewards the call above counted. The
// texture is the item's own icon, which is where the display id in the packet
// earns its place - the reward can name an item the bags have never held and
// so have no cached query behind it.
static int lua_GetLFGCompletionRewardItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    const game::LfgCompletionReward* r = gh ? &gh->getLfgCompletionReward() : nullptr;
    if (!r || !r->valid || index < 1 ||
        index > static_cast<int>(r->items.size())) {
        lua_pushnil(L);
        lua_pushnumber(L, 0);
        return 2;
    }
    const auto& item = r->items[static_cast<size_t>(index) - 1];
    const std::string icon = gh->getItemIconPath(item.displayId);
    if (!icon.empty()) lua_pushstring(L, icon.c_str()); else lua_pushnil(L);
    lua_pushnumber(L, static_cast<double>(item.count));
    return 2;
}

// RunMacroText(body) - run a macro body, one command per line.
//
// The same path the action bar takes for a macro button, so a macro run from a
// party frame's click behaves as one run from the bar: /stopmacro is honoured,
// and each line goes through the slash dispatch rather than being sent as chat.
static int lua_RunMacroText(lua_State* L) {
    auto* svc = getLuaServices(L);
    const char* body = luaL_optstring(L, 1, "");
    // 0x00566400 -> 0x00564db0, which asks the taint check (kind 3): a
    // macro is never run by insecure code.
    if (!taint::allowed(L, 3)) return 0;
    if (svc && svc->runMacroText && body && *body) {
        // A macro of its own, no saved one, with the button it was given.
        RunningMacroScope running(0, lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr);
        svc->runMacroText(body);
    }
    return 0;
}

// RunMacro(id or name) - run a saved macro by which one it is.
static int lua_RunMacro(lua_State* L) {
    auto* svc = getLuaServices(L);
    auto* gh = getGameHandler(L);
    if (!svc || !svc->runMacroText || !gh) return 0;
    if (!taint::allowed(L, 3)) return 0;   // 0x00566e80 -> 0x00564db0
    uint32_t macroId = 0;
    if (lua_isnumber(L, 1)) {
        macroId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (const char* name = lua_tostring(L, 1)) {
        // By name, which is how a macro written into another macro names it.
        for (uint32_t id : gh->getMacroIds()) {
            if (gh->getMacroName(id) == name) { macroId = id; break; }
        }
    }
    if (macroId == 0) return 0;
    const std::string body = gh->getMacroText(macroId);
    if (!body.empty()) {
        // 0x00566e80 hands 0x00564db0 the second argument as the button the
        // macro runs with, for GetRunningMacroButton.
        RunningMacroScope running(static_cast<int>(macroId),
                                  lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr);
        svc->runMacroText(body);
    }
    return 0;
}

// GetRunningMacro() → index of the saved macro running now, or nil
// (0x00564c10). Nil inside RunMacroText, whose macro is no saved one.
static int lua_GetRunningMacro(lua_State* L) {
    const RunningMacro& m = runningMacro();
    if (m.running && m.index > 0) lua_pushnumber(L, m.index);
    else lua_pushnil(L);
    return 1;
}

// GetRunningMacroButton() → the button the running macro, or the CLICK binding
// being pressed, was run with (0x00564c70 answers 0x00beaf48), or nil.
static int lua_GetRunningMacroButton(lua_State* L) {
    const RunningMacro& m = runningMacro();
    if (m.hasButton) lua_pushstring(L, m.button.c_str());
    else lua_pushnil(L);
    return 1;
}

// TriggerTutorial(id) - show one of the interface's tutorial pop-outs.
//
// Tutorials are a saved per-account set of which have been seen, and none of
// that is kept here, so nothing is shown. Answered rather than left missing
// because the bag bar fires it whenever a bag is picked up.
static int lua_TriggerTutorial(lua_State* L) { (void)L; return 0; }

// Quit() - leave the game, as the game menu's Exit button does.
//
// The same path /exit takes: a clean logout that ends the process rather than
// dropping to character select.
static int lua_Quit(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) {
        gh->requestLogout(/*exitAfterLogout=*/true);
        return 0;
    }
    // No handler to route through - the login screen, or before one exists.
    // Exit Game still has to exit; returning here left the button inert.
    if (auto* svc = getLuaServices(L); svc && svc->quitApplication) {
        svc->quitApplication();
    }
    return 0;
}

/// ForceQuit() / ForceLogout() - leave now rather than after the timer.
///
/// QUIT's OnAccept calls the first, and the popup exists to offer exactly this:
/// the server has imposed a countdown and the player would rather not wait.
/// There is no message that shortens it - the server owns the timer - so what
/// this can honestly do is ask again, which is what the real client's button
/// amounts to once the timer is already running.
///
/// Bound together because CAMP carries the same pair and it would be strange
/// for one to answer and not the other, even though Blizzard has that call
/// commented out with a note saying forced logout is unfinished.
static int lua_ForceQuit(lua_State* L) {
    // The same thing Quit does, and deliberately the same code: asking again
    // is what the popup's button amounts to once the server owns the timer.
    return lua_Quit(L);
}

static int lua_ForceLogout(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->requestLogout(/*exitAfterLogout=*/false);
    return 0;
}

// ReloadUI() - rebuild the interface, as /reload does.
//
// Only asks. The reload shuts this Lua state down and builds a new one, and
// every caller is inside it: a static popup's OnAccept after a setting that
// needs one, or /reload typed at the interface rather than at the client's own
// command handler. Doing the work here would free the state mid-call.
static int lua_ReloadUI(lua_State* L) {
    // 0x005222e0: insecure code reloads only inside a hardware event.
    if (!taint::allowedInHardwareEvent(L)) return 0;
    auto* svc = getLuaServices(L);
    if (svc && svc->requestReloadUI) svc->requestReloadUI();
    return 0;
}

// WoweeSettingList() - every client setting FrameXML has no control for.
//
// Answers an array of { key, label, kind, min, max, step, category }, from the
// one schema in ui/settings_schema.hpp. The Wowee options category is built
// from this rather than written out in Lua, so adding a setting is a row in the
// schema and nothing else.
// A string global's text, or "" where it is not a string: the interface's
// GlobalStrings, for the settings the original client has an option for.
static std::string globalStringText(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    std::string text = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return text;
}

static int lua_WoweeSettingList(lua_State* L) {
    std::size_t count = 0;
    const auto* schema = ui::clientSettingsSchema(count);
    auto* svc = getLuaServices(L);
    // The words in the client's language: German for a German client, with
    // the interface's own GlobalStrings text for the options the original
    // client has; the schema's English otherwise.
    const auto* profile = svc && svc->expansionRegistry ? svc->expansionRegistry->getActive() : nullptr;
    const bool german = profile && ui::settingsLocaleGerman(profile->locale);
    const ui::GlobalStringLookup globals = [L](const char* name) { return globalStringText(L, name); };
    lua_newtable(L);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& d = schema[i];
        const ui::SettingText text = ui::settingText(d, german, globals);
        lua_newtable(L);
        lua_pushstring(L, d.key);      lua_setfield(L, -2, "key");
        lua_pushstring(L, text.label.c_str()); lua_setfield(L, -2, "label");
        lua_pushstring(L, d.kind == ui::SettingKind::Bool  ? "bool"
                        : d.kind == ui::SettingKind::Int   ? "int"
                        : d.kind == ui::SettingKind::Enum  ? "enum"
                                                           : "float");
        lua_setfield(L, -2, "kind");
        lua_pushnumber(L, d.minValue); lua_setfield(L, -2, "min");
        lua_pushnumber(L, d.maxValue); lua_setfield(L, -2, "max");
        lua_pushnumber(L, d.step);     lua_setfield(L, -2, "step");
        // category and section stay the schema's: they name the panels'
        // frames and group the rows. What a person reads is beside them.
        lua_pushstring(L, d.category); lua_setfield(L, -2, "category");
        lua_pushstring(L, d.section);  lua_setfield(L, -2, "section");
        lua_pushstring(L, text.category.c_str()); lua_setfield(L, -2, "categorylabel");
        lua_pushstring(L, text.section.c_str());  lua_setfield(L, -2, "sectionlabel");
        lua_pushstring(L, text.tooltip.c_str());  lua_setfield(L, -2, "tooltip");
        lua_pushstring(L, text.choices.c_str());  lua_setfield(L, -2, "choices");
        lua_pushnumber(L, d.defaultValue); lua_setfield(L, -2, "default");
        lua_pushstring(L, d.enabledWhen);  lua_setfield(L, -2, "enabledwhen");
        lua_pushstring(L, d.store);        lua_setfield(L, -2, "store");
        // What the ray traced lighting makes of it while it is on, where it
        // makes it moot (rayTracingIgnoredReason); absent otherwise.
        if (const char* moot = ui::rayTracingIgnoredText(d.key, german)) {
            lua_pushstring(L, moot);
            lua_setfield(L, -2, "rtignored");
        }
        // Why this device cannot use it, where it cannot; absent otherwise.
        if (svc && svc->settingUnavailable) {
            const std::string why = svc->settingUnavailable(d.key);
            if (!why.empty()) {
                lua_pushstring(L, why.c_str());
                lua_setfield(L, -2, "unavailable");
            }
        }
        lua_rawseti(L, -2, static_cast<int>(i) + 1);
    }
    return 1;
}

// WoweeVersion() - what this client calls itself, and when it was built.
//
// The same string the login screen shows. Here so the options panel can say it
// without the version being written out a second time in Lua, where it would go
// stale the first time a tag was cut.
static int lua_WoweeVersion(lua_State* L) {
    lua_pushstring(L, core::kVersionString);
    return 1;
}

// WoweeOpenURL(url) -> true when the browser was asked.
//
// The same opener a chat link uses, checks and all: http(s) only, safe ASCII
// only, and no shell anywhere in it. The About box is the caller this was
// added for, and its address is checked like any other.
static int lua_WoweeOpenURL(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    lua_pushboolean(L, core::openExternalUrl(url ? url : "") ? 1 : 0);
    return 1;
}

// The schema row behind a key, or nullptr.
static const ui::SettingDesc* schemaRow(const std::string& key) {
    std::size_t count = 0;
    const auto* schema = ui::clientSettingsSchema(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (key == schema[i].key) return &schema[i];
    }
    return nullptr;
}

static std::vector<std::string> splitBar(const char* text) {
    std::vector<std::string> out;
    std::string all = text ? text : "";
    for (std::size_t at = 0; ; ) {
        const std::size_t bar = all.find('|', at);
        out.push_back(all.substr(at, bar == std::string::npos ? bar : bar - at));
        if (bar == std::string::npos) break;
        at = bar + 1;
    }
    return out;
}

// One Lua global called with the arguments already pushed, its one result
// left as a string. Empty when the global is not a function or raised.
static std::string callGlobal(lua_State* L, const char* name, int nargs) {
    // The function has to sit under its arguments.
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1 + nargs);
        return {};
    }
    lua_insert(L, -(nargs + 1));
    std::string out;
    if (lua_pcall(L, nargs, 1, 0) == 0) {
        if (lua_isboolean(L, -1)) out = lua_toboolean(L, -1) ? "1" : "0";
        else if (const char* text = lua_tostring(L, -1)) out = text;
    } else {
        LOG_WARNING("Setting store: ", name, " raised: ",
                    lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
    }
    lua_pop(L, 1);
    return out;
}

// A row on a store - see SettingDesc::store - read in the units its control
// shows: the index of its choice for an enum with `values`, the raw text
// otherwise. What the game's own control read.
static std::string readStoredSetting(lua_State* L, const ui::SettingDesc& row) {
    const std::string store = row.store;
    std::string raw;
    if (store.rfind("cvar:", 0) == 0) {
        lua_pushstring(L, store.c_str() + 5);
        raw = callGlobal(L, "GetCVar", 1);
    } else if (store.rfind("click:", 0) == 0) {
        lua_pushstring(L, store.c_str() + 6);
        raw = callGlobal(L, "GetModifiedClick", 1);
    } else if (store.rfind("lua:", 0) == 0) {
        const std::string spec = store.substr(4);
        raw = callGlobal(L, spec.substr(0, spec.find('|')).c_str(), 0);
    }
    if (row.values[0] == '\0') return raw;
    const auto values = splitBar(row.values);
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (values[i] == raw) return std::to_string(i);
    }
    return ui::settingNumberText(row.defaultValue);
}

// And written, the way the control wrote it: the CVar, then the global the
// interface reads and the refresh that shows it, from `after` with the new
// value in `v`.
static void writeStoredSetting(lua_State* L, const ui::SettingDesc& row, const std::string& value) {
    std::string stored = value;
    if (row.values[0] != '\0') {
        const auto values = splitBar(row.values);
        const int last = static_cast<int>(values.size()) - 1;
        const int index = std::clamp(static_cast<int>(std::atof(value.c_str()) + 0.5), 0, last);
        stored = values[static_cast<std::size_t>(index)];
    }
    const std::string store = row.store;
    if (store.rfind("cvar:", 0) == 0) {
        lua_pushstring(L, store.c_str() + 5);
        lua_pushstring(L, stored.c_str());
        // The third argument is what the change is announced as, and without
        // one the write is silent: the binary fires CVAR_UPDATE only for a
        // SetCVar that was given that argument. A panel of the interface's
        // chooses it from its own settings table; this window is not one of
        // those panels, so it chooses it here, from the pairs the interface
        // declares (cvarLabelFor). A row whose CVar nobody announces is a row
        // whose change only the `after` chunk below can make visible.
        lua_pushstring(L, cvarLabelFor(store.c_str() + 5).c_str());
        callGlobal(L, "SetCVar", 3);
    } else if (store.rfind("click:", 0) == 0) {
        lua_pushstring(L, store.c_str() + 6);
        lua_pushstring(L, stored.c_str());
        callGlobal(L, "SetModifiedClick", 2);
    } else if (store.rfind("lua:", 0) == 0) {
        const std::string spec = store.substr(4);
        const std::size_t bar = spec.find('|');
        if (bar != std::string::npos) {
            lua_pushboolean(L, stored != "0" ? 1 : 0);
            callGlobal(L, spec.substr(bar + 1).c_str(), 1);
        }
    }
    if (row.after[0] == '\0') return;
    std::string quoted = "\"";
    for (char c : stored) {
        if (c == '\\' || c == '"') quoted += '\\';
        quoted += c;
    }
    quoted += '"';
    const std::string chunk = "local v = " + quoted + "\n" + row.after;
    if (luaL_loadstring(L, chunk.c_str()) != 0 || lua_pcall(L, 0, 0, 0) != 0) {
        LOG_WARNING("Setting '", row.key, "': after-write Lua failed: ",
                    lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_pop(L, 1);
    }
}

// WoweeGetSetting(key) / WoweeSetSetting(key, value) - the values behind that
// list. Strings both ways, as a CVar is. A row on a store is answered from
// the store; the rest from the client's own fields.
static int lua_WoweeGetSetting(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    if (const auto* row = schemaRow(key); row && row->store[0] != '\0') {
        lua_pushstring(L, readStoredSetting(L, *row).c_str());
        return 1;
    }
    auto* svc = getLuaServices(L);
    if (svc && svc->getClientSetting) {
        lua_pushstring(L, svc->getClientSetting(key).c_str());
        return 1;
    }
    return luaReturnNil(L);
}

static int lua_WoweeSetSetting(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    std::string value;
    if (lua_isboolean(L, 2)) value = lua_toboolean(L, 2) ? "1" : "0";
    else if (lua_isstring(L, 2) || lua_isnumber(L, 2)) value = lua_tostring(L, 2);
    else value = "0";
    if (const auto* row = schemaRow(key); row && row->store[0] != '\0') {
        writeStoredSetting(L, *row, value);
        return 0;
    }
    if (auto* svc = getLuaServices(L); svc && svc->setClientSetting) {
        svc->setClientSetting(key, value);
    }
    return 0;
}

// WoweeSaveVariables() - write the loaded addons' SavedVariables out now.
//
// WoW writes them at logout and so does this client, which is enough for a
// client that is only ever exited tidily. This one is killed, rebuilt under
// itself and crashed in a renderer under development, and each of those loses
// whatever the player set that session - a bag window dragged somewhere is back
// where it was on the next start, which reads as "it does not remember".
//
// So an addon with something worth keeping says so, and it is on disk before
// the next thing happens. The CVar file beside it is written the same way and
// for the same reason.
static int lua_WoweeSaveVariables(lua_State* L) {
    if (auto* svc = getLuaServices(L); svc && svc->saveAddOnVariables) {
        svc->saveAddOnVariables();
    }
    return 0;
}

// WoweeShowSettings(tab) - open this client's settings window.
//
// FrameXML's game menu has Video, Sound and Interface buttons, and the frames
// they open are shells in this shim: InterfaceOptionsFrame exists so addons can
// register panels against it, and nothing draws it. The settings themselves -
// sixty-odd of them - are in this client's own window, so the menu's buttons
// come here instead of opening an empty frame.
//
// The tab name is optional and matches the window's own tab labels.
static int lua_WoweeShowSettings(lua_State* L) {
    auto* svc = getLuaServices(L);
    const char* tab = lua_isstring(L, 1) ? lua_tostring(L, 1) : nullptr;
    if (svc && svc->openSettings) svc->openSettings(tab ? tab : "");
    return 0;
}

// GetGamma() / SetGamma(value) - screen brightness, as the video options mean
// it. One is neutral. Backed by the client's own brightness setting, so the
// two sliders move together instead of disagreeing.
static int lua_GetGamma(lua_State* L) {
    auto* svc = getLuaServices(L);
    lua_pushnumber(L, (svc && svc->getGamma) ? svc->getGamma() : 1.0);
    return 1;
}

static int lua_SetGamma(lua_State* L) {
    auto* svc = getLuaServices(L);
    if (svc && svc->setGamma) svc->setGamma(static_cast<float>(luaL_optnumber(L, 1, 1.0)));
    return 0;
}

// GetVideoCaps() →
//   anisotropic, pixelShaders, vertexShaders, trilinear, buffering,
//   maxAnisotropy, hardwareCursor
//
// Everything here runs on Vulkan, so the capability questions all answer yes -
// they were written for a Direct3D 9 client that could genuinely lack them.
// maxAnisotropy is the one real number, and sixteen is the ceiling every
// device this client will start on supports; the options panel uses it only to
// bound its own dropdown.
/// Terrain texture detail, as the video panel's TerrainDetail slider sets it.
///
/// Both halves were missing, and the slider reads its own value on every
/// refresh of the panel - `self.GetCurrentValue = function (self) return
/// GetTerrainMip(); end` - so opening Video Options called nil, and moving the
/// slider called nil again.
///
/// Held rather than applied: nothing in the terrain renderer takes a mip bias
/// yet. The slider works and remembers where it was put, which is the whole of
/// what the panel can observe, and a value is here for the renderer to read
/// when it grows one.
static int& terrainMip() { static int level = 1; return level; }

static int lua_GetTerrainMip(lua_State* L) {
    lua_pushnumber(L, terrainMip());
    return 1;
}

static int lua_SetTerrainMip(lua_State* L) {
    terrainMip() = static_cast<int>(luaL_optnumber(L, 1, 1));
    return 0;
}

static int lua_GetVideoCaps(lua_State* L) {
    lua_pushboolean(L, 1);   // 1: anisotropic
    lua_pushboolean(L, 1);   // 2: pixelShaders
    lua_pushboolean(L, 1);   // 3: vertexShaders
    lua_pushboolean(L, 1);   // 4: trilinear
    lua_pushboolean(L, 1);   // 5: buffering
    lua_pushnumber(L, 16);   // 6: maxAnisotropy
    lua_pushboolean(L, 1);   // 7: hardwareCursor
    return 7;
}

// GetCVarMin(name) / GetCVarMax(name) → the range a CVar is allowed, if it
// declares one.
//
// Nil for all but the few listed here, and every caller is written for nil:
// BlizzardOptionsPanel_GetCVarMinSafe passes it through tonumber, the slider
// setup falls back with `or entry.minValue`, and the clamp reads
// `if ( minValue and value < minValue )`. Answering a made-up zero for the
// rest would clamp every graphics slider in the options panel to it.
//
// This is the seam Blizzard's own panel code offers for a range the client
// owns rather than the interface: BlizzardOptionsPanel_OnEvent takes
// GetCVarMax first and only falls back to its table. So widening a slider is
// a row here, not an edit to a shipped Lua file - which matters because the
// interface data is extracted game content and is not ours to keep changes in.
struct CVarRange {
    const char* cvar;
    float minValue;
    float maxValue;
};

constexpr CVarRange kCVarRanges[] = {
    // The interface's own scale. 1 is the size the screen's height alone gives,
    // and the shipped table stops there because the slider was only ever for
    // making the interface smaller. A screen across a room needs the other
    // direction. The floor is Blizzard's; it was the ceiling that was wrong.
    //
    // The ceiling is the tree's, not a number repeated here: past it the
    // interface's own frames no longer fit on screen, options frame included,
    // and a scale you cannot undo from inside the game is worse than one that
    // is merely too small.
    {"uiscale", 0.64f, ui::WidgetTree::kMaxUserScale},
    // View distance: the shipped slider's own 177 to 1277, which is also what
    // Renderer::setViewDistance holds the setting to.
    {"farclip", 177.0f, 1277.0f},
    // Texture filtering. The shipped slider counts Blizzard's six modes -
    // bilinear and trilinear before the anisotropic steps - and this client's
    // setting is the five its samplers have, off and then 2x to 16x, so a
    // control drawn to the shipped table hands over a 5 the setting holds to
    // 4. The page that slider was on is retired, but this is the range any
    // control over the CVar is drawn with.
    {"texturefilteringmode", 0.0f, 4.0f},
    // Mouse Look Speed, on the scale the CVar itself is measured in - the
    // shipped Mouse panel slider offers 90 to 270 and the client reads that as
    // degrees (0x006020b0 turns the view value * 0.00125 degrees per movement of
    // the mouse). The binding above converts it into this client's degrees per
    // movement, so the slider is drawn on its own numbers again instead of being
    // bent to the setting's scale, which is what let a 90 hand over a 90.
    {"camerayawmovespeed", 90.0f, 270.0f},
    // Mouse Sensitivity, the same setting in the units this client's own camera
    // counts it in. Shipped as 0.5 to 1.5, a multiplier around 1.0 for the
    // pointer, which this client does not keep: the slider moves the view, so it
    // is drawn on the view's range and its slowest setting was 2.5x the default.
    //
    // Those two numbers are Mouse Look Speed's 90 and 270 times the 0.00125
    // between the two scales. The two sliders write one value, so what they
    // reach has to be the same span - otherwise one can put the shared value
    // where the other has no handle for it, which is what this table's own
    // first finding was, seen from the other control.
    {"mousespeed", 0.1125f, 0.3375f},
    // Max camera distance, as a multiple of the original client's limit. The
    // shipped table stops at 2; this client has always been willing to go
    // further, and did it through a checkbox of its own until this slider was
    // wired to mean it.
    {"cameradistancemaxfactor", 1.0f, rendering::CameraController::kMaxDistanceFactorLimit},
};

const CVarRange* findCVarRange(lua_State* L) {
    const char* name = lua_tostring(L, 1);
    if (!name) return nullptr;
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& r : kCVarRanges) {
        if (lower == r.cvar) return &r;
    }
    return nullptr;
}

static int lua_GetCVarMin(lua_State* L) {
    if (const auto* r = findCVarRange(L)) {
        lua_pushstring(L, ui::settingNumberText(r->minValue).c_str());
        return 1;
    }
    return luaReturnNil(L);
}

static int lua_GetCVarMax(lua_State* L) {
    if (const auto* r = findCVarRange(L)) {
        lua_pushstring(L, ui::settingNumberText(r->maxValue).c_str());
        return 1;
    }
    return luaReturnNil(L);
}

// ---- Voice chat ----
//
// There is no voice chat in this client and no plan for one. These are answered
// rather than left missing because the panels that ask are otherwise perfectly
// usable: the audio options page reads the microphone level from an OnUpdate,
// so one absent global there raises every frame the page is open.
static int lua_IsVoiceChatAllowedByServer(lua_State* L) { lua_pushboolean(L, 0); return 1; }
static int lua_VoiceChat_IsRecordingLoopbackSound(lua_State* L) { lua_pushboolean(L, 0); return 1; }
static int lua_VoiceChat_IsPlayingLoopbackSound(lua_State* L) { lua_pushboolean(L, 0); return 1; }
static int lua_VoiceChat_GetCurrentMicrophoneSignalLevel(lua_State* L) { lua_pushnumber(L, 0); return 1; }

// GetVoiceSessionInfo(i) → name, active
static int lua_GetVoiceSessionInfo(lua_State* L) {
    lua_pushnil(L);
    lua_pushnil(L);
    return 2;
}

static int lua_GetNumVoiceSessionMembersBySessionID(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

// GetVoiceSessionMemberInfoBySessionID(session, member) (0x007deef0): a
// member of one of the voice sessions (0x00d31aa0), of which there are none
// without voice chat - nothing, as for a session that is not there.
static int lua_GetVoiceSessionMemberInfoBySessionID(lua_State* L) {
    (void)L;
    return 0;
}

// SetActiveVoiceChannel(displayIndex) (0x00500ae0). 0 leaves the active one -
// none here, so display 1 (0x00b743cc + 1) - and says so with
// CHANNEL_VOICE_UPDATE; another index makes that channel active only where it
// has voice on (+0x14 of its display row), which none has.
static int lua_SetActiveVoiceChannel(lua_State* L) {
    if (!lua_isnumber(L, 1)) return 0;
    if (static_cast<int>(lua_tonumber(L, 1)) != 0) return 0;
    if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("CHANNEL_VOICE_UPDATE", {"1"});
    return 0;
}

// ChannelVoiceOn("channel") / ChannelVoiceOff("channel") (0x004fea70,
// 0x004fea90 -> 0x004fe460): CMSG_CHANNEL_VOICE_ON or _OFF with the channel's
// name - a number names the channel joined at it, and none sends nothing.
template <bool On>
static int lua_ChannelVoice(lua_State* L) {
    if (!lua_isstring(L, 1))
        return luaL_error(L, On ? "Usage: ChannelVoiceOn(\"channel\")"
                                : "Usage: ChannelVoiceOff(\"channel\")");
    std::string name = lua_tostring(L, 1);
    auto* gh = getGameHandler(L);
    if (!gh || name.empty()) return 0;
    if (const int number = std::atoi(name.c_str()); number != 0) {
        const auto& joined = gh->getJoinedChannels();
        if (number < 0 || static_cast<size_t>(number) > joined.size()) return 0;
        name = joined[static_cast<size_t>(number) - 1];
    }
    gh->sendChannelVoice(name, On);
    return 0;
}

// DisplayChannelVoiceOn(displayIndex) / DisplayChannelVoiceOff(displayIndex)
// (0x004feab0, 0x004fead0 -> 0x004fe530): the same for the channel on that
// row of the channel list; a row that is not a channel sends nothing.
template <bool On>
static int lua_DisplayChannelVoice(lua_State* L) {
    if (!lua_isnumber(L, 1))
        return luaL_error(L, On ? "Usage: DisplayChannelVoiceOn(\"displayIndex\")"
                                : "Usage: DisplayChannelVoiceOff(\"displayIndex\")");
    const auto index = std::llround(lua_tonumber(L, 1));
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const auto& joined = gh->getJoinedChannels();
    if (index < 1 || static_cast<size_t>(index) > joined.size()) return 0;
    gh->sendChannelVoice(joined[static_cast<size_t>(index) - 1], On);
    return 0;
}

// VoiceChat_RecordLoopbackSound(seconds) / VoiceChat_PlayLoopbackSound()
// (0x00985d60, 0x00985de0): nothing without the voice sound system
// (0x00878cc0), which this client does not start.
static int lua_VoiceChat_RecordLoopbackSound(lua_State* L) {
    if (!lua_isnumber(L, 1))
        return luaL_error(L, "Usage: VoiceChat_RecordLoopbackSound(MaxRecordTimeInSeconds)");
    return 0;
}
static int lua_VoiceChat_PlayLoopbackSound(lua_State* L) {
    (void)L;
    return 0;
}

// RequestRaidInfo() - ask the server for saved instance lockouts. The reply is
// SMSG_RAID_INSTANCE_INFO, which the client already parses for
// GetSavedInstanceInfo; nothing was asking for it.
static int lua_RequestRaidInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->requestRaidInfo();
    return 0;
}

// CanShowAchievementUI() → whether the achievement panel may open
static int lua_CanShowAchievementUI(lua_State* L) {
    lua_pushboolean(L, 1);
    return 1;
}

// GetAddOnMemoryUsage(index) → kilobytes, and UpdateAddOnMemoryUsage() to
// refresh them
//
// Zero rather than nothing. The performance bar's tooltip adds these up -
// `totalMem = totalMem + mem` for every addon loaded - and nil there is an
// error, on an interface element that is drawn by default. Nothing here
// measures per-addon memory, and zero is what an unmeasured addon costs as far
// as this client knows.
static int lua_GetAddOnMemoryUsage(lua_State* L) {
    lua_pushnumber(L, 0);
    return 1;
}

// IsXPUserDisabled() → whether the player has turned experience off
//
// Nothing here can turn it off, so this is a definite no rather than an absent
// answer - the reputation panel reads it to decide whether to offer the bar.
static int lua_IsXPUserDisabled(lua_State* L) {
    lua_pushboolean(L, 0);
    return 1;
}

/// The taxi nodes the flight map is showing, in a fixed order.
///
/// Every taxi function is asked about a node by position in this list, and the
/// answers only line up if each of them walks the same order. They used to walk
/// `getTaxiNodes()` directly, which is an unordered_map: its order is arbitrary,
/// and rehashing it - which learning a flight path while the window is open
/// does - reorders it under a list already drawn. Clicking a destination would
/// then fly somewhere else. Sorting by node id costs a copy of a few hundred
/// integers and makes the order the same every time it is asked.
///
/// Only the nodes on the map the player is standing on are listed. That is what
/// the flight map shows, and it is what this client's own window already lists.
static std::vector<uint32_t> taxiNodeOrder(game::GameHandler* gh) {
    std::vector<uint32_t> ids;
    if (!gh) return ids;

    const auto& nodes = gh->getTaxiNodes();
    const auto current = nodes.find(gh->getTaxiCurrentNode());
    if (current == nodes.end()) return ids;

    const uint32_t mapId = current->second.mapId;
    ids.reserve(nodes.size());
    for (const auto& [id, node] : nodes) {
        if (node.mapId == mapId) ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

/// The node at a one-based position on the flight map, or 0 for no such node.
static uint32_t taxiNodeAt(game::GameHandler* gh, int index) {
    const auto ids = taxiNodeOrder(gh);
    if (index < 1 || index > static_cast<int>(ids.size())) return 0;
    return ids[static_cast<size_t>(index - 1)];
}

/// Where a taxi node sits on the flight map, as a fraction of its width and
/// height.
///
/// The flight map is the continent's map, so this is the same projection the
/// world map does - the one place it lives, in rendering/world_map, reached
/// through the continent rectangle the game handler now reads.
///
/// The order of the two conversions is the whole difficulty. TaxiNodes.dbc
/// holds positions in server order, and feeding those straight to
/// canonicalToRender transposes every marker, which is exactly how the flight
/// map's node markers came out mirrored once before. Server to canonical
/// first, then canonical to render, then project.
static bool taxiNodeMapPos(game::GameHandler* gh, uint32_t nodeId,
                           float& outU, float& outV) {
    if (!gh) return false;
    const auto& nodes = gh->getTaxiNodes();
    const auto it = nodes.find(nodeId);
    if (it == nodes.end()) return false;

    const auto& bounds = gh->getContinentBounds(it->second.mapId);
    if (!bounds.valid) return false;

    const glm::vec3 canonical = core::coords::serverToCanonical(
        glm::vec3(it->second.x, it->second.y, it->second.z));
    const glm::vec3 render = core::coords::canonicalToRender(canonical);

    rendering::world_map::ZoneBounds zb;
    zb.locLeft = bounds.left;   zb.locRight  = bounds.right;
    zb.locTop  = bounds.top;    zb.locBottom = bounds.bottom;
    const glm::vec2 uv =
        rendering::world_map::renderPosToMapUV(render, zb, /*isContinent=*/true);
    outU = uv.x;
    // Measured up from the bottom, which is not the convention the rest of the
    // interface uses and is the whole of why the flight map was wrong.
    //
    // The world map anchors what it places to TOPLEFT and negates the y it was
    // given - worldmapframe.lua:789 - so GetPlayerMapPosition counts down from
    // the top. The flight map anchors to BOTTOMLEFT and adds it, for the nodes
    // (taxiframe.lua:80) and for both ends of every route leg (140-142,
    // 179-181), so TaxiNodePosition counts up from the bottom.
    //
    // renderPosToMapUV answers the world map's way, so handing it over
    // unchanged mirrored the map vertically: Rut'theran sits 0.17 down from the
    // top of Kalimdor and was drawn 0.17 up from the bottom, which is Tanaris.
    // Every node was over somewhere it did not belong, which is what made them
    // read as the wrong places rather than as merely misplaced.
    outV = 1.0f - uv.y;
    return true;
}

/// One end of one leg of a flight, as a fraction of the map.
///
/// `End` is 0 for the leg's start and 1 for its finish; `Horizontal` picks x
/// over y. Four names, one body - they differ only in which number they read.
template <int End, bool Horizontal>
static int lua_TaxiLegCoord(lua_State* L) {
    auto* gh = getGameHandler(L);
    const uint32_t dest = taxiNodeAt(gh, static_cast<int>(luaL_optnumber(L, 1, 0)));
    const int hop = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (dest == 0 || hop < 1 || !gh) { lua_pushnumber(L, 0); return 1; }

    const auto route = gh->getTaxiRouteTo(dest);
    // Hop N runs from route[N-1] to route[N], so the last hop is size-1.
    if (hop >= static_cast<int>(route.size())) { lua_pushnumber(L, 0); return 1; }

    float u = 0, v = 0;
    if (!taxiNodeMapPos(gh, route[static_cast<size_t>(hop - 1 + End)], u, v)) {
        lua_pushnumber(L, 0);
        return 1;
    }
    lua_pushnumber(L, Horizontal ? u : v);
    return 1;
}

/// The hops of the route the flight map is drawing lines for.
///
/// TaxiNodeSetCurrent names the node the player is hovering, and the frame then
/// asks for each leg of the journey to it in turn. Kept here rather than asked
/// for again per leg because getTaxiRouteTo searches, and the frame asks for the
/// same route once per hop and again for every line it draws.
static std::vector<uint32_t>& taxiRouteShown() {
    static std::vector<uint32_t> route;
    return route;
}

void registerSystemLuaAPI(lua_State* L) {
    // Before any binding is registered, so the first GetCVar of the run - which
    // happens while a panel is being built - sees what the player last set.
    loadStoredCVars();
    loadInterfaceState();
    static const struct { const char* name; lua_CFunction func; } api[] = {
                {"Screenshot",               lua_Screenshot},
                {"CameraZoomIn",             lua_CameraZoom<1>},
                {"CameraZoomOut",            lua_CameraZoom<-1>},
                // The same two functions under the vehicle's binding names
                // (0x006018a0, 0x006018b0 jump to 0x006017e0, 0x00601840).
                {"VehicleCameraZoomIn",      lua_CameraZoom<1>},
                {"VehicleCameraZoomOut",     lua_CameraZoom<-1>},
                {"SetView",                  lua_SetView},
                {"SaveView",                 lua_SaveView},
                {"ResetView",                lua_ResetView},
                {"NextView",                 lua_StepView<1>},
                {"PrevView",                 lua_StepView<-1>},
                {"FlipCameraYaw",            lua_FlipCameraYaw},
                {"WoweeSetCamera",           lua_WoweeSetCamera},
                {"GetFarclip",               lua_GetFarclip},
                {"SetFarclip",               lua_SetFarclip},
                {"GetTexLodBias",            lua_GetTexLodBias},
                {"SetTexLodBias",            lua_SetTexLodBias},
                {"GetBaseMip",               lua_GetBaseMip},
                {"SetBaseMip",               lua_SetBaseMip},
                {"GetWaterDetail",           lua_GetWaterDetail},
                {"SetWaterDetail",           lua_SetWaterDetail},
                {"MoveViewInStart",          lua_MoveViewStart<0>},
                {"MoveViewInStop",           lua_MoveViewStop<0>},
                {"MoveViewOutStart",         lua_MoveViewStart<1>},
                {"MoveViewOutStop",          lua_MoveViewStop<1>},
                {"MoveViewLeftStart",        lua_MoveViewStart<2>},
                {"MoveViewLeftStop",         lua_MoveViewStop<2>},
                {"MoveViewRightStart",       lua_MoveViewStart<3>},
                {"MoveViewRightStop",        lua_MoveViewStop<3>},
                {"MoveViewUpStart",          lua_MoveViewStart<4>},
                {"MoveViewUpStop",           lua_MoveViewStop<4>},
                {"MoveViewDownStart",        lua_MoveViewStart<5>},
                {"MoveViewDownStop",         lua_MoveViewStop<5>},
                {"ToggleAutoRun",            lua_ToggleAutoRun},
                {"ToggleRun",                lua_ToggleRun},
                {"JumpOrAscendStart",        lua_JumpOrAscendStart},
                {"TurnLeftStart",            lua_BindingTurn<-1, true>},
                {"TurnLeftStop",             lua_BindingTurn<-1, false>},
                {"TurnRightStart",           lua_BindingTurn<1, true>},
                {"TurnRightStop",            lua_BindingTurn<1, false>},
                {"MoveForwardStart",  lua_BindingControl<rendering::CameraController::kControlForward, true>},
                {"MoveForwardStop",   lua_BindingControl<rendering::CameraController::kControlForward, false>},
                {"MoveBackwardStart", lua_BindingControl<rendering::CameraController::kControlBackward, true>},
                {"MoveBackwardStop",  lua_BindingControl<rendering::CameraController::kControlBackward, false>},
                {"StrafeLeftStart",   lua_BindingControl<rendering::CameraController::kControlStrafeLeft, true>},
                {"StrafeLeftStop",    lua_BindingControl<rendering::CameraController::kControlStrafeLeft, false>},
                {"StrafeRightStart",  lua_BindingControl<rendering::CameraController::kControlStrafeRight, true>},
                {"StrafeRightStop",   lua_BindingControl<rendering::CameraController::kControlStrafeRight, false>},
                {"AscendStop",        lua_BindingControl<rendering::CameraController::kControlAscend, false>},
                {"DescendStop",       lua_BindingControl<rendering::CameraController::kControlDescend, false>},
                {"SitStandOrDescendStart", lua_SitStandOrDescendStart},
                {"MouselookStart",    lua_BindingControl<CC::kControlMouselook, true, false>},
                {"MouselookStop",     lua_BindingControl<CC::kControlMouselook, false, false>},
                {"IsMouselooking",    lua_IsMouselooking},
                {"MoveAndSteerStart", lua_BindingControl<kMoveAndSteer, true>},
                {"MoveAndSteerStop",  lua_BindingControl<kMoveAndSteer, false>},
                {"PitchUpStart",      lua_BindingControl<CC::kControlPitchUp, true>},
                {"PitchUpStop",       lua_BindingControl<CC::kControlPitchUp, false>},
                {"PitchDownStart",    lua_BindingControl<CC::kControlPitchDown, true>},
                {"PitchDownStop",     lua_BindingControl<CC::kControlPitchDown, false>},
                {"WoweeShowSettings",        lua_WoweeShowSettings},
                {"WoweeSettingList",         lua_WoweeSettingList},
                {"WoweeVersion",             lua_WoweeVersion},
                {"WoweeOpenURL",             lua_WoweeOpenURL},
                {"WoweeGetSetting",          lua_WoweeGetSetting},
                {"WoweeSetSetting",          lua_WoweeSetSetting},
                {"WoweeSaveVariables",       lua_WoweeSaveVariables},
                {"HasLFGRestrictions",       lua_HasLFGRestrictions},
                {"GetLFGProposal",           lua_GetLFGProposal},
                {"GetLFGInfoServer",         lua_GetLFGInfoServer},
                {"GetLFGRoleUpdate",         lua_GetLFGRoleUpdate},
                {"GetLFGRoleUpdateMember",   lua_GetLFGRoleUpdateMember},
                {"GetAllowLowLevelRaid",     lua_GetAllowLowLevelRaid},
                {"SetAllowLowLevelRaid",     lua_SetAllowLowLevelRaid},
                {"GetTaxiBenchmarkMode",     lua_GetTaxiBenchmarkMode},
                {"OpeningCinematic",         lua_OpeningCinematic},
                {"OpenTrainer",              lua_OpenTrainer},
                {"CanMapChangeDifficulty",   lua_CanMapChangeDifficulty},
                {"GetGMTicketCategories",    lua_GetGMTicketCategories},
                {"GetAchievementNumRewards", lua_GetAchievementNumRewards},
                {"GetAchievementReward",     lua_GetAchievementReward},
                {"GetNumPetitionItems",      lua_GetNumPetitionItems},
                {"IsListedInLFR",            lua_IsListedInLFR},
                {"IsPartyLFG",               lua_IsPartyLFG},
                {"GetLFGDeserterExpiration", lua_GetLFGDeserterExpiration},
                {"GetLFGRandomCooldownExpiration", lua_GetLFGRandomCooldownExpiration},
                {"RefreshLFGList",           lua_RefreshLFGList},
                {"GetTrackedAchievements",   lua_GetTrackedAchievements},
                {"RequestRaidInfo",          lua_RequestRaidInfo},
                {"IsPVPTimerRunning",        lua_IsPVPTimerRunning},
                {"GetPVPTimer",              lua_GetPVPTimer},
                {"GetCurrentArenaSeason",    lua_GetCurrentArenaSeason},
                {"GetPVPRankProgress",       lua_GetPVPRankProgress},
                {"ArenaTeamRoster",              lua_ArenaTeamRoster},
                {"GetArenaTeamRosterInfo",       lua_GetArenaTeamRosterInfo},
                // How many rows that reader has. Unbound, so opening a team's
                // details called a nil global and raised before the first row
                // was read - the reader beside it worked the whole time.
                //
                // GetNumArenaTeamMembers(team [, showOffline]) (0x005a2930):
                // with offline members hidden and showOffline not asked for,
                // only the online - who are the first rows then.
                {"GetNumArenaTeamMembers", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1)) {
                return luaL_error(L, "Usage: GetNumArenaTeamMembers(team [,showOffline])");
            }
            auto* gh = getGameHandler(L);
            const uint32_t teamId =
                arenaTeamIdAtIndex(gh, static_cast<int>(luaL_optnumber(L, 1, 0)));
            const auto* roster = (gh && teamId) ? gh->getArenaTeamRoster(teamId) : nullptr;
            size_t count = 0;
            if (roster) {
                if (!gh->arenaRosterShowOffline() && !luaClientSwitchArg(L, 2, false)) {
                    for (const auto& m : roster->members) count += m.online ? 1 : 0;
                } else {
                    count = roster->members.size();
                }
            }
            lua_pushnumber(L, static_cast<lua_Number>(count));
            return 1;
        }},
                // GetBattlefieldArenaFaction() (0x0054a280): 1 when the arena
                // side byte of the battle under way is set, otherwise nil.
                {"GetBattlefieldArenaFaction", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->battlefieldArenaFaction() != 0) lua_pushnumber(L, 1);
            else lua_pushnil(L);
            return 1;
        }},
                // Get/SetArenaTeamRosterShowOffline (0x005a2a00, 0x005a35e0).
                {"GetArenaTeamRosterShowOffline", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh || gh->arenaRosterShowOffline()) lua_pushnumber(L, 1);
            else lua_pushnil(L);
            return 1;
        }},
                {"SetArenaTeamRosterShowOffline", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) {
                gh->setArenaRosterShowOffline(luaClientSwitchArg(L, 1, true));
            }
            return 0;
        }},
                // GetArenaTeamGdfInfo(team, index) (0x005a3260): the two
                // floats the roster carried for that member, nil and nil past
                // the end of it.
                {"GetArenaTeamGdfInfo", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2)) {
                return luaL_error(L, "Usage: GetArenaTeamGdfInfo(team, index)");
            }
            auto* gh = getGameHandler(L);
            const int team = static_cast<int>(lua_tonumber(L, 1));
            const int member = static_cast<int>(lua_tonumber(L, 2));
            const uint32_t teamId = (team >= 1 && team <= 3) ? arenaTeamIdAtIndex(gh, team) : 0;
            const auto* roster = (gh && teamId) ? gh->getArenaTeamRoster(teamId) : nullptr;
            if (!roster || member < 1 || member > static_cast<int>(roster->members.size())) {
                lua_pushnil(L);
                lua_pushnil(L);
                return 2;
            }
            const auto& m = roster->members[static_cast<size_t>(member) - 1];
            lua_pushnumber(L, m.gdfValue1);
            lua_pushnumber(L, m.gdfValue2);
            return 2;
        }},
                // SortArenaTeamRoster(column) - the details frame's column
                // headers. Unbound, every one of them raised on click.
                {"SortArenaTeamRoster", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* key = luaL_optstring(L, 1, "");
            if (gh && key && *key) gh->sortArenaTeamRosters(key);
            return 0;
        }},
                {"GetArenaTeamRosterSelection",  lua_GetArenaTeamRosterSelection},
                {"SetArenaTeamRosterSelection",  lua_SetArenaTeamRosterSelection},
                {"CloseArenaTeamRoster",         lua_CloseArenaTeamRoster},
                {"IsArenaTeamCaptain",           lua_IsArenaTeamCaptain},
                {"CloseBattlefield",             lua_CloseBattlefield},
                {"CanExitVehicle",               lua_CanExitVehicle},
                {"IsUsingVehicleControls",       lua_IsUsingVehicleControls},
                {"CanSwitchVehicleSeats",        lua_CanSwitchVehicleSeats},
                {"VehiclePrevSeat",              lua_VehiclePrevSeat},
                {"VehicleNextSeat",              lua_VehicleNextSeat},
                {"IsVehicleAimAngleAdjustable",  lua_IsVehicleAimAngleAdjustable},
                {"IsVehicleAimPowerAdjustable",  lua_IsVehicleAimPowerAdjustable},
                {"VehicleAimGetAngle",           lua_VehicleAimGetAngle},
                {"VehicleAimSetNormPower",       lua_VehicleAimSetNormPower},
                {"VehicleAimIncrement",          lua_VehicleAimIncrement},
                {"VehicleAimDecrement",          lua_VehicleAimDecrement},
                {"VehicleAimRequestAngle",       lua_VehicleAimRequestAngle},
                {"VehicleAimRequestNormAngle",   lua_VehicleAimRequestNormAngle},
                {"HasKey",                       lua_HasKey},
                {"GetArenaTeam",             lua_GetArenaTeam},
                {"GetRandomBGHonorCurrencyBonuses",  lua_GetRandomBGHonorCurrencyBonuses},
                {"GetHolidayBGHonorCurrencyBonuses", lua_BattlegroundHonorBonusesNone},
                {"GetBattlefieldInstanceInfo",       lua_GetBattlefieldInstanceInfo},
                {"GetNumBattlefields",       lua_GetNumBattlefields},
                // Sorting the battleground list. This client sorts its own, so
                // there is nothing to do - but the name has to exist, because
                // PVPBattlegroundFrame_OnShow calls it and a nil global raises
                // as the panel opens.
                {"SortBGList",               lua_ReturnNothing},
                // How many papers the compose frame offers. That is Stationery
                // .dbc filtered by the client's own rule - the row whose flag
                // says no paper need be bought, plus every row whose item is
                // lying about in the bags - so the count is that list's length
                // and nothing else.
                //
                // It used to answer one: the plain parchment. A popup with
                // nothing in it cannot be clicked, and SendMailFrame_CanSend
                // will not enable the Send button until a paper is picked, so
                // the honest zero at the time was what stopped letters going
                // out at all. See InventoryHandler::stationeryOptions.
                {"GetNumStationeries",       [](lua_State* L) -> int {
                    auto* gh = getGameHandler(L);
                    auto* inv = gh ? gh->getInventoryHandler() : nullptr;
                    const size_t count = inv ? inv->stationeryOptions().size() : 0;
                    lua_pushnumber(L, static_cast<lua_Number>(count));
                    return 1;
                }},
                // GetNumMutes used to answer zero here, because the ignore tab's
                // update reads it on a line of its own with nothing guarding it
                // and an unbound global took the whole tab with it. The real
                // count is bound in the social table, where the rest of the mute
                // list is - and this table registers after that one, so leaving
                // the stub here would have hidden it again.
                {"IsInLFGDungeon",           lua_IsInLFGDungeon},
                {"LFGTeleport",              lua_LFGTeleport},
                {"IsBattlefieldArena",       lua_IsBattlefieldArena},
                {"IsActiveBattlefieldArena", lua_IsActiveBattlefieldArena},
                {"CanHearthAndResurrectFromArea", lua_CanHearthAndResurrectFromArea},
                {"HearthAndResurrectFromArea", lua_HearthAndResurrectFromArea},
                // The Recruit-a-Friend level grant LEVEL_GRANT_PROPOSED offers:
                // accepted (0x00523360) or let go (0x00517d20).
                {"AcceptLevelGrant", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->acceptLevelGrant();
            return 0;
        }},
                {"DeclineLevelGrant", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->declineLevelGrant();
            return 0;
        }},
                {"GetWorldPVPQueueStatus",   lua_GetWorldPVPQueueStatus},
                {"LeaveBattlefield",         lua_LeaveBattlefield},
                {"CreateWorldMapArrowFrame",   lua_CreateWorldMapArrowFrame},
                {"ShowWorldMapArrowFrame",     lua_ShowWorldMapArrowFrame},
                {"PositionWorldMapArrowFrame", lua_PositionWorldMapArrowFrame},
                {"InitWorldMapPing",           lua_InitWorldMapPing},
                {"CreateMiniWorldMapArrowFrame",   lua_CreateMiniWorldMapArrowFrame},
                {"PositionMiniWorldMapArrowFrame", lua_PositionMiniWorldMapArrowFrame},
                {"ShowMiniWorldMapArrowFrame",     lua_ShowMiniWorldMapArrowFrame},
                {"GetBattlefieldMapIconScale",     lua_GetBattlefieldMapIconScale},
                {"PlayerIsPVPInactive",            lua_PlayerIsPVPInactive},
                {"CombatTextSetActiveUnit",        lua_CombatTextSetActiveUnit},
                {"CombatLogGetNumEntries",         lua_CombatLogGetNumEntries},
                {"CombatLogGetCurrentEntry",       lua_CombatLogGetCurrentEntry},
                {"CombatLogAdvanceEntry",          lua_CombatLogAdvanceEntry},
                // How long combat log entries are kept: the CVar
                // combatLogRetentionTime, 300 seconds by default (0x0074d9e0,
                // 0x0074d5b0 sets it).
                {"CombatLogGetRetentionTime", [](lua_State* L) -> int {
            lua_pushnumber(L, std::atoi(storedCVarValue("combatlogretentiontime", "300").c_str()));
            return 1;
        }},
                {"CombatLogSetRetentionTime", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: CombatLogSetRetentionTime(seconds)");
            const std::string value = std::to_string(static_cast<int>(lua_tonumber(L, 1)));
            auto& stored = cvarStore()["combatlogretentiontime"];
            if (stored != value) {
                stored = value;
                saveStoredCVars();
            }
            return 0;
        }},
                {"CombatLogClearEntries",          lua_CombatLogClearEntries},
                {"CombatLogSetCurrentEntry",       lua_CombatLogSetCurrentEntry},
                {"CombatLogAddFilter",             lua_CombatLogAddFilter},
                {"CombatLogResetFilter",           lua_CombatLogResetFilter},
                {"GetBattlefieldInfo",       lua_GetBattlefieldInfo},
                {"SetSelectedBattlefield",   lua_SetSelectedBattlefield},
                {"GetSelectedBattlefield",   lua_GetSelectedBattlefield},
                {"JoinBattlefield",          lua_JoinBattlefield},
                {"GetLFGCompletionReward",     lua_GetLFGCompletionReward},
                {"GetLFGCompletionRewardItem", lua_GetLFGCompletionRewardItem},
                {"RunMacroText",             lua_RunMacroText},
                // This client's own slash commands, for the bootstrap chunk
                // that puts them into SlashCmdList. Not WoW API - the names
                // are prefixed so nothing in FrameXML can collide with them.
                {"__WoweeClientCommandNames", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            lua_newtable(L);
            if (!svc || !svc->clientChatCommandNames) return 1;
            int i = 1;
            for (const auto& name : svc->clientChatCommandNames()) {
                lua_pushstring(L, name.c_str());
                lua_rawseti(L, -2, i++);
            }
            return 1; }},
                {"__WoweeRunClientCommand", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            const char* alias = luaL_optstring(L, 1, "");
            const char* args  = luaL_optstring(L, 2, "");
            bool ok = false;
            if (svc && svc->runClientChatCommand && alias && *alias)
                ok = svc->runClientChatCommand(alias, args);
            lua_pushboolean(L, ok ? 1 : 0);
            return 1; }},
                {"RunMacro",                 lua_RunMacro},
                {"GetRunningMacro",          lua_GetRunningMacro},
                {"GetRunningMacroButton",    lua_GetRunningMacroButton},
                {"TriggerTutorial",          lua_TriggerTutorial},
                {"Quit",                     lua_Quit},
                {"ForceQuit",                lua_ForceQuit},
                {"ForceLogout",              lua_ForceLogout},
                {"ReloadUI",                 lua_ReloadUI},
                {"GetGamma",                 lua_GetGamma},
                {"GetTerrainMip",            lua_GetTerrainMip},
                {"SetTerrainMip",            lua_SetTerrainMip},
                {"SetGamma",                 lua_SetGamma},
                {"WoweeReportMissingFixedControls",
                                             lua_WoweeReportMissingFixedControls},
                {"GetVideoCaps",             lua_GetVideoCaps},
                {"GetCVarMin",               lua_GetCVarMin},
                {"GetCVarMax",               lua_GetCVarMax},
                {"IsVoiceChatAllowedByServer", lua_IsVoiceChatAllowedByServer},
                {"VoiceChat_IsRecordingLoopbackSound", lua_VoiceChat_IsRecordingLoopbackSound},
                {"VoiceChat_IsPlayingLoopbackSound",   lua_VoiceChat_IsPlayingLoopbackSound},
                {"VoiceChat_GetCurrentMicrophoneSignalLevel", lua_VoiceChat_GetCurrentMicrophoneSignalLevel},
                {"GetVoiceSessionInfo",      lua_GetVoiceSessionInfo},
                {"GetNumVoiceSessionMembersBySessionID", lua_GetNumVoiceSessionMembersBySessionID},
                {"GetVoiceSessionMemberInfoBySessionID", lua_GetVoiceSessionMemberInfoBySessionID},
                {"SetActiveVoiceChannel",    lua_SetActiveVoiceChannel},
                {"ChannelVoiceOn",           lua_ChannelVoice<true>},
                {"ChannelVoiceOff",          lua_ChannelVoice<false>},
                {"DisplayChannelVoiceOn",    lua_DisplayChannelVoice<true>},
                {"DisplayChannelVoiceOff",   lua_DisplayChannelVoice<false>},
                {"VoiceChat_RecordLoopbackSound", lua_VoiceChat_RecordLoopbackSound},
                {"VoiceChat_PlayLoopbackSound",   lua_VoiceChat_PlayLoopbackSound},
                {"CanShowAchievementUI",     lua_CanShowAchievementUI},
                {"IsXPUserDisabled",         lua_IsXPUserDisabled},
                {"GetAddOnMemoryUsage",      lua_GetAddOnMemoryUsage},
                {"UpdateAddOnMemoryUsage",   lua_ReturnNothing},
                {"RunScript",                lua_RunScript},
                {"IsMouseButtonDown",        lua_IsMouseButtonDown},
                {"GetCVarDefault",           lua_GetCVarDefault},
                {"GetCVarInfo",              lua_GetCVarInfo},
                {"GetCVarAbsoluteMin",       lua_GetCVarAbsoluteMin},
                {"GetCVarAbsoluteMax",       lua_GetCVarAbsoluteMax},
                {"RegisterCVar",             lua_RegisterCVar},
                {"IsAddOnLoaded",            lua_IsAddOnLoaded},
                {"LoadAddOn",                lua_LoadAddOn},
                {"UIParentLoadAddOn",        lua_LoadAddOn},
                // This is the whole of whether the achievement micro button is
                // clickable: mainmenubarmicrobuttons.lua disables it unless
                // this and CanShowAchievementUI both answer yes, and the other
                // already did. A flat false left the button greyed out while
                // the client knew exactly which achievements were earned.
                {"HasCompletedAnyAchievement", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && !gh->getEarnedAchievements().empty() ? 1 : 0);
            return 1;
        }},
                {"TurnInGuildCharter",       lua_ReturnNothing},
                // GetMovieResolution() (0x004dd610): the width read out of
                // gxResolution's "WxH" - which here is the window's.
                {"GetMovieResolution", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            auto* win = svc ? svc->window : nullptr;
            lua_pushnumber(L, win ? win->getWidth() : 1920);
            return 1;
        }},
                // SetEuropeanNumbers([on]) (0x00510de0 -> 0x0084f010): on by
                // default; the client's format and SetFormattedText then swap
                // '.' and ',' inside numbers (0x0084f030). Kept as the client
                // keeps it; this client's string.format is Lua's own, so the
                // swap is not made.
                {"SetEuropeanNumbers", [](lua_State* L) -> int {
            europeanNumbers() = luaClientSwitchArg(L, 1, true);
            return 0;
        }},
                // SetLayoutMode([mode]) (0x0050f8f0): the frame manager's layout
                // mode, 1 when none is given. Kept; nothing here lays out by it.
                {"SetLayoutMode", [](lua_State* L) -> int {
            layoutMode() = lua_isnumber(L, 1) ? luaToClientInt(L, 1) : 1;
            return 0;
        }},
                // SetConsoleKey(key) (0x0050f990) names the key that opens the
                // developer console. This client has no console to open.
                {"SetConsoleKey", lua_ReturnNothing},
                // UploadSettings() and DownloadSettings() (0x006b9c60,
                // 0x006b9ca0) push the account data that has changed to the
                // server, or ask for what it holds. This client keeps its
                // settings locally and has no server copy to sync.
                {"UploadSettings",   lua_ReturnNothing},
                {"DownloadSettings", lua_ReturnNothing},
                // PlayDance(name) (0x005761f0 -> 0x00576100): a dance from the
                // dance studio's own cache, sent as opcode 1099. That studio
                // and its dances were never shipped, so no name finds one; the
                // usage error is the client's.
                {"PlayDance", [](lua_State* L) -> int {
            if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: PlayDance(\"dance name\")");
            return 0;
        }},
                // The pitch keys' control bits (0x005fc8e0 -> 0x005fa170,
                // 0x400 and 0x800): the mover pitches up or down at its
                // pitch rate while one is held (0x005fb1a0).
                {"VehicleAimUpStart", [](lua_State* L) -> int {
            if (!taint::allowed(L, 0)) return 0;   // as the pitch keys
            if (auto* gh = getGameHandler(L)) gh->setVehicleAimKey(true, true);
            return 0;
        }},
                {"VehicleAimUpStop", [](lua_State* L) -> int {
            if (!taint::allowed(L, 0)) return 0;   // as the pitch keys
            if (auto* gh = getGameHandler(L)) gh->setVehicleAimKey(true, false);
            return 0;
        }},
                {"VehicleAimDownStart", [](lua_State* L) -> int {
            if (!taint::allowed(L, 0)) return 0;   // as the pitch keys
            if (auto* gh = getGameHandler(L)) gh->setVehicleAimKey(false, true);
            return 0;
        }},
                {"VehicleAimDownStop", [](lua_State* L) -> int {
            if (!taint::allowed(L, 0)) return 0;   // as the pitch keys
            if (auto* gh = getGameHandler(L)) gh->setVehicleAimKey(false, false);
            return 0;
        }},
                // CMSG_REQUEST_VEHICLE_EXIT where the player's seat lets it
                // out, Flags 0x2000000 (0x005fb660 -> 0x0074c7f0); else the
                // error SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW (0x005216f0).
                {"VehicleExit", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            if (playerVehicleSeatHasFlag(gh, 0x2000000)) {
                gh->sendRequestVehicleExit();
                return 0;
            }
            lua_getglobal(L, "SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW");
            const char* text = lua_tostring(L, -1);
            gh->addUIError(text ? text : "You can't do that right now.");
            lua_pop(L, 1);
            return 0;
        }},
                {"VehicleAimGetNormAngle",   lua_VehicleAimGetNormAngle},
                {"VehicleAimGetNormPower",   lua_VehicleAimGetNormPower},
                {"GetMapInfo",               lua_GetMapInfo},
                {"GetExpansionLevel",        lua_GetExpansionLevel},
                // The difficulty the player is set to, which this client is
                // told by SMSG_INSTANCE_DIFFICULTY and kept answering as one.
                // A dropdown reading a constant shows the wrong tick and, worse,
                // sends a change nobody asked for when the menu is opened.
                // The wire counts difficulties from zero and the interface from
                // one: DUNGEON_DIFFICULTY_NORMAL is 0 on AzerothCore, and the
                // popup checks its menu entry with `GetDungeonDifficulty() ==
                // index`, index being the 1-based row. Answering the stored
                // value straight made Heroic read as Normal - `d ? d : 1` turns
                // both 0 and 1 into 1 - so the tick sat on the wrong row and
                // unitpopup's `GetDungeonDifficulty() == 1` heroic-lockout
                // check was true inside a heroic.
                //
                // GetDungeonDifficulty (0x00515790): the dungeon difficulty in
                // force and the player's own, from MSG_SET_DUNGEON_DIFFICULTY -
                // not the instance's, which GetInstanceInfo answers.
                {"GetDungeonDifficulty", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto dungeon = gh ? gh->getDungeonDifficulty() : std::pair<uint32_t, uint32_t>{0, 0};
            lua_pushnumber(L, dungeon.first + 1u);
            lua_pushnumber(L, dungeon.second + 1u);
            return 2;
        }},
                // GetRaidDifficulty (0x00515810): the raid difficulty in
                // force and the player's own, from MSG_SET_RAID_DIFFICULTY.
                {"GetRaidDifficulty", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto raid = gh ? gh->getRaidDifficulty() : std::pair<uint32_t, uint32_t>{0, 0};
            lua_pushnumber(L, raid.first + 1u);
            lua_pushnumber(L, raid.second + 1u);
            return 2;
        }},
                {"GetChatTypeIndex",         lua_ReturnOne},
                {"GetDefaultLanguage",       lua_GetDefaultLanguage},
                {"GetWeaponEnchantInfo",     lua_GetWeaponEnchantInfo},
                // The PvP reclaim timer, which this client already tracks.
                // Stubbed to zero this read as "reclaim now" and made the
                // delay text on FrameXML's corpse prompt always empty.
                {"GetCorpseRecoveryDelay", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->getCorpseReclaimDelaySec() : 0.0);
            return 1;
        }},
                // Which party slot holds the leader, in the same 1-to-4
                // ordering resolveUnitGuid gives "party1".."party4": members
                // other than the player, in order. Zero means the player leads
                // it, which is what WoW answers and what the crown reads as
                // "not on this frame".
                //
                // A constant zero meant PartyMemberFrame_UpdateLeader hid the
                // icon on every frame it ran for, so no party ever showed who
                // was leading it - while partyData has carried leaderGuid all
                // along.
                {"GetPartyLeaderIndex", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { lua_pushnumber(L, 0); return 1; }
            const auto& pd = gh->getPartyData();
            if (pd.leaderGuid == 0 || pd.leaderGuid == gh->getPlayerGuid()) {
                lua_pushnumber(L, 0);
                return 1;
            }
            int slot = 0;
            for (const auto& m : pd.members) {
                if (m.guid == gh->getPlayerGuid()) continue;
                ++slot;
                if (m.guid == pd.leaderGuid) { lua_pushnumber(L, slot); return 1; }
            }
            lua_pushnumber(L, 0);
            return 1;
        }},
                // How many of the five arena opponent slots are filled - which the
                // client answers as a prefix, not a tally: 0x0054a2c0 takes its
                // number from 0x00549630, a walk that stops at the first slot whose
                // opponent pair (0x00be9f48) and pet pair (0x00be9ef8) are both
                // empty, the owner pair not counted. Both consumers then use it as
                // an index into "ArenaEnemyFrame"..N, so the frame the watch list
                // and the enemy background anchor under is the last one the panel
                // has rather than the highest one with a unit in it. Arena rosters
                // are filled as opponents come into sight (arenaRoster_, which also
                // names them arena1..5 / arenapet1..5 for resolveUnit), and this
                // answered zero outside an arena as much as inside one - which hid
                // the enemy background and left the watch frame under the minimap
                // cluster in a fight where the panel was up.
                {"GetNumArenaOpponents", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, static_cast<double>(gh ? gh->arenaRoster().numOpponents() : 0));
            return 1;
        }},
                {"GetCurrentMultisampleFormat", lua_GetCurrentMultisampleFormat},
                // These hand back a list, not a value: the caller walks it with
                // select("#", ...) and reads it in groups. One number makes the
                // loop run once against nils, which is worse than an empty
                // list - for anything returning a list, nothing is the right
                // way to say there is none.
                // The anti-aliasing dropdown, which used to be left empty on
                // the argument that offering modes was worse than offering
                // none. Empty was worse than either: UIDropDownMenu_Refresh
                // walks the shared DropDownList1 buttons, so a dropdown that
                // adds none of its own reads whichever list was built last and
                // takes its text. Multisampling showed "1280x720 (Wide)",
                // copied from the Resolution dropdown three controls away.
                //
                // The four modes are real and are the same four this client's
                // own panel offers, so choosing one applies it. MSAA is moot
                // while FSR is upscaling - which is why the client's own combo
                // disables itself there - but the choice is still recorded and
                // still takes effect when FSR is off, which is what that combo
                // does with it too.
                {"GetMultisampleFormats",    lua_GetMultisampleFormats},
                {"GetRefreshRates",          lua_GetRefreshRates},
                // ---- The options panels behind the game menu ----
                //
                // Every reader here was bound and none of the writers were, so
                // opening Video or Sound and touching anything raised. The
                // panels sit one click inside the game menu, which is a handed-
                // over element, and none of their files was scanned by the
                // readiness report until the question "which FrameXML files
                // does no element reach" was asked.
                //
                // The answers are shaped by what the readers already say.
                // GetScreenResolutions offers exactly one mode -- the window
                // this client is already in -- and GetMultisampleFormats offers
                // none, so choosing from either list can only ever re-choose
                // what is set. These accept the call and change nothing, which
                // is the truth rather than a stub.
                {"SetScreenResolution",      lua_SetScreenResolution},
                {"SetMultisampleFormat",     lua_SetMultisampleFormat},
                // Resolution and windowed mode are settable now, so this one
                // has something to put back and does not do it. Left rather
                // than guessed: the defaults live as function-local constants
                // in the settings panel, and a Restore that picks its own would
                // disagree with the Defaults button beside it.
                {"RestoreVideoResolutionDefaults", lua_ReturnNothing},
                {"RestoreVideoEffectsDefaults",    lua_ReturnNothing},
                {"RestoreVideoStereoDefaults",     lua_ReturnNothing},
                // Applying video settings restarts the graphics device on the
                // real client. This one applies what it can as it goes and has
                // no device to tear down.
                // RestartGx() - apply the display CVars marked `restart`.
                //
                // The video panel writes every changed CVar and then, if any of
                // them wanted it, restarts the device once so they take effect
                // together; gxWindow is one of those, so the SetCVar that
                // precedes this is deliberately not where the change lands.
                // A no-op here meant ticking Windowed wrote a value and left
                // the window exactly as it was.
                //
                // Only gxWindow is acted on. gxResolution is a dropdown whose
                // value format has not been read off the control here, and a
                // resolution applied from a misread string is a window nobody
                // can put back.
                {"RestartGx", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            if (!svc || !svc->setFullscreen) return 0;
            lua_getglobal(L, "GetCVar");
            lua_pushstring(L, "gxWindow");
            if (lua_pcall(L, 1, 1, 0) != 0) { lua_pop(L, 1); return 0; }
            const char* windowed = lua_tostring(L, -1);
            const bool wantFullscreen = windowed && std::string(windowed) == "0";
            lua_pop(L, 1);
            svc->setFullscreen(wantFullscreen);
            return 0;
        }},
                {"Sound_GameSystem_RestartSoundSystem", lua_ReturnNothing},
                // A separate render scale for the player model, which this
                // client does not have. False disables the control rather than
                // leaving it offering something that would do nothing.
                {"IsPlayerResolutionAvailable", lua_ReturnFalse},
                // Which extra action bars are shown. FrameXML draws the bars
                // and MultiActionBar_Update decides from the SHOW_MULTI_ACTIONBAR_*
                // globals, which are CVars and persist on their own - so the
                // C-side toggle this mirrors has nothing left to do here.
                // Set/GetActionBarToggles - which of the four extra action
                // bars are shown.
                //
                // The setter was a no-op and the getter was not bound at all.
                // Each of the four checkboxes in Interface Options reads its
                // own state with
                //     self.value or ((select(n, GetActionBarToggles()) ...
                // and self.value is nil until something sets it, so opening
                // that panel called nil four times.
                // The Interface Options checkboxes for the extra bars.
                //
                // These were stored here and nowhere else. This client draws
                // its own action bars and suppresses FrameXML's MultiBar
                // frames, so the toggle changed a number that only this file
                // read: ticking "Bottom Left Bar" did nothing at all, which is
                // the whole of "no way to add action bars".
                //
                // Only the two the client has a bar for are passed on. Bottom
                // Right and Right Bar 2 have nothing to drive, so they are
                // still remembered but move nothing - saying so here rather
                // than mapping them onto a bar that is not theirs.
                {"SetActionBarToggles", [](lua_State* L) -> int {
            auto& shown = actionBarToggles();
            shown[0] = lua_toboolean(L, 1) != 0;   // Bottom Left
            shown[1] = lua_toboolean(L, 2) != 0;   // Bottom Right
            shown[2] = lua_toboolean(L, 3) != 0;   // Right
            shown[3] = lua_toboolean(L, 4) != 0;   // Right 2
            // The fifth is ALWAYS_SHOW_MULTIBARS, which asks for the empty
            // slots of a shown bar to stay visible. This client draws its bars
            // whole, so there is nothing for it to change - read here so it is
            // plainly ignored rather than silently dropped.
            const bool alwaysShow = lua_toboolean(L, 5) != 0;
            (void)alwaysShow;
            saveInterfaceState();
            // And the server keeps them for the character (0x005a8290,
            // CMSG_SET_ACTIONBAR_TOGGLES): the four as bits 0 to 3 of a byte.
            if (auto* gh = getGameHandler(L)) {
                uint8_t mask = 0;
                for (int i = 0; i < 4; ++i) {
                    if (shown[i]) mask |= static_cast<uint8_t>(1u << i);
                }
                gh->sendActionBarToggles(mask);
            }
            // The fourth is this client's left bar, despite being labelled
            // "Right Bar 2" where the player ticks it: the interface's fourth
            // multi-bar is MultiBarLeft, which is action page 4, which is the
            // upright bar this client draws at the left edge. Without it the
            // tick did nothing and the bar could only be turned on from this
            // client's own panel, where its checkbox then disagreed.
            if (auto* svc = getLuaServices(L); svc && svc->setClientSetting) {
                svc->setClientSetting("showbar2",     shown[0] ? "1" : "0");
                svc->setClientSetting("showrightbar", shown[2] ? "1" : "0");
                svc->setClientSetting("showleftbar",  shown[3] ? "1" : "0");
            }
            return 0;
        }},
                // Read back from the client's own settings for the two that
                // drive a bar, so the checkbox shows what is on screen rather
                // than what this file last stored. The two disagreed whenever
                // the bar was turned on from the client's own settings window.
                {"GetActionBarToggles", [](lua_State* L) -> int {
            // 0x005a8790: the active player's PLAYER_FIELD_BYTES byte 2, the
            // bars the realm keeps for the character - 1 for a bar that is on,
            // nil for one that is off, four of them.
            if (auto* gh = getGameHandler(L)) {
                if (const auto bars = gh->getActionBarToggles()) {
                    for (int i = 0; i < 4; ++i) {
                        if ((*bars & (1u << i)) != 0) lua_pushnumber(L, 1);
                        else lua_pushnil(L);
                    }
                    return 4;
                }
            }
            // Before the player's fields have arrived there is nothing kept to
            // read; what this client last had stands in.
            auto shown = actionBarToggles();
            if (auto* svc = getLuaServices(L); svc && svc->getClientSetting) {
                const auto readBool = [&svc](const char* key, bool fallback) {
                    const std::string v = svc->getClientSetting(key);
                    return v.empty() ? fallback : (v != "0");
                };
                shown[0] = readBool("showbar2", shown[0]);
                shown[2] = readBool("showrightbar", shown[2]);
                shown[3] = readBool("showleftbar", shown[3]);
            }
            for (bool on : shown) lua_pushboolean(L, on ? 1 : 0);
            return 4;
        }},
                // Voice chat, which this client has none of. The enumerations
                // hand back lists, so they answer with nothing rather than with
                // a zero; IsVoiceChatEnabled already answers false beside them.
                {"VoiceIsDisabledByClient",  lua_ReturnTrue},
                {"VoiceEnumerateCaptureDevices", lua_ReturnNothing},
                {"VoiceEnumerateOutputDevices",  lua_ReturnNothing},
                {"VoiceSelectCaptureDevice", lua_ReturnNothing},
                {"VoiceSelectOutputDevice",  lua_ReturnNothing},
                {"VoiceChat_StopPlayingLoopbackSound",   lua_ReturnNothing},
                {"VoiceChat_StopRecordingLoopbackSound", lua_ReturnNothing},
                // GetCompanionInfo(type, index) → creatureID, creatureName,
                // spellID, icon, active
                //
                // "MOUNT" or "CRITTER". Both are spells the player knows, told
                // apart by what the spell does - see rebuildCompanions.
                {"GetCompanionInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* kind = luaL_optstring(L, 1, "");
            const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
            if (!gh || index < 1) return luaReturnNil(L);
            const bool mounts = std::string(kind) == "MOUNT";
            const auto& list = gh->getCompanions(mounts);
            if (index > static_cast<int>(list.size())) return luaReturnNil(L);
            const auto& c = list[static_cast<size_t>(index) - 1];
            // A display id, and both kinds have to be asked for one. What is
            // held is a creature *template entry* either way: the summon's
            // misc value for a critter, and the mounted aura's for a mount -
            // AzerothCore reads that one with GetCreatureTemplate and picks
            // the model with ChooseDisplayId, so it is an entry there too.
            //
            // A mount's was handed to CompanionModelFrame:SetCreature
            // unresolved, on the reading that it was already a display id. An
            // entry used as a display id names a real CreatureDisplayInfo row
            // belonging to something else, so the mount tab previewed whatever
            // creature happened to sit at that row.
            //
            // Zero until the answer arrives, which draws nothing for a round
            // trip rather than drawing the wrong thing; COMPANION_UPDATE brings
            // the tab back when it lands.
            const uint32_t displayId = gh->getCreatureDisplayIdForEntry(c.creatureId);
            lua_pushnumber(L, displayId);
            lua_pushstring(L, c.name.c_str());
            lua_pushnumber(L, c.spellId);
            lua_pushstring(L, gh->getSpellIconPath(c.spellId).c_str());
            // Whether it is out. A mount is an aura on the player; a critter is
            // the pet that is following, and neither is tracked per companion -
            // so this reads from what is actually active rather than from a
            // flag nobody sets.
            bool active = false;
            for (const auto& a : gh->getPlayerAuras()) {
                if (a.spellId == c.spellId) { active = true; break; }
            }
            lua_pushboolean(L, active ? 1 : 0);
            return 5;
        }},
                // Counts, and the count is the whole point: each of these is
                // read straight into `for i = 1, X()`, where a nil limit is
                // not an empty loop but an error - "'for' limit must be a
                // number" - that takes down the handler around it. Unbound,
                // the fallback answered nil and the character sheet's title
                // list, the companion tab and the token frame each raised
                // rather than showing nothing.
                //
                // GetNumTitles() - how many title ids there are to ask about,
                // not how many are owned. paperdollframe.lua walks
                // 1..GetNumTitles() calling IsTitleKnown on each. The client
                // answers its CharTitles table's bound plus one (0x005165e0,
                // DAT_00ad337c + 1), and this install's table has 142 rows, so
                // that client asks about 1..143; this answers the three 64-bit
                // groups of PLAYER_FIELD_KNOWN_TITLES, 192, which is every bit
                // the mask can hold and 49 more than the table has rows for.
                // Asking about those 49 is harmless the way the loop is written:
                // GetTitleName is only reached behind IsTitleKnown, and the mask
                // has no bit set for a title that does not exist. It is the one
                // shape in which that would not be harmless - a realm setting a
                // bit with no row behind it - and then strtrim(GetTitleName(i))
                // raises on the nil, which is why the extra reach is recorded
                // here rather than tuned to 143.
                {"GetNumTitles", [](lua_State* L) -> int {
            lua_pushnumber(L, game::kKnownTitleCount); return 1;
        }},
                {"GetNumCompanions", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* kind = luaL_optstring(L, 1, "");
            const bool mounts = std::string(kind) == "MOUNT";
            lua_pushnumber(L, gh ? static_cast<double>(gh->getCompanions(mounts).size()) : 0.0);
            return 1;
        }},
                // The knowledge base is the server's FAQ, and there is no
                // server here answering for it. Its category dropdown is
                // reached from the "?" micro button beside the action bar, so
                // the raise was one click away rather than in a corner.
                {"KBSetup_GetCategoryCount",    lua_ReturnZero},
                {"KBSetup_GetSubCategoryCount", lua_ReturnZero},
                // The other thirteen, which the note above should have covered
                // and did not: two counts were bound and the rest of the same
                // window was not, so the "?" button still raised - on
                // KBSetup_BeginLoading, which KnowledgeBaseFrame_OnShow calls
                // before anything else.
                //
                // Every caller here guards, and guards on exactly what an
                // absent knowledge base should say. KnowledgeBaseFrame_Search
                // returns early unless KBSetup_IsLoaded, the MOTD and notice
                // are both `if ( x )`, and the article lists are walked by the
                // counts. Never loaded, nothing in it.
                {"KBSetup_IsLoaded",            lua_ReturnFalse},
                {"KBSetup_BeginLoading",        lua_ReturnNothing},
                {"KBQuery_BeginLoading",        lua_ReturnNothing},
                {"KBArticle_BeginLoading",      lua_ReturnNothing},
                {"KBSetup_GetArticleHeaderCount", lua_ReturnZero},
                {"KBSetup_GetTotalArticleCount",  lua_ReturnZero},
                {"KBQuery_GetArticleHeaderCount", lua_ReturnZero},
                {"KBQuery_GetTotalArticleCount",  lua_ReturnZero},
                {"KBSetup_GetCategoryData",     lua_ReturnNil},
                {"KBSetup_GetSubCategoryData",  lua_ReturnNil},
                {"KBArticle_GetData",           lua_ReturnNil},
                {"KBSystem_GetMOTD",            lua_ReturnNil},
                {"KBSystem_GetServerNotice",    lua_ReturnNil},
                // Three more counts read straight into a comparison or a
                // format, with the same result: the socketing window walks
                // `i <= numSockets`, the PvP frame formats the season number
                // into its off-season line, and the achievement comparison
                // concatenates its total. None of the three has data behind it
                // here - no socketing, no arena seasons, and no way to read
                // another player's achievements - so zero is what is true.
                // Three battlefield timers, all read as `X()/1000`. This
                // client has partial battlefield support - status, score,
                // winner and positions are all bound - so these are gaps in a
                // system a player reaches rather than one that does not exist,
                // and each raises the moment a battleground is queued for or
                // entered. Zero reads correctly at every call site: no time in
                // queue, no shutdown pending, no elapsed run time.
                // Both timers are read as X()/1000, so milliseconds. The
                // queue slot carries each in seconds and this answered zero for
                // both - a queue that always read as just-joined with no
                // estimate, which is the whole content of that window.
                {"GetBattlefieldTimeWaited", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 1));
            // Counted on from the server's last word, which comes about once
            // a minute - answered as it stood, the minimap tooltip's time in
            // queue sat still for a minute and then jumped.
            double ms = 0.0;
            if (gh && index >= 1 && index <= 3) {
                const auto& q = gh->getBgQueues()[static_cast<size_t>(index - 1)];
                ms = q.timeInQueueSec * 1000.0;
                if (q.statusId == 1 &&
                    q.queueTimeReceivedAt != std::chrono::steady_clock::time_point{}) {
                    ms += std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - q.queueTimeReceivedAt).count();
                }
            }
            lua_pushnumber(L, ms);
            return 1;
        }},
                {"GetBattlefieldEstimatedWaitTime", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 1));
            double ms = 0.0;
            if (gh && index >= 1 && index <= 3)
                ms = gh->getBgQueues()[static_cast<size_t>(index - 1)].avgWaitTimeSec * 1000.0;
            lua_pushnumber(L, ms);
            return 1;
        }},
                // CanJoinBattlefieldAsGroup() - whether the queue button offers
                // to take the party in.
                //
                // The server decides for itself when the request arrives; this
                // only says whether it is worth offering, which is a party the
                // player leads. Solo, there is no group to bring.
                {"CanJoinBattlefieldAsGroup", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { lua_pushboolean(L, 0); return 1; }
            const auto& pd = gh->getPartyData();
            const bool leadsAParty = !pd.isEmpty() && pd.leaderGuid == gh->getPlayerGuid();
            lua_pushboolean(L, leadsAParty ? 1 : 0);
            return 1;
        }},
                // The instance's own two clocks, answered in **milliseconds**:
                // battlefieldframe.lua keeps `GetBattlefieldInstanceExpiration()
                // /1000` as its shutdown timer (:303) and the scoreboard prints
                // SecondsToTime(GetBattlefieldInstanceRunTime()/1000) beside
                // "Time elapsed" (worldstateframe.lua:824), so each divides the
                // seconds out itself and answering seconds would count the
                // shutdown down a thousand times too fast.
                //
                // Both are stamps, not values: the status packet's two tail
                // dwords went onto the client's clock as `clock + remaining` and
                // `clock - elapsed` (0x0054ae40), which is what lets them keep
                // running on their own between the server's periodic ticks -
                // WoWee was answering nought for both, so a battle in progress
                // had no shutdown timer and a scoreboard time of 0:00.
                {"GetBattlefieldInstanceExpiration", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->battlefieldInstanceExpirationMs() : 0);
            return 1;
        }},
                {"GetBattlefieldInstanceRunTime", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->battlefieldInstanceRunTimeMs() : 0);
            return 1;
        }},
                // The aspect ratio the Mac options panel builds its recording
                // resolution from: `"640x"..floor(640*ratio)`. Answering the
                // window's own ratio is both truthful and what makes that read
                // 640x360 on a 16:9 display rather than raising.
                // The rest of the movie recorder, which this client does not
                // have. Not stubs standing in for something - "we are not
                // recording" is simply true, and the two toggles have nothing
                // to toggle.
                //
                // Bound because they hang off keybindings. Those are declared
                // platform="mac" and so should never be reachable here, but a
                // binding is dispatched by name at the moment a key is pressed
                // and answering nothing there raises in the key handler, which
                // is a bad place to find out the filter was not applied.
                {"MovieRecording_IsRecording",   lua_ReturnFalse},
                {"MovieRecording_IsCompressing", lua_ReturnFalse},
                {"MovieRecording_Toggle",        lua_ReturnNothing},
                {"MovieRecording_ToggleGUI",     lua_ReturnNothing},
                {"MovieRecording_Cancel",        lua_ReturnNothing},
                {"MovieRecording_GetAspectRatio", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            auto* win = svc ? svc->window : nullptr;
            const float w = win ? static_cast<float>(win->getWidth())  : 1920.0f;
            const float h = win ? static_cast<float>(win->getHeight()) : 1080.0f;
            lua_pushnumber(L, (w > 0.0f) ? (h / w) : 0.5625);
            return 1;
        }},
                // GetNumSockets answered zero here, which said every item has
                // no sockets. It is real now, and in lua_socket_api.cpp.
                {"GetPreviousArenaSeason",      lua_ReturnZero},
                // GetInstanceBootTimeRemaining() - the countdown on the
                // "you are not in this instance's group" dialog, which reads
                // it on show and hides itself when it is not positive.
                {"GetInstanceBootTimeRemaining", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->getInstanceBootTimeRemaining() : 0);
            return 1;
        }},
                // The tutorial popups. Both raise where they are called -
                // FlagTutorial from TutorialFrame_Update as a tutorial is
                // shown, IsTutorialFlagged from TutorialFrame_NewTutorial
                // before one is queued.
                {"FlagTutorial", [](lua_State* L) -> int {
            flagTutorial(static_cast<int>(luaL_optnumber(L, 1, 0)));
            return 0;
        }},
                // Walking the tutorials already seen. Both answer an id or
                // nothing, and nothing is what disables the button - so a
                // wrong answer here is a button that looks available and does
                // not move.
                // ClearTutorials() (0x00530820 -> 0x00530510): every one of
                // the sixty marked seen, and CMSG_TUTORIAL_CLEAR.
                {"ClearTutorials", [](lua_State* L) -> int {
            for (int id = 1; id <= 60; ++id) flagTutorial(id);
            if (auto* gh = getGameHandler(L)) gh->sendTutorialClear();
            return 0;
        }},
                {"GetNextCompleatedTutorial", [](lua_State* L) -> int {
            const int from = static_cast<int>(luaL_optnumber(L, 1, 0));
            for (int id : tutorialIds()) {
                if (id > from) { lua_pushnumber(L, id); return 1; }
            }
            return 0;
        }},
                {"GetPrevCompleatedTutorial", [](lua_State* L) -> int {
            const int from = static_cast<int>(luaL_optnumber(L, 1, 0));
            const auto ids = tutorialIds();
            for (int id : std::views::reverse(ids)) {
                if (id < from) { lua_pushnumber(L, id); return 1; }
            }
            return 0;
        }},
                {"IsTutorialFlagged", [](lua_State* L) -> int {
            lua_pushboolean(L, tutorialFlagged(
                static_cast<int>(luaL_optnumber(L, 1, 0))) ? 1 : 0);
            return 1;
        }},
                // GetDebugStats() fills the debug stats overlay, which is one
                // SetText of whatever string this hands back. Framerate is the
                // part of it this client actually knows.
                {"GetDebugStats", [](lua_State* L) -> int {
            const double fps = static_cast<double>(ImGui::GetIO().Framerate);
            char line[64];
            std::snprintf(line, sizeof(line), "%.1f fps", fps);
            lua_pushstring(L, line);
            return 1;
        }},
                // GMSurveyQuestion(index) (0x005ac750): the current survey's
                // question text, nil outside 1-10 or with none there.
                {"GMSurveyQuestion", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GMSurveyGetQuestion(index)");
            auto* gh = getGameHandler(L);
            const auto text = gh ? gh->gmSurveyQuestion(luaToClientInt(L, 1)) : std::nullopt;
            if (text) lua_pushstring(L, text->c_str()); else lua_pushnil(L);
            return 1;
        }},
                // GMSurveyAnswer(question, answer) (0x005ac7d0). Its usage
                // check passes when either argument is a number.
                {"GMSurveyAnswer", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1) && !lua_isnumber(L, 2))
                return luaL_error(L, "Usage: GMSurveyGetAnswer(questionIndex, answerIndex)");
            auto* gh = getGameHandler(L);
            const auto text = gh ? gh->gmSurveyAnswer(luaToClientInt(L, 1), luaToClientInt(L, 2))
                                 : std::nullopt;
            if (text) lua_pushstring(L, text->c_str()); else lua_pushnil(L);
            return 1;
        }},
                // GMSurveyNumAnswers(question) (0x005ac870).
                {"GMSurveyNumAnswers", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GMSurveyGetNumAnswers(questionIndex)");
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->gmSurveyNumAnswers(luaToClientInt(L, 1)) : 0);
            return 1;
        }},
                // GMSurveyAnswerSubmit(question, rank, comment) (0x005ac390):
                // questions 1-10, kept until GMSurveySubmit sends them.
                {"GMSurveyAnswerSubmit", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2) || !lua_isstring(L, 3))
                return luaL_error(L, "Usage: GMSurveyAnswerSubmit(question, rank, comment)");
            const long question = std::lround(lua_tonumber(L, 1));
            const long rank = std::lround(lua_tonumber(L, 2));
            if (question < 1 || question > 10)
                return luaL_error(L, "GMSurveyAnswerSubmit: Questions limited from %d to %d", 1, 10);
            if (auto* gh = getGameHandler(L))
                gh->setGmSurveyAnswer(static_cast<uint32_t>(question - 1), static_cast<uint8_t>(rank),
                                      lua_tostring(L, 3));
            return 0;
        }},
                // GMSurveyCommentSubmit(comment) (0x005ac480).
                {"GMSurveyCommentSubmit", [](lua_State* L) -> int {
            if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: GMSurveyCommentSubmit(comment)");
            if (auto* gh = getGameHandler(L)) gh->setGmSurveyComment(lua_tostring(L, 1));
            return 0;
        }},
                // GMSurveySubmit() (0x005ad230 -> 0x005acd50).
                {"GMSurveySubmit", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->submitGmSurvey();
            return 0;
        }},
                // Movie recording, which is a Mac client feature this build
                // does not have. Its siblings above already answer; these are
                // the ones MacOptionsFrame reaches as it loads, so each one
                // was a raise on opening the Mac options panel.
                {"MovieRecording_IsSupported",  lua_ReturnFalse},
                {"MovieRecording_IsCursorRecordingSupported", lua_ReturnFalse},
                {"MovieRecording_DataRate",     lua_ReturnZero},
                {"MovieRecording_MaxLength",    lua_ReturnZero},
                {"MovieRecording_GetMovieFullPath", [](lua_State* L) -> int {
            lua_pushstring(L, "");
            return 1;
        }},
                {"MovieRecording_GetViewportWidth", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            auto* win = svc ? svc->window : nullptr;
            lua_pushnumber(L, win ? win->getWidth() : 1920);
            return 1;
        }},
                // How many of a category the compared player has earned - one
                // value, which the summary puts straight into a bar and a
                // "n/total" label beside the player's own. Zero for everyone
                // drew their bar empty however much they had done.
                {"GetComparisonCategoryNumAchievements", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto category = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (!gh) { lua_pushnumber(L, 0); return 1; }
            const auto* set = gh->getInspectedPlayerAchievements(
                gh->getAchievementComparisonGuid());
            if (!set) { lua_pushnumber(L, 0); return 1; }
            gh->ensureAchievementCategoriesLoaded();
            uint32_t done = 0;
            for (uint32_t id : gh->getCategoryAchievements(category))
                if (set->count(id)) ++done;
            lua_pushnumber(L, done);
            return 1;
        }},
                {"GetMultiCastTotemSpells",  lua_ReturnNil},
                {"GetVoiceStatus",           lua_ReturnFalse},
                {"GetMuteStatus",            lua_ReturnFalse},
                {"GetActiveVoiceChannel",    lua_ReturnNil},
                {"GetVoiceCurrentSessionID", lua_ReturnNil},
                // GetPartyMember(1-4) -> 1 when that party slot holds someone,
                // else nil (0x0052c1d0). PartyMemberFrame_UpdateMember shows a
                // party frame on this and nothing else, so answering false for
                // every slot kept all four frames hidden in every group.
                {"GetPartyMember", [](lua_State* L) -> int {
            const int slot = lua_isnumber(L, 1) ? static_cast<int>(lua_tonumber(L, 1) + 0.5) : 0;
            if (slot < 1 || slot > 4) return luaL_error(L, "Usage: GetPartyMember(1-4)");
            auto* gh = getGameHandler(L);
            if (!gh || !gh->isInGroup() ||
                game::partySlotGuid(gh->getPartyData(), gh->getPlayerGuid(), slot) == 0)
                return luaReturnNil(L);
            lua_pushnumber(L, 1);
            return 1;
        }},
                // GetZonePVPInfo() → pvpType, isSubZonePvP, factionName
                //
                // minimap.lua unpacks three and answered nil for all of them,
                // so the zone name on the minimap was always the default
                // colour and its tooltip never said whose territory it was.
                // Middle value stays nil: it is for a sub-zone that differs
                // from its parent, which this reads at zone granularity.
                {"GetZonePVPInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { lua_pushnil(L); return 1; }
            const auto [type, faction] = gh->getZonePvpInfo(gh->getWorldStateZoneId());
            if (type.empty()) { lua_pushnil(L); return 1; }
            lua_pushstring(L, type.c_str());
            lua_pushnil(L);
            if (faction.empty()) lua_pushnil(L); else lua_pushstring(L, faction.c_str());
            return 3;
        }},
                {"GetMouseButtonClicked",    lua_ReturnNil},
                {"GetChatWindowSavedPosition",   lua_GetChatWindowSavedPosition},
                {"GetChatWindowSavedDimensions", lua_GetChatWindowSavedDimensions},
                {"SetChatWindowSavedPosition",   lua_SetChatWindowSavedPosition},
                {"SetChatWindowSavedDimensions", lua_SetChatWindowSavedDimensions},
                {"SetChatWindowSize",            lua_SetChatWindowSize},
                {"SetChatWindowColor",           lua_SetChatWindowColor},
                {"SetChatWindowAlpha",           lua_SetChatWindowAlpha},
                {"SetChatWindowShown",           lua_SetChatWindowShown},
                {"SetChatWindowLocked",          lua_SetChatWindowLocked},
                {"SetChatWindowDocked",          lua_SetChatWindowDocked},
                {"SetChatWindowUninteractable",  lua_SetChatWindowUninteractable},
                {"ResetChatWindows",             lua_ResetChatWindows},
                {"SetChatColorNameByClass",      lua_SetChatColorNameByClass},
                {"ResetChatColors",              lua_ResetChatColors},
                {"GetExistingLocales",       lua_ReturnNil},
                // Read from the real keyboard: a shift-click means something
                // different from a click, and FrameXML asks on every press.
                {"IsShiftKeyDown",           lua_IsShiftKeyDown},
                {"IsLeftShiftKeyDown",       lua_IsShiftKeyDown},
                {"IsRightShiftKeyDown",      lua_IsShiftKeyDown},
                {"IsControlKeyDown",         lua_IsControlKeyDown},
                {"IsLeftControlKeyDown",     lua_IsControlKeyDown},
                {"IsRightControlKeyDown",    lua_IsControlKeyDown},
                {"IsAltKeyDown",             lua_IsAltKeyDown},
                {"IsLeftAltKeyDown",         lua_IsAltKeyDown},
                {"IsRightAltKeyDown",        lua_IsAltKeyDown},
                {"IsModifierKeyDown",        lua_IsModifierKeyDown},
                // The four predicates actionbutton.lua asks about a slot.
                //
                // All answered false, and the pair below the first is why a
                // stack of potions on the bar showed no number: the count is
                // drawn only inside `if ( IsConsumableAction(action) or
                // IsStackableAction(action) )`, and GetActionCount underneath
                // it has been counting across every bag the whole time.
                {"IsAttackAction", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int slot = static_cast<int>(luaL_optnumber(L, 1, 0)) - 1;
            if (!gh || slot < 0) { lua_pushboolean(L, 0); return 1; }
            const auto& bar = gh->getActionBar();
            // 6603 is Auto Attack, the one action that flashes the button red
            // for as long as the swing keeps going.
            constexpr uint32_t kAutoAttack = game::SPELL_ID_ATTACK;
            lua_pushboolean(L, slot < static_cast<int>(bar.size()) &&
                               bar[slot].type == game::ActionBarSlot::SPELL &&
                               bar[slot].id == kAutoAttack);
            return 1;
        }},
                {"IsConsumableAction", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int slot = static_cast<int>(luaL_optnumber(L, 1, 0)) - 1;
            if (!gh || slot < 0) { lua_pushboolean(L, 0); return 1; }
            const auto& bar = gh->getActionBar();
            bool consumable = false;
            if (slot < static_cast<int>(bar.size()) &&
                bar[slot].type == game::ActionBarSlot::ITEM) {
                const auto* info = gh->getItemInfo(bar[slot].id);
                consumable = info && info->valid && info->itemClass == 0;  // Consumable
            }
            lua_pushboolean(L, consumable);
            return 1;
        }},
                {"IsEquippedAction", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int slot = static_cast<int>(luaL_optnumber(L, 1, 0)) - 1;
            if (!gh || slot < 0) { lua_pushboolean(L, 0); return 1; }
            const auto& bar = gh->getActionBar();
            bool worn = false;
            if (slot < static_cast<int>(bar.size()) &&
                bar[slot].type == game::ActionBarSlot::ITEM) {
                const auto& inv = gh->getInventory();
                for (int e = 0; e < game::Inventory::NUM_EQUIP_SLOTS && !worn; ++e) {
                    const auto& s = inv.getEquipSlot(static_cast<game::EquipSlot>(e));
                    worn = !s.empty() && s.item.itemId == bar[slot].id;
                }
            }
            lua_pushboolean(L, worn);
            return 1;
        }},
                {"IsStackableAction", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int slot = static_cast<int>(luaL_optnumber(L, 1, 0)) - 1;
            if (!gh || slot < 0) { lua_pushboolean(L, 0); return 1; }
            const auto& bar = gh->getActionBar();
            bool stackable = false;
            if (slot < static_cast<int>(bar.size()) &&
                bar[slot].type == game::ActionBarSlot::ITEM) {
                const auto* info = gh->getItemInfo(bar[slot].id);
                stackable = info && info->valid && info->maxStack > 1;
            }
            lua_pushboolean(L, stackable);
            return 1;
        }},
                {"IsFlyableArea",            lua_IsFlyableArea},
                // The renderer knows whether the camera is inside a WMO, and
                // the macro conditionals [indoors] / [outdoors] have read it
                // all along. These two answered a flat false and a flat true.
                {"IsIndoors", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            lua_pushboolean(L, svc && svc->isPlayerIndoors && svc->isPlayerIndoors());
            return 1;
        }},
                {"IsOutdoors", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            lua_pushboolean(L, !(svc && svc->isPlayerIndoors && svc->isPlayerIndoors()));
            return 1;
        }},
                // IsPossessBarVisible() (0x005a8820): the possess bar's spell
                // (0xc234e0) is there and applies no CONTROL_VEHICLE (236,
                // 0x007fde50) - a vehicle has its own bar.
                {"IsPossessBarVisible", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t spellId = gh ? gh->getPossessSpellId() : 0;
            bool visible = false;
            if (spellId != 0) {
                visible = true;
                gh->getSpellName(spellId);  // fills the cache
                const auto it = gh->spellNameCacheRef().find(spellId);
                if (it != gh->spellNameCacheRef().end())
                    for (uint32_t type : it->second.effectAuraIds)
                        if (type == 236) visible = false;
            }
            lua_pushboolean(L, visible ? 1 : 0);
            return 1;
        }},
                // GetPossessInfo(index) → texture, name, enabled (0x005d5820),
                // of the possess bar's spell, with a player and the spell
                // there. 1 is the spell: its ActiveIconID's icon, else its
                // SpellIconID's. 2 is the way out: SpellIcon 693's icon, the
                // spell's name, and enabled unless the spell cannot be
                // cancelled (Attributes 0x80000000). Anything else, three nils.
                {"GetPossessInfo", [](lua_State* L) -> int {
            const long index = std::lround(lua_isnumber(L, 1) ? lua_tonumber(L, 1) : 0.0);
            if (index == 0) return luaL_error(L, "Usage: GetPossessInfo(index)");
            auto* gh = getGameHandler(L);
            const uint32_t spellId = gh ? gh->getPossessSpellId() : 0;
            if (gh && spellId != 0 && gh->getEntityManager().getEntity(gh->getPlayerGuid())) {
                const std::string name = gh->getSpellName(spellId);
                const auto it = gh->spellNameCacheRef().find(spellId);
                const bool known = it != gh->spellNameCacheRef().end();
                if (index == 1 && known) {
                    const std::string icon = it->second.activeIconId != 0
                        ? gh->getIconPath(it->second.activeIconId) : gh->getSpellIconPath(spellId);
                    if (icon.empty()) lua_pushnil(L); else lua_pushstring(L, icon.c_str());
                    lua_pushstring(L, name.c_str());
                    lua_pushboolean(L, 1);
                    return 3;
                }
                if (index == 2) {
                    constexpr uint32_t kCancelIcon = 0x2b5;
                    const std::string icon = gh->getIconPath(kCancelIcon);
                    if (icon.empty()) lua_pushnil(L); else lua_pushstring(L, icon.c_str());
                    const auto attrs = gh->getSpellAttributes(spellId);
                    lua_pushstring(L, name.c_str());
                    lua_pushboolean(L, attrs && (*attrs & 0x80000000u) != 0 ? 0 : 1);
                    return 3;
                }
            }
            lua_pushnil(L);
            lua_pushnil(L);
            lua_pushnil(L);
            return 3;
        }},
                // IsRaidOfficer() - whether this player is an assistant.
                //
                // The same question UnitIsRaidOfficer already answers for
                // anyone else, and off the same bit: MEMBER_FLAG_ASSISTANT is
                // 0x01 in AzerothCore's Group.h, and the party members carry
                // their flags. Only the player's own answer was hardcoded no.
                //
                // It gates the assistant-only entries on the unit menus and,
                // with IsPartyLeader beside it, whether the chat frame offers
                // to send a raid warning at all.
                {"IsRaidOfficer", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { lua_pushboolean(L, 0); return 1; }
            const uint64_t self = gh->getPlayerGuid();
            constexpr uint8_t kMemberFlagAssistant = 0x01;
            for (const auto& mem : gh->getPartyData().members) {
                if (mem.guid == self) {
                    lua_pushboolean(L, (mem.flags & kMemberFlagAssistant) ? 1 : 0);
                    return 1;
                }
            }
            lua_pushboolean(L, 0);
            return 1;
        }},
                // IsReferAFriendLinked moved to lua_unit_api.cpp: it is one bit
                // of UNIT_DYNAMIC_FLAGS (0x40) read off the guid's own object.
                {"IsStereoVideoAvailable",   lua_ReturnFalse},
                {"IsVoiceChatEnabled",       lua_ReturnFalse},
                // False always, which is what disabled the zoom-out button at
                // every level of the map. The only way back out of a zone was
                // the right-click that reaches the same handler without
                // asking this first.
                {"IsZoomOutAvailable", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            const bool can = svc && svc->canZoomMapOut && svc->canZoomMapOut();
            lua_pushboolean(L, can ? 1 : 0);
            return 1;
        }},
                // False, which is what the client answers - a shared 3-byte
                // `return 0` for this name and GetMapDebugObjectInfo; the note
                // sits at GetNumMapDebugObjects.
                {"HasDebugZoneMap",          lua_ReturnFalse},
                // IsDesaturateSupported() (0x00511fb0): 1 where the device
                // can draw a texture desaturated (0x00483290), as every one
                // here does; nothing otherwise.
                {"IsDesaturateSupported", [](lua_State* L) -> int {
            lua_pushnumber(L, 1);
            return 1;
        }},
                // Whether Wintergrasp queueing is on offer, which is the
                // battleground frame's timer icon and its tooltip. 0x00543600
                // answers the number 1 when world state 4375 holds a non-zero
                // value and nil otherwise - never false, which is what stood
                // here; the interface only tests it for truth, so both read
                // the same there, but the world state is the thing deciding it.
                {"CanQueueForWintergrasp", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh || gh->getWorldState(game::world_state_ui::kWintergraspQueueState).value_or(0) == 0)
                return luaReturnNil(L);
            lua_pushnumber(L, 1.0);
            return 1;
        }},
                {"ConvertToRaid",            lua_ReturnNothing},
                {"FillLocalizedClassList",   lua_ReturnNothing},
                // Sends the request rather than doing nothing. The reply is
                // parsed, fires GUILD_EVENT_LOG_UPDATE and friendsframe
                // listens for it, so answering nothing here was the one link
                // missing at this end - and FrameXML's own call to it is
                // commented out, which is the link missing at the other.
                {"QueryGuildEventLog", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->requestGuildEventLog();
            return 0;
        }},
                {"RegisterForSave",          lua_RegisterForSave},
                {"RegisterForSavePerCharacter", lua_RegisterForSavePerCharacter},
                {"RegisterStaticConstants",  lua_ReturnNothing},
                {"SetChatWindowName",        lua_SetChatWindowName},
                {"SetupFullscreenScale",     lua_SetupFullscreenScale},
                // DropCursorMoney is real now, and lives with the rest of the
                // money cursor in lua_inventory_api.cpp. Two registrations of
                // one name would be settled by load order.
                // AchievementMicroButton_Update() - called by the achievement
                // addon and defined nowhere. mainmenubarmicrobuttons.lua has
                // AchievementMicroButton_OnEvent but not this, so it is a hole
                // in this FrameXML rather than a binding this client owes. A
                // no-op, because what it would do is show a micro button that
                // is hidden, and leaving it hidden is the honest outcome.
                {"AchievementMicroButton_Update", [](lua_State* L) -> int {
            (void)L; return 0; }},
                {"BNFeaturesEnabled",        lua_ReturnFalse},
                {"BNFeaturesEnabledAndConnected", lua_ReturnFalse},
                {"BNGetMaxPlayersInConversation", lua_ReturnZero},
                {"GetSummonFriendCooldown",  lua_ReturnNoCooldown},
                {"GetScreenResolutions",     lua_GetScreenResolutions},
                {"GetCurrentResolution",     lua_GetCurrentResolution},
                // Counts a loop bounds itself with. FrameXML writes
                // "for i = 0, num-1" straight after asking, so nothing is not
                // an answer - it is arithmetic on nil and the file is lost.
                // The channel list panel walks these two, and both answered
                // "there are none" while the client knew exactly which
                // channels the player had joined - GetChannelList reports them
                // from the same vector. A stub saying empty is how a working
                // panel shows nothing.
                {"GetNumDisplayChannels", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? static_cast<double>(gh->getJoinedChannels().size()) : 0.0);
            return 1;
        }},
                {"GetNumMapOverlays",        lua_GetNumMapOverlays},
                // The debug-object trio (this, GetMapDebugObjectInfo and
                // HasDebugZoneMap) - and the reference answers "none" for all
                // three, in the shipped binary and not by any table:
                // GetNumMapDebugObjects is one call that pushes the number 0
                // (0x00608560), and a single 3-byte `return 0` answers the other
                // two names, reached from both entries of its method table
                // (0x008e5250 at 0x00acd02c and 0x00acd03c). Nothing anywhere in
                // the executable names a table they could read, and none is in
                // this install's dbfilesclient, so worldmapframe.lua's
                // NUM_WORLDMAP_DEBUG_OBJECTS loop - which fills the frames it
                // then walks for a pin and a tooltip - never ran there either.
                // Zero is not a stand-in here; it is what the client says.
                {"GetNumMapDebugObjects",    lua_ReturnZero},
                // How many team mates the battleground has reported. The loop
                // that draws them does not use this - it walks to
                // MAX_RAID_MEMBERS and hides whatever answers zero - but the
                // count is asked for beside it and a flat zero said the
                // positions were not there while they were.
                {"GetNumBattlefieldPositions", [](lua_State* L) -> int {
            lua_pushnumber(L, bgCount(getGameHandler(L), 0));
            return 1;
        }},
                {"GetBattlefieldPosition",   lua_GetBattlefieldPosition},
                // Both of these want a position normalised to the map frame
                // currently on screen, which only the map that is drawing knows
                // - and this client draws its own, so FrameXML's WorldMapFrame
                // is suppressed and neither is reached.
                //
                // Zero is not a placeholder here, it is the answer: both call
                // sites read `if ( x == 0 and y == 0 )` and hide the marker.
                // The corpse and the spirit healer are drawn by this client's
                // own map, from getCorpseCanonicalPos and
                // getDeathReleaseLocation. Handing the world map over means
                // giving these the projection, not just filling them in.
                {"GetCorpseMapPosition",     lua_GetBattlefieldPosition},
                {"GetDeathReleasePosition",  lua_GetBattlefieldPosition},
                {"GetNumBattlefieldVehicles", lua_GetNumBattlefieldVehicles},
                {"GetBattlefieldVehicleInfo", lua_GetBattlefieldVehicleInfo},
                {"GetChatWindowInfo",        lua_GetChatWindowInfo},
                // What a chat window listens to, and the whole reason chat
                // shows anything. ChatFrame_OnLoad registers a chat frame for
                // no CHAT_MSG_ event at all - every one of them comes from
                // ChatFrame_RegisterForMessages walking this list on
                // UPDATE_CHAT_WINDOWS. Answering nothing registered nothing,
                // so every message the client parsed, coloured and routed
                // arrived at a frame that had not asked for it: the chat
                // window stayed empty from login to logout.
                {"GetChatWindowMessages", [](lua_State* L) -> int {
            const ChatWindowSettings* w = chatWindow(L, 1);
            if (!w) return 0;
            // An empty General is unset, not configured, and answers the
            // defaults - which is what the real client does and what FrameXML
            // is written against.
            //
            // FCF_ResetChatWindows calls ChatFrame_RemoveAllMessageGroups on
            // ChatFrame1 and never adds anything back; the defaults it expects
            // to reappear come from the client. Ours took the removals
            // literally, wrote "groups=" empty to disk, and answered nothing
            // ever after - so ChatFrame_RegisterForMessages registered for no
            // event at all and every line of chat was filtered out. The window
            // was not blank because nothing arrived: it was blank because
            // nothing was listening.
            //
            // Only window one, and only when it holds nothing. A window the
            // player has emptied one group at a time is a real setting and is
            // left alone; General with none at all is the wipe above.
            if (w->messageGroups.empty() && lua_tonumber(L, 1) == 1) {
                for (const char* g : kDefaultChatGroups) lua_pushstring(L, g);
                return static_cast<int>(std::size(kDefaultChatGroups));
            }
            for (const auto& g : w->messageGroups) lua_pushstring(L, g.c_str());
            return static_cast<int>(w->messageGroups.size());
        }},
                // Name and zone-channel id in pairs, which is the shape
                // ChatFrame_RegisterForChannels reads: it steps two at a time
                // and fills channelList and zoneChannelList together.
                {"GetChatWindowChannels", [](lua_State* L) -> int {
            ChatWindowSettings* w = chatWindow(L, 1);
            if (!w) return 0;
            // Same rule, same reason: ChatFrame_RemoveAllChannels runs beside
            // the group wipe in FCF_ResetChatWindows, and a General with no
            // channels matched every channel line against nothing.
            settleWindowChannels(L, *w, lua_tonumber(L, 1) == 1);
            for (const auto& c : w->channels) {
                lua_pushstring(L, c.first.c_str());
                lua_pushnumber(L, c.second);
            }
            return static_cast<int>(w->channels.size()) * 2;
        }},
                // The four the chat settings panel edits. Ticking a message
                // group or joining a channel goes through these, and they did
                // nothing - so a change made in the panel lasted until the
                // frame was next asked what it listened to, which is the next
                // login, and then went back.
                {"AddChatWindowMessages", [](lua_State* L) -> int {
            ChatWindowSettings* w = chatWindow(L, 1);
            const char* group = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;
            if (!w || !group) return 0;
            for (const auto& g : w->messageGroups) if (g == group) return 0;
            w->messageGroups.emplace_back(group);
            saveInterfaceState();
            return 0;
        }},
                {"RemoveChatWindowMessages", [](lua_State* L) -> int {
            ChatWindowSettings* w = chatWindow(L, 1);
            const char* group = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;
            if (!w || !group) return 0;
            auto& v = w->messageGroups;
            v.erase(std::remove(v.begin(), v.end(), std::string(group)), v.end());
            saveInterfaceState();
            return 0;
        }},
                // Answers the zone channel id, and has to: ChatFrame_AddChannel
                // puts the channel into the frame's own list only `if
                // ( zoneChannel )`, so returning nothing meant the channel was
                // recorded here and never reached the frame that shows it.
                // Zero is a real answer for a channel that is not zone based,
                // and passes that test - zero being true in Lua.
                {"AddChatWindowChannel", [](lua_State* L) -> int {
            ChatWindowSettings* w = chatWindow(L, 1);
            const char* name = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;
            if (!w || !name) return 0;
            settleWindowChannels(L, *w, lua_tonumber(L, 1) == 1);
            // A zone channel's shortcut answers its ChatChannels.dbc id, which
            // is what the frame finds its lines by.
            int zone = static_cast<int>(luaL_optnumber(L, 3, 0));
            if (auto* gh = getGameHandler(L); gh && gh->getChatHandler()) {
                for (const auto& r : gh->getChatHandler()->channelRows())
                    if (game::chat_channels::sameName(r.shortcut, name)) zone = static_cast<int>(r.id);
            }
            for (auto& c : w->channels) {
                if (c.first != name) continue;
                if (zone != 0) c.second = zone;
                lua_pushnumber(L, c.second);
                return 1;
            }
            w->channels.emplace_back(name, zone);
            saveInterfaceState();
            lua_pushnumber(L, zone);
            return 1;
        }},
                {"RemoveChatWindowChannel", [](lua_State* L) -> int {
            ChatWindowSettings* w = chatWindow(L, 1);
            const char* name = lua_isstring(L, 2) ? lua_tostring(L, 2) : nullptr;
            if (!w || !name) return 0;
            auto& v = w->channels;
            v.erase(std::remove_if(v.begin(), v.end(),
                        [&](const std::pair<std::string, int>& c) { return c.first == name; }),
                    v.end());
            saveInterfaceState();
            return 0;
        }},
                {"GetNumBattlefieldFlagPositions", [](lua_State* L) -> int {
            // Only when both are carried, for the reason the accessor gives:
            // one carrier cannot be told from the other, and the loop this
            // bounds draws a named flag texture per entry.
            auto* gh = getGameHandler(L);
            const int n = bgCount(gh, 1);
            lua_pushnumber(L, n == 2 ? 2 : 0);
            return 1;
        }},
                {"GetBattlefieldFlagPosition",     lua_GetBattlefieldFlagPosition},
                // Time left on a loot roll that is not running, which
                // GroupLootFrame compares against a bar range at once.
                // How many levels the map being shown has, which is all that
                // shows or hides the world map's level dropdown and its two
                // arrows (WorldMapLevelDropDown_Update). Answering none had both
                // hidden everywhere - including on Northrend, whose map has two.
                //
                // 0x00546190 counts the distinct floor numbers of every
                // DungeonMap.dbc row on the shown area's *map* - the floors
                // belong to the map, not to the area, so every Northrend zone
                // answers two and a Kalimdor zone none - and adds a level of its
                // own for the areas whose WorldMapArea default floor is the -1
                // that means "a surface sits above these floors".
                {"GetNumDungeonMapLevels", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            lua_pushnumber(L, svc && svc->getMapDungeonLevelCount
                                 ? svc->getMapDungeonLevelCount() : 0);
            return 1;
        }},
                // Which page a multi-cast button reads its actions from, added
                // to NUM_ACTIONBAR_PAGES by ActionButton_CalculateAction for a
                // button whose buttonType is MULTICASTACTIONBUTTON
                // (actionbutton.lua:146). It is not a state and was never read
                // as one: the client's binding (0x005b71e0) is one fldl of the
                // double 6.0 at 0x00a0b708 and a push of it - nothing read
                // first. So those nine buttons sit on page 12, slots 133..141,
                // bar on screen or not; the zero here did not mean "no
                // multi-cast bar", it put them on page 6, on top of the main
                // bar's own buttons. WoWee draws that bar itself (the Totems
                // takeover covers MultiCastActionBarFrame), so this only
                // decides where a FrameXML-side button looks.
                //
                // The same address is what the table hands GetNumGlyphSockets,
                // whose body here is six as well (lua_quest_api.cpp): the
                // linker folded two bindings that each push the one pooled
                // 6.0, so both sixes are the client's own number and not a
                // count either of them works out.
                {"GetMultiCastBarOffset", [](lua_State* L) -> int {
            lua_pushnumber(L, 6.0);
            return 1;
        }},
                // Which extra action bar the current form or stance uses.
                // actionbutton.lua adds it to NUM_ACTIONBAR_PAGES to pick the
                // page a bonus button reads from, so a flat zero left a druid
                // or a warrior looking at the wrong page in every form.
                //
                // 0x005a7f20 answers DAT_00c1e59c, which 0x005a83c0 sets from
                // the form's SpellShapeshiftForm.dbc BonusActionBar (5 for the
                // pet's actions or a form's preset spells).
                {"GetBonusBarOffset", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? static_cast<double>(gh->getBonusActionBarOffset()) : 0.0);
            return 1;
        }},
                {"GetNumBattlegroundTypes",  lua_GetNumBattlegroundTypes},
                {"GetBattlegroundInfo",      lua_GetBattlegroundInfo},
                {"RequestBattlegroundInstanceInfo", lua_RequestBattlegroundInstanceInfo},
                // Which of those levels is showing, one-based, or zero for
                // none - the number the dropdown lights, the quest log passes on
                // to open a map (questlogframe.lua) and the battlefield minimap
                // picks its art with. 0x00544fc0 reads the DungeonMap row the
                // map has selected and answers its floor, shifted up by one for
                // the areas that carry a surface level - where no selection
                // answers one rather than none, which is what the arrows count
                // on: they step from one to two and never back off the bottom.
                {"GetCurrentMapDungeonLevel", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            lua_pushnumber(L, svc && svc->getMapDungeonLevel
                                 ? svc->getMapDungeonLevel() : 0);
            return 1;
        }},
                {"Sound_GameSystem_GetNumOutputDrivers", lua_Sound_GetNumOutputDrivers},
                {"Sound_ChatSystem_GetNumInputDrivers",  lua_ReturnZero},
                {"Sound_ChatSystem_GetNumOutputDrivers", lua_ReturnZero},
                {"Sound_ChatSystem_GetInputDriverNameByIndex",  lua_ReturnNil},
                {"Sound_ChatSystem_GetOutputDriverNameByIndex", lua_ReturnNil},
                // Nothing selected, which is a number rather than
                // nothing: SkillFrame passes the result straight to
                // GetSkillLineInfo as an index.
                // Which row the skill list has selected. The client has no
                // opinion about it - it is what the player last clicked - so
                // it is held here, the way the friends and ignore lists are.
                //
                // Answering a constant zero meant no row ever matched, because
                // the list is one-based: SkillFrame_SetStatusBar compares each
                // row against this to decide which border to light, so nothing
                // ever looked selected however many times it was clicked.
                {"GetSelectedSkill", [](lua_State* L) -> int {
            lua_pushnumber(L, selectedSkill()); return 1; }},
                // Whether the floors being named sit under a surface level,
                // which decides whether the level dropdown names its entries by
                // the floor or by one below it (WorldMapLevelDropDown_Initialize
                // looks the label up as DUNGEON_FLOOR_<map><n> either way).
                // 0x00544c40 asks one question of the area being shown: is its
                // default floor the -1 that also adds a level above the floors.
                // Two maps in 3.3.5 answer it, and the rest answer nothing - not
                // false, which is what a flat answer here said for all of them.
                {"DungeonUsesTerrainMap", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            if (svc && svc->mapDungeonUsesTerrainMap && svc->mapDungeonUsesTerrainMap()) {
                lua_pushboolean(L, 1);
                return 1;
            }
            return 0;
        }},
                // GetChannelDisplayInfo(i) → name, header, collapsed,
                //   channelNumber, count, active, category, voiceEnabled,
                //   voiceActive
                //
                // header is FALSE, not "". An empty string is true in Lua, so
                // returning one marks every channel as a category header:
                // channelframe.lua does `if ( self.header )` and would draw
                // each row as a heading, then call ExpandChannelHeader on it.
                {"GetChannelDisplayInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (!gh || index < 1) { lua_pushnil(L); return 1; }
            const auto& joined = gh->getJoinedChannels();
            if (index > static_cast<int>(joined.size())) { lua_pushnil(L); return 1; }
            lua_pushstring(L, joined[static_cast<size_t>(index) - 1].c_str());
            lua_pushboolean(L, 0);      // header - flat list, no categories
            lua_pushboolean(L, 0);      // collapsed
            lua_pushnumber(L, index);   // channelNumber
            // The roster arrives with SMSG_CHANNEL_LIST, which the panel asks
            // for by calling GetNumChannelMembers on the row it is drawing.
            lua_pushnumber(L, static_cast<lua_Number>(
                gh->getChannelRoster(joined[static_cast<size_t>(index) - 1]).size()));
            lua_pushboolean(L, 1);      // active
            lua_pushstring(L, "CHANNEL_CATEGORY_CUSTOM");
            lua_pushboolean(L, 0);      // voiceEnabled
            lua_pushboolean(L, 0);      // voiceActive
            return 9;
        }},
                // Whether the unit frames should draw their threat
                // indicator, which is the threatWarning CVar read against
                // where the player is. A flat false meant the Display panel's
                // aggro-warning dropdown set a value nothing ever looked at.
                {"IsThreatWarningEnabled", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int setting = 3;
            if (auto it = cvarStore().find("threatwarning"); it != cvarStore().end()) {
                try { setting = std::stoi(it->second); } catch (const std::exception&) {}
            }
            bool on = false;
            switch (setting) {
                case 1: on = gh && gh->isInInstance(); break;
                case 2: on = gh && !gh->getPartyData().members.empty(); break;
                case 3: on = true; break;
                default: on = false; break;   // 0, and anything unrecognised
            }
            lua_pushboolean(L, on ? 1 : 0);
            return 1;
        }},
                // IsAutoRepeatAction(slot) - the button flashes for as long as
                // an auto-repeat is running. There are exactly two in 3.3.5,
                // Auto Shot and the wand's Shoot, which is how IsAttackAction
                // beside it identifies auto-attack: by id rather than by an
                // attribute word this client does not cache.
                {"IsAutoRepeatAction", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int slot = static_cast<int>(luaL_optnumber(L, 1, 0)) - 1;
            bool repeating = false;
            if (gh && slot >= 0) {
                const auto& bar = gh->getActionBar();
                if (slot < static_cast<int>(bar.size()) &&
                    bar[slot].type == game::ActionBarSlot::SPELL) {
                    constexpr uint32_t kAutoShot = 75, kShoot = 5019;
                    repeating = bar[slot].id == kAutoShot || bar[slot].id == kShoot;
                }
            }
            lua_pushboolean(L, repeating);
            return 1;
        }},
                // No, and deliberately no even on macOS, which this client does
                // run on. It gates two things. The first is the game menu's Mac
                // Options button, and the panel behind it is Blizzard's movie
                // recorder and compression settings - every one of which this
                // client has nothing behind, so answering yes would add a
                // button that opens a window of dead controls. The second is
                // the Mac spelling of key names in binding text, which is
                // cosmetic and wrong in the smaller direction.
                //
                // Turn this on with the recorder, not before it.
                // nil, 0x00510b90 - the same function the Windows build
                // registers for IsLinuxClient.
                {"IsMacClient",              lua_ReturnNil},
                {"IsLinuxClient",            lua_ReturnNil},
                // IsWindowsClient() (0x004dce40): 1. IsDebugBuild()
                // (0x0050fe80): false.
                {"IsWindowsClient", [](lua_State* L) -> int { lua_pushnumber(L, 1); return 1; }},
                {"IsDebugBuild",             lua_ReturnFalse},
                // GameMovieFinished() (0x0051d590 -> 0x00518870): only while a
                // movie plays (0x00bd0800), which none does here -
                // SMSG_TRIGGER_MOVIE is answered with CMSG_COMPLETE_MOVIE at
                // once - so nothing.
                {"GameMovieFinished",        lua_ReturnNothing},
                // debuglocals([level]) (0x00819a60): the locals only inside the
                // error handler's call (0x00d3f79c, set around it by
                // 0x00819730), which this client never makes - so nothing.
                {"debuglocals",              lua_ReturnNothing},
                // ReplaceTradeEnchant() (0x00510b80 -> 0x0080c5f0): only with
                // an enchant waiting on the trade window's slot (0x00d3f4e0
                // 0x4010), which this client never puts there - so nothing.
                {"ReplaceTradeEnchant",      lua_ReturnNothing},
                // IsPartyLeader() - whether *this* player leads the group.
                //
                // The client has known this all along: the party data carries a
                // leader guid and PARTY_LEADER_CHANGED is fired when it moves.
                // Answering a flat false told FrameXML the player never leads,
                // which is what gates the leader-only entries on the unit
                // right-click menus and the loot-method controls.
                {"IsPartyLeader", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { lua_pushboolean(L, 0); return 1; }
            const uint64_t leader = gh->getPartyData().leaderGuid;
            lua_pushboolean(L, leader != 0 && leader == gh->getPlayerGuid());
            return 1;
        }},
                {"UnitFactionGroup",         lua_UnitFactionGroup},
                // HasPetSpells() → numSpells, petToken
                //
                // Answering nil meant the pet tab was never set up, so a hunter
                // or a warlock with a pet out had no pet spell book at all -
                // SpellBookFrame_Update only calls SpellBookFrame_SetTabType
                // for it when this says there are spells.
                //
                // Both values or neither: the tab's label is built as
                // _G["PET_TYPE_"..token], which raises on a nil token. Only two
                // of those globals exist, DEMON and PET, and WoW picks the
                // first for warlocks and the second for everyone else.
                {"HasPetSpells", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { lua_pushnil(L); return 1; }
            const auto& spells = gh->getPetSpells();
            if (spells.empty()) { lua_pushnil(L); return 1; }
            lua_pushnumber(L, static_cast<lua_Number>(spells.size()));
            constexpr uint8_t kWarlock = 9;
            lua_pushstring(L, gh->getPlayerClass() == kWarlock ? "DEMON" : "PET");
            return 2;
        }},
                // The death knight rune bar, which this client has tracked
                // since it started parsing rune state and never answered for.
                // runeframe.lua is reached through the player frame, handed
                // over by default, so a death knight has been looking at six
                // runes drawn from a nil type and no cooldown at all.
                //
                // RuneType here is Blood, Unholy, Frost, Death from zero;
                // runeframe.lua numbers the same four from one.
                {"GetRuneType", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int id = static_cast<int>(luaL_optinteger(L, 1, 0));
            if (!gh || id < 1 || id > game::runes::kMax) { lua_pushnil(L); return 1; }
            lua_pushinteger(L, static_cast<lua_Integer>(gh->getPlayerRunes().type[static_cast<size_t>(id) - 1]) + 1);
            return 1;
        }},
                // GetRuneCooldown(id) -> start, duration, runeReady
                // (0x00613020): when its cooldown began, in seconds of
                // GetTime's clock, the inverse of its type's regen rate
                // (PLAYER_RUNE_REGEN, 0 under 0.01), and whether it has none
                // running.
                {"GetRuneCooldown", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int id = static_cast<int>(luaL_optinteger(L, 1, 0));
            if (!gh || id < 1 || id > game::runes::kMax) return 0;
            const auto& runes = gh->getPlayerRunes();
            const auto i = static_cast<size_t>(id) - 1;
            const float rate = gh->runeRegen()[runes.type[i] & 3];
            lua_pushnumber(L, static_cast<double>(runes.start[i]) * 0.001);
            lua_pushnumber(L, rate <= 0.01f ? 0.0 : 1.0 / rate);
            lua_pushboolean(L, runes.start[i] == 0);
            return 3;
        }},
                // GetRuneCount(id) (0x00613140): 1 for a ready rune, else 0.
                {"GetRuneCount", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int id = static_cast<int>(luaL_optinteger(L, 1, 0));
            if (!gh || id < 1 || id > game::runes::kMax) return 0;
            lua_pushnumber(L, (gh->getPlayerRunes().ready & (1u << (id - 1))) != 0 ? 1.0 : 0.0);
            return 1;
        }},
                // The selection is the panel's own state and nothing else
                // reads it, so it lives here and round-trips. Answering nil
                // for the getter meant the highlight never moved.
                {"GetSelectedDisplayChannel", [](lua_State* L) -> int {
            lua_pushnumber(L, selectedDisplayChannel());
            return 1;
        }},
                // Whether the player owns the channel that panel has selected.
                // It answered a flat false, and that is the whole of whether
                // Make Moderator and Remove Moderator appear on the unit menu -
                // so ChannelModerator and ChannelUnmoderator were built and
                // could not be reached from the place that names them.
                //
                // Ownership comes from SMSG_CHANNEL_NOTIFY's OWNER_CHANGED,
                // whose guid is the new owner's: Channel::MakeOwnerChanged
                // writes _ownerGUID into it. The index is the panel's own
                // one-based one, the same GetChannelDisplayInfo reads.
                {"IsDisplayChannelOwner", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int sel = selectedDisplayChannel();
            if (!gh || sel < 1) { lua_pushboolean(L, 0); return 1; }
            const auto& joined = gh->getJoinedChannels();
            if (sel > static_cast<int>(joined.size())) { lua_pushboolean(L, 0); return 1; }
            lua_pushboolean(L, gh->ownsChatChannel(joined[static_cast<size_t>(sel) - 1]) ? 1 : 0);
            return 1;
        }},
                // IsDisplayChannelModerator() (0x004ffae0): the same channel
                // lookup as the binding above and one flag test wider - the
                // byte is read as & 3 instead of & 1, so the owner counts here.
                //
                // The byte itself is the local player's own row of that
                // channel's roster: the roster walk copies the whole thing, wire
                // flags and the three bits the client ORs on for itself at
                // 0x20/0x40/0x80 (00500000.c:3186-3220, none of which & 1 or & 3
                // can see), and does it into one global only for the row whose
                // guid pair matches the local player's. Whose channel that byte
                // describes is therefore whichever roster arrived last, and a
                // channel switch can leave it behind; reading the selected
                // channel's own roster says what is meant and cannot go stale.
                // The roster only exists once something asked for it, which the
                // panel does with GetNumChannelMembers when a row is clicked -
                // before the menu that asks this is built.
                //
                // The reference tests one dword of the channel record first and
                // answers nil when it reads above -1. No instruction carries
                // that address but the two bindings themselves, so it is not
                // modelled here - the same call the binding above made.
                //
                // Outside voice chat this is reached for one thing: the "Invite"
                // entry a custom channel's row menu gets (channelframe.lua:459),
                // which leads to InviteToChannel. The unit menu asks it twice
                // more (unitpopup.lua:853,865), both under IsVoiceChatEnabled()
                // and GetVoiceStatus(), which are answered no, so those two stay
                // out of reach either way.
                {"IsDisplayChannelModerator", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int sel = selectedDisplayChannel();
            if (!gh || sel < 1) return luaReturnNil(L);
            const auto& joined = gh->getJoinedChannels();
            if (sel > static_cast<int>(joined.size())) return luaReturnNil(L);
            const uint64_t self = gh->getPlayerGuid();
            bool moderator = false;
            for (const auto& m : gh->getChannelRoster(joined[static_cast<size_t>(sel) - 1])) {
                if (m.guid != self) continue;
                moderator = m.owner || m.moderator;
                break;
            }
            lua_pushboolean(L, moderator ? 1 : 0);
            return 1;
        }},
                {"SetSelectedDisplayChannel", [](lua_State* L) -> int {
            selectedDisplayChannel() = static_cast<int>(luaL_optnumber(L, 1, 0));
            return 0;
        }},
                // No categories exist to open or close - see the header note
                // on GetChannelDisplayInfo - but the row click handler calls
                // one of these on whatever it was given.
                {"ExpandChannelHeader",      lua_ReturnNothing},
                {"CollapseChannelHeader",    lua_ReturnNothing},
                // Who is in a channel. The server sends a roster only on
                // request and this client never asks, so there is nobody to
                // report; the count above is zero for the same reason.
                // GetNumChannelMembers(channelIndex) - how many are in it, and
                // the request that makes that true. The panel calls this in
                // statement position when a channel row is clicked, which is
                // the only thing that asks the server for a roster at all.
                {"GetNumChannelMembers", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int channel = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (!gh || channel < 1) { lua_pushnumber(L, 0); return 1; }
            const auto& joined = gh->getJoinedChannels();
            if (channel > static_cast<int>(joined.size())) { lua_pushnumber(L, 0); return 1; }
            const auto& name = joined[static_cast<size_t>(channel) - 1];
            gh->requestChannelList(name);
            lua_pushnumber(L, static_cast<lua_Number>(gh->getChannelRoster(name).size()));
            return 1;
        }},
                // GetChannelRosterInfo(channelIndex, rosterIndex) →
                //   name, owner, moderator, muted, active, enabled
                //
                // The list was printed to chat and dropped, so the channel
                // panel had nothing to draw and reported every channel as
                // empty. The last two are about voice chat, which this client
                // has none of.
                {"GetChannelRosterInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int channel = static_cast<int>(luaL_optnumber(L, 1, 0));
            const int member = static_cast<int>(luaL_optnumber(L, 2, 0));
            if (!gh || channel < 1 || member < 1) { return luaReturnNil(L); }
            const auto& joined = gh->getJoinedChannels();
            if (channel > static_cast<int>(joined.size())) { return luaReturnNil(L); }
            const auto& roster = gh->getChannelRoster(joined[static_cast<size_t>(channel) - 1]);
            if (member > static_cast<int>(roster.size())) { return luaReturnNil(L); }
            const auto& m = roster[static_cast<size_t>(member) - 1];
            lua_pushstring(L, m.name.c_str());          // 1: name
            lua_pushboolean(L, m.owner ? 1 : 0);        // 2: owner
            lua_pushboolean(L, m.moderator ? 1 : 0);    // 3: moderator
            lua_pushboolean(L, m.muted ? 1 : 0);        // 4: muted
            lua_pushboolean(L, 0);                      // 5: active (voice)
            lua_pushboolean(L, 0);                      // 6: enabled (voice)
            return 6;
        }},
                // The countdown the Wintergrasp zone-info line and the
                // battleground frame's timer both print, in whole seconds.
                //
                // 0x005435a0 asks 0x00548d10 for two world states. It looks at
                // 3801 first: absent or zero, the binding returns without
                // pushing anything, which is the nil both lines test. Then
                // 4354 minus the world state clock - time() shifted by whatever
                // SMSG_WORLD_STATE_UI_TIMER_UPDATE said the server's time is
                // (0x005486e0 sets the shift, 0x00548700 reads through it) -
                // and the record holds an absolute epoch second, so the
                // difference is the time left. The result is clamped at zero,
                // and an absent 4354 counts as a zero value rather than as
                // missing, so that answers 0: the battleground frame's
                // WINTERGRASP_IN_PROGRESS branch.
                {"GetWintergraspWaitTime", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return luaReturnNil(L);
            if (gh->getWorldState(game::world_state_ui::kWintergraspTrackedState).value_or(0) == 0)
                return luaReturnNil(L);
            const int64_t endsAt = static_cast<int32_t>(
                gh->getWorldState(game::world_state_ui::kWintergraspNextBattleState).value_or(0));
            const int64_t left = endsAt - gh->worldStateNow();
            lua_pushnumber(L, static_cast<double>(left < 0 ? 0 : left));
            return 1;
        }},
                {"GetNumWorldStateUI",       lua_GetNumWorldStateUI},
                {"GetWorldStateUIInfo",      lua_GetWorldStateUIInfo},
                // Whether the player is standing on a world PvP objective.
                //
                // This gates the whole of uiType 1 in worldstateframe, and the
                // CVar it is gated behind defaults to "2" - show these only in
                // a PvP area - so a flat false hid every world PvP objective
                // display anywhere but inside a battleground, where the
                // instanceType check lets them through instead. The note that
                // stood here said the branch is unreachable; the branch is
                // reachable and this was what closed it.
                //
                // Answered from AreaTable's Flags against the *area* under the
                // player rather than the zone: the bit sits on the subzone -
                // Halaa, The Overlook, the Plaguelands towers - and resolving
                // to the zone loses it. Wintergrasp is marked with a flag of
                // its own and is the one that would have survived either way.
                {"IsSubZonePVPPOI", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            const bool on = svc && svc->isOnOutdoorPvpObjective &&
                            svc->isOnOutdoorPvpObjective();
            lua_pushboolean(L, on ? 1 : 0);
            return 1;
        }},
                {"GetNumVoiceSessions",      lua_ReturnZero},
                // Asked from WorldMapFrame_OnUpdate, so every frame the map is
                // open. The handler throttles and refuses outside a
                // battleground; before this nothing ever asked, and the reply
                // that fills the position list is only ever sent on request -
                // so the list was empty for FrameXML's map and for this
                // client's own minimap alike.
                {"RequestBattlefieldPositions", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->requestBattlefieldPositions();
            return 0;
        }},
                // Re-positions four arrow actors the client keeps for itself,
                // which are not the arrows this interface draws. 0x00545fe0 is
                // a thunk onto 0x005449f0: it walks four objects held at
                // 0x00be8ee8, 0x00be8ef0, 0x00be8eec and 0x00be8ef4, updates one
                // only while it exists, holds something at +0x2a0 and a
                // 0x00824f00(0, 0) lookup succeeds, and the same routine copies
                // what the unit's vtable slot 0x34 answers - the facing, of the
                // same pair that answers a map position - into each object's
                // float at +0x328. One of the four is built around
                // "Interface\\Minimap\\MinimapArrow.mdx". The player arrow and
                // the quest pins this tree moves are named frames, and the two
                // verbs for those (ShowWorldMapArrowFrame and
                // PositionWorldMapArrowFrame, above) are both answered.
                {"UpdateWorldMapArrowFrames",   lua_ReturnNothing},
                {"SetSelectedSkill", [](lua_State* L) -> int {
            selectedSkill() = static_cast<int>(luaL_optnumber(L, 1, 0)); return 0; }},
                {"Sound_GameSystem_GetOutputDriverNameByIndex", lua_Sound_GetOutputDriverNameByIndex},
                {"PlaySound",           lua_PlaySound},
                {"PlaySoundFile",       lua_PlaySoundFile},
                {"PlayMusic",           lua_PlayMusic},
                {"StopMusic",           lua_StopMusic},
                {"GetPlayerMapPosition", lua_GetPlayerMapPosition},
                {"GetPlayerFacing",     lua_GetPlayerFacing},
                {"GetCVar",             lua_GetCVar},
                {"GetCVarBool",         lua_GetCVarBool},
                {"SetCVar",             lua_SetCVar},
                {"GetLocale",         lua_GetLocale},
                // Gender-aware lookup of a global string by token name.
                //
                // The reputation list is the caller that matters:
                // ReputationFrame_Update builds each row's standing label as
                // GetText("FACTION_STANDING_LABEL"..standingID, gender), and an
                // unbound global there is not a blank label but an error that
                // takes the whole list down with it.
                {"GetText", [](lua_State* L) -> int {
            const char* token = luaL_optstring(L, 1, nullptr);
            if (!token) { lua_pushstring(L, ""); return 1; }
            // globalstrings.lua carries FACTION_STANDING_LABEL3_FEMALE beside
            // FACTION_STANDING_LABEL3, so a gendered token wins when it exists.
            // 2 = male, 3 = female; 1 and absent mean neuter/unknown.
            const int gender = static_cast<int>(luaL_optnumber(L, 2, 0));
            const char* suffix = gender == 3 ? "_FEMALE" : (gender == 2 ? "_MALE" : nullptr);
            if (suffix) {
                lua_getglobal(L, (std::string(token) + suffix).c_str());
                if (lua_isstring(L, -1)) return 1;
                lua_pop(L, 1);
            }
            lua_getglobal(L, token);
            if (lua_isstring(L, -1)) return 1;
            lua_pop(L, 1);
            // A token with no string behind it shows as itself rather than as
            // nil, which would clear whatever label asked for it.
            lua_pushstring(L, token);
            return 1; }},
                {"GetBuildInfo",      lua_GetBuildInfo},
                {"GetCurrentMapAreaID", lua_GetCurrentMapAreaID},
                {"SetMapToCurrentZone", lua_SetMapToCurrentZone},
                {"GetCurrentMapContinent", lua_GetCurrentMapContinent},
                {"GetCurrentMapZone",   lua_GetCurrentMapZone},
                {"SetMapZoom",          lua_SetMapZoom},
                {"GetMapContinents",    lua_GetMapContinents},
                {"GetMapZones",         lua_GetMapZones},
                {"GetNumMapLandmarks",  lua_GetNumMapLandmarks},
                // The rest of the world map's own calls. None of them was
                // bound, and two run on every open: WorldMapFrame_OnUpdate
                // asks UpdateMapHighlight for whatever is under the cursor on
                // every frame the mouse is over the map, and the zoom-out
                // button reaches ZoomOut whenever the map is not a continent.
                //
                // Answered rather than filled in. This client draws its own
                // world map and keeps its zones, overlays and area POIs in
                // rendering/world_map, which no callback reaches from here yet
                // - so these say "nothing there" honestly instead of raising,
                // and the map opens empty rather than not at all.
                //
                // Nothing under the cursor: eight values, and the first is the
                // area name the label is set from. Nil leaves it blank, which
                // is what a cursor over no zone should do.
                // UpdateMapHighlight(x, y) →
                //   name, fileName, texPercentageX, texPercentageY,
                //   textureX, textureY, scrollChildX, scrollChildY
                //
                // Asked on every frame the cursor is over the map, and only
                // the first value is read for anything this client can answer:
                // WorldMapFrame_OnUpdate sets the area label from it. The
                // other seven place a highlight *texture* over the zone, which
                // needs the per-zone highlight art the client's own map layer
                // loads on demand - nil leaves the label named and the glow
                // off, which is the honest half.
                {"UpdateMapHighlight", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            const float u = static_cast<float>(luaL_optnumber(L, 1, -1.0));
            const float v = static_cast<float>(luaL_optnumber(L, 2, -1.0));
            std::string name;
            if (svc && svc->getMapZoneNameAt) name = svc->getMapZoneNameAt(u, v);
            if (name.empty()) lua_pushnil(L);
            else              lua_pushstring(L, name.c_str());
            for (int i = 0; i < 7; ++i) lua_pushnil(L);
            return 8;
        }},
                // Clicking a continent drills into the zone under the point.
                // Doing nothing leaves the map where it was, which is a map
                // that does not zoom rather than one that zooms wrongly.
                // Clicking a continent drills into the zone under the point,
                // which is the same ZMP lookup the hover uses.
                {"ProcessMapClick", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            if (svc && svc->clickMapPoint) {
                svc->clickMapPoint(static_cast<float>(luaL_optnumber(L, 1, -1.0)),
                                   static_cast<float>(luaL_optnumber(L, 2, -1.0)));
            }
            fireWorldMapUpdate(L);
            return 0;
        }},
                // Zoom out one step. SetMapZoom answers the continent and
                // cosmic cases beside it, and FrameXML only reaches this one
                // once a dungeon level is showing - which is what the three
                // floor bindings now let it pick. From a floor the map does not
                // step out a level at all: it shows the area that floor's own
                // row belongs to.
                {"ZoomOut", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            if (svc && svc->zoomMapOut) svc->zoomMapOut();
            fireWorldMapUpdate(L);
            return 0;
        }},
                // Show a zone by its WorldMapArea id. Nothing at all before,
                // which is how the quest log's "show on map" and the map's own
                // windowed-size toggle both landed wherever the map already
                // was.
                {"SetMapByID", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            const auto id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (svc && svc->setMapWorldAreaId && id != 0) svc->setMapWorldAreaId(id);
            fireWorldMapUpdate(L);
            return 0;
        }},
                // The level dropdown's pick and its two arrows, which hand the
                // level straight over. It is one-based, one past the floor for
                // the areas that carry a surface level (there, level one is no
                // floor at all - the client stores -1 for it), and a level that
                // no floor row of this map answers changes nothing whatever
                // (0x00547b80, then the row scan at 0x00547a70).
                //
                // Nothing is answered either way. What the client does after a
                // pick that took is re-run its whole map refresh (0x0050f490 ->
                // 0x00546310), which is the update below.
                {"SetDungeonMapLevel", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            const int level = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (svc && svc->setMapDungeonLevel && svc->setMapDungeonLevel(level))
                fireWorldMapUpdate(L);
            return 0;
        }},
                // ClickLandmark(mapLinkID) - clicking a pin that leads
                // somewhere.  0x00546ef0 looks the link up by key in a table of
                // its own (key range 0x00ad36e4..0x00ad36e0, key to row at
                // 0x00ad36f4), then walks the *other* list the map change built:
                // 0x00546310 fills a second array alongside the pins -
                // 0x00be8ee4 ids at 0x00be8f2c - by going over the table at
                // 0x00ad4f0c rows / 0x00ad4f20 (stride 0x44), keeping a row
                // whose second dword is the map being shown and, of the
                // up-to-four ids in its dwords 2..5, the first that lands on a
                // row with +0x28 at zero or above whose +0xc answers at
                // 0x00546100 for this player.  What is stored is that row's own
                // id.  Clicking then looks each stored id up at 0x00ad4edc and
                // wants the row it finds to have its second dword equal the
                // clicked link's second dword, and hands the map-change routine
                // that row's first dword with the link as its third argument -
                // which is how a pin opens a map instead of a tooltip.  Neither
                // table is named; AreaGroup.dbc's shape (an id, a map, four area
                // ids) is the nearest thing to the 0x44-stride one and it is not
                // it, being 8 fields and 32-byte records.  And no pin answered
                // here carries a mapLinkID, so nothing is clickable yet.
                //
                // Two steps of 0x00546ef0 are worth having written down for
                // whoever names that table: the argument is checked against the
                // WorldMapLink table's own id range first (0x00ad36e4 below,
                // 0x00ad36e0 above, the index array at 0x00ad36f4) and a link
                // outside it answers nothing at all rather than a failed lookup;
                // and the map-change routine is then called as
                // 0x00546310(-2, area id, link id).  That routine is the world
                // map's one way to change what is shown - twenty-three call sites
                // across the map code, five of them passing the same -2 - so an
                // answer here has somewhere to go; it is the id table, not the
                // missing verb, that holds this up.
                {"ClickLandmark",       lua_ReturnNothing},
                // The pins on the shown map: counted by GetNumMapLandmarks and
                // described by GetMapLandmarkInfo, whose note above is where the
                // reference's own families are worked out.
                {"GetMapLandmarkInfo",  lua_GetMapLandmarkInfo},
                {"GetMapOverlayInfo",   lua_GetMapOverlayInfo},
                // Which map a quest wants opened, and on which dungeon floor
                // (0x005e56c0 -> 0x005e2b60). Chosen off the quest POI list -
                // the areas the server sent for the quests in the log - and
                // against the map that is showing, which is why FrameXML shows
                // WorldMapFrame before asking: with the map closed the answer
                // is the pair of zeroes. Those zeroes are also what "no such
                // map" answers with, and it is a pair of zeroes rather than
                // nil because that is what the caller tests:
                // WorldMap_OpenToQuest asks `if ( mapID ~= 0 )` and then
                // `if ( floorNumber ~= 0 )`, and nil ~= 0 is true in Lua - so
                // answering nil ran both branches, on the path the quest
                // tracker takes every time a tracked quest is clicked.
                //
                // What is not here is the reference's last resort: with no
                // area listed for the quest it walks the quest's zone up its
                // AreaTable parents and opens the WorldMapArea row of the
                // top of that chain at floor 0, which needs both tables where
                // this has only the POI list. So a quest the server gave no
                // points of interest for leaves the map where it was.
                {"GetQuestWorldMapAreaID", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1)) {
                return luaL_error(L, "Usage: GetQuestWorldMapAreaID(questID)");
            }
            const auto [mapId, floorNumber] =
                questPoiOpenTarget(L, static_cast<int32_t>(lua_tonumber(L, 1)));
            lua_pushnumber(L, mapId);      // a WorldMapArea id
            lua_pushnumber(L, floorNumber);  // a DungeonMap floor
            return 2; }},
                // GetNumQuestItemDrops and GetQuestLogItemDrop are registered
                // with the rest of the quest log's readers (lua_quest_api.cpp):
                // both are asked about a quest log index, and the quest's items
                // live with its objectives rather than with the map.
                // Two debug readers, for the zone-map overlay Blizzard ships
                // switched off. Nothing here has a debug zone map at all, and
                // the client agrees: one 3-byte `return 0` stands behind both
                // GetMapDebugObjectInfo and HasDebugZoneMap in its method table
                // (0x008e5250) - see the note at GetNumMapDebugObjects.
                {"GetDebugZoneMap",        lua_ReturnNil},
                {"GetMapDebugObjectInfo",  lua_ReturnNil},
                {"GetTrackingTexture",  lua_GetTrackingTexture},
                {"GetNumTrackingTypes", lua_GetNumTrackingTypes},
                {"GetTrackingInfo",     lua_GetTrackingInfo},
                {"__WoweeActiveTrackingSpell", lua_ActiveTrackingSpell},
                {"SetTracking",         lua_SetTracking},
                {"GetZoneText",          lua_GetZoneText},
                {"GetRealZoneText",      lua_GetRealZoneText},
                {"GetSubZoneText",       lua_GetSubZoneText},
                {"GetMinimapZoneText",   lua_GetMinimapZoneText},
                {"GetGameTime",             lua_GetGameTime},
                {"CombatLog_Object_IsA", lua_CombatLog_Object_IsA},
                {"GetNumAddOns",      lua_GetNumAddOns},
                {"GetAddOnInfo",      lua_GetAddOnInfo},
                // Turning an addon off, which the "an addon did something it
                // is not allowed to" popup offers as its first button before
                // reloading. Answered with nothing, that button reloaded and
                // brought the same addon back.
                //
                // The change lands on the next run rather than at once: an
                // addon already loaded has its functions in the state and its
                // frames on screen, and neither can be taken back out.
                {"DisableAddOn", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            const char* name = luaL_optstring(L, 1, nullptr);
            if (svc && svc->setAddOnEnabled && name && *name)
                svc->setAddOnEnabled(name, false);
            return 0;
        }},
                {"EnableAddOn", [](lua_State* L) -> int {
            auto* svc = getLuaServices(L);
            const char* name = luaL_optstring(L, 1, nullptr);
            if (svc && svc->setAddOnEnabled && name && *name)
                svc->setAddOnEnabled(name, true);
            return 0;
        }},
                {"GetAddOnMetadata",  lua_GetAddOnMetadata},
                {"IsAddOnLoadOnDemand", lua_IsAddOnLoadOnDemand},
                {"GetAddOnDependencies", lua_GetAddOnDependencies},
                {"IsInInstance",         lua_IsInInstance},
                {"GetInstanceInfo",      lua_GetInstanceInfo},
                {"GetInstanceDifficulty", lua_GetInstanceDifficulty},
                // CanShowResetInstances() (0x0051a880 -> 0x005194e0): nil, in
                // every state this build can reach. Read off the code rather
                // than the name: with the reset flag set (0xbd0890) and the
                // current map a party or raid instance, it first requires the
                // map's MapDifficulty row for the difficulty to have a reset
                // time (+0x10) and then, for the previous instance, that the
                // same row - of the *current* map, still in esi - has none; out
                // of an instance it fails the second test of the current map's
                // type outright. The two cannot both hold, so the button it
                // guards never shows.
                {"CanShowResetInstances", lua_ReturnNil},
                {"strsplit",          lua_strsplit},
                {"strtrim",           lua_strtrim},
                {"strlenutf8",        lua_strlenutf8},
                {"wipe",              lua_wipe},
                {"strconcat",         lua_strconcat},
                {"strreplace",        lua_strreplace},
                {"date",              lua_wow_date},
                {"time",              lua_wow_time},
                {"difftime",          lua_wow_difftime},
                {"debugprofilestart", lua_debugprofilestart},
                {"debugprofilestop",  lua_debugprofilestop},
                {"debugload",         lua_compiledOutNothing},
                {"debuginfo",         lua_compiledOutNothing},
                {"debugprint",        lua_compiledOutNothing},
                {"debugdump",         lua_compiledOutNothing},
                {"debugbreak",        lua_compiledOutNothing},
                {"debughook",         lua_compiledOutNothing},
                {"debugtimestamp",    lua_compiledOutNothing},
                {"ResetPerformanceValues",   lua_compiledOutNothing},
                {"TeleportToDebugObject",    lua_compiledOutNothing},
                {"ToggleCollision",          lua_compiledOutNothing},
                {"ToggleCollisionDisplay",   lua_compiledOutNothing},
                {"TogglePerformanceDisplay", lua_compiledOutNothing},
                {"TogglePerformancePause",   lua_compiledOutNothing},
                {"TogglePerformanceValues",  lua_compiledOutNothing},
                {"TogglePlayerBounds",       lua_compiledOutNothing},
                {"TogglePortals",            lua_compiledOutNothing},
                {"ToggleTris",               lua_compiledOutNothing},
                {"CloseGuildRoster",         lua_compiledOutNothing},
                {"GetExtendedItemInfo",      lua_compiledOutNothing},
                {"QuestPOIUpdateTexture",    lua_compiledOutNothing},
                {"GetTime",           lua_wow_gettime},
                {"IsConnectedToServer", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && gh->isConnected() ? 1 : 0);
            return 1;
        }},
                // GetRealmName() - the realmName cvar (0x00510e00 ->
                // 0x006b0dc0): the list name of the realm last connected to,
                // "" before any.
                {"GetRealmName", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushstring(L, gh ? gh->getRealmName().c_str() : "");
            return 1;
        }},
                // ShowHelm(show) and ShowCloak(show) are setters, and these
                // toggled regardless of what they were passed.
                //
                // interfaceoptionspanels.xml drives both from a checkbox:
                // `self:SetChecked(value); ShowHelm(value)`. Toggling on a set
                // inverts the answer whenever the state already matched - the
                // box would tick and the helm would go away - and the panel
                // re-applies its value on every open, so it flipped again each
                // time the options were shown.
                //
                // The client only has a toggle, so this toggles only when that
                // lands on what was asked for. Same shape as SetPVP.
                // The two getters beside ShowHelm and ShowCloak below, which
                // have worked all along - so the checkbox wrote the setting
                // correctly and could not show it, and reading it raised as
                // the Display panel was built.
                {"ShowingHelm", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, (gh && gh->isHelmVisible()) ? 1 : 0);
            return 1;
        }},
                {"ShowingCloak", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, (gh && gh->isCloakVisible()) ? 1 : 0);
            return 1;
        }},
                // The equipment manager exists here - GetNumEquipmentSets and
                // the rest of its API are bound - so the panel's checkbox is
                // offering something real.
                {"CanUseEquipmentSets",         lua_ReturnTrue},
                // Tutorials can be reset because they are kept: the flags live
                // in the CVar file. Answering false would grey out a button
                // that would have worked.
                {"CanResetTutorials",           lua_ReturnTrue},
                {"ResetTutorials", [](lua_State* L) -> int {
            (void)L;
            cvarStore()[kTutorialCVar].clear();
            saveStoredCVars();
            return 0;
        }},
                // A Mac-only mouse this build does not look for.
                {"DetectWowMouse",              lua_ReturnFalse},
                // No Battle.net, so no chat to filter.
                {"BNSetMatureLanguageFilter",   lua_ReturnNothing},
                {"ShowHelm", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            const bool want = lua_isnumber(L, 1) ? (lua_tonumber(L, 1) != 0)
                                                 : (lua_toboolean(L, 1) != 0);
            if (want != gh->isHelmVisible()) gh->toggleHelm();
            return 0;
        }},
                {"ShowCloak", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            const bool want = lua_isnumber(L, 1) ? (lua_tonumber(L, 1) != 0)
                                                 : (lua_toboolean(L, 1) != 0);
            if (want != gh->isCloakVisible()) gh->toggleCloak();
            return 0;
        }},
                {"TogglePVP", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh) gh->togglePvp();
            return 0;
        }},
                {"Minimap_Ping", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            float x = static_cast<float>(luaL_optnumber(L, 1, 0));
            float y = static_cast<float>(luaL_optnumber(L, 2, 0));
            if (gh) gh->sendMinimapPing(x, y);
            return 0;
        }},
                {"RequestTimePlayed", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh) gh->requestPlayedTime();
            return 0;
        }},
                {"Logout", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh) gh->requestLogout();
            return 0;
        }},
                {"CancelLogout", [](lua_State* L) -> int {
            // 0x0051ac90: insecure code only inside a hardware event.
            if (!taint::allowedInHardwareEvent(L)) return 0;
            auto* gh = getGameHandler(L);
            if (gh) gh->cancelLogout();
            return 0;
        }},
                {"NumTaxiNodes", [](lua_State* L) -> int {
            lua_pushnumber(L, static_cast<double>(taxiNodeOrder(getGameHandler(L)).size()));
            return 1;
        }},
                {"TaxiNodeName", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = taxiNodeAt(gh, static_cast<int>(luaL_optnumber(L, 1, 0)));
            if (id == 0) { lua_pushstring(L, ""); return 1; }
            const auto& nodes = gh->getTaxiNodes();
            const auto it = nodes.find(id);
            lua_pushstring(L, it != nodes.end() ? it->second.name.c_str() : "");
            return 1;
        }},
                // TaxiNodeGetType(index) → "CURRENT" | "REACHABLE" | "DISTANT" | "NONE"
                //
                // A name, not a number. The flight map compares this against
                // those four words - to pick the pin's colour, and first of all
                // to decide whether the node is on the map at all. Answering 0
                // or 1 is never equal to any of them, so every node counted as
                // shown, including the ones the player has never been to.
                {"TaxiNodeGetType", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = taxiNodeAt(gh, static_cast<int>(luaL_optnumber(L, 1, 0)));
            if (id == 0) { lua_pushstring(L, "NONE"); return 1; }
            if (id == gh->getTaxiCurrentNode())  { lua_pushstring(L, "CURRENT"); return 1; }
            if (!gh->isKnownTaxiNode(id))        { lua_pushstring(L, "NONE"); return 1; }
            // Known, but nothing flies there from where the player is standing:
            // shown in yellow rather than hidden, so it reads as somewhere they
            // have been rather than somewhere that does not exist.
            lua_pushstring(L, gh->hasTaxiRouteTo(id) ? "REACHABLE" : "DISTANT");
            return 1;
        }},
                {"TakeTaxiNode", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = taxiNodeAt(gh, static_cast<int>(luaL_optnumber(L, 1, 0)));
            if (id != 0) gh->activateTaxi(id);
            return 0;
        }},
                // TaxiNodePosition(index) → x, y as fractions of the map.
                {"TaxiNodePosition", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = taxiNodeAt(gh, static_cast<int>(luaL_optnumber(L, 1, 0)));
            float u = 0, v = 0;
            if (id == 0 || !taxiNodeMapPos(gh, id, u, v)) { return luaReturnNil(L); }
            lua_pushnumber(L, u);
            lua_pushnumber(L, v);
            return 2;
        }},
                // TaxiNodeSetCurrent(index) - the node the map is drawing a
                // route to. Works out the journey once; the frame then asks
                // about each leg of it.
                {"TaxiNodeSetCurrent", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = taxiNodeAt(gh, static_cast<int>(luaL_optnumber(L, 1, 0)));
            taxiRouteShown() = (id != 0 && gh) ? gh->getTaxiRouteTo(id)
                                               : std::vector<uint32_t>{};
            return 0;
        }},
                // GetNumRoutes(index) → how many hops the flight takes.
                //
                // The map draws one line per hop, and reads this to decide how
                // many. It also asks about every node in the list to find the
                // ones a single hop away, so this answers for the node asked
                // about rather than for whatever was last set current.
                {"GetNumRoutes", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = taxiNodeAt(gh, static_cast<int>(luaL_optnumber(L, 1, 0)));
            if (id == 0 || !gh) { lua_pushnumber(L, 0); return 1; }
            const auto route = gh->getTaxiRouteTo(id);
            // A route of N nodes is N-1 hops; one of fewer than two is no
            // journey at all.
            lua_pushnumber(L, route.size() >= 2
                                  ? static_cast<double>(route.size() - 1) : 0.0);
            return 1;
        }},
                // TaxiGetSrcX/Y(index, hop) and TaxiGetDestX/Y(index, hop) -
                // the ends of one leg, on the same scale as TaxiNodePosition.
                //
                // The index names the destination and the hop names the leg, so
                // these route afresh rather than trusting whatever was last set
                // current: DrawOneHopLines walks every node without ever
                // calling TaxiNodeSetCurrent.
                {"TaxiGetSrcX",  lua_TaxiLegCoord<0, true>},
                {"TaxiGetSrcY",  lua_TaxiLegCoord<0, false>},
                {"TaxiGetDestX", lua_TaxiLegCoord<1, true>},
                {"TaxiGetDestY", lua_TaxiLegCoord<1, false>},
                // SetTaxiMap(texture) - the flight map's own picture.
                //
                // This client has continent artwork, but not in the shape this
                // call wants: its flight map is a mode of the world map, which
                // draws the continent from tiles and puts taxi nodes over it,
                // where SetTaxiMap hands over one texture to point at a single
                // TAXIMAP image. So the texture keeps whatever its XML set.
                // Named rather than left out so it does not read as a gap that
                // was missed.
                // SetTaxiMap(texture) - the continent behind the flight points.
                //
                // TaxiFrame_OnEvent hands its TaxiMap texture to this and
                // expects the picture of the continent to come back on it; the
                // node buttons are then placed over it by TaxiNodePosition,
                // which answers a fraction of the map rather than a position on
                // screen. A no-op here left those buttons floating on an empty
                // panel, which is not a flight map - it is a set of unlabelled
                // dots with nothing to read them against.
                //
                // The files are keyed by map id, not by name: this install has
                // taximap0, taximap1, taximap530 and taximap571 - Eastern
                // Kingdoms, Kalimdor, Outland and Northrend. So the id is the
                // whole of the lookup and no table of continent names is
                // needed, which is as well, because a name table would be a
                // second place for the same fact to be written down.
                {"SetTaxiMap", [](lua_State* L) -> int {
            auto* tree = getWidgetTree(L);
            if (!tree || !lua_istable(L, 1)) return 0;
            // The widget id the frame table carries, read the same way
            // SetPortraitTexture reads it - widgetIdOf is not visible here.
            lua_getfield(L, 1, "__wid");
            const uint32_t id = static_cast<uint32_t>(lua_tointeger(L, -1));
            lua_pop(L, 1);
            if (id == 0) return 0;
            auto* w = tree->get(id);
            if (!w) return 0;
            w->solidColor = false;
            // The handler is not required to reach this: without one the map
            // id is zero, which is a real map rather than a refusal, and a
            // client with nobody logged in has no flight window to draw
            // anyway. Requiring it would only make the wiring untestable
            // outside a session.
            auto* gh = getGameHandler(L);
            w->texturePath = "Interface\\TaxiFrame\\TaxiMap" +
                             std::to_string(gh ? gh->getCurrentMapId() : 0u);
            // A texture is sized from its file; see WidgetTree::markMeasureDirty.
            tree->markMeasureDirty(id);
            return 0;
        }},
                {"CloseTaxiMap", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->closeTaxi();
            return 0;
        }},
                // TaxiNodeCost(index) → the fare in copper, for the tooltip.
                {"TaxiNodeCost", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = taxiNodeAt(gh, static_cast<int>(luaL_optnumber(L, 1, 0)));
            lua_pushnumber(L, id != 0 ? static_cast<double>(gh->getTaxiCostTo(id)) : 0.0);
            return 1;
        }},
                {"GetNetStats", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            uint32_t ms = gh ? gh->getLatencyMs() : 0;
            lua_pushnumber(L, 0);   // bandwidthIn
            lua_pushnumber(L, 0);   // bandwidthOut
            lua_pushnumber(L, ms);  // latencyHome
            lua_pushnumber(L, ms);  // latencyWorld
            return 4;
        }},
                {"GetCurrentTitle", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->getChosenTitleBit() : -1);
            return 1;
        }},
                // GetTitleName(titleMaskID) - the client's own usage line, and
                // the argument is a bit of the known-titles mask rather than a
                // CharTitles row id. The two are not the same number in this
                // table (the row with id 155 carries bit 122), and the title
                // list hands the same loop index to IsTitleKnown, which reads a
                // mask bit, and to this, and works - so this one matches on the
                // bit, which is the TitleBit column and not the first one.
                //
                // The words come back in the gender of the player whose title
                // list this is, which is the local player: the client's
                // GetTitleName reads its own object's gender byte.
                {"GetTitleName", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int bit = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || bit < 0) { return luaReturnNil(L); }
            std::string title = gh->getFormattedTitle(static_cast<uint32_t>(bit));
            if (title.empty()) { return luaReturnNil(L); }
            lua_pushstring(L, title.c_str());
            return 1;
        }},
                // SetCurrentTitle(bit) - and the comment that used to sit here
                // saying CMSG_SET_TITLE was not exposed was stale:
                // sendSetTitle builds and sends it, and has for a while.
                //
                // The server validates it. HandleSetTitleOpcode refuses any bit
                // the character does not own and silently sets none, so there
                // is nothing to check here that the server does not check
                // better.
                {"SetCurrentTitle", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            // 0x00522e30: insecure code only inside a hardware event.
            if (gh && !taint::allowedInHardwareEvent(L)) return 0;
            if (gh) gh->sendSetTitle(static_cast<int32_t>(luaL_optnumber(L, 1, -1)));
            return 0;
        }},
                // GetInspectArenaTeamData(index) →
                //   name, size, rating, weekPlayed, weekWins, seasonPlayed,
                //   seasonWins, playerRating
                //
                // Real: the inspect reply carries these and this client already
                // parses them into InspectResult.
                {"GetInspectArenaTeamData", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto* ir = gh ? gh->getInspectResult() : nullptr;
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (!ir || index < 1 ||
                index > static_cast<int>(ir->arenaTeams.size())) {
                return 0;   // nothing, which is how WoW says "no team here"
            }
            const auto& t = ir->arenaTeams[index - 1];
            // The type is the team size: 2, 3 or 5.
            lua_pushstring(L, t.name.c_str());
            lua_pushnumber(L, t.type);
            lua_pushnumber(L, t.personalRating);
            lua_pushnumber(L, t.weekGames);
            lua_pushnumber(L, t.weekWins);
            lua_pushnumber(L, t.seasonGames);
            lua_pushnumber(L, t.seasonWins);
            lua_pushnumber(L, t.personalRating);
            return 8;
        }},
                // CanInspect(unit [, showError]) - a player other than a
                // corpse, which is as much as this client can judge; the server
                // refuses the rest and the reply simply does not arrive.
                {"CanInspect", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const char* uid = luaL_optstring(L, 1, "target");
            if (!gh) { lua_pushboolean(L, 0); return 1; }
            std::string uidStr(uid);
            for (char& c : uidStr) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            const uint64_t guid = resolveUnitGuid(gh, uidStr);
            if (guid == 0) { lua_pushboolean(L, 0); return 1; }
            // High word 0x0000 marks a player guid in this range; creatures
            // carry 0xF13/0xF14 and cannot be inspected at all.
            const bool isPlayer = ((guid >> 48) & 0xFFFF) == 0;
            lua_pushboolean(L, isPlayer ? 1 : 0);
            return 1;
        }},
                // Honour data for an inspected player is not in the reply this
                // client parses, so it says so plainly rather than reporting
                // zeros as though they were the answer: HasInspectHonorData is
                // false, and the panel's honour section stays empty instead of
                // claiming the player has never won anything.
                {"HasInspectHonorData", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto* r = gh ? gh->getInspectResult() : nullptr;
            lua_pushboolean(L, (r && r->hasHonorData) ? 1 : 0);
            return 1;
        }},
                // The tab asks for these when it opens and redraws on
                // INSPECT_HONOR_UPDATE. It did nothing, so the answer never
                // came and the tab sat empty behind a HasInspectHonorData that
                // was false for good.
                {"RequestInspectHonorData", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto* r = gh ? gh->getInspectResult() : nullptr;
            if (gh && r && r->guid != 0) gh->requestInspectHonorData(r->guid);
            return 0;
        }},
                {"GetInspectHonorData", [](lua_State* L) -> int {
            // Six: today's kills and honour, yesterday's, lifetime kills, and
            // the lifetime *rank*, which inspecthonorframe feeds straight into
            // GetPVPRankInfo and shows NONE for when that answers nothing.
            //
            // The rank byte is what the server puts in that slot, and what it
            // puts there is the honour points truncated to eight bits - the
            // rank ladder was retired in this expansion and the field was
            // reused rather than removed. Passed on as sent rather than
            // interpreted here.
            auto* gh = getGameHandler(L);
            const auto* r = gh ? gh->getInspectResult() : nullptr;
            if (!r || !r->hasHonorData) {
                for (int i = 0; i < 6; ++i) lua_pushnumber(L, 0);
                return 6;
            }
            lua_pushnumber(L, r->honorTodayKills);
            lua_pushnumber(L, r->honorTodayContribution);
            lua_pushnumber(L, r->honorYesterdayKills);
            lua_pushnumber(L, r->honorYesterdayContribution);
            lua_pushnumber(L, r->honorLifetimeKills);
            lua_pushnumber(L, r->honorRank);
            return 6;
        }},
                {"GetInspectPVPRankProgress", [](lua_State* L) -> int {
            lua_pushnumber(L, 0);
            return 1;
        }},
                // UnitPVPRank(unit) - the old honour rank, which no WotLK
                // server sends; GetPVPRankInfo is fed from it and handles zero.
                {"UnitPVPRank", [](lua_State* L) -> int {
            (void)L;
            lua_pushnumber(L, 0);
            return 1;
        }},
                // NotifyInspect(unit) - ask the server for this player's gear,
                // talents and achievements. The inspect window calls it as it
                // opens and again whenever it is pointed at someone else, and
                // it is the only request either of those makes.
                //
                // This did nothing, on the reasoning that the client inspects
                // by itself when a player is targeted. It does, but that is a
                // different thing: a background queue that keeps gear visuals
                // right, throttled to one request every two seconds and only
                // for players already spawned nearby. So the window opened on
                // whatever that queue happened to have fetched last - often
                // another player entirely, and on a first inspect nothing.
                //
                // inspectUnit sends the achievements query alongside on Wrath,
                // which is what the comparison tab reads.
                {"NotifyInspect", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            const uint64_t guid = resolveUnitGuid(gh, luaL_optstring(L, 1, "target"));
            if (guid) gh->inspectUnit(guid);
            return 0;
        }},
                // ClearInspectPlayer() (0x005e7890) drops the client's inspect
                // record: a guid pair at DAT_00c24220, six more fields behind
                // it, and a call out to the talent code with the emptied pair -
                // 0x005c9f70, which checks that pair as a talent record
                // (TalentInfo.cpp line 0x4b3) and refreshes the inspected
                // talents only when it names someone. It has to clear, because
                // that store is one slot wide - the inspect frame's whole state
                // is "whatever is in it", so leaving it filled would leave the
                // next player's window showing the last player's gear.
                //
                // Nothing here is one slot wide. The inspected gear and the
                // inspected achievements are both maps keyed by the player's
                // guid, and the readers go through the unit they are asked
                // about, which is also how a hover tooltip over a bystander
                // gets the gear that player is wearing. Clearing on window
                // close would therefore not reset a stale window; it would
                // delete data that is still being read, and the next hover
                // would need a fresh inspect request for a player who is
                // standing right there. So there is nothing to clear, and the
                // no-op is the answer rather than a gap.
                //
                // Nor is there an event to raise in its place. The guid pair the
                // reference hands its talent code is the one it has just zeroed,
                // so that call fails its own checked cast and never reaches the
                // panel or the interface: ClearInspectPlayer raises no script
                // event at all, which was the other half of the gap it looked
                // like.
                {"ClearInspectPlayer", [](lua_State* L) -> int {
            (void)L;
            return 0;
        }},
                // Both answer the amount held *and the cap*. staticpopup.lua
                // does `MerchantFrame.honorPoints + currentHonor > maxHonor`
                // when confirming a PvP refund, and comparing a number against
                // a nil raises - so refunding a honour purchase took the
                // confirmation down rather than showing it.
                //
                // The caps are the client's own constants for this expansion,
                // not something the server sends: 75000 honour, 5000 arena
                // points. They are only ever used to warn that a refund would
                // overflow, which is exactly what they are right for.
                {"GetHonorCurrency", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->getHonorPoints() : 0);
            lua_pushnumber(L, 75000);
            return 2;
        }},
                // GetMaxArenaCurrency() (0x005d6ff0): 10000.
                {"GetMaxArenaCurrency", [](lua_State* L) -> int { lua_pushnumber(L, 10000); return 1; }},
                // GetDamageBonusStat() (0x0051afb0): the player's class row in
                // ChrClasses, its second column + 1; 0 without one.
                {"GetDamageBonusStat", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            auto* am = gh ? gh->services().assetManager : nullptr;
            double stat = 0;
            if (am && am->isInitialized() && gh->getPlayerClass() != 0) {
                auto classes = am->loadDBCOptional("ChrClasses.dbc");
                const int32_t row = classes && classes->isLoaded() && classes->getFieldCount() >= 2
                                        ? classes->findRecordById(gh->getPlayerClass()) : -1;
                if (row >= 0) stat = static_cast<double>(classes->getInt32(static_cast<uint32_t>(row), 1)) + 1;
            }
            lua_pushnumber(L, stat);
            return 1;
        }},
                {"GetArenaCurrency", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->getArenaPoints() : 0);
            lua_pushnumber(L, 5000);
            return 2;
        }},
                {"GetBindLocation", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { lua_pushstring(L, "Unknown"); return 1; }
            lua_pushstring(L, gh->getWhoAreaName(gh->getHomeBindZoneId()).c_str());
            return 1;
        }},
                // SetSavedInstanceExtend(index, doExtend) - the Extend button
                // on the raid lockout list, which raidframe.lua enables for any
                // selected row. Everything around it was bound; this one was
                // not, so pressing it raised.
                //
                // The call gives a position in the list and the wire wants the
                // map and difficulty, which is what the lockout at that
                // position holds.
                {"SetSavedInstanceExtend", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (!gh || index < 1) return 0;
            const auto& locks = gh->getInstanceLockouts();
            if (index > static_cast<int>(locks.size())) return 0;
            const auto& l = locks[static_cast<size_t>(index) - 1];
            gh->setSavedInstanceExtend(l.mapId, l.difficulty, lua_toboolean(L, 2) != 0);
            return 0;
        }},
                {"GetNumSavedInstances", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? gh->getInstanceLockouts().size() : 0);
            return 1;
        }},
                {"GetSavedInstanceInfo", [](lua_State* L) -> int {
            // GetSavedInstanceInfo(index) → name, id, reset, difficulty, locked, extended, instanceIDMostSig, isRaid, maxPlayers
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || index < 1) { return luaReturnNil(L); }
            const auto& lockouts = gh->getInstanceLockouts();
            if (index > static_cast<int>(lockouts.size())) { return luaReturnNil(L); }
            const auto& l = lockouts[index - 1];
            // The real name, out of Map.dbc, which the client already reads for
            // its own loading screens. The number was a placeholder and it
            // reached the Raid Info panel as the instance's name.
            std::string mapName = gh->getMapName(l.mapId);
            if (mapName.empty()) mapName = "Instance " + std::to_string(l.mapId);
            // As 0x004ff2c0 returns them: the instance id's two halves, the
            // difficulty counted from one, raid from Map.dbc's InstanceType,
            // the size and difficulty name from MapDifficulty.dbc.
            lua_pushstring(L, mapName.c_str());     // name
            lua_pushnumber(L, static_cast<double>(static_cast<uint32_t>(l.instanceId)));  // id
            const int64_t left = static_cast<int64_t>(l.resetTime) - static_cast<int64_t>(time(nullptr));
            lua_pushnumber(L, static_cast<double>(left > 0 ? left : 0)); // reset (seconds until)
            lua_pushnumber(L, l.difficulty + 1);    // difficulty
            lua_pushboolean(L, l.locked ? 1 : 0);  // locked
            lua_pushboolean(L, l.extended ? 1 : 0); // extended
            lua_pushnumber(L, static_cast<double>(static_cast<uint32_t>(l.instanceId >> 32))); // instanceIDMostSig
            lua_pushboolean(L, gh->mapInstanceType(l.mapId) == 2 ? 1 : 0); // isRaid
            lua_pushnumber(L, gh->savedInstanceMaxPlayers(l.mapId, l.difficulty)); // maxPlayers
            lua_pushstring(L, gh->savedInstanceDifficultyName(l.mapId, l.difficulty).c_str());
            return 10;
        }},
                {"CalendarGetDate", [](lua_State* L) -> int {
            // CalendarGetDate() → weekday, month, day, year
            time_t now = time(nullptr);
            const std::tm t = core::localTime(now);
            lua_pushnumber(L, t.tm_wday + 1); // weekday (1=Sun)
            lua_pushnumber(L, t.tm_mon + 1);  // month (1-12)
            lua_pushnumber(L, t.tm_mday);     // day
            lua_pushnumber(L, t.tm_year + 1900); // year
            return 4;
        }},
                // How many calendar invites are waiting to be answered. The
                // calendar addon is refused by name, but this is not only its
                // question: gametime.lua asks it for the indicator on the
                // minimap's date button, and that file is core FrameXML. A
                // constant zero left the button dark however many invites had
                // arrived.
                {"CalendarGetNumPendingInvites", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? static_cast<double>(gh->getCalendarPendingInvites()) : 0);
            return 1;
        }},
                {"CalendarGetNumDayEvents", [](lua_State* L) -> int {
            lua_pushnumber(L, static_cast<double>(calendarDayRows(L).size()));
            return 1;
        }},
                // Fifteen values, in the order the interface unpacks them:
                //
                //   title, hour, minute, calendarType, sequenceType, eventType,
                //   texture, modStatus, inviteStatus, invitedBy, difficulty,
                //   inviteType, sequenceIndex, numSequenceDays, difficultyName
                //
                // A nil title is how the list ends - the interface tests
                // `if ( title and sequenceType ~= "ONGOING" )` - so an index
                // past the day's rows answers nothing at all rather than a row
                // of blanks, which would draw an empty button on every day.
                {"CalendarGetDayEvent", [](lua_State* L) -> int {
            const auto rows = calendarDayRows(L);
            const int index = static_cast<int>(luaL_optnumber(L, 3, 0));
            if (index < 1 || static_cast<size_t>(index) > rows.size()) return 0;
            const auto& row = rows[static_cast<size_t>(index) - 1];
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            const wowee::game::CalendarData& cal = gh->getCalendarData();

            if (row.kind == wowee::game::CalendarEntryKind::RaidLockout) {
                const auto& lock = cal.lockouts[row.index];
                lua_pushstring(L, gh->getMapName(lock.mapId).c_str());
                lua_pushnumber(L, row.hour);
                lua_pushnumber(L, row.minute);
                lua_pushstring(L, "RAID_LOCKOUT");
                lua_pushstring(L, "");             // sequenceType
                lua_pushnumber(L, 0);              // eventType
                lua_pushnumber(L, 0);              // texture
                lua_pushstring(L, "");             // modStatus
                lua_pushnumber(L, 1);              // inviteStatus
                lua_pushstring(L, "");             // invitedBy
                lua_pushnumber(L, lock.difficulty);
                lua_pushnumber(L, 1);              // inviteType
                lua_pushnumber(L, row.sequenceIndex + 1);
                lua_pushnumber(L, row.numSequenceDays);
                lua_pushstring(L, "");             // difficultyName
                return 15;
            }
            if (row.kind == wowee::game::CalendarEntryKind::Holiday) {
                const auto& holiday = cal.holidays[row.index];
                lua_pushstring(L, holiday.textureFilename.c_str());
                lua_pushnumber(L, 0);              // hour
                lua_pushnumber(L, 0);              // minute
                lua_pushstring(L, "HOLIDAY");
                // Its day of the run (0x005b8040): the interface draws the
                // name on its first day and skips the "ONGOING" ones. A
                // holiday's record carries no invitation: status 0 and type
                // 1, each from 1 (0x005c1070).
                lua_pushstring(L, calendarSequenceType(wowee::game::calendar_flag::kHoliday, row.sequenceIndex,
                                                       row.numSequenceDays));
                lua_pushnumber(L, 0);              // eventType
                lua_pushnumber(L, static_cast<double>(holiday.id));  // texture
                lua_pushstring(L, "");             // modStatus
                lua_pushnumber(L, 1);              // inviteStatus
                lua_pushstring(L, "");             // invitedBy
                lua_pushnumber(L, 0);              // difficulty
                lua_pushnumber(L, 1);              // inviteType
                lua_pushnumber(L, row.sequenceIndex + 1);
                lua_pushnumber(L, row.numSequenceDays);
                lua_pushstring(L, "");             // difficultyName
                return 15;
            }

            const auto& ev = cal.events[row.index];
            // The invite flags and status the calendar keeps for it
            // (0x005c2e30): moderator text (0x005b8080), status from 1, and
            // the invite type (0x005b80b0).
            const auto now = gh->getGameDate();
            CalendarRecord rec;
            rec.flags = ev.flags;
            rec.invite = wowee::game::calendarInviteState(cal, ev, gh->getPlayerGuid(), now ? &*now : nullptr);
            lua_pushstring(L, ev.title.c_str());
            lua_pushnumber(L, ev.eventTime.hour);
            lua_pushnumber(L, ev.eventTime.minute);
            lua_pushstring(L, calendarTypeName(ev));
            lua_pushstring(L, "");                 // sequenceType: single day
            lua_pushnumber(L, static_cast<double>(ev.type) + 1);
            lua_pushnumber(L, 0);                  // texture
            lua_pushstring(L, calendarModeratorText(rec.invite.flags));
            lua_pushnumber(L, rec.invite.status + 1);
            lua_pushstring(L, "");                 // invitedBy
            lua_pushnumber(L, 0);                  // difficulty
            lua_pushnumber(L, calendarInviteType(rec));
            lua_pushnumber(L, row.sequenceIndex + 1);
            lua_pushnumber(L, row.numSequenceDays);
            lua_pushstring(L, "");                 // difficultyName
            return 15;
        }},
                // name, description, texture - indexed into the same day list
                // as CalendarGetDayEvent, so a row that is not a holiday
                // answers nothing rather than the wrong holiday.
                {"CalendarGetHolidayInfo", [](lua_State* L) -> int {
            const auto rows = calendarDayRows(L);
            const int index = static_cast<int>(luaL_optnumber(L, 3, 0));
            if (index < 1 || static_cast<size_t>(index) > rows.size()) return 0;
            const auto& row = rows[static_cast<size_t>(index) - 1];
            if (row.kind != wowee::game::CalendarEntryKind::Holiday) return 0;
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            const auto& holiday = gh->getCalendarData().holidays[row.index];
            lua_pushstring(L, holiday.textureFilename.c_str());
            lua_pushstring(L, "");
            lua_pushstring(L, holiday.textureFilename.c_str());
            return 3;
        }},
                // name, calendarType, raidID, hour, minute, difficulty,
                // difficultyName - the seven the raid view unpacks. Indexed
                // into the same day list as the other two readers, so a row
                // that is not a lockout answers nothing rather than the wrong
                // raid.
                {"CalendarGetRaidInfo", [](lua_State* L) -> int {
            const auto rows = calendarDayRows(L);
            const int index = static_cast<int>(luaL_optnumber(L, 3, 0));
            if (index < 1 || static_cast<size_t>(index) > rows.size()) return 0;
            const auto& row = rows[static_cast<size_t>(index) - 1];
            if (row.kind != wowee::game::CalendarEntryKind::RaidLockout) return 0;
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            const auto& lock = gh->getCalendarData().lockouts[row.index];
            lua_pushstring(L, gh->getMapName(lock.mapId).c_str());
            lua_pushstring(L, "RAID_LOCKOUT");
            lua_pushnumber(L, static_cast<double>(lock.mapId));
            lua_pushnumber(L, row.hour);
            lua_pushnumber(L, row.minute);
            lua_pushnumber(L, lock.difficulty);
            lua_pushstring(L, "");
            return 7;
        }},
                // Ask the server for the calendar. The addon's OnShow calls
                // this, and the answer arrives as CALENDAR_UPDATE_EVENT_LIST.
                {"OpenCalendar", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->requestCalendar();
            return 0;
        }},
                // Whether a calendar request is out and unanswered - set by
                // every calendar send, cleared by the server's answer or error
                // (0x005b8c10 pushes 0x00c207f3 as a boolean).
                {"CalendarIsActionPending", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && gh->isCalendarActionPending() ? 1 : 0);
            return 1;
        }},
                // minLevel, maxLevel, rank - the default filter a guild event
                // is created with. The whole guild, which is what the server
                // defaults to as well.
                {"CalendarDefaultGuildFilter", [](lua_State* L) -> int {
            lua_pushnumber(L, 1);
            lua_pushnumber(L, 80);
            lua_pushnumber(L, 0);
            return 3;
        }},
                // Whether the player may put an event on the guild's calendar.
                // The server decides for real; this is the guild-membership
                // half of it, which is the half that gates the button.
                {"CanEditGuildEvent", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, (gh && !gh->getGuildName().empty()) ? 1 : 0);
            return 1;
        }},
                // Arena team events need a team. No arena team is tracked, so
                // the honest answer is no and the option stays out of the
                // create menu rather than leading to a request that fails.
                {"IsInArenaTeam", [](lua_State* L) -> int {
            lua_pushboolean(L, 0);
            return 1;
        }},
                // ---- Opening one event ----
                //
                // The interface names a row, this turns it into an event id
                // and asks the server; the answer arrives as
                // CALENDAR_UPDATE_EVENT and CalendarGetEventInfo reads it.
                {"CalendarOpenEvent", [](lua_State* L) -> int {
            const auto rows = calendarDayRows(L);
            const int index = static_cast<int>(luaL_optnumber(L, 3, 0));
            auto* gh = getGameHandler(L);
            if (!gh || index < 1 || static_cast<size_t>(index) > rows.size()) return 0;
            const auto& row = rows[static_cast<size_t>(index) - 1];
            if (row.kind != wowee::game::CalendarEntryKind::Event) return 0;
            gh->requestCalendarEvent(gh->getCalendarData().events[row.index].eventId);
            return 0;
        }},
                {"CalendarCloseEvent", [](lua_State* L) -> int {
            (void)L;
            return 0;
        }},
                // Twenty-five values, in the order the view frame unpacks
                // them. A nil title is how it knows there is nothing to show -
                // `if ( not title ) then return end` - so an unopened event
                // answers nothing rather than a row of blanks.
                {"CalendarGetEventInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            const auto& ev = gh->getCalendarEventDetail();
            if (ev.eventId == 0) return 0;
            lua_pushstring(L, ev.title.c_str());
            lua_pushstring(L, ev.description.c_str());
            lua_pushstring(L, gh->lookupName(ev.creatorGuid).c_str());
            lua_pushnumber(L, ev.type + 1);          // wire counts from zero
            lua_pushnumber(L, ev.repeatOption + 1);  // and so does this
            lua_pushnumber(L, ev.maxInvites);
            // textureIndex: the dungeon's 1-based place in the event type's
            // CalendarEventGetTextures list, 0 off it (0x005b9700 + 1).
            {
                long place = 0;
                if (ev.type <= 1) {
                    const auto rows = gh->calendarEventTextures(ev.type == 0 ? 1 : 2);
                    for (size_t i = 0; i < rows.size(); ++i) {
                        if (static_cast<int32_t>(rows[i].dungeonId) == ev.dungeonId) {
                            place = static_cast<long>(i) + 1;
                            break;
                        }
                    }
                }
                lua_pushnumber(L, static_cast<lua_Number>(place));
            }
            lua_pushnumber(L, ev.eventTime.weekday + 1);
            lua_pushnumber(L, ev.eventTime.month);
            lua_pushnumber(L, ev.eventTime.day);
            lua_pushnumber(L, ev.eventTime.fullYear());
            lua_pushnumber(L, ev.eventTime.hour);
            lua_pushnumber(L, ev.eventTime.minute);
            // The lockout date, which only a raid-reset event carries. The
            // same date rather than nothing: the frame formats it without
            // guarding, so nil there raises where a repeated date only reads
            // oddly on an event that has no lockout to show.
            lua_pushnumber(L, ev.eventTime.weekday + 1);
            lua_pushnumber(L, ev.eventTime.month);
            lua_pushnumber(L, ev.eventTime.day);
            lua_pushnumber(L, ev.eventTime.fullYear());
            lua_pushnumber(L, ev.eventTime.hour);
            lua_pushnumber(L, ev.eventTime.minute);
            // CALENDAR_FLAG_INVITES_LOCKED is 0x10, the bit the calendar's
            // own handlers test (0x005c29c0, 0x005bf590); 0x1000 was read here.
            lua_pushboolean(L, (ev.flags & wowee::game::calendar_flag::kInvitesLocked) ? 1 : 0);
            lua_pushboolean(L, 0);              // autoApprove
            lua_pushboolean(L, 0);              // pendingInvite
            lua_pushnumber(L, calendarInviteStatusFor(gh->getCalendarData(),
                                                      ev.eventId) + 1);
            // CALENDAR_INVITETYPE_NORMAL, which is 1 - the interface's range
            // starts there, so zero is not "normal" but out of range.
            lua_pushnumber(L, 1);
            lua_pushstring(L, (ev.flags & 0x0400u) ? "GUILD_EVENT" : "PLAYER");
            return 25;
        }},
                // CalendarCanSendInvite() - may an invitation go now
                // (0x005b8b30): no calendar request pending and the last
                // invitation two seconds gone (0x00c207f8). Nothing about who
                // may invite - the create-event frame that asks is the
                // editor's - where this answered from the player's own row on
                // the open event's list, and so said no to every invitation to
                // an event still being made.
                {"CalendarCanSendInvite", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && gh->calendarCanSendInvite() ? 1 : 0);
            return 1;
        }},
                {"CalendarEventGetNumInvites", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushnumber(L, gh ? static_cast<double>(
                gh->getCalendarEventDetail().invitees.size()) : 0);
            return 1;
        }},
                // name, level, className, classFilename, inviteStatus - the
                // five the invite list reads.
                {"CalendarEventGetInvite", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (!gh) return 0;
            const auto& list = gh->getCalendarEventDetail().invitees;
            if (index < 1 || static_cast<size_t>(index) > list.size()) return 0;
            const auto& inv = list[static_cast<size_t>(index) - 1];
            lua_pushstring(L, gh->lookupName(inv.guid).c_str());
            lua_pushnumber(L, inv.level);
            lua_pushstring(L, "");              // className
            lua_pushstring(L, "");              // classFilename
            lua_pushnumber(L, inv.status + 1);   // wire counts from zero
            lua_pushnumber(L, inv.rank);
            lua_pushboolean(L, inv.isGuildMember ? 1 : 0);
            return 7;
        }},
                // The open event's own questions (0x005b8a50, 0x005b8aa0):
                // whether the player may edit it, and its calendarType - nil
                // with none open.
                {"CalendarEventCanEdit", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && gh->calendarCanEditOpenEvent() ? 1 : 0);
            return 1;
        }},
                {"CalendarEventGetCalendarType", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh || !gh->isCalendarEventOpen()) {
                lua_pushnil(L);
                return 1;
            }
            lua_pushstring(L, wowee::game::calendarTypeForFlags(
                gh->getCalendarEventDetail().flags));
            return 1;
        }},
                // The selected row of the invite list, by its invite id
                // (0x005b8ae0, 0x005bbb00): 0 where no row carries it, or with
                // no event open.
                {"CalendarEventSelectInvite", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1))
                return luaL_error(L, "Usage: CalendarEventSelectInvite(index)");
            const auto index = static_cast<int>(lua_tointeger(L, 1));
            auto* gh = getGameHandler(L);
            if (gh && index >= 1) gh->calendarSelectInvite(static_cast<size_t>(index) - 1);
            return 0;
        }},
                {"CalendarEventGetSelectedInvite", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int selected = 0;
            if (gh && gh->isCalendarEventOpen()) {
                const auto& open = gh->getCalendarEventDetail();
                for (size_t i = 0; i < open.invitees.size(); ++i) {
                    if (open.invitees[i].inviteId == open.selectedInviteId) {
                        selected = static_cast<int>(i) + 1;
                        break;
                    }
                }
            }
            lua_pushinteger(L, selected);
            return 1;
        }},
                // The invite list's order, as its criterion and whether it is
                // reversed (0x005b8690): "" and false with no event open.
                {"CalendarEventGetInviteSortCriterion", [](lua_State* L) -> int {
            static constexpr std::array<const char*, 6> kCriteria{
                "name", "level", "class", "status", "party", "notes"};
            auto* gh = getGameHandler(L);
            if (!gh || !gh->isCalendarEventOpen()) {
                lua_pushstring(L, "");
                lua_pushboolean(L, 0);
                return 2;
            }
            const auto& open = gh->getCalendarEventDetail();
            const size_t criterion = open.inviteSortCriterion;
            lua_pushstring(L, criterion < kCriteria.size() ? kCriteria[criterion] : "");
            lua_pushboolean(L, criterion < kCriteria.size() &&
                                   open.inviteSortReversed[criterion] ? 1 : 0);
            return 2;
        }},
                // CalendarEventSortInvites("criterion"[, reverse]) (0x005b8570):
                // one of the six words, anything else "status"; `reverse`
                // turns that one's reversal over, and left out does so too
                // (0x00815500 defaults it to 1).
                {"CalendarEventSortInvites", [](lua_State* L) -> int {
            if (!lua_isstring(L, 1))
                return luaL_error(L, "Usage: CalendarEventSortInvites(\"criteria\", reverse)");
            static constexpr std::array<const char*, 6> kCriteria{
                "name", "level", "class", "status", "party", "notes"};
            std::string word = lua_tostring(L, 1);
            toLowerInPlace(word);
            uint8_t criterion = 3;
            for (size_t i = 0; i < kCriteria.size(); ++i)
                if (word == kCriteria[i]) { criterion = static_cast<uint8_t>(i); break; }
            const bool toggle = luaClientSwitchArg(L, 2, true);
            if (auto* gh = getGameHandler(L)) gh->calendarSortOpenInvites(criterion, toggle);
            return 0;
        }},
                // The player's own answer to the open event (0x005bd6e0,
                // 0x005bd760, 0x005bd750, 0x005bd7d0).
                {"CalendarEventAvailable", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->calendarRespondToOpenEvent(1);
            return 0;
        }},
                {"CalendarEventDecline", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->calendarRespondToOpenEvent(2);
            return 0;
        }},
                {"CalendarEventTentative", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->calendarRespondToOpenEvent(8);
            return 0;
        }},
                {"CalendarEventSignUp", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->calendarSignUpToOpenEvent();
            return 0;
        }},
                // The open event's lock (0x10) and auto-approve (0x20) flags
                // (0x005bb870 - 0x005bb930), on the draft the edit commits as
                // well.
                {"CalendarEventSetLocked", [](lua_State* L) -> int {
            return calendarSetOpenEventFlag(L, 0x10, true);
        }},
                {"CalendarEventClearLocked", [](lua_State* L) -> int {
            return calendarSetOpenEventFlag(L, 0x10, false);
        }},
                {"CalendarEventSetAutoApprove", [](lua_State* L) -> int {
            return calendarSetOpenEventFlag(L, 0x20, true);
        }},
                {"CalendarEventClearAutoApprove", [](lua_State* L) -> int {
            return calendarSetOpenEventFlag(L, 0x20, false);
        }},
                // CalendarEventRemoveInvite(index) (0x005bb0c0).
                {"CalendarEventRemoveInvite", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1))
                return luaL_error(L, "Usage: CalendarEventRemoveInvite(index)");
            const auto index = static_cast<long long>(std::lround(lua_tonumber(L, 1))) - 1;
            auto* gh = getGameHandler(L);
            if (gh && index >= 0) gh->calendarRemoveOpenInvite(static_cast<size_t>(index));
            return 0;
        }},
                // CalendarEventGetInviteResponseTime(index) → weekday, month,
                // day, year, hour, minute (0x005ba420): when that row answered,
                // all six 0 where it has no time or there is no such row.
                {"CalendarEventGetInviteResponseTime", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1))
                return luaL_error(L, "Usage: CalendarEventGetInviteResponseTime(index)");
            const auto index = static_cast<long long>(std::lround(lua_tonumber(L, 1))) - 1;
            auto* gh = getGameHandler(L);
            const wowee::game::CalendarEventInvitee* row = nullptr;
            if (gh && gh->isCalendarEventOpen() && index >= 0) {
                const auto& list = gh->getCalendarEventDetail().invitees;
                if (static_cast<size_t>(index) < list.size()) row = &list[static_cast<size_t>(index)];
            }
            if (!row || wowee::game::packWowPackedTime(row->statusTime) == 0) {
                for (int i = 0; i < 6; ++i) lua_pushnumber(L, 0);
                return 6;
            }
            const auto& t = row->statusTime;
            lua_pushnumber(L, t.weekday + 1);
            lua_pushnumber(L, t.month);
            lua_pushnumber(L, t.day);
            lua_pushnumber(L, t.fullYear());
            lua_pushnumber(L, t.hour);
            lua_pushnumber(L, t.minute);
            return 6;
        }},
                // CalendarEventGetTextures(eventType) → name, texture,
                // expansion, difficulty per row (0x005bb970): raids for 1,
                // dungeons for 2, nothing for any other.
                {"CalendarEventGetTextures", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1))
                return luaL_error(L, "Usage: CalendarEventGetTextures(eventType)");
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            const auto rows = gh->calendarEventTextures(
                static_cast<int>(std::lround(lua_tonumber(L, 1))));
            if (rows.empty() || !lua_checkstack(L, static_cast<int>(rows.size() * 4))) return 0;
            for (const auto& r : rows) {
                lua_pushstring(L, r.name.c_str());
                lua_pushstring(L, r.texture.c_str());
                lua_pushinteger(L, static_cast<lua_Integer>(r.expansion));
                lua_pushstring(L, r.difficulty.c_str());
            }
            return static_cast<int>(rows.size() * 4);
        }},
                // Whether the open event is among the invitations still
                // waiting on an answer (0x005bdd90); false with none open.
                {"CalendarEventHasPendingInvite", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && gh->calendarOpenEventHasPendingInvite() ? 1 : 0);
            return 1;
        }},
                // Whether an event has been copied to paste (0x005b8540).
                {"CalendarContextEventClipboard", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && gh->calendarHasCopiedEvent() ? 1 : 0);
            return 1;
        }},
                // ---- Creating an event ----
                //
                // The staging half of the calendar's write side. Each of these
                // writes one field of the draft and CalendarAddEvent sends it;
                // none of them talks to the server on its own, which is what
                // lets the create form be filled in in any order and cancelled
                // without trace.
                // The new event becomes the open one (0x005bbe50), which is
                // what CalendarEventInvite and CalendarAddEvent act on. Its
                // date and time start unset (0x005f2a20): CalendarAddEvent
                // refuses it until they are given.
                {"CalendarNewEvent", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh || !gh->openCalendarNewEvent(wowee::game::CalendarNewKind::Player)) return 0;
            calendarDraft() = wowee::game::CalendarEventDraft{};
            calendarDraft().flags = gh->getCalendarEventDetail().flags;
            return 0;
        }},
                // Only in a guild; otherwise the error dialog and nothing
                // opened (0x005bcd80 -> 0x005bbf20). Flagged 0x40, with no
                // one on its list and no invitations to it.
                {"CalendarNewGuildAnnouncement", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh || !gh->openCalendarNewEvent(wowee::game::CalendarNewKind::Announcement)) return 0;
            calendarDraft() = wowee::game::CalendarEventDraft{};
            calendarDraft().flags = gh->getCalendarEventDetail().flags;
            return 0;
        }},
                // Only in a guild; otherwise the error dialog and nothing
                // opened (0x005bc030). CALENDAR_FLAG_GUILD_EVENT, which is
                // what the server reads to decide the event belongs to the
                // guild rather than the player.
                {"CalendarNewGuildEvent", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh || !gh->openCalendarNewEvent(wowee::game::CalendarNewKind::GuildEvent)) return 0;
            calendarDraft() = wowee::game::CalendarEventDraft{};
            calendarDraft().flags = gh->getCalendarEventDetail().flags;
            return 0;
        }},
                {"CalendarEventSetTitle", [](lua_State* L) -> int {
            // Clipped to what the server accepts. It refuses the whole packet
            // over 31 characters rather than truncating, so a long title would
            // silently create nothing at all.
            std::string title(luaL_optstring(L, 1, ""));
            if (title.size() > 31) title.resize(31);
            calendarDraft().title = std::move(title);
            return 0;
        }},
                {"CalendarEventSetDescription", [](lua_State* L) -> int {
            std::string desc(luaL_optstring(L, 1, ""));
            if (desc.size() > 255) desc.resize(255);
            calendarDraft().description = std::move(desc);
            return 0;
        }},
                // Interface types count from one and the wire counts from
                // zero, in the same order: constants.lua has
                // CALENDAR_EVENTTYPE_RAID = 1 where CalendarMgr.h has
                // CALENDAR_TYPE_RAID = 0. Passed straight through, every event
                // would be created as the next type along - a raid saved as a
                // dungeon, with nothing to see but the wrong icon.
                {"CalendarEventSetType", [](lua_State* L) -> int {
            const int uiType = static_cast<int>(luaL_optnumber(L, 1, 1));
            calendarDraft().type =
                static_cast<uint8_t>(uiType > 0 ? uiType - 1 : 0);
            return 0;
        }},
                {"CalendarEventSetDate", [](lua_State* L) -> int {
            auto& d = calendarDraft();
            d.eventTime.month = static_cast<int>(luaL_optnumber(L, 1, d.eventTime.month));
            d.eventTime.day   = static_cast<int>(luaL_optnumber(L, 2, d.eventTime.day));
            const int year    = static_cast<int>(luaL_optnumber(L, 3, d.eventTime.fullYear()));
            d.eventTime.yearSince2000 = year - 2000;
            // The weekday is part of the packed field, so it has to follow the
            // date rather than be left at whatever the last one was.
            d.eventTime.weekday = wowee::game::weekdayOf(
                d.eventTime.month, d.eventTime.day, d.eventTime.fullYear()) - 1;
            d.hasDate = true;
            return 0;
        }},
                {"CalendarEventSetTime", [](lua_State* L) -> int {
            auto& d = calendarDraft();
            d.eventTime.hour   = static_cast<int>(luaL_optnumber(L, 1, d.eventTime.hour));
            d.eventTime.minute = static_cast<int>(luaL_optnumber(L, 2, d.eventTime.minute));
            d.hasTime = true;
            return 0;
        }},
                // The same boundary as the type. The interface hands over a
                // dropdown button id, which counts from one, and the wire's
                // CalendarRepeatType counts from zero - so "Never" would have
                // been sent as "Weekly".
                {"CalendarEventSetRepeatOption", [](lua_State* L) -> int {
            const int uiOption = static_cast<int>(luaL_optnumber(L, 1, 1));
            calendarDraft().repeatOption =
                static_cast<uint8_t>(uiOption > 0 ? uiOption - 1 : 0);
            return 0;
        }},
                // CalendarEventSetTextureID(textureIndex) (0x005bb800 ->
                // 0x005b9630): the dungeon at that 1-based place in the list
                // CalendarEventGetTextures gives for the event's type - a raid
                // (wire type 0) the raids', a dungeon (1) the dungeons'. Any
                // other type, or a place off the list, changes nothing.
                {"CalendarEventSetTextureID", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1))
                return luaL_error(L, "Usage: CalendarEventSetTextureID(textureIndex)");
            auto* gh = getGameHandler(L);
            auto& draft = calendarDraft();
            if (!gh || draft.type > 1) return 0;
            const long index = std::lround(lua_tonumber(L, 1)) - 1;
            const auto rows = gh->calendarEventTextures(draft.type == 0 ? 1 : 2);
            if (index >= 0 && static_cast<size_t>(index) < rows.size())
                draft.dungeonId = static_cast<int32_t>(rows[static_cast<size_t>(index)].dungeonId);
            return 0;
        }},
                // Editing the open event, and deleting it.
                //
                // Both act on whichever event is open rather than on the
                // draft, which is what the interface means: the edit form is
                // filled from the open event and CalendarUpdateEvent commits
                // it back. With nothing open there is nothing to change, so
                // both do nothing rather than inventing an id.
                {"CalendarUpdateEvent", [](lua_State* L) -> int {
            // 0x005bced0 asks the taint check first (kind 0x13: insecure code
            // only inside a hardware event).
            if (!taint::allowed(L, 0x13)) return 0;
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            const auto& open = gh->getCalendarEventDetail();
            if (open.eventId == 0) return 0;
            gh->updateCalendarEvent(open.eventId, 0, calendarDraft());
            return 0;
        }},
                {"CalendarRemoveEvent", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            const auto& open = gh->getCalendarEventDetail();
            if (open.eventId == 0) return 0;
            gh->removeCalendarEvent(open.eventId, 0);
            return 0;
        }},
                // Whether the edit form has anything to commit. The interface
                // greys its save button on this, so answering true always
                // would offer to save an event nobody had touched.
                {"CalendarEventHaveSettingsChanged", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { lua_pushboolean(L, 0); return 1; }
            const auto& open = gh->getCalendarEventDetail();
            const auto& d = calendarDraft();
            const bool changed =
                open.eventId != 0 &&
                (d.title != open.title || d.description != open.description ||
                 d.type != open.type ||
                 d.eventTime.day != open.eventTime.day ||
                 d.eventTime.month != open.eventTime.month ||
                 d.eventTime.hour != open.eventTime.hour ||
                 d.eventTime.minute != open.eventTime.minute);
            lua_pushboolean(L, changed ? 1 : 0);
            return 1;
        }},
                // Setting another invitee's status or moderator rank, which
                // is what an event's owner does to confirm, bench or promote
                // someone. The player's own answer is a different packet
                // (CMSG_CALENDAR_EVENT_RSVP) and goes through the RSVP verbs.
                //
                // The index is into the open event's invite list, and the
                // status arrives one-based from the interface where the wire
                // counts from zero.
                {"CalendarEventSetStatus", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            const int uiStatus = static_cast<int>(luaL_optnumber(L, 2, 1));
            if (!gh) return 0;
            const auto& ev = gh->getCalendarEventDetail();
            if (index < 1 || static_cast<size_t>(index) > ev.invitees.size()) return 0;
            const auto& inv = ev.invitees[static_cast<size_t>(index) - 1];
            gh->setCalendarInviteStatus(inv.guid, ev.eventId, inv.inviteId,
                                        static_cast<uint8_t>(uiStatus > 0 ? uiStatus - 1 : 0));
            return 0;
        }},
                {"CalendarEventSetModerator", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (!gh) return 0;
            const auto& ev = gh->getCalendarEventDetail();
            if (index < 1 || static_cast<size_t>(index) > ev.invitees.size()) return 0;
            const auto& inv = ev.invitees[static_cast<size_t>(index) - 1];
            gh->setCalendarInviteModerator(inv.guid, ev.eventId, inv.inviteId, 1);
            return 0;
        }},
                {"CalendarEventClearModerator", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (!gh) return 0;
            const auto& ev = gh->getCalendarEventDetail();
            if (index < 1 || static_cast<size_t>(index) > ev.invitees.size()) return 0;
            const auto& inv = ev.invitees[static_cast<size_t>(index) - 1];
            gh->setCalendarInviteModerator(inv.guid, ev.eventId, inv.inviteId, 0);
            return 0;
        }},
                // CalendarEventInvite(name) - invite someone to the open event
                // (0x005bcf00 -> 0x005bc360): to a new one before it exists,
                // or to the one opened. Which, and whether the player may, is
                // the open event's; the checks are calendarEventInvite's.
                {"CalendarEventInvite", [](lua_State* L) -> int {
            const char* name = lua_tostring(L, 1);
            auto* gh = getGameHandler(L);
            if (!gh || !name) return 0;
            gh->calendarEventInvite(name);
            return 0;
        }},
                // The three bounds go on the wire: this has its own opcode
                // rather than being an invitation with the name left out, and
                // the server picks the members from them. The rank counts from
                // 1 here and from 0 on the wire (0x005bccb0).
                {"CalendarMassInviteGuild", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1) && !lua_isnumber(L, 2) && !lua_isnumber(L, 3))
                return luaL_error(L, "Usage: CalendarMassInviteGuild(minLevel, maxLevel, minRank)");
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            gh->massInviteGuildToCalendarEvent(
                static_cast<uint32_t>(std::lround(lua_tonumber(L, 1))),
                static_cast<uint32_t>(std::lround(lua_tonumber(L, 2))),
                static_cast<uint32_t>(std::lround(lua_tonumber(L, 3)) - 1));
            return 0;
        }},
                // CalendarMassInviteArenaTeam(index) (0x005bce60): arena team
                // 1-3 (its usage text names CalendarNewArenaTeamEvent).
                {"CalendarMassInviteArenaTeam", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1))
                return luaL_error(L, "Usage: CalendarNewArenaTeamEvent(index)");
            if (auto* gh = getGameHandler(L))
                gh->massInviteArenaTeamToCalendarEvent(
                    static_cast<uint32_t>(std::lround(lua_tonumber(L, 1)) - 1));
            return 0;
        }},
                // And the commit (0x005bcc70), held five seconds after the
                // last (createCalendarEvent).
                {"CalendarAddEvent", [](lua_State* L) -> int {
            if (!taint::allowed(L, 0x13)) return 0;   // 0x005bcc70
            if (auto* gh = getGameHandler(L)) {
                gh->createCalendarEvent(calendarDraft());
            }
            return 0;
        }},
                // CalendarCanAddEvent() - may an event be added now
                // (0x005b8ba0): no calendar request pending and the last added
                // five seconds gone (0x00c207fc).
                {"CalendarCanAddEvent", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && gh->calendarCanAddEvent() ? 1 : 0);
            return 1;
        }},
                // ---- The right-click menu on a day's event ----
                //
                // CalendarContextSelectEvent names the row the menu is about
                // and every verb below acts on that one. Kept as state because
                // the menu asks for it back - the dropdown is built in one
                // call and clicked in another, with nothing carried between.
                {"CalendarContextSelectEvent", [](lua_State* L) -> int {
            calendarContextRow() = {
                .monthOffset = static_cast<int>(luaL_optnumber(L, 1, 0)),
                .day = static_cast<int>(luaL_optnumber(L, 2, 0)),
                .index = static_cast<int>(luaL_optnumber(L, 3, 0))};
            return 0;
        }},
                {"CalendarContextGetEventIndex", [](lua_State* L) -> int {
            const auto& row = calendarContextRow();
            if (row.day < 1 || row.index < 1) {
                // Three nils rather than none: the interface unpacks this as a
                // group, and a short answer is the shape framexml_short_returns
                // exists to catch.
                lua_pushnil(L); lua_pushnil(L); lua_pushnil(L);
                return 3;
            }
            lua_pushnumber(L, row.monthOffset);
            lua_pushnumber(L, row.day);
            lua_pushnumber(L, row.index);
            return 3;
        }},
                // Answering an invitation, which is the one calendar action
                // that needs nothing staged first. The status values are the
                // server's CalendarInviteStatus (CalendarMgr.h:73).
                // The record's invitation (0x005ba700, 0x005ba7f0, 0x005ba910,
                // 0x005baa20): still to answer (invite flags 1); "CREATOR" or
                // "MODERATOR"; its status, from 1 (0 for no record); its
                // type.
                {"CalendarContextInviteIsPending", [](lua_State* L) -> int {
            std::optional<CalendarRecord> rec;
            if (!calendarRecordArg(L, rec))
                return luaL_error(L, "Usage: CalendarContextInviteIsPending([-1.0.1], monthDay, index)");
            lua_pushboolean(L, rec && (rec->invite.flags & 1u) != 0 ? 1 : 0);
            return 1;
        }},
                {"CalendarContextInviteModeratorStatus", [](lua_State* L) -> int {
            std::optional<CalendarRecord> rec;
            if (!calendarRecordArg(L, rec))
                return luaL_error(L, "Usage: CalendarContextInviteModeratorStatus([-1.0.1], monthDay, index)");
            if (rec && (rec->invite.flags & 6u) != 0) lua_pushstring(L, calendarModeratorText(rec->invite.flags));
            else lua_pushnil(L);
            return 1;
        }},
                {"CalendarContextInviteStatus", [](lua_State* L) -> int {
            std::optional<CalendarRecord> rec;
            if (!calendarRecordArg(L, rec))
                return luaL_error(L, "Usage: CalendarContextInviteStatus([-1.0.1], monthDay, index)");
            lua_pushnumber(L, rec ? rec->invite.status + 1 : 0);
            return 1;
        }},
                {"CalendarContextInviteType", [](lua_State* L) -> int {
            std::optional<CalendarRecord> rec;
            if (!calendarRecordArg(L, rec))
                return luaL_error(L, "Usage: CalendarContextInviteType([-1.0.1], monthDay, index)");
            lua_pushnumber(L, rec ? calendarInviteType(*rec) : 0);
            return 1;
        }},
                // CalendarGetDayEventSequenceInfo (0x005b9ca0): the day's
                // place in the record's run (from 0), the run's length, and
                // the sequence type; 0, 0, "" for no record.
                {"CalendarGetDayEventSequenceInfo", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2) || !lua_isnumber(L, 3))
                return luaL_error(L, "Usage: CalendarGetDayEventSequenceInfo([-1.0.1], monthDay, index)");
            std::optional<CalendarRecord> rec;
            calendarRecordArg(L, rec);
            if (!rec) {
                lua_pushnumber(L, 0);
                lua_pushnumber(L, 0);
                lua_pushstring(L, "");
                return 3;
            }
            lua_pushnumber(L, rec->entry.sequenceIndex);
            lua_pushnumber(L, rec->entry.numSequenceDays);
            lua_pushstring(L, calendarSequenceType(rec->flags, rec->entry.sequenceIndex,
                                                   rec->entry.numSequenceDays));
            return 3;
        }},
                // The open event's moderation (0x005b88b0, 0x005b88e0).
                {"CalendarEventIsModerator", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && gh->calendarIsModerator() ? 1 : 0);
            return 1;
        }},
                {"CalendarEventCanModerate", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: CalendarEventCanModerate(index)");
            auto* gh = getGameHandler(L);
            const auto index = static_cast<int>(std::lround(lua_tonumber(L, 1)));
            lua_pushboolean(L, gh && index >= 1 && gh->calendarCanModerate(static_cast<size_t>(index) - 1) ? 1 : 0);
            return 1;
        }},
                // CalendarEventSetSize(size) (0x005bb410 -> 0x005f1230): where
                // the player may edit the open event, the most it may hold -
                // more than one, and not for an announcement or a guild event
                // (0x440).
                {"CalendarEventSetSize", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1)) return 0;
            auto* gh = getGameHandler(L);
            const auto size = static_cast<uint32_t>(std::lround(lua_tonumber(L, 1)));
            if (!gh || !gh->calendarCanEditOpenEvent()) return 0;
            constexpr uint32_t kNoSize = wowee::game::calendar_flag::kAnnouncement |
                                         wowee::game::calendar_flag::kGuildEvent;
            if ((gh->getCalendarEventDetail().flags & kNoSize) == 0 && size > 1) calendarDraft().maxInvites = size;
            return 0;
        }},
                // CalendarEventSetLockoutDate(month, day, year) and
                // CalendarEventSetLockoutTime(hour, minute) (0x005bb650,
                // 0x005bb750 -> 0x005f12e0, 0x005f1340): the open event's
                // second time, where the player may edit it and it is no
                // announcement or guild event; else, or for a date or time
                // that is none, CALENDAR_ERROR_INVALID_DATE / _TIME.
                {"CalendarEventSetLockoutDate", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2) || !lua_isnumber(L, 3))
                return luaL_error(L, "Usage: CalendarEventSetLockoutDate(month, day, year)");
            auto* gh = getGameHandler(L);
            const int month = static_cast<int>(std::lround(lua_tonumber(L, 1)));
            const int day = static_cast<int>(std::lround(lua_tonumber(L, 2)));
            const int year = static_cast<int>(std::lround(lua_tonumber(L, 3)));
            constexpr uint32_t kNoLockout = wowee::game::calendar_flag::kAnnouncement |
                                            wowee::game::calendar_flag::kGuildEvent;
            const bool editable = gh && gh->calendarCanEditOpenEvent() &&
                                  (gh->getCalendarEventDetail().flags & kNoLockout) == 0;
            // 0x0076c480: a day of a month of 2000 to 2031.
            const bool valid = month >= 1 && month <= 12 && year >= 2000 && year < 2032 && day >= 1 &&
                               day <= wowee::game::calendarMonthAt(month, year, 0).numDays;
            if (!editable || !valid) {
                if (gh) gh->calendarSayError("CALENDAR_ERROR_INVALID_DATE", "Enter a valid date.");
                return 0;
            }
            auto& d = calendarDraft();
            d.lockoutTime.month = month;
            d.lockoutTime.day = day;
            d.lockoutTime.yearSince2000 = year - 2000;
            d.lockoutTime.weekday = wowee::game::weekdayOf(month, day, year) - 1;
            d.hasLockoutDate = true;
            return 0;
        }},
                {"CalendarEventSetLockoutTime", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1) || !lua_isnumber(L, 2))
                return luaL_error(L, "Usage: CalendarEvenSettLockoutTime(hour, minute)");
            auto* gh = getGameHandler(L);
            const auto hour = static_cast<uint32_t>(std::lround(lua_tonumber(L, 1)));
            const auto minute = static_cast<uint32_t>(std::lround(lua_tonumber(L, 2)));
            constexpr uint32_t kNoLockout = wowee::game::calendar_flag::kAnnouncement |
                                            wowee::game::calendar_flag::kGuildEvent;
            const bool editable = gh && gh->calendarCanEditOpenEvent() &&
                                  (gh->getCalendarEventDetail().flags & kNoLockout) == 0;
            // 0x0076c3c0: an hour under 24 and a minute under 60.
            if (!editable || hour >= 24 || minute >= 60) {
                if (gh) gh->calendarSayError("CALENDAR_ERROR_INVALID_TIME", "Enter a valid time.");
                return 0;
            }
            auto& d = calendarDraft();
            d.lockoutTime.hour = static_cast<int>(hour);
            d.lockoutTime.minute = static_cast<int>(minute);
            d.hasLockoutTime = true;
            return 0;
        }},
                {"CalendarContextInviteAvailable", [](lua_State* L) -> int {
            return calendarRespondToContextInvite(L, 1);   // accepted
        }},
                {"CalendarContextInviteDecline", [](lua_State* L) -> int {
            return calendarRespondToContextInvite(L, 2);   // declined
        }},
                {"CalendarContextInviteTentative", [](lua_State* L) -> int {
            return calendarRespondToContextInvite(L, 8);   // tentative
        }},
                {"CalendarContextInviteRemove", [](lua_State* L) -> int {
            return calendarRespondToContextInvite(L, 9);   // removed
        }},
                // Whether the player may edit or delete the row the menu is
                // about, which is whether they created it. The server decides
                // for real and refuses otherwise; this is what greys the menu
                // entry rather than letting it be clicked and rejected.
                {"CalendarContextEventCanEdit", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) { lua_pushboolean(L, 0); return 1; }
            const auto rows = calendarDayRows(L);
            const int index = static_cast<int>(luaL_optnumber(L, 3, 0));
            if (index < 1 || static_cast<size_t>(index) > rows.size()) {
                lua_pushboolean(L, 0);
                return 1;
            }
            const auto& row = rows[static_cast<size_t>(index) - 1];
            if (row.kind != wowee::game::CalendarEntryKind::Event) {
                lua_pushboolean(L, 0);
                return 1;
            }
            const auto& ev = gh->getCalendarData().events[row.index];
            lua_pushboolean(L, ev.creatorGuid != 0 &&
                                   ev.creatorGuid == gh->getPlayerGuid() ? 1 : 0);
            return 1;
        }},
                // CalendarContextEventCanComplain([monthOffset, day, index])
                // (0x005bd4f0) and CalendarContextEventComplain (0x005bd600):
                // the row named, or the one the menu is about, a usage error
                // with neither. Whether and how is calendarCanComplain's and
                // calendarComplain's.
                {"CalendarContextEventCanComplain", [](lua_State* L) -> int {
            uint64_t eventId = 0;
            if (!calendarNamedOrContextEvent(L, eventId))
                return luaL_error(L, "Usage: CalendarContextEventCanComplain([-1.0.1], monthDay, index)");
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && eventId && gh->calendarCanComplain(eventId) ? 1 : 0);
            return 1;
        }},
                {"CalendarContextEventComplain", [](lua_State* L) -> int {
            uint64_t eventId = 0;
            if (!calendarNamedOrContextEvent(L, eventId))
                return luaL_error(L, "Usage: CalendarContextEventComplain([-1.0.1], monthDay, index)");
            if (auto* gh = getGameHandler(L); gh && eventId) gh->calendarComplain(eventId);
            return 0;
        }},
                // CalendarContextDeselectEvent() (0x005b8510): the menu is
                // about no event any more.
                {"CalendarContextDeselectEvent", [](lua_State*) -> int {
            calendarContextRow() = {};
            return 0;
        }},
                // CalendarGetMonthNames() / CalendarGetWeekdayNames()
                // (0x005b80e0, 0x005b8120): the interface's MONTH_JANUARY ..
                // MONTH_DECEMBER and WEEKDAY_SUNDAY .. WEEKDAY_SATURDAY.
                {"CalendarGetMonthNames", [](lua_State* L) -> int {
            static constexpr const char* kMonths[12] = {
                "MONTH_JANUARY", "MONTH_FEBRUARY", "MONTH_MARCH", "MONTH_APRIL",
                "MONTH_MAY", "MONTH_JUNE", "MONTH_JULY", "MONTH_AUGUST",
                "MONTH_SEPTEMBER", "MONTH_OCTOBER", "MONTH_NOVEMBER", "MONTH_DECEMBER"};
            for (const char* key : kMonths) pushGlobalText(L, key);
            return 12;
        }},
                {"CalendarGetWeekdayNames", [](lua_State* L) -> int {
            static constexpr const char* kDays[7] = {
                "WEEKDAY_SUNDAY", "WEEKDAY_MONDAY", "WEEKDAY_TUESDAY", "WEEKDAY_WEDNESDAY",
                "WEEKDAY_THURSDAY", "WEEKDAY_FRIDAY", "WEEKDAY_SATURDAY"};
            for (const char* key : kDays) pushGlobalText(L, key);
            return 7;
        }},
                // Delete the row the menu is about.
                {"CalendarContextEventRemove", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            const auto& row = calendarContextRow();
            const auto rows = calendarRowsFor(*gh, row.monthOffset, row.day);
            if (row.index < 1 || static_cast<size_t>(row.index) > rows.size()) return 0;
            const auto& entry = rows[static_cast<size_t>(row.index) - 1];
            if (entry.kind != wowee::game::CalendarEntryKind::Event) return 0;
            gh->removeCalendarEvent(
                gh->getCalendarData().events[entry.index].eventId, 0);
            return 0;
        }},
                // CalendarContextEventCopy([monthOffset, day, index]) - keep
                // an event to paste (0x005bd340): the row named, or with any
                // of the three missing the one the menu is about (0x00c207e8),
                // a usage error with neither. Whether the player may is
                // calendarCopyEvent's.
                {"CalendarContextEventCopy", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            CalendarContextRow row = calendarContextRow();
            const bool named = !lua_isnoneornil(L, 1) && !lua_isnoneornil(L, 2) &&
                               !lua_isnoneornil(L, 3);
            if (named) {
                row = {.monthOffset = static_cast<int>(std::lround(lua_tonumber(L, 1))),
                       .day = static_cast<int>(std::lround(lua_tonumber(L, 2))),
                       .index = static_cast<int>(std::lround(lua_tonumber(L, 3)))};
            } else if (row.day < 1 || row.index < 1) {
                return luaL_error(L, "Usage: Script_CalendarContextEventCopy([-1.0.1], monthDay, index)");
            }
            const auto rows = calendarRowsFor(*gh, row.monthOffset, row.day);
            if (row.index < 1 || static_cast<size_t>(row.index) > rows.size()) return 0;
            const auto& entry = rows[static_cast<size_t>(row.index) - 1];
            if (entry.kind != wowee::game::CalendarEntryKind::Event) return 0;
            gh->calendarCopyEvent(gh->getCalendarData().events[entry.index].eventId);
            return 0;
        }},
                // CalendarContextEventPaste(monthOffset, day) - the copied
                // event onto that day at its own hour (0x005bd460 ->
                // 0x005bc870), held five seconds after the last event added
                // or pasted, as CalendarAddEvent is.
                {"CalendarContextEventPaste", [](lua_State* L) -> int {
            if (lua_isnoneornil(L, 1) || lua_isnoneornil(L, 2))
                return luaL_error(L, "Usage: Script_CalendarContextEventPaste([-1.0.1], monthDay)");
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            const auto viewed = calendarViewedMonth();
            const auto info = wowee::game::calendarMonthAt(
                viewed.first, viewed.second, static_cast<int>(std::lround(lua_tonumber(L, 1))));
            gh->calendarPasteEvent(info.month, info.year,
                                   static_cast<int>(std::lround(lua_tonumber(L, 2))), info.numDays);
            return 0;
        }},
                // Signing up to a guild event, which is an RSVP with the
                // signed-up status rather than a separate request.
                {"CalendarContextEventSignUp", [](lua_State* L) -> int {
            return calendarRespondToContextInvite(L, 6);   // SIGNED_UP
        }},
                // Which of the six kinds the menu's row is, so the menu can
                // offer the right verbs. Read from the same day list the row
                // was chosen out of.
                {"CalendarContextEventGetCalendarType", [](lua_State* L) -> int {
            const auto& row = calendarContextRow();
            auto* gh = getGameHandler(L);
            // Nil rather than nothing, for the same reason as
            // CalendarContextGetEventIndex above: the arity belongs at the
            // binding rather than resting on Lua filling the gap.
            if (!gh || row.day < 1) { lua_pushnil(L); return 1; }
            const auto rows = calendarRowsFor(*gh, row.monthOffset, row.day);
            if (row.index < 1 || static_cast<size_t>(row.index) > rows.size()) {
                lua_pushnil(L);
                return 1;
            }
            const auto& entry = rows[static_cast<size_t>(row.index) - 1];
            if (entry.kind == wowee::game::CalendarEntryKind::Holiday) {
                lua_pushstring(L, "HOLIDAY");
            } else {
                lua_pushstring(L, calendarTypeName(
                    gh->getCalendarData().events[entry.index]));
            }
            return 1;
        }},
                // Which event is selected, as monthOffset, day, index.
                //
                // Nothing is selected until an event view exists to select
                // into, so all three are nil - but three nils rather than no
                // values at all. The interface unpacks it as a group, and a
                // binding that answers short is the shape framexml_short_returns
                // is pinned at zero to catch: it reads as correct here only
                // because Lua happens to fill the rest with nil.
                {"CalendarGetEventIndex", [](lua_State* L) -> int {
            lua_pushnil(L);
            lua_pushnil(L);
            lua_pushnil(L);
            return 3;
        }},
                // The first unanswered invite on a day, or zero.
                //
                // A number always, never nothing: the interface compares it
                // straight away - `if ( pendingInviteIndex > 0 )` with no
                // guard in front - so nil raises rather than reading as "no
                // invite". The invite list the server sends is not per-day, so
                // a day has one only when one of its events is in it.
                {"CalendarGetFirstPendingInvite", [](lua_State* L) -> int {
            const auto rows = calendarDayRows(L);
            auto* gh = getGameHandler(L);
            if (!gh) { lua_pushnumber(L, 0); return 1; }
            const auto& cal = gh->getCalendarData();
            for (size_t i = 0; i < rows.size(); ++i) {
                if (rows[i].kind != wowee::game::CalendarEntryKind::Event) continue;
                const uint64_t id = cal.events[rows[i].index].eventId;
                for (const auto& invite : cal.invites) {
                    // 0 is CALENDAR_INVITESTATUS_INVITED - still unanswered.
                    if (invite.eventId == id && invite.status == 0) {
                        lua_pushnumber(L, static_cast<double>(i + 1));
                        return 1;
                    }
                }
            }
            lua_pushnumber(L, 0);
            return 1;
        }},
                // ---- The month the grid is drawn from ----
                //
                // The viewed month is the interface's own state in WoW too:
                // CalendarSetMonth and CalendarSetAbsMonth move it and
                // CalendarGetMonth reads it back, so it is kept here rather
                // than recomputed from the clock on every call - which would
                // make the previous-month button do nothing.
                {"CalendarGetMonth", [](lua_State* L) -> int {
            // The offset is optional: CalendarGetMonth() means the viewed
            // month, and the interface calls it that way and with -1 and 1 in
            // the same breath to fill the leading and trailing cells.
            const int offset = static_cast<int>(luaL_optnumber(L, 1, 0));
            const auto viewed = calendarViewedMonth();
            const auto info = wowee::game::calendarMonthAt(
                viewed.first, viewed.second, offset);
            lua_pushnumber(L, info.month);
            lua_pushnumber(L, info.year);
            lua_pushnumber(L, info.numDays);
            lua_pushnumber(L, info.firstWeekday);
            return 4;
        }},
                {"CalendarSetMonth", [](lua_State* L) -> int {
            const int offset = static_cast<int>(luaL_checknumber(L, 1));
            const auto viewed = calendarViewedMonth();
            const auto info = wowee::game::calendarMonthAt(
                viewed.first, viewed.second, offset);
            calendarSetViewedMonth(info.month, info.year);
            return 0;
        }},
                {"CalendarSetAbsMonth", [](lua_State* L) -> int {
            calendarSetViewedMonth(static_cast<int>(luaL_checknumber(L, 1)),
                                   static_cast<int>(luaL_checknumber(L, 2)));
            return 0;
        }},
                // The ends of the range, both as weekday, month, day, year.
                //
                // Nothing on the wire says how far the server will let the
                // player look or book, so this client offers the year either
                // side of today. A year ahead is the limit the interface
                // already has a string for - CALENDAR_ERROR_CREATEDATE_AFTER_MAX
                // - and the same span back keeps past events reachable, which
                // is what the previous-month button is for.
                // CalendarGetMaxDate() (0x005b82a0): the last day there is,
                // 31 December 2030 - the day before 1 January 2031 - as
                // weekday, month, day, year.
                {"CalendarGetMaxDate", [](lua_State* L) -> int {
            lua_pushnumber(L, wowee::game::weekdayOf(12, 31, 2030));
            lua_pushnumber(L, 12);
            lua_pushnumber(L, 31);
            lua_pushnumber(L, 2030);
            return 4;
        }},
                // CalendarGetMinHistoryDate() (0x005b8350): the game clock's
                // date (0x00d37f98) fourteen days back - with none, its
                // unset fields: 0, 0, 0, 1999.
                {"CalendarGetMinHistoryDate", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const auto now = gh ? gh->getGameDate() : std::nullopt;
            if (!now) {
                lua_pushnumber(L, 0);
                lua_pushnumber(L, 0);
                lua_pushnumber(L, 0);
                lua_pushnumber(L, 1999);
                return 4;
            }
            std::tm t{};
            t.tm_year = now->fullYear() - 1900;
            t.tm_mon = now->month - 1;
            t.tm_mday = now->day - 14;
            t.tm_isdst = -1;
            const std::time_t back = std::mktime(&t);
            const std::tm b = core::localTime(back);
            lua_pushnumber(L, b.tm_wday + 1);
            lua_pushnumber(L, b.tm_mon + 1);
            lua_pushnumber(L, b.tm_mday);
            lua_pushnumber(L, b.tm_year + 1900);
            return 4;
        }},
                // CalendarGetAbsMonth(month[, year]) (0x005b9ac0 ->
                // 0x005b8f70) → month, year, numDays, firstWeekday: the year
                // given, else the game year the first call saw (0x00acff90,
                // 2004 with none), held within five of the game year and to
                // 2004-2030; the month taken within its year, not carried.
                {"CalendarGetAbsMonth", [](lua_State* L) -> int {
            if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: CalendarGetAbsMonth(month[, year])");
            auto* gh = getGameHandler(L);
            const auto now = gh ? gh->getGameDate() : std::nullopt;
            const int gameYear = now ? now->yearSince2000 : -1;
            static int defaultYear = -1;
            if (defaultYear < 0) defaultYear = gameYear < 0 ? 4 : gameYear;
            const int month = static_cast<int>(lua_tonumber(L, 1));
            int year = lua_isnumber(L, 2) ? static_cast<int>(lua_tonumber(L, 2)) - 2000 : defaultYear;
            year = std::clamp(year, gameYear - 5, gameYear + 5);
            year = std::clamp(year, 4, 30);
            const int month0 = std::abs((month - 1) % 12);
            const auto info = wowee::game::calendarMonthAt(month0 + 1, year + 2000, 0);
            lua_pushnumber(L, month0 + 1);
            lua_pushnumber(L, year + 2000);
            lua_pushnumber(L, info.numDays);
            lua_pushnumber(L, info.firstWeekday);
            return 4;
        }},
                {"CalendarGetMinDate", [](lua_State* L) -> int {
            return pushCalendarBoundDate(L, -12);
        }},
                {"CalendarGetMaxCreateDate", [](lua_State* L) -> int {
            return pushCalendarBoundDate(L, 12);
        }},
                // Counted from zero, like GetExpansionLevel beside it and like
                // the table it is used to index. This answered one higher, and
                // reputationframe.lua does
                //     MAX_PLAYER_LEVEL = MAX_PLAYER_LEVEL_TABLE[GetAccountExpansionLevel()]
                // against a table holding only 0, 1 and 2 - so on Wrath it read
                // nothing and MAX_PLAYER_LEVEL became nil, which
                // `newLevel < MAX_PLAYER_LEVEL` then raised on every level gained.
                // On the earlier two it simply came out an expansion too high,
                // and a level 60 was never treated as capped.
                // ... and it comes from the account, not from the client.  The
                // binding beside it answers the expansion this build speaks;
                // this one answers the byte the realm chose for *this* account,
                // which is the only thing that ever writes the client's +0x2f2d
                // (0x004ddc20 reads that and nothing else).  The two agree for
                // an upgraded account and part company for a trial one: the
                // realm sends 0 there, and MAX_PLAYER_LEVEL_TABLE[0] is 60, so
                // a trial character caps at 60 in the original and at 80 here.
                // Clamping is deliberately not done - a byte outside 0..2 makes
                // the table read nil in the original too, and that is what it
                // should do here.
                {"GetAccountExpansionLevel", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->accountState().present) {
                lua_pushnumber(L, gh->accountState().accountExpansion);
                return 1;
            }
            // No block in the response - the result was neither OK nor the
            // queue - so the realm never said anything about the account.  The
            // original would be reading a zero it never filled; there is no
            // session to notice that in, and the build's own expansion is the
            // least wrong answer to give a frame that is asking.
            lua_pushnumber(L, expansionLevelZeroBased(L));
            return 1;
        }},
    };
    for (const auto& [name, func] : api) {
        lua_pushcfunction(L, func);
        lua_setglobal(L, name);
    }
}

} // namespace wowee::addons
