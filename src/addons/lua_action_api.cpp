// lua_action_api.cpp - Action bar, cursor/pickup, keyboard input, key bindings, and pet actions Lua API bindings.
// Extracted from lua_engine.cpp as part of §5.1 (Tame LuaEngine).
#include "game/item_text.hpp"
#include "game/equip_slot_fit.hpp"
// For the cursor's own sounds: the coordinator reaches the manager that asks a
// SoundEntries row by name, which is how the client asks them.
#include "audio/audio_coordinator.hpp"
#include "audio/ui_sound_manager.hpp"
#include "addons/lua_api_helpers.hpp"
#include "addons/lua_taint.hpp"
#include "addons/lua_api_registrations.hpp"
// For fireEvent: paging has to reach the frames as well as the addons.
#include "addons/lua_engine.hpp"
#include "game/inventory_slots.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/keybinding_manager.hpp"
#include "ui/gamepad_controls.hpp"

#include <span>
#include "core/config_paths.hpp"
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include "game/pet_action.hpp"
#include "game/macro_eval.hpp"
#include "game/shapeshift_forms.hpp"
#include "game/game_utils.hpp"
#include "game/macro_options.hpp"
#include "game/interface_text.hpp"
#include "game/vehicle.hpp"
#include "ui/chat/game_state_adapter.hpp"
#include "imgui.h"
#include <optional>
#include <SDL3/SDL_keyboard.h>

namespace wowee::addons {

// What the interface is holding.
//
// This is the original interface's cursor, and it is not the only one: this
// client's inventory screen keeps a held item of its own. They do not need
// merging, because whichever interface is drawing the bags is the one the
// player can pick anything up from, and that is the cursor in use - with the
// bags owned, as they are by default, this is it.
//
// What that does mean is that CursorHasItem answers for this cursor alone. An
// addon asking while the player is dragging in this client's own bags is told
// no, which is true of the cursor it can see and not of the player's hand.
// MERCHANT is a fifth kind and it is not an item: what the cursor holds is a
// vendor's list index, not something the player owns. FrameXML's merchant
// window puts one there on every left-click - PickupMerchantItem - and buying
// is what happens when it is dropped into a bag, so without it a left-click at
// a vendor did nothing at all and only right-click bought.
//
// EQUIPMENT_SET is the client's cursor type 0xd (0x00520dc0): s_cursorId is the
// set's id (DAT_00ac80a0), which may be 0 - a set, not nothing.
/// AMMO is the ammunition lifted off the paperdoll's ammo slot (the client's
/// cursor type 9): an item to the interface, holding an entry rather than an
/// item, since the ammunition never left the bags.
enum class CursorType { NONE, SPELL, ITEM, MACRO, MERCHANT, MONEY, GUILDBANK, EQUIPMENT_SET, AMMO };
static CursorType s_cursorType = CursorType::NONE;
static uint32_t   s_cursorId   = 0;    // spellId, itemId, or action slot
static int        s_cursorSlot = 0;    // source slot for placement
/// Which container the cursor's item came out of, and one of the four values
/// game::slots documents: 0 the backpack, 1-4 a worn bag, -1 the paperdoll,
/// kCursorNoSource the action bar. See cursorSourceIsInventory.
static int        s_cursorBag  = -1;
static uint64_t   s_cursorMoney = 0;   // copper, when the cursor carries money
/// How much of a guild bank stack the cursor took, or zero for all of it. The
/// wire carries the split on the *withdrawal*, not on the pickup, so the amount
/// has to wait on the cursor until the drop says where it is going.
static uint32_t   s_cursorSplit = 0;
static void setCursorType(lua_State* L, CursorType type);
/// Where the cursor's item came from, in wire bag/slot space. Declared here
/// because the guild bank pickup above needs it and it is defined further down
/// with the other cursor helpers.
static bool cursorWireSlot(uint8_t& bag, uint8_t& slot);
/// Whether a drop on an inventory slot is a move at all, and puts the cursor
/// down when it is not. See kCursorNoSource: an item off the action bar has
/// nowhere to be moved from, so dropping it in a bag or on the paperdoll does
/// nothing but end the drag, which is what the real client does.
static bool droppedItemFromNowhere(lua_State* L);
/// Sell what the cursor is holding. Defined below, beside the buy it mirrors.
static void soldHeldItem(lua_State* L);

uint64_t cursorMoney() {
    return s_cursorType == CursorType::MONEY ? s_cursorMoney : 0;
}

void setCursorMoney(lua_State* L, uint64_t copper) {
    // Through setCursorType rather than by assignment: it is what fires
    // CURSOR_UPDATE and what shows the action bars' empty slots. Setting the
    // type by hand would put money on the cursor with nothing told about it.
    s_cursorMoney = copper;
    if (copper == 0) {
        if (s_cursorType == CursorType::MONEY) setCursorType(L, CursorType::NONE);
        return;
    }
    s_cursorId = 0;
    s_cursorSlot = 0;
    s_cursorBag = -1;
    setCursorType(L, CursorType::MONEY);
}

uint32_t cursorItemId() {
    return s_cursorType == CursorType::ITEM ? s_cursorId : 0;
}

int cursorEquipSlot() {
    // s_cursorBag is -1 only for a pickup off the paperdoll, where s_cursorSlot
    // is the one-based slot the item came out of.
    return (s_cursorType == CursorType::ITEM && s_cursorBag == -1) ? s_cursorSlot : 0;
}

bool cursorItemCameFromInventory() {
    // The same test cursorWireSlot makes before it will name a source, because
    // it is the same question: an item the wire can point at is an item that
    // can be destroyed.
    return s_cursorType == CursorType::ITEM &&
           game::slots::cursorSourceIsInventory(s_cursorBag);
}

/// PickupPetAction(slot) - pick a pet ability up off the pet bar.
///
/// Defined and does nothing. The cursor here holds spells, items, actions and
/// macros; a pet action is a fifth kind in its own slot space, and moving one
/// is a server operation this client does not send. Inventing a cursor state
/// that cannot be put down anywhere would be worse than not picking up.
///
/// Defined because it is reachable three ways - shift-clicking a pet button,
/// dragging one, and dropping on one - and the pet bar is drawn. Undefined,
/// the first shift-click on a pet ability takes the bar down. Clicking a pet
/// ability to use it goes through a different path and works; only rearranging
/// the bar is lost.
//
// It asks the taint check first all the same (0x005d65c0, kind 0xe).
static int lua_PickupPetAction(lua_State* L) {
    taint::allowed(L, 0xe);
    return 0;
}

/// Change what the cursor is holding, and say so.
///
/// CURSOR_UPDATE is how the interface learns the cursor picked something up or
/// put it down - action buttons redraw their highlight on it, and the
/// equipment and container frames test the cursor from it. Thirteen places set
/// this and none of them said anything, so the cursor changed silently.
///
/// Every one of those thirteen goes through here. Converting some and not
/// others would give a cursor that announces half its changes, which is harder
/// to reason about than one that announces none.
///
/// Only on an actual change: several of these run on every click of an empty
/// slot, setting NONE over NONE, and the interface would redraw for each.
/// Defined below, beside the rest of the cursor state it clears. Declared here
/// because the sites that put the cursor down come first in the file, and each
/// of them used to clear only part of what a pickup sets.
static void clearCursorItem(lua_State* L);
/// The equipment set of an id, and its icon. Defined beside the set pickup.
static const game::EquipmentSetInfo* equipmentSetById(game::GameHandler* gh, uint32_t setId);
static std::string equipmentSetIcon(const game::EquipmentSetInfo* set);

/// What the cursor sounds like going up and going down.
///
/// The client asks for both by name, and asks them in the routine that moves
/// the cursor rather than in one place it all passes through: every pickup
/// plays INTERFACESOUND_CURSORGRABOBJECT just after it sets the cursor kind
/// (0x00520960:426, 0x00520a80:505, 0x00520be0:527, 0x00520c60:566,
/// 0x00520dc0:614), and ClearCursor plays the sound of the kind it is putting
/// down in its switch over that kind (0x00519280: the item kinds at :7790 and
/// the shared tail at :7818 ask INTERFACESOUND_CURSORDROPOBJECT, while the
/// money cursor asks LOOTWINDOWCOINSOUND at :7771 instead - the same row its
/// pickup asks at 00520000.c:360, the coin being a different sound on both
/// sides; the kind that hands an item back to the world, 1, plays nothing).
/// All three names are rows of SoundEntries.dbc, which is how they are asked
/// here too.
static void askCursorSound(lua_State* L, CursorType type, bool pickedUp) {
    // Silent while the interface is building itself, as PlaySound is.
    if (LuaEngine::uiSoundsSuppressed()) return;
    auto* svc = getLuaServices(L);
    auto* ac = svc ? svc->audioCoordinator : nullptr;
    auto* sfx = ac ? ac->getUiSoundManager() : nullptr;
    if (!sfx) return;
    if (type == CursorType::MONEY) {
        sfx->playByName("LOOTWINDOWCOINSOUND");
        return;
    }
    sfx->playByName(pickedUp ? "INTERFACESOUND_CURSORGRABOBJECT"
                             : "INTERFACESOUND_CURSORDROPOBJECT");
}

static void setCursorType(lua_State* L, CursorType type) {
    if (s_cursorType == type) return;
    // Putting anything down forgets the guild bank split. It rides on the
    // cursor between the pickup and the drop, and a leftover would be applied
    // to the next withdrawal - a stack the player never asked to divide.
    if (type != CursorType::GUILDBANK) s_cursorSplit = 0;
    const CursorType prevType = s_cursorType;
    const bool wasCarrying = (s_cursorType != CursorType::NONE);
    const bool nowCarrying = (type != CursorType::NONE);
    s_cursorType = type;
    lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
    auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (!engine) return;
    // Which side of the announcement the sound sits on follows the client:
    // ClearCursor plays the drop inside its switch and raises CURSOR_UPDATE
    // (event 275) at its tail afterwards (00510000.c:7768-7830), while a
    // pickup's grab comes after the ClearCursor call that made the
    // announcement (00520000.c:523-530). A carried kind changing straight to
    // another carried kind - clicking an action button while a bag item is on
    // the cursor - is the client's ClearCursor followed by a pickup, so it
    // plays both, the old kind's drop before the announcement and the new
    // kind's grab after it.
    if (wasCarrying) askCursorSound(L, prevType, false);
    engine->fireEvent("CURSOR_UPDATE", {});
    if (nowCarrying) askCursorSound(L, type, true);
    // Picking something up shows the empty slots of the action bars, so there
    // is somewhere visible to drop it; putting it down hides them again. Every
    // action button registers for both and neither was ever fired, so dragging
    // a spell towards the bar showed no targets at all - the bar looked as
    // though it would not take it.
    //
    // Fired on the change rather than on every set: this function is called
    // with the same type repeatedly and the grid would flicker.
    if (nowCarrying && !wasCarrying) {
        engine->fireEvent("ACTIONBAR_SHOWGRID", {});
    } else if (wasCarrying && !nowCarrying) {
        engine->fireEvent("ACTIONBAR_HIDEGRID", {});
    }
}

/// The pet bar's slot a bonus bar slot (0-based) refers to while the bonus bar
/// holds the pet's actions (0x005d4ad0 -> 0x005ab800): slots 120..131 are
/// 0x10000000 | i for the pet bar's first ten (0x005d3240), the last two
/// empty. The packed pet slot, or nothing for an ordinary slot; an empty pet
/// slot is 0.
struct PetActionRef {
    uint32_t index = 0;
    uint32_t packed = 0;
};
static std::optional<PetActionRef> petActionRef(game::GameHandler* gh, int slot) {
    if (!gh || !gh->petActionsOnBonusBar()) return std::nullopt;
    const int i = slot - game::GameHandler::kBonusPetSlotFirst;
    if (i < 0 || i >= game::GameHandler::PET_ACTION_BAR_SLOTS) return std::nullopt;
    return PetActionRef{static_cast<uint32_t>(i), gh->getPetActionSlot(i)};
}
using game::pet::petSlotSpell;
/// Whether this pet belongs to a hunter (0x0071b630).
///
/// Asked of the pet, never of the player: the pet's own UNIT_FIELD_PETNUMBER has
/// to be non-zero - that is what makes a pet a pet rather than a guardian to the
/// client - and the unit its UNIT_FIELD_CREATEDBY pair names has to be a player
/// reading class 3 in byte 1 of UNIT_FIELD_BYTES_0. So a warlock's voidwalker,
/// whose master is no hunter, answers false, and so does a pet whose creator this
/// client never got a record for: a failed lookup is false, not unknown.
///
/// Shared because more than one binding asks it - HasPetUI directly,
/// GetPetHappiness through the happiness row lookup at 0x0071f390, which is the
/// same predicate again, and GetUnitHealthModifier/GetUnitPowerModifier, whose
/// answer of exactly one is this test and nothing else - and the happiness icon
/// has to agree with what HasPetUI said about it. Declared in
/// addons/lua_api_helpers.hpp, which is where the other cross-file pet answers
/// live; the definition stays here, beside the pet bindings that started it.
bool hunterPetOf(game::GameHandler* gh, const game::Unit* pet) {
    if (!gh || !pet) return false;
    const uint16_t number = game::fieldIndex(game::UF::UNIT_FIELD_PETNUMBER);
    if (number == 0xFFFF || pet->getField(number) == 0) return false;
    // The creator guid's high half lives one index above the low half.
    const uint16_t by = game::fieldIndex(game::UF::UNIT_FIELD_CREATEDBY);
    if (by == 0xFFFF) return false;
    const uint64_t creator =
        pet->getField(by)
        | (static_cast<uint64_t>(pet->getField(static_cast<uint16_t>(by + 1))) << 32);
    uint8_t classId = 0;
    if (auto owner = gh->getEntityManager().getEntity(creator)) {
        const uint32_t bytes0 =
            owner->getField(game::fieldIndex(game::UF::UNIT_FIELD_BYTES_0));
        classId = static_cast<uint8_t>((bytes0 >> 8) & 0xFF);
    }
    if (classId == 0 && creator == gh->getPlayerGuid()) classId = gh->getPlayerClass();
    if (classId == 0) classId = gh->lookupPlayerClass(creator);
    return classId == 3;   // hunter
}
/// The global naming a pet command's or stance's icon (0x005a97f0:
/// "PET_%s_TEXTURE"), nothing for anything else.
static const char* petTokenTexture(uint32_t packed) {
    const uint32_t action = game::pet::petActionId(packed);
    switch (game::pet::petActionType(packed)) {
        case game::pet::ActionType::Command:
            switch (action) {
                case game::pet::kStay: return "PET_WAIT_TEXTURE";
                case game::pet::kFollow: return "PET_FOLLOW_TEXTURE";
                case game::pet::kAttack: return "PET_ATTACK_TEXTURE";
                case game::pet::kAbandon: return "PET_DISMISS_TEXTURE";
                default: return nullptr;
            }
        case game::pet::ActionType::Reaction:
            switch (action) {
                case game::pet::kPassive: return "PET_PASSIVE_TEXTURE";
                case game::pet::kDefensive: return "PET_DEFENSIVE_TEXTURE";
                case game::pet::kAggressive: return "PET_AGGRESSIVE_TEXTURE";
                default: return nullptr;
            }
        default:
            return nullptr;
    }
}
/// Uses a pet slot as the pet bar's button does (0x005d4210): at the unit
/// named by `unitArg`, else the target.
static void castPetSlot(lua_State* L, game::GameHandler* gh, uint32_t packed, int unitArg) {
    if (!gh || !gh->hasPet() || (packed & 0x00FFFFFFu) == 0) return;
    // CGPetInfo::SendPetAction asks the taint check first (0x005d4210, kind
    // 2): a pet's action is refused outright to insecure code.
    if (!taint::allowed(L, 2)) return;
    uint64_t target = 0;
    if (const char* uid = luaL_optstring(L, unitArg, nullptr)) {
        std::string u(uid);
        toLowerInPlace(u);
        target = resolveUnitGuid(gh, u);
    }
    if (target == 0 && gh->hasTarget()) target = gh->getTargetGuid();
    gh->usePetSlot(packed, target);
}

static int lua_HasAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnFalse(L); }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1; // WoW uses 1-indexed slots
    if (const auto ref = petActionRef(gh, slot)) {
        // 0x005a8160: a command or a stance, or a spell.
        const uint32_t state = (ref->packed >> 24) & 0x3f;
        lua_pushboolean(L, ref->packed != 0 && (state == 6 || state == 7 || petSlotSpell(ref->packed) != 0));
        return 1;
    }
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size())) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, !bar[slot].isEmpty());
    return 1;
}

// ---- What a macro shows (UIMacros.cpp) ---------------------------------
//
// The spell or item a macro's icon, tooltip and cooldown follow, read out of
// its text the way the client reads it (game::macro_eval): once when the text
// changes (0x00565c40), and again for a macro whose answer can change - a
// conditional, a slot number, a /castsequence - or whose answer named nothing
// yet. The client re-evaluates those on the events that can change them
// (0x00566660, 0x00566160); here they are worked out when asked.
namespace {
struct MacroShown {
    std::string body;
    game::macro_eval::Parsed parsed;
    game::macro_eval::Result result;
    bool read = false;
};
std::unordered_map<uint32_t, MacroShown>& macrosShown() {
    static std::unordered_map<uint32_t, MacroShown> shown;
    return shown;
}
}  // namespace

/// A casting slash command's kind (the table at 0x00ace300): the command's
/// global strings SLASH_CAST1, SLASH_CAST2 ... compared exactly.
static int macroSlashKind(lua_State* L, const std::string& word) {
    static const std::pair<const char*, int> kCommands[] = {
        {"SLASH_CAST", game::macro_eval::kCast},
        {"SLASH_CASTRANDOM", game::macro_eval::kRandom},
        {"SLASH_CASTSEQUENCE", game::macro_eval::kSequence},
        {"SLASH_USE", game::macro_eval::kCast},
        {"SLASH_USERANDOM", game::macro_eval::kRandom},
        {"SLASH_EQUIP", game::macro_eval::kEquip},
        {"SLASH_EQUIP_TO_SLOT", game::macro_eval::kEquipSlot},
    };
    for (const auto& [name, kind] : kCommands) {
        for (int i = 1; i < 10; ++i) {
            const std::string key = std::string(name) + std::to_string(i);
            lua_getglobal(L, key.c_str());
            const char* text = lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr;
            const bool match = text && *text && word == text;
            const bool end = !text || !*text;
            lua_pop(L, 1);
            if (end) break;
            if (match) return kind;
        }
    }
    return 0;
}

/// A spell in a spellbook by its exact name and, when given, rank (0x0053f5e0):
/// with no rank the highest the book has.
static uint32_t macroSpellInBook(game::GameHandler* gh, const std::vector<uint32_t>& book,
                                 const std::string& name, const std::string& rank) {
    uint32_t best = 0;
    int bestRank = -1;
    for (uint32_t id : book) {
        if (gh->getSpellName(id) != name) continue;
        const std::string& r = gh->getSpellRank(id);
        if (!rank.empty()) {
            if (r == rank) return id;
            continue;
        }
        int n = 0;
        const size_t digits = r.find_first_of("0123456789");
        if (digits != std::string::npos) n = std::atoi(r.c_str() + digits);
        if (n > bestRank) {
            bestRank = n;
            best = id;
        }
    }
    return best;
}

namespace {
/// Call a global with string arguments, leaving `results` values on the
/// stack; false (and nothing left) when it is not a function or it raised.
bool callGlobal(lua_State* L, const char* name, std::initializer_list<const char*> args, int results) {
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    for (const char* a : args) lua_pushstring(L, a);
    if (lua_pcall(L, static_cast<int>(args.size()), results, 0) != 0) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}
bool globalTruth(lua_State* L, const char* name, std::initializer_list<const char*> args = {}) {
    if (!callGlobal(L, name, args, 1)) return false;
    const bool v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}
int globalNumber(lua_State* L, const char* name) {
    if (!callGlobal(L, name, {}, 1)) return 0;
    const int v = lua_isnumber(L, -1) ? static_cast<int>(lua_tonumber(L, -1)) : 0;
    lua_pop(L, 1);
    return v;
}
std::string globalString(lua_State* L, const char* name, std::initializer_list<const char*> args = {}) {
    if (!callGlobal(L, name, args, 1)) return {};
    std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return v;
}

/// What the macro conditions read (game::macro_options::State), from this
/// client: the unit queries the chat panel's conditions already answer as the
/// client does (GameStateAdapter), and the rest through the interface's own
/// calls where the interface already answers the same question.
class LuaMacroOptionState : public game::macro_options::State {
public:
    LuaMacroOptionState(lua_State* L, game::GameHandler& gh) : L_(L), gh_(gh), units_(gh, nullptr) {}

    uint64_t unitGuid(const std::string& unit) override {
        return unit.empty() ? gh_.getTargetGuid() : resolveUnitOrName(&gh_, unit);
    }
    bool inCombat() override {
        const auto flagged = [this](uint64_t guid) {
            const auto e = guid ? gh_.getEntityManager().getEntity(guid) : nullptr;
            return e && (e->getField(game::fieldIndex(game::UF::UNIT_FIELD_FLAGS)) & 0x80000u) != 0;
        };
        return flagged(gh_.getPlayerGuid()) || flagged(gh_.getPetGuid());
    }
    bool exists(uint64_t unit) override { return units_.entityExists(unit); }
    bool canAssist(uint64_t unit) override { return units_.entityMayAssist(unit); }
    bool canAttack(uint64_t unit) override { return units_.entityMayAttack(unit); }
    bool inPartyAsMember(uint64_t unit) override {
        if (!gh_.isInGroup()) return false;
        const auto& pd = gh_.getPartyData();
        const bool raid = game::groupIsRaid(pd.groupType);
        for (const auto& m : pd.members)
            if (m.guid == unit && (!raid || m.subGroup == pd.subGroup)) return true;
        return false;
    }
    bool inRaidAsMember(uint64_t unit) override {
        if (!inRaid()) return false;
        if (unit == gh_.getPlayerGuid()) return true;
        for (const auto& m : gh_.getPartyData().members)
            if (m.guid == unit) return true;
        return false;
    }
    bool dead(uint64_t unit) override { return units_.entityIsDead(unit); }
    /// 0x005eee70: 0x007261b0 for the clause's unit where there is one,
    /// the area alone where there is not.
    bool flyable(uint64_t unit) override {
        const bool present = unit != 0 && gh_.getEntityManager().getEntity(unit) != nullptr;
        return flyableHere(L_, present ? unit : 0);
    }
    bool outdoors() override { return globalTruth(L_, "IsOutdoors"); }
    bool hasPlayer() override { return gh_.getEntityManager().getEntity(gh_.getPlayerGuid()) != nullptr; }
    // The player's movement flags: SWIMMING 0x200000 and FLYING 0x2000000;
    // mounted is a mount display with WATER_WALK (0x10000000) clear.
    bool swimming() override { return (gh_.getMovementInfo().flags & 0x200000u) != 0; }
    bool flying() override { return (gh_.getMovementInfo().flags & 0x2000000u) != 0; }
    bool mounted() override { return gh_.isMounted() && (gh_.getMovementInfo().flags & 0x10000000u) == 0; }
    bool stealthed() override { return units_.isStealthed(); }
    bool inParty() override { return gh_.isInGroup(); }
    bool inRaid() override { return gh_.isInGroup() && game::groupIsRaid(gh_.getPartyData().groupType); }
    int shapeshiftForm() override { return globalNumber(L_, "GetShapeshiftForm"); }
    bool pet(std::string& name, std::string& family) override {
        if (!globalTruth(L_, "UnitExists", {"pet"})) return false;
        name = globalString(L_, "UnitName", {"pet"});
        family = globalString(L_, "UnitCreatureFamily", {"pet"});
        return true;
    }
    bool modifier(const std::string& name) override {
        if (name.empty()) return globalTruth(L_, "IsModifierKeyDown");
        static const std::pair<const char*, const char*> kKeys[] = {
            {"shift", "IsShiftKeyDown"}, {"ctrl", "IsControlKeyDown"}, {"alt", "IsAltKeyDown"},
            {"lshift", "IsLeftShiftKeyDown"}, {"rshift", "IsRightShiftKeyDown"},
            {"lctrl", "IsLeftControlKeyDown"}, {"rctrl", "IsRightControlKeyDown"},
            {"lalt", "IsLeftAltKeyDown"}, {"ralt", "IsRightAltKeyDown"},
        };
        std::string lower = name;
        toLowerInPlace(lower);
        for (const auto& [key, fn] : kKeys)
            if (lower == key) return globalTruth(L_, fn);
        return globalTruth(L_, "IsModifiedClick", {name.c_str()});
    }
    std::string clickButton() override { return currentClickButton(); }
    std::string macroButton() override {
        return runningMacro().running && runningMacro().hasButton ? runningMacro().button : std::string();
    }
    std::string mouseButtonName(int number) override {
        lua_getglobal(L_, "GetMouseButtonName");
        if (!lua_isfunction(L_, -1)) {
            lua_pop(L_, 1);
            return {};
        }
        lua_pushnumber(L_, number);
        if (lua_pcall(L_, 1, 1, 0) != 0) {
            lua_pop(L_, 1);
            return {};
        }
        std::string v = lua_isstring(L_, -1) ? lua_tostring(L_, -1) : "";
        lua_pop(L_, 1);
        return v;
    }
    int actionBarPage() override { return globalNumber(L_, "GetActionBarPage"); }
    int bonusBarOffset() override { return globalNumber(L_, "GetBonusBarOffset"); }
    bool equipped(const std::string& type) override { return globalTruth(L_, "IsEquippedItemType", {type.c_str()}); }
    bool channeling(std::string& spellName) override {
        spellName = globalString(L_, "UnitChannelInfo", {"player"});
        return !spellName.empty();
    }
    int activeSpec() override { return globalNumber(L_, "GetActiveTalentGroup"); }
    bool vehicleUi(uint64_t unit) override {
        const auto* seat = gh_.vehicleRoster().seatOf(unit);
        return seat && (seat->flags & game::vehicle::kSeatFlagHasVehicleUI) != 0;
    }
    bool playerVehicleUi() override { return vehicleUi(gh_.getPlayerGuid()); }
    std::string cursorType() override { return globalString(L_, "GetCursorInfo"); }
    void unknownOption(const std::string& name) override {
        gh_.addUIError(game::interfaceFormat("ERR_UNKNOWN_MACRO_OPTION_S", "Unknown macro option: %s", {name}));
    }

private:
    lua_State* L_;
    game::GameHandler& gh_;
    ui::GameStateAdapter units_;
};
}  // namespace

/// What macro evaluation needs from this client.
static game::macro_eval::Hooks macroHooks(lua_State* L, game::GameHandler* gh) {
    game::macro_eval::Hooks h;
    // The conditional parser (0x005f0df0); the applying clause's unit, when
    // it names one (0x0060abf0 with no fallback to the target).
    h.options = [L, gh](const std::string& text, std::string& result, uint64_t& target) {
        LuaMacroOptionState state(L, *gh);
        std::string unit;
        if (!game::macro_options::parse(text, state, result, unit)) return false;
        target = unit.empty() ? 0 : state.unitGuid(unit);
        return true;
    };
    // QueryCastSequence, the interface's own (ChatFrame.lua), as 0x00564900
    // calls it: its item, else its spell.
    h.castSequence = [L](const std::string& sequence) {
        std::string entry;
        lua_getglobal(L, "QueryCastSequence");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return entry;
        }
        lua_pushstring(L, sequence.c_str());
        if (lua_pcall(L, 1, 2, 0) != 0) {
            lua_pop(L, 1);
            return entry;
        }
        if (lua_isstring(L, -2)) entry = lua_tostring(L, -2);
        else if (lua_isstring(L, -1)) entry = lua_tostring(L, -1);
        lua_pop(L, 2);
        return entry;
    };
    // 0x00709de0: an item link's id, or an item known by its exact name.
    h.itemByName = [gh](const std::string& name) -> uint32_t {
        const size_t at = name.find("item:");
        if (at != std::string::npos) return static_cast<uint32_t>(std::strtoul(name.c_str() + at + 5, nullptr, 10));
        for (const auto& [id, info] : gh->getItemInfoCache())
            if (info.valid && info.name == name) return id;
        return 0;
    };
    // 0x00540200: "!" dropped; the whole text as a name, else the name before
    // "(" with the rank inside it.
    h.spellByName = [gh](const std::string& text, bool& pet) -> uint32_t {
        std::string name = !text.empty() && text[0] == '!' ? text.substr(1) : text;
        if (name.empty()) return 0;
        std::vector<uint32_t> book(gh->getKnownSpells().begin(), gh->getKnownSpells().end());
        const auto& petBook = gh->getPetSpells();
        const auto lookup = [&](const std::string& n, const std::string& r) -> uint32_t {
            pet = false;
            if (const uint32_t id = macroSpellInBook(gh, book, n, r)) return id;
            pet = true;
            return macroSpellInBook(gh, petBook, n, r);
        };
        if (const uint32_t id = lookup(name, "")) return id;
        const size_t open = name.find('(');
        if (open == std::string::npos) return 0;
        std::string rank = name.substr(open + 1);
        const size_t close = rank.find(')');
        if (close != std::string::npos) rank.resize(close);
        return lookup(name.substr(0, open), rank);
    };
    h.itemInSlot = [gh](int bag, int slot) -> uint32_t {
        const auto& inv = gh->getInventory();
        const game::ItemSlot* s = nullptr;
        if (bag < 0) {
            s = slot < game::Inventory::NUM_EQUIP_SLOTS ? inventorySlotItem(inv, slot + 1)
                                                       : containerItemSlot(inv, 0, slot - game::Inventory::NUM_EQUIP_SLOTS + 1);
        } else {
            s = containerItemSlot(inv, bag + 1, slot + 1);
        }
        return s ? s->item.itemId : 0;
    };
    return h;
}

/// The macro's state, read again when its text changed and worked out again
/// when its answer can have changed.
static const MacroShown* macroShown(lua_State* L, game::GameHandler* gh, uint32_t id) {
    if (!gh || id == 0) return nullptr;
    const std::string& body = gh->getMacroText(id);
    if (body.empty()) {
        macrosShown().erase(id);
        return nullptr;
    }
    MacroShown& m = macrosShown()[id];
    if (!m.read || m.body != body) {
        m.body = body;
        m.read = true;
        m.parsed = game::macro_eval::parse(body, [L](const std::string& w) { return macroSlashKind(L, w); });
        m.result = game::macro_eval::evaluate(m.parsed, macroHooks(L, gh));
    } else if (m.parsed.dynamic || m.result.spell < 0) {
        m.result = game::macro_eval::evaluate(m.parsed, macroHooks(L, gh));
    }
    return &m;
}

/// 0x00566660, every frame (it is called from the game UI's idle, 0x0052b1c0):
/// each macro on the bars whose answer can change is worked out again, and
/// the bar slots of one whose answer moved redraw (0x005aa920).
static int lua_WoweeUpdateShownMacros(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const auto& bar = gh->getActionBar();
    std::vector<uint32_t> done;
    for (size_t i = 0; i < bar.size(); ++i) {
        if (bar[i].type != game::ActionBarSlot::MACRO) continue;
        const uint32_t id = bar[i].id;
        if (std::find(done.begin(), done.end(), id) != done.end()) continue;
        done.push_back(id);
        const auto it = macrosShown().find(id);
        if (it == macrosShown().end() || !it->second.parsed.dynamic) continue;
        const game::macro_eval::Result before = it->second.result;
        const MacroShown* m = macroShown(L, gh, id);
        if (!m || (m->result.spell == before.spell && m->result.item == before.item &&
                   m->result.target == before.target))
            continue;
        for (size_t j = i; j < bar.size(); ++j) {
            if (bar[j].type == game::ActionBarSlot::MACRO && bar[j].id == id)
                gh->fireAddonEvent("ACTIONBAR_SLOT_CHANGED", {std::to_string(j + 1)});
        }
    }
    return 0;
}

/// The macro a Lua argument names (0x00566df0): its index, or its name without
/// case (0x00566330). 0 for none.
static uint32_t macroArg(lua_State* L, game::GameHandler* gh, int idx) {
    if (!gh) return 0;
    if (lua_isnumber(L, idx)) {
        const int index = luaToClientInt(L, idx);
        return index >= 1 && index <= 0x48 && !gh->getMacroText(static_cast<uint32_t>(index)).empty()
                   ? static_cast<uint32_t>(index) : 0;
    }
    if (!lua_isstring(L, idx)) return 0;
    std::string want = lua_tostring(L, idx);
    if (want.empty()) return 0;
    toLowerInPlace(want);
    for (uint32_t id : gh->getMacroIds()) {
        std::string name = gh->getMacroName(id);
        toLowerInPlace(name);
        if (name == want && !gh->getMacroText(id).empty()) return id;
    }
    return 0;
}

/// A macro's texture (0x00566ac0): the icon it was given - unless that is the
/// question mark, which asks for the icon of what the macro shows: its item's,
/// else its spell's (the active icon while the spell's form is in effect), else
/// the question mark after all.
static std::string macroTexture(lua_State* L, game::GameHandler* gh, uint32_t id) {
    static const char* const kQuestionMark = "Interface\\Icons\\INV_Misc_QuestionMark";
    std::string icon = gh->getMacroIcon(id);
    if (!icon.empty() && std::all_of(icon.begin(), icon.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        // The macro frame hands CreateMacro an index into the icon list.
        lua_getglobal(L, "GetMacroIconInfo");
        if (lua_isfunction(L, -1)) {
            lua_pushnumber(L, std::atoi(icon.c_str()));
            if (lua_pcall(L, 1, 1, 0) == 0) {
                icon = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
                lua_pop(L, 1);
            } else {
                lua_pop(L, 1);
                icon.clear();
            }
        } else {
            lua_pop(L, 1);
            icon.clear();
        }
    }
    if (!icon.empty() && icon.find('\\') == std::string::npos) icon = "Interface\\Icons\\" + icon;
    // Compared as the client compares it (0x0076e760): the name exactly.
    const size_t slash = icon.find_last_of('\\');
    const std::string name = slash == std::string::npos ? icon : icon.substr(slash + 1);
    if (!icon.empty() && name != "INV_Misc_QuestionMark") return icon;
    const MacroShown* m = macroShown(L, gh, id);
    if (m && m->result.item != 0) {
        const auto* info = gh->getItemInfo(m->result.item);
        const std::string path = info ? gh->getItemIconPath(info->displayInfoId) : std::string();
        if (!path.empty()) return path;
    } else if (m && m->result.spell > 0) {
        const auto spell = static_cast<uint32_t>(m->result.spell);
        const auto it = gh->spellNameCacheRef().find(spell);
        if (it != gh->spellNameCacheRef().end() && it->second.activeIconId != 0 && !m->result.petSpell) {
            for (const auto& form : game::knownShapeshiftForms(gh->getPlayerClass(), gh->getKnownSpells())) {
                if (form.spellId == spell && gh->formSpellInEffect(spell)) {
                    const std::string active = gh->getIconPath(it->second.activeIconId);
                    if (!active.empty()) return active;
                    break;
                }
            }
        }
        const std::string path = gh->getSpellIconPath(spell);
        if (!path.empty()) return path;
    }
    return kQuestionMark;
}

/// SetMacroSpell / SetMacroItem (0x00567320, 0x005670d0).
static int setMacroShown(lua_State* L, bool item) {
    if (!lua_isstring(L, 1) && !lua_isnumber(L, 1))
        return luaL_error(L, item ? "Usage: SetMacroItem(index or name, item [, target])"
                                  : "Usage: SetMacroSpell(index or name, spell [, target])");
    auto* gh = getGameHandler(L);
    const uint32_t id = macroArg(L, gh, 1);
    MacroShown* m = id ? const_cast<MacroShown*>(macroShown(L, gh, id)) : nullptr;
    if (!m) return 0;
    const char* text = lua_isstring(L, 2) ? lua_tostring(L, 2) : "";
    const auto hooks = macroHooks(L, gh);
    game::macro_eval::Result r;
    if (item) {
        r.item = hooks.itemByName(text);
        r.spell = r.item != 0 ? 0 : -1;
    } else {
        bool pet = false;
        const uint32_t spell = hooks.spellByName(text, pet);
        r.spell = spell != 0 ? static_cast<int32_t>(spell) : -1;
        r.petSpell = spell != 0 && pet;
    }
    if (const char* unit = lua_isstring(L, 3) ? lua_tostring(L, 3) : nullptr; unit && *unit) {
        std::string u(unit);
        toLowerInPlace(u);
        r.target = resolveUnitGuid(gh, u);
    }
    m->result = r;
    // No longer following its conditions (+0x574 cleared).
    m->parsed.dynamic = false;
    // 0x005aa920: every bar slot holding the macro redrawn.
    const auto& bar = gh->getActionBar();
    for (size_t i = 0; i < bar.size(); ++i) {
        if (bar[i].type == game::ActionBarSlot::MACRO && bar[i].id == id)
            gh->fireAddonEvent("ACTIONBAR_SLOT_CHANGED", {std::to_string(i + 1)});
    }
    if (item ? r.item != 0 : r.spell > 0) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

// GetActionAutocast(slot) (0x005a9290) → autoCastAllowed, autoCastEnabled:
// for a slot whose spell the pet casts - the pet's own on the bonus bar, or
// a macro showing a pet spell (0x005a8c30) - the two top bits of the pet's
// spell list entry for it (0x005d3410); nil, nil for any other.
static int lua_GetActionAutocast(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetActionAutocast(slot)");
    auto* gh = getGameHandler(L);
    const int slot = luaToClientInt(L, 1) - 1;
    uint32_t petSpell = 0;
    if (gh) {
        if (const auto ref = petActionRef(gh, slot)) {
            // A spell the charmer casts (AttributesEx6 0x40000) is not the
            // pet's.
            const uint32_t spell = petSlotSpell(ref->packed);
            const auto it = gh->spellNameCacheRef().find(spell);
            if (spell != 0 && (it == gh->spellNameCacheRef().end() || (it->second.attrEx6 & 0x40000u) == 0))
                petSpell = spell;
        } else if (slot >= 0 && slot < static_cast<int>(gh->getActionBar().size())) {
            const auto& action = gh->getActionBar()[static_cast<size_t>(slot)];
            if (action.type == game::ActionBarSlot::MACRO) {
                const MacroShown* m = macroShown(L, gh, action.id);
                if (m && m->result.item == 0 && m->result.spell > 0 && m->result.petSpell)
                    petSpell = static_cast<uint32_t>(m->result.spell);
            }
        }
    }
    bool known = false;
    if (petSpell != 0) {
        const auto& book = gh->getPetSpells();
        known = std::find(book.begin(), book.end(), petSpell) != book.end();
    }
    if (known && gh->isPetSpellAutocastable(petSpell)) lua_pushnumber(L, 1); else lua_pushnil(L);
    if (known && gh->isPetSpellAutocast(petSpell)) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 2;
}

// GetActionTexture(slot) → texturePath or nil
static int lua_GetActionTexture(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    if (const auto ref = petActionRef(gh, slot)) {
        // 0x005a97f0: a pet spell's icon, a command's or a stance's global.
        if (const uint32_t spell = petSlotSpell(ref->packed)) {
            const std::string icon = gh->getSpellIconPath(spell);
            if (icon.empty()) lua_pushnil(L);
            else lua_pushstring(L, icon.c_str());
            return 1;
        }
        const char* token = petTokenTexture(ref->packed);
        if (!token) return luaReturnNil(L);
        lua_getglobal(L, token);
        return 1;
    }
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        lua_pushnil(L);
        return 1;
    }
    const auto& action = bar[slot];
    if (action.type == game::ActionBarSlot::MACRO) {
        // The macro's texture (0x005a97f0 -> 0x00566ac0).
        if (gh->getMacroText(action.id).empty()) return luaReturnNil(L);
        lua_pushstring(L, macroTexture(L, gh, action.id).c_str());
        return 1;
    }
    if (action.type == game::ActionBarSlot::EQUIPMENT_SET) {
        // The set's icon, empty for one no longer there (0x005a97f0 ->
        // 0x005ae6b0).
        lua_pushstring(L, equipmentSetIcon(equipmentSetById(gh, action.id)).c_str());
        return 1;
    }
    if (action.type == game::ActionBarSlot::SPELL) {
        std::string icon = gh->getSpellIconPath(action.id);
        if (!icon.empty()) {
            lua_pushstring(L, icon.c_str());
            return 1;
        }
    } else if (action.type == game::ActionBarSlot::ITEM && action.id != 0) {
        const auto* info = gh->getItemInfo(action.id);
        if (info && info->displayInfoId != 0) {
            std::string icon = gh->getItemIconPath(info->displayInfoId);
            if (!icon.empty()) {
                lua_pushstring(L, icon.c_str());
                return 1;
            }
        }
    }
    lua_pushnil(L);
    return 1;
}

// IsCurrentAction(slot) → boolean
static int lua_IsCurrentAction(lua_State* L) {
    // Currently no "active action" tracking; return false
    (void)L;
    lua_pushboolean(L, 0);
    return 1;
}

// IsUsableAction(slot) → (1, nil) usable, (nil, 1) its power is short,
// (nil, nil) everything else. Never a pair of no's - see the shapes below.
static int lua_IsUsableAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnil(L); lua_pushnil(L); return 2; }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    if (const auto ref = petActionRef(gh, slot)) {
        // 0x005a9e20 for a pet's slot: its spell is cast by the pet
        // (0x005d3390), which must be there and afford it of its own power
        // (0x0080b5d0, 0x008017e0) - a vehicle's steam or energy, not the
        // player's mana. A command or a stance is usable while it is set.
        bool usable = ref->packed != 0;
        bool noPower = false;
        if (const uint32_t spell = petSlotSpell(ref->packed)) {
            const auto pet = std::dynamic_pointer_cast<game::Unit>(
                gh->getEntityManager().getEntity(gh->getPetGuid()));
            if (!pet) {
                usable = false;
            } else {
                auto* sh = gh->getSpellHandler();
                if (sh && !sh->enoughPower(spell, *pet)) {
                    // The same test, one answer: the pet affords the spell of
                    // its own power, and enoughPower() is what spells 0x008017e0
                    // in both directions (the pet's runic energy, a vehicle's
                    // steam, health under a -2 cost).
                    usable = false;
                    noPower = true;
                }
            }
        }
        lua_pushboolean(L, usable ? 1 : 0);
        lua_pushboolean(L, noPower ? 1 : 0);
        return 2;
    }
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        // The client's third shape: its slot test runs to 144 and an empty or
        // out-of-range slot reads (nil, nil), never a pair of no's.
        lua_pushnil(L);
        lua_pushnil(L);
        return 2;
    }
    const auto& action = bar[slot];
    // Not the cooldown. WoW's answer here is about whether the action *could*
    // be used - known, affordable, the right stance - and a spell waiting on a
    // cooldown is all of those. The wait is shown by the sweep over the icon,
    // which is drawn from GetActionCooldown and nothing to do with this.
    //
    // Folding it in here painted the icon dark grey for the whole of every
    // cooldown, because ActionButton_UpdateUsable has three branches and
    // "not usable, mana is fine" is the one that means unusable.
    // The client answers this one without computing anything at all: 0x005a7e60
    // reads two precomputed per-slot arrays - DAT_00c1ded8 (the slot's action is
    // there to be used) and DAT_00c1dc98 (its power is short) - over slots 1 to
    // 144, and turns the pair into exactly three shapes: usable and affordable
    // (1, nil), affordable-but-out-of-power (nil, 1), and everything else - an
    // empty slot, a slot number off the bar, an action that is not there to be
    // used - (nil, nil). No shape is (false, false), and combo points are not
    // among the tests: they gate the *cast* (0x0080b5d0 on AttributesEx
    // 0x100000|0x400000 against 0x00513920's per-target store), which is why a
    // finisher with no points on its target sits bright on the client's bar and
    // only fails when pressed.
    bool usable = true;
    bool noPower = false;
    if (action.type == game::ActionBarSlot::SPELL) {
        usable = gh->getKnownSpells().count(action.id) > 0;
        if (usable && action.id != 0) {
            const auto pe = gh->getEntityManager().getEntity(gh->getPlayerGuid());
            const auto* unit = pe ? dynamic_cast<game::Unit*>(pe.get()) : nullptr;
            // The client has one cost test, 0x008017e0, and it is the same one
            // behind the button and behind the flags the button is drawn from.
            // Asking it here rather than writing a second copy of it matters:
            // the flags ACTIONBAR_UPDATE_USABLE announces are kept by
            // enoughPower(), so a hand-rolled manaCost compare here could call
            // an action affordable while the rescan that repaints the icon
            // called it short - and that test reads the power type the *spell*
            // names (0x0071c2e0), not the caster's current one, and takes the
            // cost after its modifiers, not the template's number.
            auto* sh = gh->getSpellHandler();
            if (unit && sh && !sh->enoughPower(action.id, *unit)) {
                noPower = true;
                usable = false;
            }
        }
    }
    if (usable) {
        lua_pushnumber(L, 1.0);
        lua_pushnil(L);
    } else if (noPower) {
        lua_pushnil(L);
        lua_pushnumber(L, 1.0);
    } else {
        lua_pushnil(L);
        lua_pushnil(L);
    }
    return 2;
}

// IsActionInRange(slot) → 1 if in range, 0 if out, nil if no range check applicable
static int lua_IsActionInRange(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnNil(L); }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    // The pet casts a pet slot's spell, from where it is (0x005a9e20 ->
    // 0x005d3390): its range is measured from the pet.
    if (const auto ref = petActionRef(gh, slot)) {
        const uint32_t spell = petSlotSpell(ref->packed);
        if (spell == 0 || gh->getPetGuid() == 0) return luaReturnNil(L);
        return pushSpellRangeAnswer(L, gh, spell, gh->getTargetGuid(), gh->getPetGuid());
    }
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        lua_pushnil(L);
        return 1;
    }
    const auto& action = bar[slot];
    uint32_t spellId = 0;
    if (action.type == game::ActionBarSlot::SPELL) {
        spellId = action.id;
    } else {
        // Items/macros: no range check for now
        lua_pushnil(L);
        return 1;
    }
    return pushSpellRangeAnswer(L, gh, spellId, gh->getTargetGuid());
}

// GetActionInfo(slot) → actionType, id, subType
static int lua_GetActionInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return 0; }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    if (const auto ref = petActionRef(gh, slot)) {
        // 0x005a8f10: "spell", its place in the pet's spellbook, "pet", the
        // spell - for any of the ten, whatever it holds.
        const uint32_t spell = petSlotSpell(ref->packed);
        const auto& book = gh->getPetSpells();
        const auto at = std::find(book.begin(), book.end(), spell);
        const int index = spell != 0 && at != book.end() ? static_cast<int>(at - book.begin()) : -1;
        lua_pushstring(L, "spell");
        lua_pushnumber(L, index + 1);
        lua_pushstring(L, "pet");
        lua_pushnumber(L, spell);
        return 4;
    }
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        return 0;
    }
    const auto& action = bar[slot];
    switch (action.type) {
        case game::ActionBarSlot::SPELL:
            lua_pushstring(L, "spell");
            lua_pushnumber(L, action.id);
            lua_pushstring(L, "spell");
            // The fourth value, which for a spell action is the same id.
            // vehiclemenubar reads it to build a link on a modified click -
            // HandleModifiedItemClick(GetSpellLink(spellID)) - so without it
            // shift-clicking a vehicle button linked nothing. Only spells have
            // one; an item or a macro answers nil, as the real client does.
            lua_pushnumber(L, action.id);
            return 4;
        case game::ActionBarSlot::ITEM:
            lua_pushstring(L, "item");
            lua_pushnumber(L, action.id);
            lua_pushstring(L, "item");
            return 3;
        case game::ActionBarSlot::MACRO:
            lua_pushstring(L, "macro");
            lua_pushnumber(L, action.id);
            lua_pushstring(L, "macro");
            return 3;
        case game::ActionBarSlot::EQUIPMENT_SET:
            // "equipmentset" and its name, while it is there (0x005a8f10).
            if (const auto* set = equipmentSetById(gh, action.id)) {
                lua_pushstring(L, "equipmentset");
                lua_pushstring(L, set->name.c_str());
                return 2;
            }
            return 0;
        default:
            return 0;
    }
}

// GetActionCount(slot) → count (item stack count or 0)
static int lua_GetActionCount(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { return luaReturnZero(L); }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        lua_pushnumber(L, 0);
        return 1;
    }
    const auto& action = bar[slot];
    if (action.type == game::ActionBarSlot::ITEM && action.id != 0) {
        lua_pushnumber(L, gh->getInventory().countItem(action.id));
    } else {
        lua_pushnumber(L, 0);
    }
    return 1;
}

// GetActionCooldown(slot) → start, duration, enable (0x005a91c0 ->
// 0x005a8e40): a spell's from the player's history, a pet slot's spell from
// the pet's (0x00809000), an item's as its use spell cast from it
// (0x00809030).
static int lua_GetActionCooldown(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) { lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 1); return 3; }
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    if (const auto ref = petActionRef(gh, slot)) {
        const uint32_t spell = petSlotSpell(ref->packed);
        return pushHistoryCooldown(L, spell ? gh->petSpellCooldown(spell) : game::spell_history::History::Cooldown{});
    }
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) {
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 0);
        lua_pushnumber(L, 1);
        return 3;
    }
    const auto& action = bar[slot];
    if (action.type == game::ActionBarSlot::SPELL) return pushHistoryCooldown(L, gh->playerSpellCooldown(action.id));
    if (action.type == game::ActionBarSlot::ITEM) return pushHistoryCooldown(L, gh->itemCooldown(action.id));
    if (action.type == game::ActionBarSlot::MACRO) {
        // What the macro shows (0x005a8c30 -> 0x005a89f0): its item's
        // cooldown, else its spell's.
        if (const MacroShown* m = macroShown(L, gh, action.id)) {
            if (m->result.item != 0) return pushHistoryCooldown(L, gh->itemCooldown(m->result.item));
            if (m->result.spell > 0 && !m->result.petSpell)
                return pushHistoryCooldown(L, gh->playerSpellCooldown(static_cast<uint32_t>(m->result.spell)));
        }
    }
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 1);
    return 3;
}

// UseAction(slot, unit, button) - activate action bar slot (1-indexed)
//
// The second argument is a unit, and it was ignored. securetemplates is the
// only caller and it passes what the button's own attributes resolved to:
// nil for an ordinary click, and "player" when the self-cast modifier is
// held - which is the whole of how self-casting works. Dropping it meant
// every action went at the current target, so self-cast did nothing, and a
// unit-frame button carrying its own unit acted on whoever was targeted
// instead.
static int lua_UseAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int slot = static_cast<int>(luaL_checknumber(L, 1)) - 1;
    if (const auto ref = petActionRef(gh, slot)) {
        // 0x005abbc0 -> 0x005d4210: the pet bar's slot, used.
        castPetSlot(L, gh, ref->packed, 2);
        return 0;
    }
    const auto& bar = gh->getActionBar();
    if (slot < 0 || slot >= static_cast<int>(bar.size()) || bar[slot].isEmpty()) return 0;
    const auto& action = bar[slot];
    // The end of the chain a click travels, and the last place it can stop
    // without saying so: an empty slot or a type nothing acts on look the
    // same from the button. At debug level, because pressing a
    // button on the action bar is the most ordinary thing a player does and
    // this was a warning on every one of them.
    LOG_DEBUG("UseAction: slot ", slot + 1, " type=", static_cast<int>(action.type), " id=", action.id);
    if (action.type == game::ActionBarSlot::SPELL) {
        // A unit that resolves wins; anything else falls back to the target,
        // which is what an ordinary click has always done. An unresolvable
        // unit is not treated as "cast on nobody" - a raid frame naming a
        // member who has gone out of range should still cast at the target
        // rather than silently at nothing.
        uint64_t target = gh->hasTarget() ? gh->getTargetGuid() : 0;
        if (const char* unit = lua_tostring(L, 2); unit && *unit) {
            std::string uid(unit);
            toLowerInPlace(uid);
            if (const uint64_t named = resolveUnitGuid(gh, uid)) target = named;
        }
        // A spell that belongs to an item - a Hearthstone is spell 8690 - has
        // to go as CMSG_USE_ITEM rather than a cast, or the server drops it.
        // This client's own action bar did that and was the only place that
        // did; the bar is FrameXML's now and this is the path its buttons take.
        //
        // Either way through 0x0080cce0, and its taint check (0x005abbc0 ->
        // 0x0053bc40, or UseItem -> 0x0080da40).
        if (!spellCastAllowed(L, gh, action.id)) {
            // Refused, and the interface told (ADDON_ACTION_FORBIDDEN).
        } else if (const uint32_t itemForSpell = gh->getItemIdForSpell(action.id)) {
            gh->useItemById(itemForSpell, target);
        } else {
            gh->castSpell(action.id, target);
        }
    } else if (action.type == game::ActionBarSlot::ITEM && action.id != 0) {
        if (itemUseAllowed(L, gh, action.id, false)) gh->useItemById(action.id);
    } else if (action.type == game::ActionBarSlot::EQUIPMENT_SET) {
        // WEAR_EQUIPMENT_SET with the set's name, for the interface to equip
        // it (0x005abbc0 -> 0x005ae660).
        if (const auto* set = equipmentSetById(gh, action.id))
            gh->fireAddonEvent("WEAR_EQUIPMENT_SET", {set->name});
    }
    // Macro execution requires GameScreen context; not available from pure Lua API
    return 0;
}

// --- Cursor / Drag-Drop System ---
// Tracks what the player is "holding" on the cursor (spell, item, action).


/// The equipment set of that id (0x005ae5c0), or none.
static const game::EquipmentSetInfo* equipmentSetById(game::GameHandler* gh, uint32_t setId) {
    if (!gh) return nullptr;
    for (const auto& set : gh->getEquipmentSets())
        if (set.setId == setId) return &set;
    return nullptr;
}

/// The set's icon (0x005ae6b0): under Interface\Icons, or nothing.
static std::string equipmentSetIcon(const game::EquipmentSetInfo* set) {
    return set && !set->iconName.empty() ? "Interface\\Icons\\" + set->iconName : std::string();
}

/// An equipment set onto the cursor (0x00520dc0): a set that is there, after
/// whatever was held is let go (0x00519280), with its icon on the pointer.
static void pickupEquipmentSet(lua_State* L, game::GameHandler* gh, uint32_t setId) {
    const auto* set = equipmentSetById(gh, setId);
    if (!set) return;
    const std::string icon = equipmentSetIcon(set);
    clearCursorItem(L);
    s_cursorId = setId;
    setCursorType(L, CursorType::EQUIPMENT_SET);
    wowee::ui::frameXmlSetCursorItem(icon);
}

// PickupEquipmentSet(setIndex) (0x005af2c0): the set at that place in the
// list (0x005ae580).
static int lua_PickupEquipmentSet(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: PickupEquipmentSet(setIndex)");
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const long index = static_cast<long>(lua_tonumber(L, 1)) - 1;
    const auto& sets = gh->getEquipmentSets();
    if (index >= 0 && static_cast<size_t>(index) < sets.size())
        pickupEquipmentSet(L, gh, sets[static_cast<size_t>(index)].setId);
    return 0;
}

// PickupEquipmentSetByName(name) (0x005af250): the set of that name
// (0x005ae600), off a set button's drag - after its taint check
// (0x005191c0(0xe), refused to insecure code in combat lockdown).
static int lua_PickupEquipmentSetByName(lua_State* L) {
    if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: PickupEquipmentSetByName(\"setName\")");
    if (!taint::allowed(L, 0xe)) return 0;
    auto* gh = getGameHandler(L);
    const char* name = lua_tostring(L, 1);
    if (!gh || !name || !*name) return 0;
    for (const auto& set : gh->getEquipmentSets()) {
        if (set.name == name) {
            pickupEquipmentSet(L, gh, set.setId);
            break;
        }
    }
    return 0;
}

static int lua_ClearCursor(lua_State* L) {
    // Everything a pickup set, including the icon drawn on the pointer.
    //
    // This cleared the type, the id, the slot and the bag and left the icon,
    // which is a separate piece of state that only clearCursorItem touches. So
    // cancelling a pickup - Escape, a right-click, a click on nothing, all of
    // which reach this - put the item back and left its picture stuck to the
    // cursor for the rest of the session.
    clearCursorItem(L);
    return 0;
}

static int lua_GetCursorInfo(lua_State* L) {
    switch (s_cursorType) {
        case CursorType::SPELL:
            lua_pushstring(L, "spell");
            lua_pushnumber(L, 0);          // bookSlotIndex
            lua_pushstring(L, "spell");    // bookType
            lua_pushnumber(L, s_cursorId); // spellId
            return 4;
        case CursorType::ITEM:
        // An item out of the guild bank is an item. Where it came from is this
        // file's business - PickupContainerItem reads the cursor type to know a
        // drop is a withdrawal - and saying so here would only give the
        // interface a kind it has no branch for.
        case CursorType::GUILDBANK:
        // And the ammunition off the ammo slot: "item" and its entry
        // (0x00515200 cases 7, 9 and 0xb).
        case CursorType::AMMO:
            lua_pushstring(L, "item");
            lua_pushnumber(L, s_cursorId);
            return 2;
        case CursorType::MACRO:
            lua_pushstring(L, "macro");
            lua_pushnumber(L, s_cursorId);
            return 2;
        case CursorType::MONEY:
            // containerframe.lua branches on this exact string to decide that
            // a click in a bag is money being put back.
            lua_pushstring(L, "money");
            lua_pushnumber(L, static_cast<lua_Number>(s_cursorMoney));
            return 2;
        case CursorType::MERCHANT:
            // containerframe.lua branches on this exact string to decide
            // whether a click in a bag is a purchase.
            lua_pushstring(L, "merchant");
            lua_pushnumber(L, s_cursorId);   // the vendor's list index
            return 2;
        case CursorType::EQUIPMENT_SET:
            // "equipmentset" and the set's name, while the set is there
            // (0x00515200 case 0xd -> 0x005ae5c0).
            if (const auto* set = equipmentSetById(getGameHandler(L), s_cursorId)) {
                lua_pushstring(L, "equipmentset");
                lua_pushstring(L, set->name.c_str());
                return 2;
            }
            return 0;
        default:
            return 0;
    }
}

static int lua_CursorHasItem(lua_State* L) {
    // A guild bank item counts. The bank frame's own OnClick tests this before
    // deciding a click is a drop, so answering no for one would have left the
    // item stuck on the cursor with nowhere that would take it.
    // The ammunition off the ammo slot is an item too (0x00515100 answers 1
    // for cursor type 9 as for type 1).
    lua_pushboolean(L, (s_cursorType == CursorType::ITEM ||
                        s_cursorType == CursorType::GUILDBANK ||
                        s_cursorType == CursorType::AMMO) ? 1 : 0);
    return 1;
}

static int lua_CursorHasSpell(lua_State* L) {
    lua_pushboolean(L, s_cursorType == CursorType::SPELL ? 1 : 0);
    return 1;
}

// CursorHasMoney() (0x005151c0) / CursorHasMacro() (0x00515180): the
// cursor's type is 2 / 8. 1 or nil.
static int lua_CursorHasMoney(lua_State* L) {
    if (s_cursorType == CursorType::MONEY) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 1;
}
static int lua_CursorHasMacro(lua_State* L) {
    if (s_cursorType == CursorType::MACRO) lua_pushnumber(L, 1); else lua_pushnil(L);
    return 1;
}

// PickupAction(slot) - picks up an action from the action bar
/// PickupAction(slot) - take what is in a slot onto the cursor, and put down
/// what the cursor was already holding.
///
/// A swap, which is what the real client does and what the interface expects:
/// ActionButton_OnClick calls this for a click on a button whether or not
/// anything is held.
///
/// The slot used to be left exactly as it was. Picking an action up put a copy
/// on the cursor and the button went on showing it, so dragging something off
/// the bar looked like it had done nothing at all - and because the slot never
/// changed, no ACTIONBAR_SLOT_CHANGED was fired and nothing redrew. The empty
/// half only understood a spell, so an item or a macro dropped on a free slot
/// vanished.
static int lua_PickupAction(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int slot = static_cast<int>(luaL_checknumber(L, 1));
    // CGActionBar::PickupAction asks the taint check (0x005abe70, kind 0xe):
    // the bar is not rearranged by insecure code in combat lockdown.
    if (!taint::allowed(L, 0xe)) return 0;
    const auto& bar = gh->getActionBar();
    if (slot < 1 || slot > static_cast<int>(bar.size())) return 0;

    // Read before anything is written: setActionBarSlot rewrites the entry
    // this would otherwise still be pointing at.
    const auto existing = bar[slot - 1];
    const bool hadAction = !existing.isEmpty();
    const CursorType held = s_cursorType;
    const uint32_t heldId = s_cursorId;

    const auto typeForCursor = [](CursorType c) {
        return c == CursorType::SPELL ? game::ActionBarSlot::SPELL
             : c == CursorType::ITEM  ? game::ActionBarSlot::ITEM
             : c == CursorType::EQUIPMENT_SET ? game::ActionBarSlot::EQUIPMENT_SET
                                      : game::ActionBarSlot::MACRO;
    };
    const bool holding = (heldId != 0 &&
                          (held == CursorType::SPELL || held == CursorType::ITEM ||
                           held == CursorType::MACRO)) ||
                         held == CursorType::EQUIPMENT_SET;

    // The set already in the slot: the cursor lets it go, the slot keeps it
    // (0x005ab120).
    if (held == CursorType::EQUIPMENT_SET && existing.type == game::ActionBarSlot::EQUIPMENT_SET &&
        existing.id == heldId) {
        clearCursorItem(L);
        return 0;
    }
    if (holding) {
        gh->setActionBarSlot(slot - 1, typeForCursor(held), heldId);
    } else if (hadAction) {
        // Nothing held, so the slot empties. This is the fire that redraws the
        // button: setActionBarSlot raises ACTIONBAR_SLOT_CHANGED for it.
        gh->setActionBarSlot(slot - 1, game::ActionBarSlot::EMPTY, 0);
    }

    if (hadAction && existing.type == game::ActionBarSlot::EQUIPMENT_SET) {
        // A set goes to the cursor as a set, if it is still there; one
        // deleted since lets the cursor go (0x005ab120 -> 0x005ae4f0,
        // 0x00520dc0).
        if (equipmentSetById(gh, existing.id)) pickupEquipmentSet(L, gh, existing.id);
        else clearCursorItem(L);
    } else if (hadAction) {
        // What was there goes to the cursor, whether this was a swap or a
        // plain pick-up. existing.type is never EMPTY here (hadAction), so
        // this covers every case - CursorType::ACTION was a dead end lua_
        // PlaceAction had no branch for, and picking a macro up off the bar
        // fell to it by default: the macro left its slot, rode the cursor as
        // an unplaceable "action", and vanished on drop instead of landing
        // anywhere.
        setCursorType(L, (existing.type == game::ActionBarSlot::SPELL) ? CursorType::SPELL :
                       (existing.type == game::ActionBarSlot::ITEM)  ? CursorType::ITEM :
                       CursorType::MACRO);
        s_cursorId = existing.id;
        s_cursorSlot = slot;
        // Which button, not which slot of which bag - a fourth numbering, and
        // the reason the source has to say so. An item action carries a real
        // itemId, so without this the cursor was indistinguishable from one
        // lifted off the paperdoll and the button number was spent as an
        // equipment slot. See kCursorNoSource.
        s_cursorBag = game::slots::kCursorNoSource;
        // The shared cursor as well, which is a separate record of the same
        // thing and would otherwise still be naming wherever the *previous*
        // pickup came from: clicking a button while carrying a bag item swaps
        // the two, and the bag slot it left behind stayed on this cursor as
        // the source PutItemInBag would have moved from.
        cursorItemSlot() = {};
        // And on the pointer, or the drag reads as not having started.
        if (existing.type == game::ActionBarSlot::SPELL) {
            wowee::ui::frameXmlSetCursorItem(gh->getSpellIconPath(existing.id));
        } else if (existing.type == game::ActionBarSlot::ITEM) {
            const auto* info = gh->getItemInfo(existing.id);
            wowee::ui::frameXmlSetCursorItem(
                info ? gh->getItemIconPath(info->displayInfoId) : std::string());
        } else {
            std::string icon = gh->getMacroIcon(existing.id);
            if (icon.empty()) icon = "Interface\\Icons\\INV_Misc_QuestionMark";
            wowee::ui::frameXmlSetCursorItem(icon);
        }
    } else {
        clearCursorItem(L);
    }
    return 0;
}

// PlaceAction(slot) - places cursor content into an action bar slot
//
// A swap, as the real one is: whatever the slot held goes onto the cursor, so
// a bar can be rearranged by dropping one action on another and then placing
// the one that came off. This wrote the new action over the old and cleared
// the cursor, so the action underneath was simply gone - and it is PlaceAction
// an action button's OnReceiveDrag calls, so every drop onto an occupied
// button lost what was there.
//
// PickupAction while holding something is exactly that swap, so this is it.
// With nothing placeable held it does nothing, and leaves the cursor alone.
static int lua_PlaceAction(lua_State* L) {
    const bool holding = (s_cursorId != 0 &&
                          (s_cursorType == CursorType::SPELL ||
                           s_cursorType == CursorType::ITEM ||
                           s_cursorType == CursorType::MACRO)) ||
                         s_cursorType == CursorType::EQUIPMENT_SET;
    if (!holding) return 0;
    return lua_PickupAction(L);
}

// PickupSpell(bookSlot, bookType) - picks up a spell from the spellbook
static int lua_PickupSpell(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    if (!taint::allowed(L, 0xe)) return 0;   // 0x0053baf0
    // Through the shared resolver, so the book is read as well as the slot:
    // dragging a pet spell out of the book picked up whichever of the player's
    // spells sat at that index.
    const uint32_t picked = spellIdForCall(L, gh);
    if (picked == 0) return 0;
    setCursorType(L, CursorType::SPELL);
    s_cursorId = picked;
    // On the pointer as well as in the state, the way every other pickup here
    // does it. Dragging a spell out of the book carried nothing visible, so
    // the drag read as not having started.
    wowee::ui::frameXmlSetCursorItem(gh->getSpellIconPath(s_cursorId));
    return 0;
}

// PlaceGlyphInSocket(socket) (0x005b75a0) - the glyph the pending spell
// applies put in that socket (0x0080c3e0), and the glyph item used with it:
// 1 when it went, nil when it did not; nothing for a socket outside the six.
// A glyph is not dragged in - using it from a bag holds its spell until a
// socket is clicked.
static int lua_PlaceGlyphInSocket(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: PlaceGlyphInSocket(index)");
    auto* gh = getGameHandler(L);
    const int socket = static_cast<int>(std::lround(lua_tonumber(L, 1)));
    if (!gh || socket < 1 || socket > game::GameHandler::MAX_GLYPH_SLOTS) return 0;
    if (gh->placePendingGlyph(static_cast<uint32_t>(socket - 1))) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

// PickupGuildBankItem(tab, slot) - both halves of a guild bank drag.
//
// The frame calls this from OnDragStart and from OnReceiveDrag, so one function
// picks up and puts down, the way PickupContainerItem does for bags. It was a
// no-op, so nothing could be moved into or out of the guild bank at all -
// while the packet builder for the move sat verified against the server, and
// AutoStoreGuildBankItem beside it has been using half of it for right-clicks.
//
// The tab and slot are FrameXML's, counted from one; the wire counts from zero,
// which is the shift AutoStoreGuildBankItem already applies.
static int lua_PickupGuildBankItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int tab = static_cast<int>(luaL_optnumber(L, 1, 0));
    const int slot = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (!gh || tab < 1 || slot < 1) return 0;

    if (droppedItemFromNowhere(L)) return 0;

    // Carrying a bag item, so this is the deposit.
    uint8_t srcBag = 0, srcSlot = 0;
    if (cursorWireSlot(srcBag, srcSlot)) {
        gh->guildBankDepositItem(static_cast<uint8_t>(tab - 1),
                                 static_cast<uint8_t>(slot - 1), srcBag, srcSlot);
        setCursorType(L, CursorType::NONE);
        return 0;
    }
    // Carrying a bank item already: that is a move within the bank, and the
    // request builder says plainly that it does not write that path. Dropping
    // the cursor is better than sending something the server will read as a
    // withdrawal to nowhere.
    if (s_cursorType == CursorType::GUILDBANK) {
        setCursorType(L, CursorType::NONE);
        return 0;
    }

    // Otherwise pick the slot up, if there is anything in it.
    const auto& bank = gh->getGuildBankData();
    for (const auto& it : bank.tabItems) {
        if (it.slotId != slot - 1 || it.itemEntry == 0) continue;
        setCursorType(L, CursorType::GUILDBANK);
        s_cursorId = it.itemEntry;
        s_cursorBag = tab;
        s_cursorSlot = slot;
        s_cursorSplit = 0;   // the whole stack, unless SplitGuildBankItem says otherwise
        const auto* info = gh->getItemInfo(it.itemEntry);
        wowee::ui::frameXmlSetCursorItem(
            info ? gh->getItemIconPath(info->displayInfoId) : std::string());
        return 0;
    }
    return 0;
}

// SplitGuildBankItem(tab, slot, amount) - take part of a stack.
//
// The stack split dialog calls this instead of PickupGuildBankItem when the
// player asks for fewer than all of them. It is the same pickup with a number
// attached: the wire carries the split on the *withdrawal* rather than on the
// pickup, so the amount rides on the cursor until the drop says where it goes.
static int lua_SplitGuildBankItem(lua_State* L) {
    const int amount = static_cast<int>(luaL_optnumber(L, 3, 0));
    lua_settop(L, 2);
    lua_PickupGuildBankItem(L);
    if (s_cursorType == CursorType::GUILDBANK && amount > 0)
        s_cursorSplit = static_cast<uint32_t>(amount);
    return 0;
}

// PickupCompanion(mode, index) - drag a mount or a critter to the bar.
//
// Registered here rather than beside the other companion calls because the
// cursor lives in this file, and because that is what this is: a pickup.
//
// A companion is summoned by a spell, and this client's action bar holds
// spells, so the cursor carries the summon rather than a companion of its own
// kind. What lands on the bar is the same icon casting the same thing; the
// difference is invisible to everyone but the tooltip, which names the spell.
// Modelling a fourth action type to say "companion" would change nothing a
// player can see.
static int lua_PickupCompanion(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    if (!taint::allowed(L, 0xe)) return 0;   // 0x0053bbc0
    const char* mode = luaL_optstring(L, 1, "");
    const int index = static_cast<int>(luaL_optnumber(L, 2, 0));
    if (index < 1) return 0;
    const auto& list = gh->getCompanions(std::string(mode) == "MOUNT");
    if (index > static_cast<int>(list.size())) return 0;
    const uint32_t spellId = list[static_cast<size_t>(index) - 1].spellId;
    if (spellId == 0) return 0;
    setCursorType(L, CursorType::SPELL);
    s_cursorId = spellId;
    wowee::ui::frameXmlSetCursorItem(gh->getSpellIconPath(spellId));
    return 0;
}

// PickupSpellBookItem(bookSlot, bookType) - alias for PickupSpell
static int lua_PickupSpellBookItem(lua_State* L) {
    return lua_PickupSpell(L);
}

// PickupContainerItem(bag, slot) - picks up an item from a bag

/// Where the item on the cursor lives, in the numbering the server uses.
///
/// The server addresses one flat space: equipment is slots 0 to 22, the
/// backpack follows at 23, and a bag's contents are addressed by that bag's own
/// equipment slot together with an index inside it. FrameXML counts bags 0 to 4
/// with 1-based slots instead, so the two have to be translated - the same
/// translation this client's own bag window does before sending a swap.
static bool cursorWireSlot(uint8_t& bag, uint8_t& slot) {
    if (s_cursorType != CursorType::ITEM) return false;
    // Nowhere to move it from. This used to fall into the equipped branch
    // below, because everything negative was the paperdoll - see
    // kCursorNoSource for what that cost.
    if (!game::slots::cursorSourceIsInventory(s_cursorBag)) return false;

    // Minus one is two different places. The paperdoll picks an item up with
    // it, and the interface numbers the bank's own slots BANK_CONTAINER, which
    // is also minus one - so an item lifted out of the bank read as a worn one,
    // and the move that followed named an equipment slot: bank slot five went
    // out as equipment slot four. The flag the cursor already carries is the
    // only thing that tells the two apart, and it was not being read.
    if (cursorItemSlot().equipped) {
        bag = 0xFF;
        slot = static_cast<uint8_t>(s_cursorSlot - 1);
        return true;
    }
    // Everything else through the shared pair, which knows the backpack, the
    // keyring, the bank, its bags and the worn bags. Written out here it knew
    // three of them.
    bag = containerWireBag(s_cursorBag);
    slot = containerWireSlot(s_cursorBag, s_cursorSlot);
    return true;
}

/// Put the cursor down.
static void clearCursorItem(lua_State* L) {
    s_cursorMoney = 0;
    s_cursorSplit = 0;
    setCursorType(L, CursorType::NONE);
    s_cursorId = 0;
    s_cursorSlot = 0;
    s_cursorBag = -1;
    wowee::ui::frameXmlSetCursorItem(std::string());
    cursorItemSlot() = {};
}

static bool droppedItemFromNowhere(lua_State* L) {
    if (s_cursorType != CursorType::ITEM) return false;
    if (game::slots::cursorSourceIsInventory(s_cursorBag)) return false;
    clearCursorItem(L);
    return true;
}

void pickupMerchantItem(lua_State* L, int index) {
    // Index zero is not a pickup at all: it means "put what the cursor is
    // holding into the merchant's sell slot", which is how 3.3.5 sells from a
    // bag. ContainerFrameItemButton_OnClick picks the item up and calls this
    // with no argument, and luaL_optnumber turns that into the zero below.
    //
    // This returned here, so a right-click at a vendor lifted the item onto
    // the cursor, asked to sell it, was answered with nothing, and left it
    // stuck to the pointer. ShowContainerSellCursor beside this is the other
    // half of the same path and is a no-op for a reason - it only changes the
    // cursor art - which is what made this one look deliberate too.
    if (index < 1) {
        soldHeldItem(L);
        return;
    }
    setCursorType(L, CursorType::MERCHANT);
    s_cursorId = static_cast<uint32_t>(index);
    s_cursorSlot = 0;
    s_cursorBag = -1;
    // Show it on the pointer, as a bag pickup does. Without this the cursor
    // holds something invisible and the click reads as having done nothing.
    if (auto* gh = getGameHandler(L)) {
        const auto& items = gh->getVendorItems().items;
        if (index <= static_cast<int>(items.size())) {
            ui::frameXmlSetCursorItem(
                gh->getItemIconPath(items[index - 1].displayInfoId));
        }
    }
}

/// Sell whatever the cursor is holding, and put it down either way.
///
/// Bags and the backpack only. An item on the paperdoll can be dragged to a
/// merchant in the real client, but the two sell verbs here are the ones that
/// keep the buyback list, and neither knows an equipment slot - inventing a
/// third path for a case a right-click in a bag cannot reach would be guessing
/// at the bookkeeping rather than restoring behaviour.
static void soldHeldItem(lua_State* L) {
    // A merchant item on the cursor was never taken out of anything, so there
    // is nothing to sell and nothing to put back.
    if (s_cursorType == CursorType::MERCHANT) { clearCursorItem(L); return; }
    if (s_cursorType != CursorType::ITEM) return;
    auto* gh = getGameHandler(L);
    if (!gh) return;
    if (!game::slots::cursorSourceIsInventory(s_cursorBag)) return;

    if (s_cursorBag == 0) {
        gh->sellItemBySlot(s_cursorSlot - 1);
    } else if (s_cursorBag > 0) {
        gh->sellItemInBag(s_cursorBag - 1, s_cursorSlot - 1);
    } else {
        return;   // worn, and held rather than dropped on the floor
    }
    clearCursorItem(L);
}

bool boughtHeldMerchantItem(lua_State* L) {
    if (s_cursorType != CursorType::MERCHANT) return false;
    auto* gh = getGameHandler(L);
    const int index = static_cast<int>(s_cursorId);
    if (gh && index >= 1) {
        const auto& items = gh->getVendorItems().items;
        if (index <= static_cast<int>(items.size())) {
            const auto& vi = items[index - 1];
            gh->buyItem(gh->getVendorGuid(), vi.itemId, vi.slot, 1);
        }
    }
    clearCursorItem(L);
    return true;
}


/// Put the item in a bag slot on the cursor, with everything that goes with it:
/// the icon, the greyed slot, and the record of where it came from.
///
/// Split out of lua_PickupContainerItem so PickupItem can reach it. Writing the
/// cursor twice is how one of them would come to set the icon and the other not.
static void pickupFromContainerSlot(lua_State* L, game::GameHandler* gh,
                                    int bag, int slot) {
    const auto& inv = gh->getInventory();
    // Every container this numbering reaches, not the backpack and the four
    // worn bags alone: the bank's own slots are container -1 and its bags are
    // 5 through 11, and bankframe.lua picks an item up with
    // PickupContainerItem(BANK_CONTAINER, self:GetID()). Missing them, the bank
    // answered "nothing there to pick up" for every square that had something
    // in it.
    const game::ItemSlot* itemSlot = containerItemSlot(inv, bag, slot);
    if (itemSlot && !itemSlot->empty()) {
        setCursorType(L, CursorType::ITEM);
        s_cursorId = itemSlot->item.itemId;
        s_cursorBag = bag;
        s_cursorSlot = slot;
        uint32_t displayId = itemSlot->item.displayInfoId;
        if (displayId == 0) {
            if (const auto* info = gh->getItemInfo(itemSlot->item.itemId)) {
                displayId = info->displayInfoId;
            }
        }
        wowee::ui::frameXmlSetCursorItem(
            displayId ? gh->getItemIconPath(displayId) : std::string());
        cursorItemSlot() = {bag, slot, false};
        // The slot draws greyed while its item is on the cursor, which is how a
        // bag shows that something has been picked up out of it.
        gh->fireAddonEvent("ITEM_LOCK_CHANGED",
                           {std::to_string(bag), std::to_string(slot)});
        LOG_WARNING("FrameXML pickup: bag ", bag, " slot ", slot,
                    " item ", itemSlot->item.itemId);
    } else {
        LOG_WARNING("FrameXML pickup: bag ", bag, " slot ", slot,
                    " - nothing there to pick up");
    }
}

void pickupSplitFromContainer(lua_State* L, game::GameHandler* gh,
                              int bag, int slot, int count) {
    pickupFromContainerSlot(L, gh, bag, slot);
    // Only if the pickup took: an empty slot leaves the cursor alone, and an
    // amount left on it would ride along with whatever is picked up next.
    if (s_cursorType == CursorType::ITEM && count > 0) {
        s_cursorSplit = static_cast<uint32_t>(count);
    }
}


/// PickupItem(id | "name" | link) - put an item on the cursor by naming it
/// rather than by pointing at a slot.
///
/// A macro's own way to pick something up, and the 'item' branch of
/// securehandlers.lua's cursor dispatcher - every other branch there was bound
/// and this one raised.
///
/// The first matching slot wins. WoW picks the first too, and a stack split
/// across two bags is one item as far as this is concerned.
static int lua_ReturnNothing2(lua_State* L) { (void)L; return 0; }

static int lua_PickupItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;

    uint32_t wantId = 0;
    std::string wantName;
    if (lua_isnumber(L, 1)) {
        wantId = static_cast<uint32_t>(lua_tonumber(L, 1));
    } else if (const char* text = lua_tostring(L, 1)) {
        // A link carries the id in it - "|Hitem:6948:0:..." - and is what a
        // shift-click puts in a macro, so it is the common case rather than
        // the exotic one.
        const std::string str(text);
        const size_t at = str.find("Hitem:");
        if (at != std::string::npos) {
            wantId = static_cast<uint32_t>(std::strtoul(str.c_str() + at + 6, nullptr, 10));
        } else {
            wantName = str;
        }
    }
    if (wantId == 0 && wantName.empty()) return 0;

    auto matches = [&](const game::ItemSlot& s) {
        if (s.empty()) return false;
        if (wantId != 0) return s.item.itemId == wantId;
        return s.item.name == wantName;
    };

    const auto& inv = gh->getInventory();
    for (int i = 0; i < inv.getBackpackSize(); ++i) {
        if (matches(inv.getBackpackSlot(i))) {
            pickupFromContainerSlot(L, gh, 0, i + 1);
            return 0;
        }
    }
    for (int b = 0; b < game::Inventory::NUM_BAG_SLOTS; ++b) {
        for (int sl = 0; sl < inv.getBagSize(b); ++sl) {
            if (matches(inv.getBagSlot(b, sl))) {
                pickupFromContainerSlot(L, gh, b + 1, sl + 1);
                return 0;
            }
        }
    }
    return 0;
}

static int lua_PickupContainerItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int bag = static_cast<int>(luaL_checknumber(L, 1));
    int slot = static_cast<int>(luaL_checknumber(L, 2));

    // Same as the paperdoll: a click while something is waiting for an item to
    // be applied to is that choice, not a pickup.
    if (completedItemTarget(L, containerSlotGuid(gh, bag, slot))) return 0;

    // With the repair cursor up, a click on an item is the repair. The real
    // client consumes the click in C for the same reason, which is why
    // containerframe.lua has no branch of its own for it.
    if (repairedHeldItem(gh, containerSlotGuid(gh, bag, slot))) return 0;

    // A bag click while the cursor holds a vendor's item is the purchase.
    // containerframe.lua routes it here after checking GetCursorInfo.
    if (boughtHeldMerchantItem(L)) return 0;

    // Carrying something out of the guild bank, so this is the withdrawal.
    // The destination is this bag slot rather than the server's choice, which
    // is what dropping on a particular square means.
    if (s_cursorType == CursorType::GUILDBANK) {
        const uint8_t dstBag = containerWireBag(bag);
        const uint8_t dstSlot = containerWireSlot(bag, slot);
        gh->guildBankWithdrawItem(static_cast<uint8_t>(s_cursorBag - 1),
                                  static_cast<uint8_t>(s_cursorSlot - 1),
                                  dstBag, dstSlot, s_cursorSplit);
        setCursorType(L, CursorType::NONE);
        return 0;
    }

    // Carrying something the bags cannot take: the drag ends here and nothing
    // moves. Before cursorWireSlot, which would otherwise answer no and let
    // this fall through to the pickup below - putting the bag's own item on a
    // cursor that is already holding one.
    if (droppedItemFromNowhere(L)) return 0;

    // Already carrying something, so this is the drop rather than the pickup.
    // One function does both halves of a drag in WoW, and without this half a
    // dragged item was picked up and never put down anywhere.
    uint8_t srcBag = 0, srcSlot = 0;
    if (cursorWireSlot(srcBag, srcSlot)) {
        // Through the shared pair rather than written out here. This knew the
        // backpack and treated everything else as a worn bag, so a drop into
        // the bank - container -1 - asked wornBagContainer for container 17 and
        // sent the move to a container that is not one. Nothing in the bank
        // window could be dragged anywhere, including a bag sitting in it.
        const uint8_t dstBag = containerWireBag(bag);
        const uint8_t dstSlot = containerWireSlot(bag, slot);
        // At warning level because it is the outcome of a drag and happens
        // once per drop, and because the log carries nothing below warning -
        // which is why "did the move go out" could not be answered at all.
        LOG_WARNING("FrameXML drop: bag ", s_cursorBag, " slot ", s_cursorSlot,
                    " (wire ", (int)srcBag, "/", (int)srcSlot, ") -> bag ", bag,
                    " slot ", slot, " (wire ", (int)dstBag, "/", (int)dstSlot, ")");
        // Back where it came from: put it down rather than asking the server to
        // swap a slot with itself.
        //
        // The equipped flag counts here too, for the same reason it counts
        // above: a worn item on the cursor carries bag minus one, and so does
        // the bank, so dropping the helm on bank slot one read as putting it
        // back where it came from and quietly did nothing.
        if (s_cursorBag == bag && s_cursorSlot == slot && !cursorItemSlot().equipped) {
            const int sameBag = bag, sameSlot = slot;
            clearCursorItem(L);
            gh->fireAddonEvent("ITEM_LOCK_CHANGED",
                               {std::to_string(sameBag), std::to_string(sameSlot)});
            return 0;
        }

        // CGPlayer_C::SwapItems asks the taint check (0x006df890, kind 0x16):
        // insecure code moves nothing in combat lockdown.
        if (!taint::allowed(L, 0x16)) return 0;
        const int wasBag = s_cursorBag, wasSlot = s_cursorSlot;
        // Part of a stack moves as a split rather than a swap: a swap would
        // send the whole thing, which is not what was picked up.
        if (s_cursorSplit > 0) {
            gh->splitItemTo(srcBag, srcSlot, dstBag, dstSlot,
                            static_cast<uint8_t>(s_cursorSplit));
        } else {
            gh->swapContainerItems(srcBag, srcSlot, dstBag, dstSlot);
        }
        clearCursorItem(L);
        gh->fireAddonEvent("ITEM_LOCK_CHANGED",
                           {std::to_string(wasBag), std::to_string(wasSlot)});
        gh->fireAddonEvent("ITEM_LOCK_CHANGED",
                           {std::to_string(bag), std::to_string(slot)});
        return 0;
    }

    pickupFromContainerSlot(L, gh, bag, slot);
    return 0;
}

// PickupInventoryItem(slot) - picks up an equipped item
// ClickSendMailItemButton(index, isRightClick) - attach what is held to the
// letter, or take an attachment back.
//
// Lives here rather than with the other mail functions because the cursor
// does, and this is entirely about the cursor: carrying an item means attach
// it, carrying nothing means the slot clicked gives its item back to the bags.
// The attachment index is optional - the drop handler calls this with no
// arguments at all, meaning "attach to wherever there is room", which is what
// attachItemFrom* already does.
static int lua_ClickSendMailItemButton(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;

    if (s_cursorType == CursorType::ITEM) {
        // The cursor counts the backpack as bag zero and the worn bags from
        // one, with slots from one; the attach functions count both from zero.
        const bool attached = (s_cursorBag == 0)
            ? gh->attachItemFromBackpack(s_cursorSlot - 1)
            : (s_cursorBag > 0
                   ? gh->attachItemFromBag(s_cursorBag - 1, s_cursorSlot - 1)
                   : false);
        if (attached) clearCursorItem(L);
        return 0;
    }

    // Nothing held: the click takes an attachment off the letter. Without an
    // index there is nothing to take off, since the caller meant a drop.
    const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (index >= 1) gh->detachMailAttachment(index - 1);
    return 0;
}

// --- The petition vendor's turn-in slot (PetitionVendor.cpp) ---
//
// The client keeps one charter in a slot at the petition vendor (0x00c0f7d8):
// ClickPetitionButton drops the cursor's charter into it, or takes the one
// there back onto the cursor, and TurnInPetition hands it in. The slot belongs
// to the vendor it was filled at - the vendor's showlist empties it
// (0x005a22a0) - so it is kept beside that vendor's guid.
namespace {
struct PetitionSlot {
    int bag = -1;
    int slot = 0;
    uint64_t guid = 0;
    uint64_t vendor = 0;
};
PetitionSlot& petitionSlot() {
    static PetitionSlot s;
    return s;
}
}  // namespace

// ClickPetitionButton() (0x005a2200 -> 0x005a2140): with a petition vendor
// open, the cursor's item goes into the slot if it is a charter (item flag
// 0x2000) and an empty cursor empties the slot; either way PETITION_VENDOR_
// UPDATE, the cursor is cleared, and whatever the slot held before comes back
// onto the cursor. Anything else held is not a charter and is left alone.
static int lua_ClickPetitionButton(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const uint64_t vendor = gh->getPetitionNpcGuid();
    if (vendor == 0) {   // 0x005a2140: no vendor open, nothing done
        LOG_WARNING("ClickPetitionButton: no petition vendor is open");
        return 0;
    }
    auto& slot = petitionSlot();
    if (slot.vendor != vendor) slot = {};
    const PetitionSlot old = slot;

    if (s_cursorType == CursorType::ITEM) {
        if (s_cursorBag < 0) {
            LOG_WARNING("ClickPetitionButton: the held item is not in a bag");
            return 0;
        }
        const uint64_t guid = containerSlotGuid(gh, s_cursorBag, s_cursorSlot);
        const auto* info = gh->getItemInfo(cursorItemId());
        if (guid == 0 || !info || (info->itemFlags & 0x2000u) == 0) {
            LOG_WARNING("ClickPetitionButton: the held item is not a charter (item flag 0x2000)");
            return 0;
        }
        slot = {s_cursorBag, s_cursorSlot, guid, vendor};
        clearCursorItem(L);
    } else if (s_cursorType == CursorType::NONE) {
        slot = {};
    } else {
        LOG_WARNING("ClickPetitionButton: the cursor holds something that is not an item");
        return 0;
    }
    gh->fireAddonEvent("PETITION_VENDOR_UPDATE", {});
    // The charter that was in the slot, picked up again - still where it was.
    if (old.guid != 0 && !(old.bag == slot.bag && old.slot == slot.slot) &&
        containerSlotGuid(gh, old.bag, old.slot) == old.guid) {
        pickupFromContainerSlot(L, gh, old.bag, old.slot);
    }
    return 0;
}

// TurnInPetition() (0x005a2600): with the vendor open and a charter in its
// slot, CMSG_TURN_IN_PETITION for it, the five tabard words left at -1 - it is
// TurnInArenaPetition that designs a banner.
static int lua_TurnInPetition(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const auto& slot = petitionSlot();
    const uint64_t vendor = gh->getPetitionNpcGuid();
    if (vendor == 0 || slot.guid == 0 || slot.vendor != vendor) return 0;
    gh->turnInPetition(slot.guid, /*blankDesign=*/true);
    return 0;
}

// PickupBagFromSlot(inventorySlot) - pick up or put down a bank bag.
//
// The bank's bag buttons hand over an inventory slot rather than an index:
// BankButtonIDToInvSlotID puts the seven bank bags at sixty-eight upward, and
// the wire counts from zero, so the slot sent is one less. That is the same
// arithmetic PickupInventoryItem does for worn equipment, and the cursor
// carries it the same way - a negative bag means "not in a container", which
// cursorWireSlot turns back into container 0xFF and the slot less one.
//
// Deferred for a long time on the belief that this client had no cursor to
// drop things from. It has had one all along, with both halves of a drag in
// one function, and the wire numbers here are the ones its own bank window
// already sends.
/// The nth worn bag, as Lua numbers inventory slots - the four beside the
/// backpack. BagSlotButton_OnDrag passes exactly these.
static constexpr int kFirstWornBagInventorySlot =
    game::slots::toInventorySlot(game::slots::wornBagContainer(0));

static int lua_PickupBagFromSlot(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const int slot = static_cast<int>(luaL_optnumber(L, 1, 0));

    const int bankIndex = slot - game::slots::firstBankBagInventorySlot();
    const int wornIndex = slot - kFirstWornBagInventorySlot;
    const bool isBank = bankIndex >= 0 && bankIndex < game::slots::bankBagCount();
    const bool isWorn = wornIndex >= 0 && wornIndex < game::Inventory::NUM_BAG_SLOTS;
    // Only the bank half was handled, so dragging one of the four worn bags did
    // nothing at all - it returned here before doing anything. Latent until the
    // bag bar was handed over, because this client's own bar never called it.
    if (!isBank && !isWorn) return 0;

    if (droppedItemFromNowhere(L)) return 0;

    // Carrying something: this is the drop, into the bag slot.
    uint8_t srcBag = 0, srcSlot = 0;
    if (cursorWireSlot(srcBag, srcSlot)) {
        const int srcWorn = s_cursorSlot - kFirstWornBagInventorySlot;
        const bool fromWorn = s_cursorBag == -1 &&
                              srcWorn >= 0 && srcWorn < game::Inventory::NUM_BAG_SLOTS;
        if (fromWorn && isWorn) {
            // Both are worn bags, which has its own verb: it swaps the bags'
            // contents locally as well as sending the move, so the bags on
            // screen do not have to wait for the server to agree.
            gh->swapBagSlots(srcWorn, wornIndex);
        } else {
            gh->swapContainerItems(srcBag, srcSlot, 0xFF,
                                   static_cast<uint8_t>(game::slots::toWireSlot(slot)));
        }
        clearCursorItem(L);
        return 0;
    }

    const auto& held = isBank
        ? gh->getInventory().getBankBagItem(bankIndex)
        : gh->getInventory().getEquipSlot(static_cast<game::EquipSlot>(
              static_cast<int>(game::EquipSlot::BAG1) + wornIndex));
    if (held.empty()) {
        // Dragging a bag that is plainly in the slot has been reported doing
        // nothing, and every step of the arithmetic checks out on paper: the
        // button's id, the worn index, the equipment slot. So say what the
        // slot actually held when the drag arrived, rather than returning in
        // silence as this has always done for an empty slot.
        LOG_WARNING("PickupBagFromSlot: slot ", slot, " (worn index ", wornIndex,
                    ", bank index ", bankIndex, ") reads as empty - nothing to pick up");
        return 0;
    }
    setCursorType(L, CursorType::ITEM);
    s_cursorId = held.item.itemId;
    s_cursorSlot = slot;
    s_cursorBag = -1;
    // The shared cursor as well as this file's own. Everything that receives a
    // drop reads cursorItemSlot(), and a worn bag is an equipped item like any
    // other - PickupInventoryItem below sets exactly this. Without it the bag
    // lifted onto the pointer and then every drop target reported nothing
    // held: PutItemInBag returned false, so BagSlotButton_OnClick fell through
    // to ToggleBag and the bag opened instead of moving.
    cursorItemSlot() = {-1, slot, true};
    // Same omission as the spell above: a bag dragged off its slot was held
    // invisibly. The display id falls back to the item's own, which is where
    // the equipped-item pickup below reads it from when the slot has none.
    uint32_t displayId = held.item.displayInfoId;
    if (displayId == 0) {
        if (const auto* info = gh->getItemInfo(held.item.itemId)) {
            displayId = info->displayInfoId;
        }
    }
    wowee::ui::frameXmlSetCursorItem(
        displayId ? gh->getItemIconPath(displayId) : std::string());
    // And say the slot is locked, as the equipped pickup below does.
    // IsInventoryItemLocked already answers true for it - cursorEquipSlot
    // reads the same s_cursorSlot this just set - but nothing asks again
    // without this, so the square greyed only on the next unrelated redraw.
    // One argument, because the paperdoll's handler tests `not arg2`.
    gh->fireAddonEvent("ITEM_LOCK_CHANGED", {std::to_string(slot)});
    return 0;
}

static int lua_PickupInventoryItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    int slot = static_cast<int>(luaL_checknumber(L, 1));

    // The ammo slot, Lua slot 0 (0x005e85d0 with slot -1). The ammunition is a
    // reference to arrows or bullets in the bags, not an item that moves:
    // - the ammunition lifted off it goes back on (CMSG_SET_AMMO, 0x268);
    // - a held item is auto-equipped (0x006dfc40): a projectile becomes the
    //   ammunition, anything else goes to its own slot;
    // - with nothing held, the ammunition comes off onto the cursor - an item
    //   to the interface, as cursor type 9 - and CMSG_SET_AMMO 0 takes it off,
    //   so long as the bags still carry some of it.
    if (slot == 0) {
        if (s_cursorType == CursorType::AMMO) {
            gh->setAmmo(s_cursorId);
            clearCursorItem(L);
            return 0;
        }
        if (s_cursorType == CursorType::ITEM) {
            if (s_cursorBag == 0) {
                gh->autoEquipItemBySlot(s_cursorSlot - 1);
            } else if (s_cursorBag >= 1 && s_cursorBag <= game::Inventory::NUM_BAG_SLOTS) {
                gh->autoEquipItemInBag(s_cursorBag - 1, s_cursorSlot - 1);
            } else {
                LOG_WARNING("PickupInventoryItem(0): the held item is not in a carried bag");
            }
            clearCursorItem(L);
            return 0;
        }
        if (s_cursorType != CursorType::NONE) {
            LOG_WARNING("PickupInventoryItem(0): the cursor holds something that is not an item");
            return 0;
        }
        const uint32_t ammo = gh->getAmmoId();
        if (ammo == 0 || gh->countItemInBags(ammo) == 0) {
            LOG_WARNING("PickupInventoryItem(0): no ammunition is put in, or none is carried");
            return 0;
        }
        setCursorType(L, CursorType::AMMO);
        s_cursorId = ammo;
        s_cursorSlot = 0;
        s_cursorBag = -1;
        uint32_t displayId = 0;
        if (const auto* info = gh->getItemInfo(ammo)) displayId = info->displayInfoId;
        wowee::ui::frameXmlSetCursorItem(displayId != 0 ? gh->getItemIconPath(displayId) : std::string());
        gh->setAmmo(0);
        return 0;
    }
    if (slot < 1 || slot > 23) return 0;

    // Before the pickup, because the paperdoll has no SpellCanTargetItem branch
    // of its own - a left-click on a worn weapon always arrives here, and while
    // a stone or an oil is waiting for a target that click is the target.
    if (slot <= 19 && completedItemTarget(L, gh->getEquipSlotGuid(slot - 1))) return 0;

    // Worn gear is what usually needs repairing, and the paperdoll has no
    // branch of its own for it either.
    if (slot <= 19 && repairedHeldItem(gh, gh->getEquipSlotGuid(slot - 1))) return 0;

    if (droppedItemFromNowhere(L)) return 0;

    // Carrying something: equip it here, which is the other half of the drag.
    uint8_t srcBag = 0, srcSlot = 0;
    if (cursorWireSlot(srcBag, srcSlot)) {
        if (!taint::allowed(L, 0x16)) return 0;   // SwapItems, 0x006df890
        gh->swapContainerItems(srcBag, srcSlot, 0xFF,
                               static_cast<uint8_t>(slot - 1));
        clearCursorItem(L);
        return 0;
    }

    const auto& inv = gh->getInventory();
    const auto& eq = inv.getEquipSlot(static_cast<game::EquipSlot>(slot - 1));
    if (!eq.empty()) {
        setCursorType(L, CursorType::ITEM);
        s_cursorId = eq.item.itemId;
        s_cursorSlot = slot;
        s_cursorBag = -1;
        // The same three things a container pickup does, which this had none
        // of: remember where it came from, put its icon on the pointer, and say
        // the slot is locked. Without them dragging off the character sheet
        // carried nothing visible and left the slot looking untouched.
        cursorItemSlot() = {-1, slot, true};
        uint32_t displayId = eq.item.displayInfoId;
        if (displayId == 0) {
            if (const auto* info = gh->getItemInfo(eq.item.itemId)) {
                displayId = info->displayInfoId;
            }
        }
        wowee::ui::frameXmlSetCursorItem(
            displayId ? gh->getItemIconPath(displayId) : std::string());
        // One argument, not two: the paperdoll's handler is
        // `if ( not arg2 and arg1 == self:GetID() )`, so an equipment lock is
        // the slot alone. A second argument makes that test fail and the square
        // never greys.
        gh->fireAddonEvent("ITEM_LOCK_CHANGED", {std::to_string(slot)});
        LOG_WARNING("FrameXML pickup: equipment slot ", slot,
                    " item ", eq.item.itemId);
    } else {
        LOG_WARNING("FrameXML pickup: equipment slot ", slot,
                    " - nothing equipped there");
    }
    return 0;
}

// DeleteCursorItem() - destroys the item on the cursor.
//
// It only put the cursor down. Nothing was ever destroyed: the item vanished
// from the cursor, the server was never told, and the next bag update put it
// straight back. StaticPopup "DELETE_ITEM" calls this on Accept, so the whole
// drag-an-item-out-to-destroy-it gesture ended in the item reappearing.
//
// cursorWireSlot already converts the held position into the bag and slot the
// server expects, which is the same translation the drop half of a drag uses.
static int lua_DeleteCursorItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    uint8_t bag = 0, slot = 0;
    if (gh && cursorWireSlot(bag, slot)) {
        // The whole stack: WoW asks about a partial stack before it gets here,
        // and the popup that calls this is the confirmation for all of it.
        gh->destroyItem(bag, slot, 0);
    }
    clearCursorItem(L);
    return 0;
}

// AutoEquipCursorItem() - equip item from cursor
static int lua_AutoEquipCursorItem(lua_State* L) {
    auto* gh = getGameHandler(L);
    // CGPlayer_C::AutoEquipCursorItem asks the taint check (0x006dfc40, kind
    // 0x16) before anything moves.
    if (gh && s_cursorType == CursorType::ITEM && s_cursorId != 0 &&
        !taint::allowed(L, 0x16)) {
        return 0;
    }
    if (gh && s_cursorType == CursorType::ITEM && s_cursorId != 0) {
        gh->useItemById(s_cursorId);
    }
    clearCursorItem(L);
    return 0;
}

// EquipCursorItem(slot) (0x0051a3d0): the held item into that equipment slot.
// Dropped back where it came from when the slot already holds it, or when it
// came off the paperdoll and does not fit there; otherwise, not fitting, worn
// wherever it does fit, as AutoEquipCursorItem would (0x006dfc40).
static int lua_EquipCursorItem(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: EquipCursorItem(slot)");
    auto* gh = getGameHandler(L);
    if (!gh || s_cursorType != CursorType::ITEM || s_cursorId == 0) {
        LOG_WARNING("EquipCursorItem: no item on the cursor to equip");
        return 0;
    }
    const int slot = luaToClientInt(L, 1);
    if (slot >= 1 && slot <= 19) {
        const CursorItemSlot held = cursorItemSlot();
        if (held.equipped && held.slot == slot) {
            LOG_WARNING("EquipCursorItem: slot ", slot, " already holds it; put back");
            clearCursorItem(L);
            return 0;
        }
        const auto* info = gh->getItemInfo(s_cursorId);
        uint8_t srcBag = 0, srcSlot = 0;
        if (info && game::inventoryTypeFitsSlot(slot, info->inventoryType) &&
            cursorWireSlot(srcBag, srcSlot)) {
            if (!taint::allowed(L, 0x16)) return 0;   // SwapItems, 0x006df890
            gh->swapContainerItems(srcBag, srcSlot, 0xFF, static_cast<uint8_t>(slot - 1));
            clearCursorItem(L);
            return 0;
        }
        if (held.equipped) {
            LOG_WARNING("EquipCursorItem: worn item does not fit slot ", slot, "; put back");
            clearCursorItem(L);
            return 0;
        }
    }
    LOG_WARNING("EquipCursorItem: slot ", slot, " does not take it; worn where it fits");
    return lua_AutoEquipCursorItem(L);
}

// --- Frame System ---
// Minimal WoW-compatible frame objects with RegisterEvent/SetScript/GetScript.
// Frames are Lua tables with a metatable that provides methods.

// Frame method: frame:RegisterEvent("EVENT")

// The modifier state comes from the keyboard rather than from ImGui, which
// learns it from events this client does not always route there once FrameXML
// owns the mouse. lua_system_api registers the same three names against
// SDL_GetModState; these agree with it now instead of racing it, and the
// duplicate Is*KeyDown bindings that used to live here are gone with them.
static bool shiftHeld() { return (SDL_GetModState() & SDL_KMOD_SHIFT) != 0; }
static bool ctrlHeld()  { return (SDL_GetModState() & SDL_KMOD_CTRL)  != 0; }
static bool altHeld()   { return (SDL_GetModState() & SDL_KMOD_ALT)   != 0; }


/// What bindings.xml declares for a modified-click action, or empty.
///
/// One list, read once. The emitter keeps every <ModifiedClick action= default=>
/// in __WoweeModifiedClick, which is the same file the key bindings panel is
/// built from - so this cannot drift from what the interface believes.
///
/// It had drifted. A table written out by hand here answered ALT for FOCUSCAST,
/// which bindings.xml declares as NONE, so alt-clicking a spell tried to cast
/// it at the focus when nothing had asked for that; and both flyout actions are
/// declared ALT and fell through to a shift default.
static std::string modifiedClickBinding(lua_State* L, const std::string& action) {
    // The client's own table, kept in C: asking it taints no click handler,
    // whoever set the binding last.
    taint::Quiet quiet;
    lua_getglobal(L, "__WoweeModifiedClick");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return {}; }
    lua_getfield(L, -1, action.c_str());
    const char* value = lua_tostring(L, -1);
    std::string out = value ? value : "";
    lua_pop(L, 2);
    return out;
}

/// Whether the modifiers a binding names are all held.
///
/// A binding is a dash-separated list ending in an optional button -
/// "SHIFT-BUTTON1", "CTRL", "NONE". The button half is the caller's business:
/// it already knows which button was pressed, and every caller here is inside
/// a handler for one.
static bool modifiersHeldFor(const std::string& binding) {
    if (binding.empty() || binding == "NONE") return false;
    bool wantShift = false, wantCtrl = false, wantAlt = false;
    std::string wantButton;
    size_t at = 0;
    while (at <= binding.size()) {
        const size_t dash = binding.find('-', at);
        const std::string part = binding.substr(
            at, dash == std::string::npos ? std::string::npos : dash - at);
        if (part == "SHIFT") wantShift = true;
        else if (part == "CTRL") wantCtrl = true;
        else if (part == "ALT") wantAlt = true;
        // A binding names a button as often as it names a modifier, and the
        // two named actions on the same modifier are told apart by nothing
        // else: CHATLINK is SHIFT-BUTTON1 and SOCKETITEM is SHIFT-BUTTON2.
        // Reading only the modifier made both true on any shift-click, so
        // shift-clicking an item in a bag put the link in chat and opened the
        // socketing window behind it - or, when the link could not be
        // inserted, only the socketing window.
        else if (part == "BUTTON1") wantButton = "LeftButton";
        else if (part == "BUTTON2") wantButton = "RightButton";
        if (dash == std::string::npos) break;
        at = dash + 1;
    }
    if (!wantShift && !wantCtrl && !wantAlt) return false;
    if (!wantButton.empty()) {
        // Only while a click is being dispatched. Asked at any other moment -
        // an OnUpdate deciding what a tooltip should say - the modifier alone
        // is the whole of the question, and the button has not been pressed.
        const std::string& clicking = currentClickButton();
        if (!clicking.empty() && clicking != wantButton) return false;
    }
    return (!wantShift || shiftHeld()) && (!wantCtrl || ctrlHeld()) &&
           (!wantAlt || altHeld());
}

// IsModifiedClick(action) → boolean
static int lua_IsModifiedClick(lua_State* L) {
    const char* action = luaL_optstring(L, 1, "");
    std::string act(action);
    for (char& c : act) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (act.empty()) {
        // No action named means "is this click modified at all", which is how
        // the paperdoll and the bags branch: IsModifiedClick() picks between
        // OnModifiedClick and the plain OnClick. Answering with shift alone
        // sent every ctrl-click down the plain path, so ctrl-clicking a worn
        // item picked it up instead of putting it in the dressing room.
        lua_pushboolean(L, (shiftHeld() || ctrlHeld() || altHeld()) ? 1 : 0);
        return 1;
    }
    const std::string binding = modifiedClickBinding(L, act);
    const bool answer = binding.empty() ? shiftHeld() : modifiersHeldFor(binding);
    // The gate the comparison tooltip goes through, said once each way.
    //
    // Shift-hovering an item that differs from the worn one produced no
    // refusal from SetHyperlinkCompareItem at all, which means FrameXML never
    // got as far as calling it - and this is what it asks first. Whether the
    // answer is yes or no, and which binding decided it, is the difference
    // between a modifier this client never sees and a comparison that is
    // refused further along.
    if (act == "COMPAREITEMS") {
        static bool saidYes = false, saidNo = false;
        bool& said = answer ? saidYes : saidNo;
        if (!said) {
            said = true;
            LOG_WARNING("IsModifiedClick(COMPAREITEMS) = ", answer ? "true" : "false",
                        ", binding '", binding.empty() ? "(none - falls back to shift)" : binding,
                        "', shift held ", shiftHeld() ? "yes" : "no");
        }
    }
    lua_pushboolean(L, answer ? 1 : 0);
    return 1;
}

// GetModifiedClick(action) → the binding it is on ("SHIFT", "CTRL-BUTTON1", "NONE")
static int lua_GetModifiedClick(lua_State* L) {
    const char* action = luaL_optstring(L, 1, "");
    std::string act(action);
    for (char& c : act) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const std::string binding = modifiedClickBinding(L, act);
    lua_pushstring(L, binding.empty() ? "SHIFT" : binding.c_str());
    return 1;
}

// SetModifiedClick(action, binding) - rebinding one from the options panel.
//
// It wrote nowhere, so the panel accepted a change and the next question about
// that action answered the old value. The table the emitter fills is the same
// one read above, so writing to it is the whole of it.
// GetNumModifiedClickActions() (0x0055dc60) and GetModifiedClickAction(index)
// (0x0055ea70): the modified clicks Bindings.xml declares, in its order.
static int lua_GetNumModifiedClickActions(lua_State* L) {
    double n = 0;
    {
        taint::Quiet quiet;   // see modifiedClickBinding
        lua_getglobal(L, "__WoweeModifiedClickOrder");
        if (lua_istable(L, -1)) n = static_cast<double>(lua_objlen(L, -1));
        lua_pop(L, 1);
    }
    lua_pushnumber(L, n);
    return 1;
}

static int lua_GetModifiedClickAction(lua_State* L) {
    if (!lua_isnumber(L, 1)) return luaL_error(L, "Usage: GetModifiedClickAction(index)");
    const int index = luaToClientInt(L, 1);
    std::string action;
    {
        taint::Quiet quiet;
        lua_getglobal(L, "__WoweeModifiedClickOrder");
        if (lua_istable(L, -1) && index >= 1) {
            lua_rawgeti(L, -1, index);
            if (lua_isstring(L, -1)) action = lua_tostring(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    if (action.empty()) lua_pushnil(L); else lua_pushstring(L, action.c_str());
    return 1;
}

// GetMouseButtonName(button) (0x00514ba0): a button's number as the name the
// client gives it - 1 LeftButton, 2 RightButton, 3 MiddleButton, then
// Button4 to Button31, "UNKNOWN" for any other number (0x005eec20,
// 0x0096f530); a string is its own name; anything else nil.
static int lua_GetMouseButtonName(lua_State* L) {
    if (lua_type(L, 1) == LUA_TNUMBER) {
        const int n = luaToClientInt(L, 1);
        std::string name;
        if (n == 1) name = "LeftButton";
        else if (n == 2) name = "RightButton";
        else if (n == 3) name = "MiddleButton";
        else if (n >= 4 && n <= 31) name = "Button" + std::to_string(n);
        else name = "UNKNOWN";
        lua_pushstring(L, name.c_str());
        return 1;
    }
    if (lua_isstring(L, 1)) {
        lua_pushvalue(L, 1);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static int lua_SetModifiedClick(lua_State* L) {
    const char* action = luaL_optstring(L, 1, "");
    const char* binding = luaL_optstring(L, 2, "NONE");
    if (!action || !*action) return 0;
    std::string act(action);
    for (char& c : act) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    {
        taint::Quiet quiet;   // see modifiedClickBinding
        lua_getglobal(L, "__WoweeModifiedClick");
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
        lua_pushstring(L, binding);
        lua_setfield(L, -2, act.c_str());
        lua_pop(L, 1);
    }
    // And the store, so it comes back next time. Bindings.xml rewrites the
    // table with its defaults at every load, and SaveBindings writes keys,
    // not clicks: nothing else kept the self-cast, focus-cast and auto-loot
    // keys past the session they were set in.
    std::string stored = "modifiedclick_" + act;
    for (char& c : stored) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    setStoredCVar(stored, binding);
    return 0;
}


// --- Trading ---
//
// Placing an item is the only part that needs the cursor, which is why these
// live here. The peer's side is read-only: a trade slot of theirs can be looked
// at and not touched, so clicking one does nothing rather than pretending.

// The two info bindings answer a fixed shape, and the shapes are not the same
// length.  0x00587eb0 (this player's side) answers five values - name, icon,
// count, then a number, then the enchant slot's spell name - and 0x00587c60
// (the partner's side) answers six, with the client's usability answer between
// the number and the enchantment.  FrameXML reads that fourth of ours as
// isUsable on one half of the window and quality on the other
// (tradeframe.lua:68 and :96), but it is the same value both times: the item
// record's quality, which the client reads out of its record cache at +0x14
// through a gate at +0x28 and answers -1 when the gate says there is none.  The
// container binding 0x005d7a90 reads the same pair for the value FrameXML calls
// quality.  Neither half has a usability flag in that place at all.
static void pushTradeItemLead(lua_State* L, game::GameHandler* gh,
                              const game::GameHandler::TradeSlot& slot) {
    const auto* info = gh->getItemInfo(slot.itemId);
    // The name the client shows is the record's with the rolled property beside
    // it: 0x00706d70(buf, size, entry, propertyId) is what both legs call, and
    // the property it takes is the row's own, so an item traded with "of the
    // Bear" on it is named with those words - and coloured, in the link, by the
    // record's quality rather than by anything the roll would suggest.
    const std::string name = info
        ? gh->itemNameWithProperty(info->name, slot.inst.randomPropertyId)
        : std::string{};
    lua_pushstring(L, name.c_str());
    lua_pushstring(L, gh->getItemIconPath(
        info && info->displayInfoId ? info->displayInfoId : slot.displayId).c_str());
    lua_pushnumber(L, slot.stackCount);
    lua_pushnumber(L, info ? static_cast<double>(info->quality) : -1.0);
}

// The enchant slot's text, which only ever answers for the seventh slot.  Both
// legs ask for the *other* side's spell, and the swap is the client's rather
// than a slip in one call site: 0x00704680 writes the packet's side byte
// straight into 0xca0ff0 + side*4, and side 0 is the half whose slots the client
// keeps item objects for (the parser's own-side branch, `local_5 == 0`), while
// 0x00587eb0 reads back index 1 and 0x00587c60 index 0.  The watcher agrees with
// the readers: 0x00586dc0 compares 0x00703c50(0) against 0xbfa668 and fires
// TRADE_TARGET_ITEM_CHANGED for slot 7 when it moves, and 0x00703c50(1) against
// 0xbfa66c for TRADE_PLAYER_ITEM_CHANGED.  Each half of the window names the
// enchantment the other half offered.
static void pushTradeEnchantment(lua_State* L, game::GameHandler* gh, bool ownSide) {
    const uint32_t spell = gh->getTradeEnchantSpell(!ownSide);
    if (spell == 0) { lua_pushnil(L); return; }
    // A spell id with no record answers nothing, which is what 0x004cfd20
    // returning nil leaves the binding to push.
    const std::string& name = gh->getSpellName(spell);
    if (name.empty()) { lua_pushnil(L); return; }
    lua_pushstring(L, name.c_str());
}

// An empty slot - and a bad index, which falls off the end of the client's
// arrays into the same tail - answers its own empty shape and not a lone nil:
// nil, nil, 0, 0 and nil, or nil, nil, 0, 0, nil, nil on the partner's side.
static int pushTradeItemEmpty(lua_State* L, bool partnerSide) {
    lua_pushnil(L);
    lua_pushnil(L);
    lua_pushnumber(L, 0);
    lua_pushnumber(L, 0);
    lua_pushnil(L);
    if (partnerSide) lua_pushnil(L);
    return partnerSide ? 6 : 5;
}

static int lua_GetTradePlayerItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (i < 1 || i > game::GameHandler::TRADE_SLOT_COUNT) return pushTradeItemEmpty(L, false);
    const auto& slot = gh->getMyTradeSlots()[i - 1];
    if (!slot.occupied || slot.itemId == 0) return pushTradeItemEmpty(L, false);
    pushTradeItemLead(L, gh, slot);
    if (i - 1 == game::GameHandler::TRADE_SLOT_NONTRADED) pushTradeEnchantment(L, gh, true);
    else lua_pushnil(L);
    return 5;
}

static int lua_GetTradeTargetItemInfo(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (!gh) return 0;
    const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (i < 1 || i > game::GameHandler::TRADE_SLOT_COUNT) return pushTradeItemEmpty(L, true);
    const auto& slot = gh->getPeerTradeSlots()[i - 1];
    if (!slot.occupied || slot.itemId == 0) return pushTradeItemEmpty(L, true);
    pushTradeItemLead(L, gh, slot);
    // The partner's half is the only one with a usability answer, and it is nil
    // and not false when the item fails - 0x006dc3f0 measures the record's
    // required level, class and race against this player and 0x00587c60 pushes
    // nothing when it says no.  WoWee has no such measure for an item it was
    // never offered, so it keeps answering the usable answer, which is what the
    // client gives for an item that meets its requirements.
    lua_pushboolean(L, 1);
    if (i - 1 == game::GameHandler::TRADE_SLOT_NONTRADED) pushTradeEnchantment(L, gh, false);
    else lua_pushnil(L);
    return 6;
}

static int pushTradeLink(lua_State* L, game::GameHandler* gh,
                         const game::GameHandler::TradeSlot& slot) {
    const auto* info = (gh && slot.occupied) ? gh->getItemInfo(slot.itemId) : nullptr;
    if (!info || info->name.empty()) { return luaReturnNil(L); }
    // The row's own instance numbers, not an empty set: 0x0061e290 is handed
    // the enchant, the three gems, the property and the seed from the trade row
    // (0xca0b80 + 0x0c, 0x10, 0x40 and 0x3c), and the tooltip the trade window
    // asks for is built from this link.  A traded item that was enchanted,
    // socketed or rolled would otherwise read as its plain template.
    const std::string name = gh->itemNameWithProperty(info->name, slot.inst.randomPropertyId);
    const std::string link = game::itemChatLink(slot.itemId, static_cast<uint32_t>(info->quality),
                                                name, slot.inst, gh->getPlayerLevel());
    lua_pushstring(L, link.c_str());
    return 1;
}

static int lua_GetTradePlayerItemLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || i < 1 || i > game::GameHandler::TRADE_SLOT_COUNT) { return luaReturnNil(L); }
    return pushTradeLink(L, gh, gh->getMyTradeSlots()[i - 1]);
}

static int lua_GetTradeTargetItemLink(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || i < 1 || i > game::GameHandler::TRADE_SLOT_COUNT) { return luaReturnNil(L); }
    return pushTradeLink(L, gh, gh->getPeerTradeSlots()[i - 1]);
}

// ClickTradeButton(slot [, rightButton]) - put what is held into the slot, or
// take back what is already there when nothing is held
static int lua_ClickTradeButton(lua_State* L) {
    auto* gh = getGameHandler(L);
    const int i = static_cast<int>(luaL_optnumber(L, 1, 0));
    if (!gh || i < 1 || i > game::GameHandler::TRADE_SLOT_COUNT) return 0;
    // The client answers this one click with two different requests (0x005879d0),
    // and which one it sends is decided by the second argument: tradeframe.xml's
    // OnMouseUp handler hands it `button == "RightButton"`, and staticpopup.lua
    // hands the enchant slot a true.  The test there is on the value and not on
    // whether anything was passed (0x0084e0b0 answers 0 for an absent argument and
    // for a false one alike), so a left click through that same handler takes the
    // first leg, which is what makes this a boolean and not an argument count.
    const bool rightButton = lua_toboolean(L, 2) != 0;
    uint8_t srcBag = 0, srcSlot = 0;
    // Through the same translation as every other send. This passed FrameXML's
    // own numbering straight to the server - bags counted 0 to 4 and slots from
    // one - where the wire wants the flat space GetItemByPos reads: container
    // 255 with an absolute slot, or a worn bag's equipment slot with a 0-based
    // index inside it. The server finds no item at the slot it was given and
    // answers TRADE_STATUS_TRADE_CANCELED, so offering an item ended the trade.
    if (rightButton) {
        // A right click takes the offered item back and asks nothing of what the
        // cursor holds: the second leg reads the slot, clears it (0x007041a0,
        // opcode 0x11e with the slot byte) and puts the item away, and never
        // reaches the code that would place the held item.  Which of the two
        // requests went out was all this got wrong - it offered what was held,
        // so right-clicking a filled slot added to the trade instead of taking
        // the slot's occupant back.  It asked whether the slot holds something
        // first, and that gate is deliberately not reproduced: this client's own
        // seven slots are filled from the server's extended update, so a slot it
        // had not heard of yet would answer "nothing to take" and the take-back
        // would never be sent at all.
        //
        // What the leg also does locally - the item leaving its row on the table
        // and the client's own marker on it going away (0x00707020) - is the
        // server's move to make here, and there is no core to compare against:
        // WOWEE_SERVER_SRC is unset, so which core answers which verb stays
        // unobserved either way.
        gh->clearTradeItem(static_cast<uint8_t>(i - 1));
        return 0;
    }
    if (s_cursorType == CursorType::ITEM && s_cursorBag >= 0 &&
        cursorWireSlot(srcBag, srcSlot)) {
        gh->setTradeItem(static_cast<uint8_t>(i - 1), srcBag, srcSlot);
        clearCursorItem(L);
    } else {
        // The first leg is more than the send: whatever the row held goes onto
        // the cursor afterwards (0x00520770, handed the row's previous item), and
        // on a swap the displaced item rides the cursor as well instead of going
        // back into the bag it came from.  Not reproduced - a trade row has no
        // place in this client's cursor numbering, and nothing behind it sends
        // such an item anywhere once it is let go, so the item would ride the
        // cursor and vanish on the drop instead of going where the server put it.
        gh->clearTradeItem(static_cast<uint8_t>(i - 1));
    }
    return 0;
}

// The other side's slots belong to the other player.
static int lua_ClickTargetTradeButton(lua_State* L) { (void)L; return 0; }

static int lua_SetTradeMoney(lua_State* L) {
    auto* gh = getGameHandler(L);
    if (gh) gh->setTradeGold(static_cast<uint64_t>(luaL_optnumber(L, 1, 0)));
    return 0;
}

static int lua_CloseTrade(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->cancelTrade();
    return 0;
}

static int lua_BeginTrade(lua_State* L) {
    if (auto* gh = getGameHandler(L)) gh->acceptTradeRequest();
    return 0;
}

// --- Keybinding API ---
//
// Which commands exist, and in what order, comes from bindings.xml: the emitter
// turns it into __WoweeBindings (the list the UI walks, header rows included)
// and __WoweeBindingScripts (what each one runs). What each is *bound to* lives
// here instead, because it outlives any one Lua state and has to reach a file.

namespace {

/// command → its two keys, empty meaning not bound. Two because the UI offers a
/// primary and a secondary and will write either.
std::map<std::string, std::array<std::string, 2>>& bindingKeys() {
    static std::map<std::string, std::array<std::string, 2>> keys;
    return keys;
}

std::string bindingsFilePath() {
    return core::getConfigRoot() + "/bindings.cfg";
}

/// ImGui spells keys as "C" and "Space"; the interface expects "C" and "SPACE",
/// and compares them as strings everywhere.
std::string wowKeyFromImGui(ImGuiKey key) {
    // An action with nothing bound to it answers "None", which uppercased is a
    // perfectly ordinary-looking key name and would show as one.
    if (key == ImGuiKey_None) return "";
    const char* name = ImGui::GetKeyName(key);
    if (!name || !*name) return "";
    std::string out(name);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

/// The commands the client has a real action behind. Rebinding one of these has
/// to reach the manager, or the list would show the new key while the client
/// went on answering to the old one.
struct LiveBinding {
    const char* command;
    wowee::ui::KeybindingManager::Action action;
};
const LiveBinding kLiveBindings[] = {
    {"TOGGLECHARACTER0",  wowee::ui::KeybindingManager::Action::TOGGLE_CHARACTER_SCREEN},
    {"TOGGLEBACKPACK",    wowee::ui::KeybindingManager::Action::TOGGLE_BAGS},
    {"TOGGLESPELLBOOK",   wowee::ui::KeybindingManager::Action::TOGGLE_SPELLBOOK},
    {"TOGGLETALENTS",     wowee::ui::KeybindingManager::Action::TOGGLE_TALENTS},
    {"TOGGLEQUESTLOG",    wowee::ui::KeybindingManager::Action::TOGGLE_QUESTS},
    {"TOGGLEWORLDMAP",    wowee::ui::KeybindingManager::Action::TOGGLE_WORLD_MAP},
    {"TOGGLEMINIMAP",     wowee::ui::KeybindingManager::Action::TOGGLE_MINIMAP},
    {"TOGGLEACHIEVEMENT", wowee::ui::KeybindingManager::Action::TOGGLE_ACHIEVEMENTS},
    {"TOGGLEGUILDTAB",    wowee::ui::KeybindingManager::Action::TOGGLE_GUILD_ROSTER},
};

/// The named key whose name matches, or none. ImGui offers no reverse lookup,
/// and the set is small enough that walking it costs nothing next to the file
/// write that follows a rebind.
ImGuiKey imGuiKeyFromWow(const std::string& name) {
    if (name.empty()) return ImGuiKey_None;
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        if (wowKeyFromImGui(static_cast<ImGuiKey>(k)) == name) {
            return static_cast<ImGuiKey>(k);
        }
    }
    return ImGuiKey_None;
}

/// Hand a binding to the client's own keybinding manager, if it answers that
/// command at all.
///
/// The interface allows two keys per command and the manager holds one per
/// action, so what is pushed here is keys[0] - the primary. The second stays
/// in the Lua map above. Both are kept past the session: SaveBindings writes
/// the two slots of every command in that map, and LoadBindings reads them
/// back and pushes the primary here again.
///
/// A binding for a command not in kLiveBindings does not reach the manager at
/// all. That is intended: those are the commands the interface answers for
/// itself.
void pushBindingToClient(const std::string& command, const std::string& key) {
    for (const auto& live : kLiveBindings) {
        if (command != live.command) continue;
        const ImGuiKey imKey = imGuiKeyFromWow(key);
        wowee::ui::KeybindingManager::getInstance().setKeyForAction(live.action, imKey);
        return;
    }
}

/// The key the client is listening for, when this is a command it acts on.
///
/// Read at the moment it is asked rather than from the copy below, because the
/// client's own settings panel can rebind these too - and then the copy is a
/// second answer to a question the manager already owns, one keystroke out of
/// date and shown on every action button.
std::optional<std::string> liveKeyFor(const std::string& command) {
    for (const auto& live : kLiveBindings) {
        if (command != live.command) continue;
        std::string key =
            wowKeyFromImGui(wowee::ui::KeybindingManager::getInstance()
                                .getKeyForAction(live.action));
        if (key.empty()) return std::nullopt;
        return key;
    }
    return std::nullopt;
}

/// The keys the client is actually listening for, asked of the manager that
/// listens rather than restated here - a second copy would be wrong the moment
/// either side moved. Commands the client has no action for keep their retail
/// default, so the list reads correctly even where nothing acts on it yet.
void seedBindingDefaults() {
    auto& keys = bindingKeys();
    if (!keys.empty()) return;

    static const struct { const char* command; const char* key; } kDefaults[] = {
        {"MOVEFORWARD", "W"},   {"MOVEBACKWARD", "S"},
        {"TURNLEFT", "A"},      {"TURNRIGHT", "D"},
        {"STRAFELEFT", "Q"},    {"STRAFERIGHT", "E"},
        {"JUMP", "SPACE"},      {"TOGGLEAUTORUN", "NUMLOCK"},
        // Tab targets, and has since the client was written; the panel said
        // this one was unbound because nothing had ever told it.
        {"TARGETNEARESTENEMY", "TAB"},
        {"TOGGLEGAMEMENU", "ESCAPE"},
        // Run through their scripts, ToggleSheath() and ToggleRun().
        {"TOGGLESHEATH", "Z"},  {"TOGGLERUN", "NUMPADDIVIDE"},
        {"SCREENSHOT", "PRINTSCREEN"},
        {"ACTIONBUTTON1", "1"}, {"ACTIONBUTTON2", "2"},
        {"ACTIONBUTTON3", "3"}, {"ACTIONBUTTON4", "4"},
        {"ACTIONBUTTON5", "5"}, {"ACTIONBUTTON6", "6"},
        {"ACTIONBUTTON7", "7"}, {"ACTIONBUTTON8", "8"},
        {"ACTIONBUTTON9", "9"}, {"ACTIONBUTTON10", "0"},
        {"ACTIONBUTTON11", "-"},{"ACTIONBUTTON12", "="},
        // The nameplate keys, which the client answers itself.
        {"NAMEPLATES", "V"}, {"FRIENDNAMEPLATES", "SHIFT-V"}, {"ALLNAMEPLATES", "CTRL-V"},
    };
    for (const auto& d : kDefaults) keys[d.command] = {d.key, ""};

    // Where a command corresponds to something the client really does, the key
    // shown is the one it really answers to.
    auto& manager = wowee::ui::KeybindingManager::getInstance();
    for (const auto& live : kLiveBindings) {
        const std::string key = wowKeyFromImGui(manager.getKeyForAction(live.action));
        if (!key.empty()) keys[live.command] = {key, ""};
    }

    // And the controller's own defaults, as second keys.
    //
    // The pad's scheme was written in C++ and pressed keys directly, so the
    // game's Key Bindings panel showed Action Button 3 on "3" and said
    // nothing about the D-pad: a player who opened the one screen that exists
    // for finding out what a button does saw a controller bound to nothing,
    // and had no way to change any of it.
    //
    // Read off the table that performs the scheme rather than restated here,
    // so the two cannot drift. The rows with no command - the bumper that is
    // only a modifier, the button that raises the pointer, the stick click
    // that sits down - stay the client's own, having nothing in the
    // interface's vocabulary to be.
    //
    // Second rather than first, so the keyboard's key is still the one shown
    // where only one fits.
    const auto seedPad = [&keys](std::span<const wowee::ui::PadBinding> rows) {
        for (const wowee::ui::PadBinding& row : rows) {
            const char* command = row.command;
            if (!command || !*command) continue;
            const char* padKey = wowee::ui::padKeyName(row.button);
            if (!padKey || !*padKey) continue;
            auto& pair = keys[command];
            if (pair[1].empty()) pair[1] = padKey;
        }
    };
    seedPad(wowee::ui::padBindings());
    seedPad(wowee::ui::padExtraBindings());
}

/// The command list the emitter built, or zero if bindings.xml never loaded.
int bindingCount(lua_State* L) {
    lua_getglobal(L, "__WoweeBindings");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
    const int n = static_cast<int>(lua_objlen(L, -1));
    lua_pop(L, 1);
    return n;
}

/// The command at a one-based position, empty when out of range.
std::string bindingAt(lua_State* L, int index) {
    lua_getglobal(L, "__WoweeBindings");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return ""; }
    lua_rawgeti(L, -1, index);
    const char* s = lua_tostring(L, -1);
    std::string out = s ? s : "";
    lua_pop(L, 2);
    return out;
}

}  // namespace

bool clientActsOnBinding(const std::string& command) {
    // The nine the client really performs, asked of the same list that keeps
    // their keys in step rather than restated - a second copy would be wrong
    // the moment either side moved.
    for (const auto& live : kLiveBindings) {
        if (command == live.command) return true;
    }
    // And the rest of what the client answers without going through a binding
    // at all. Each is a real call site: movement and jumping are polled every
    // frame, the action bar takes 1 through 0 and the two beside them in
    // GameScreen, chat opens on Enter and on slash, Tab targets, and Escape has
    // a whole chain of its own in escape_action.hpp.
    //
    // By command rather than by key, because the conflict is that the *action*
    // happens twice. Most bindings toggle something, so two answers to one
    // press cancel out and the key reads as dead - which is worse than doing
    // nothing, since it looks like the binding never ran.
    static constexpr const char* kClientAnswers[] = {
        "MOVEFORWARD", "MOVEBACKWARD", "TURNLEFT", "TURNRIGHT",
        "STRAFELEFT", "STRAFERIGHT", "JUMP", "TOGGLEAUTORUN",
        "TOGGLEGAMEMENU", "OPENCHAT", "OPENCHATSLASH", "TARGETNEARESTENEMY",
        "SCREENSHOT", "NAMEPLATES", "FRIENDNAMEPLATES", "ALLNAMEPLATES",
        "ACTIONBUTTON1", "ACTIONBUTTON2", "ACTIONBUTTON3", "ACTIONBUTTON4",
        "ACTIONBUTTON5", "ACTIONBUTTON6", "ACTIONBUTTON7", "ACTIONBUTTON8",
        "ACTIONBUTTON9", "ACTIONBUTTON10", "ACTIONBUTTON11", "ACTIONBUTTON12",
    };
    for (const char* name : kClientAnswers) {
        if (command == name) return true;
    }
    return false;
}

int clientImGuiKeyForBinding(const std::string& command) {
    for (const auto& live : kLiveBindings) {
        if (command != live.command) continue;
        return static_cast<int>(
            wowee::ui::KeybindingManager::getInstance().getKeyForAction(live.action));
    }
    return 0;
}

// GetBindingKey(command) → key1, key2 (or nil)
static int lua_GetBindingKey(lua_State* L) {
    seedBindingDefaults();
    const std::string command = luaL_checkstring(L, 1);
    if (auto live = liveKeyFor(command)) {
        lua_pushstring(L, live->c_str());
        lua_pushnil(L);
        return 2;
    }
    auto it = bindingKeys().find(command);
    if (it == bindingKeys().end()) { lua_pushnil(L); lua_pushnil(L); return 2; }
    for (const std::string& key : it->second) {
        if (key.empty()) lua_pushnil(L); else lua_pushstring(L, key.c_str());
    }
    return 2;
}

// GetBindingAction(key) → command, or "" for a key that holds nothing
//
// Empty rather than nil, which is what the real client answers and what the
// interface is written against. Blizzard's key binding panel reads it as
//
//     local oldAction = GetBindingAction(keyPressed, KeyBindingFrame.mode);
//     if ( oldAction ~= "" and oldAction ~= KeyBindingFrame.selected ) then
//         local key1, key2 = GetBindingKey(oldAction, ...)
//
// and nil is not equal to "", so a nil sent the panel into that branch with
// nil in hand: "bad argument #1 to 'GetBindingKey' (string expected, got
// nil)", thrown out of OnKeyDown before the new binding was ever set. Every
// attempt to bind a key that was not already bound to something failed that
// way - a controller button being the case where that is always true.
static int lua_GetBindingAction(lua_State* L) {
    seedBindingDefaults();
    const std::string key = luaL_checkstring(L, 1);
    for (const auto& [command, keys] : bindingKeys()) {
        if (keys[0] == key || keys[1] == key) {
            lua_pushstring(L, command.c_str());
            return 1;
        }
    }
    lua_pushstring(L, "");
    return 1;
}

// GetNumBindings() → how many rows the list has, headers included
static int lua_GetNumBindings(lua_State* L) {
    lua_pushinteger(L, bindingCount(L));
    return 1;
}

// GetBinding(index) → command, key1, key2. A header row is a command like
// "HEADER_MOVEMENT" with no keys, which is how the UI tells the two apart.
static int lua_GetBinding(lua_State* L) {
    seedBindingDefaults();
    const int index = static_cast<int>(luaL_checkinteger(L, 1));
    const std::string command = bindingAt(L, index);
    if (command.empty()) { lua_pushnil(L); return 1; }
    lua_pushstring(L, command.c_str());
    if (auto live = liveKeyFor(command)) {
        lua_pushstring(L, live->c_str());
        lua_pushnil(L);
        return 3;
    }
    auto it = bindingKeys().find(command);
    if (it == bindingKeys().end()) { lua_pushnil(L); lua_pushnil(L); return 3; }
    for (const std::string& key : it->second) {
        if (key.empty()) lua_pushnil(L); else lua_pushstring(L, key.c_str());
    }
    return 3;
}

// SetBinding(key, command) → true. A nil command clears whatever holds the key,
// which is how the UI unbinds.
static bool bindKey(lua_State* L, const std::string& key, const char* command);

static int lua_SetBinding(lua_State* L) {
    const std::string key = luaL_checkstring(L, 1);
    const char* command = lua_isnoneornil(L, 2) ? nullptr : luaL_checkstring(L, 2);
    if (!bindKey(L, key, command)) return 0;
    lua_pushboolean(L, 1);
    return 1;
}

/// The key to the command, or to nothing (0x00562ed0). False when the taint
/// check refuses, which is the only refusal this client's bindings have.
static bool bindKey(lua_State* L, const std::string& key, const char* command) {
    seedBindingDefaults();
    // 0x00563520 -> 0x00562ed0, which asks the taint check (kind 0xd: not
    // insecure code in combat lockdown).
    if (!taint::allowed(L, 0xd)) return false;

    // A key belongs to one command at a time, so it leaves the one that had it
    // before it joins another - otherwise both claim it and which one answers
    // depends on map order.
    for (auto& [existing, keys] : bindingKeys()) {
        for (std::string& slot : keys) {
            if (slot != key) continue;
            slot.clear();
            // The command that just lost the key has to be told, not only the
            // one that gained it, or the client answers to both. What it
            // answers to now is whichever slot still holds something, which is
            // not always the first.
            pushBindingToClient(existing, keys[0].empty() ? keys[1] : keys[0]);
        }
    }
    if (command) {
        auto& keys = bindingKeys()[command];
        if (keys[0].empty()) keys[0] = key; else keys[1] = key;
        pushBindingToClient(command, keys[0]);
    }
    // Six frames wait on this, the action buttons among them: it is what
    // redraws the little key printed in the corner. Without it a rebind takes
    // effect while every button goes on showing the old key.
    if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("UPDATE_BINDINGS", {});
    return true;
}

/// SetBindingSpell, SetBindingItem and SetBindingMacro (0x005635e0,
/// 0x00563700, 0x00563820): the key to "SPELL name", "ITEM name" or
/// "MACRO name" - the commands the binding dispatch casts, uses or runs
/// itself (0x00563150) - and 1, or nil when the binding was refused. The
/// optional mode is the binding set; this client keeps one.
static int setBindingWithPrefix(lua_State* L, const char* prefix, const char* usage) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2)) return luaL_error(L, "%s", usage);
    const std::string command = std::string(prefix) + lua_tostring(L, 2);
    if (bindKey(L, lua_tostring(L, 1), command.c_str())) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}
static int lua_SetBindingSpell(lua_State* L) {
    return setBindingWithPrefix(L, "SPELL ", "Usage: SetBindingSpell(\"KEY\", \"spellname\"[, mode])");
}
static int lua_SetBindingItem(lua_State* L) {
    return setBindingWithPrefix(L, "ITEM ", "Usage: SetBindingItem(\"KEY\", \"itemname\"[, mode])");
}
static int lua_SetBindingMacro(lua_State* L) {
    return setBindingWithPrefix(L, "MACRO ",
                                "Usage: SetBindingMacro(\"KEY\", \"macroname\"|macroid[, mode])");
}

/// SetBindingClick(key, buttonName [, mouseButton [, mode]]) (0x00563940):
/// the key to "CLICK buttonName:mouseButton", LeftButton when none is named,
/// so pressing the key clicks that button.
static int lua_SetBindingClick(lua_State* L) {
    if (!lua_isstring(L, 1) || !lua_isstring(L, 2)) {
        return luaL_error(L, "Usage: SetBindingClick(\"KEY\", \"buttonName\"[, \"mouseButton\"][, mode])");
    }
    const char* mouse = lua_isstring(L, 3) ? lua_tostring(L, 3) : "LeftButton";
    const std::string command = std::string("CLICK ") + lua_tostring(L, 2) + ":" + mouse;
    if (bindKey(L, lua_tostring(L, 1), command.c_str())) lua_pushnumber(L, 1);
    else lua_pushnil(L);
    return 1;
}

// SaveBindings(which) → writes what is bound where, for the next run
static int lua_SaveBindings(lua_State* L) {
    if (!taint::allowed(L, 0xd)) return 0;   // 0x005640c0 -> 0x00562b80
    const std::string path = bindingsFilePath();
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        LOG_WARNING("Could not write the key bindings to ", path);
        return 0;
    }
    for (const auto& [command, keys] : bindingKeys()) {
        // Both slots on one line, the second empty when there is no second key,
        // so a command that lost one is recorded as having lost it.
        out << command << "=" << keys[0] << "," << keys[1] << "\n";
    }
    return 0;
}

// LoadBindings(which) → what a previous run saved, over the defaults
static int lua_LoadBindings(lua_State* L) {
    if (!taint::allowed(L, 0xd)) return 0;   // 0x00564070 -> 0x00562b80
    seedBindingDefaults();
    std::ifstream in(bindingsFilePath());
    if (!in) return 0;   // Nothing saved yet is not a failure.
    std::string line;
    while (std::getline(in, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string command = line.substr(0, eq);
        const std::string rest = line.substr(eq + 1);
        const size_t comma = rest.find(',');
        bindingKeys()[command] = {
            rest.substr(0, comma),
            comma == std::string::npos ? "" : rest.substr(comma + 1)
        };
        // What was saved is what the client should answer to, not what it
        // started with - otherwise a rebind survives in the list and nowhere
        // else, and only until the next save overwrites it.
        pushBindingToClient(command, bindingKeys()[command][0]);
    }
    if (auto* gh = getGameHandler(L)) gh->fireAddonEvent("UPDATE_BINDINGS", {});
    return 0;
}

// RunBinding(command, keystate) → runs what bindings.xml says the command does
static int lua_RunBinding(lua_State* L) {
    const std::string command = luaL_checkstring(L, 1);
    const char* keystate = lua_isnoneornil(L, 2) ? "down" : luaL_checkstring(L, 2);

    // Run under the caller's own taint (0x0055fad0 hands 0x0055f860 the
    // running one), the script found without tainting it further.
    const int caller = taint::current();
    {
        taint::Quiet quiet;
        lua_getglobal(L, "__WoweeBindingScripts");
        if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
        lua_getfield(L, -1, command.c_str());
        if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return 0; }
    }
    lua_pushstring(L, keystate);
    int rc = 0;
    {
        taint::Scope binding(caller);
        rc = lua_pcall(L, 1, 0, 0);
    }
    if (rc != 0) {
        LOG_WARNING("Binding ", command, " failed: ",
                    lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return 0;
}
// The override-binding family, none of which this client implements - only
// the Click variant was ever *bound*, and the other four raised. They are
// reached from restrictedframes.lua, the secure-frame machinery behind action
// buttons and unit frames, so a raise there breaks the frame rather than the
// binding. Bound alongside it, doing the same nothing.
//
// What they do ask is the taint check (0x00563480 and 0x005604f0, kind 0xd:
// not insecure code in combat lockdown), and that is kept: its refusal is
// an event the interface hears.
static int lua_SetOverrideBindingClick(lua_State* L) { taint::allowed(L, 0xd); return 0; }
static int lua_SetOverrideBinding(lua_State* L) { taint::allowed(L, 0xd); return 0; }
static int lua_SetOverrideBindingSpell(lua_State* L) { taint::allowed(L, 0xd); return 0; }
static int lua_SetOverrideBindingMacro(lua_State* L) { taint::allowed(L, 0xd); return 0; }
static int lua_SetOverrideBindingItem(lua_State* L) { taint::allowed(L, 0xd); return 0; }
static int lua_ClearOverrideBindings(lua_State* L) { taint::allowed(L, 0xd); return 0; }

// Frame methods: SetPoint, SetSize, SetWidth, SetHeight, GetWidth, GetHeight, GetCenter, SetAlpha, GetAlpha

void registerActionLuaAPI(lua_State* L) {
    static const struct { const char* name; lua_CFunction func; } api[] = {
                {"HasAction",           lua_HasAction},
                // A macro's name, which only a macro has; ActionButton_Update
                // shows it under the icon and expects nothing for a spell.
                // GetActionText(slot) → the name written under the button.
                //
                // Only a macro has one, which is what the action button draws
                // in its Name font string - so with nil answered for every slot
                // a bar full of macros was a row of nameless icons, all of them
                // the same question mark unless the player had picked an icon.
                // The names were there: GetMacroInfo has been reading them off
                // the same handler all along.
                {"GetActionText", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int slot = static_cast<int>(luaL_optnumber(L, 1, 0)) - 1;
            if (!gh || slot < 0) { lua_pushnil(L); return 1; }
            const auto& bar = gh->getActionBar();
            if (slot >= static_cast<int>(bar.size())) { lua_pushnil(L); return 1; }
            const auto& action = bar[static_cast<size_t>(slot)];
            if (action.type != game::ActionBarSlot::MACRO) { lua_pushnil(L); return 1; }
            const std::string name = gh->getMacroName(action.id);
            if (name.empty()) { lua_pushnil(L); return 1; }
            lua_pushstring(L, name.c_str());
            return 1;
        }},
                // Whether the action has a range to be in or out of. Nothing
                // here tracks that yet, and false is what "no range check"
                // looks like - the button simply never dims for distance.
                {"ActionHasRange",      [](lua_State* L) -> int {
                    lua_pushboolean(L, 0);
                    return 1;
                }},
                {"GetActionTexture",    lua_GetActionTexture},
                {"GetActionAutocast",   lua_GetActionAutocast},
                {"__WoweeUpdateShownMacros", lua_WoweeUpdateShownMacros},
                {"IsCurrentAction",     lua_IsCurrentAction},
                {"IsUsableAction",      lua_IsUsableAction},
                {"IsActionInRange",     lua_IsActionInRange},
                {"GetActionInfo",       lua_GetActionInfo},
                {"GetActionCount",      lua_GetActionCount},
                {"GetActionCooldown",   lua_GetActionCooldown},
                {"UseAction",           lua_UseAction},
                {"PickupAction",        lua_PickupAction},
                {"PlaceAction",         lua_PlaceAction},
                {"PickupEquipmentSet",  lua_PickupEquipmentSet},
                {"PickupEquipmentSetByName", lua_PickupEquipmentSetByName},
                {"PickupSpell",         lua_PickupSpell},
                {"PickupCompanion",     lua_PickupCompanion},
                {"PickupGuildBankItem", lua_PickupGuildBankItem},
                {"PlaceGlyphInSocket",  lua_PlaceGlyphInSocket},
                {"SplitGuildBankItem",  lua_SplitGuildBankItem},
                {"PickupSpellBookItem", lua_PickupSpellBookItem},
                {"PickupContainerItem", lua_PickupContainerItem},
                {"PickupItem",          lua_PickupItem},
                // The totem bar's summon flyout calls these two from its
                // OnClick and OnLeave, and nothing in this interface defines
                // them - not multicastactionbarframe.lua beside the XML that
                // calls them, not anywhere. An upstream gap rather than one of
                // this client's, but the raise lands here all the same, and it
                // lands on a shaman clicking their own totem bar.
                //
                // Answered with nothing, which makes the button inert. There
                // is no honest alternative: the missing function is what would
                // have decided which totem the flyout chose, and inventing one
                // would summon whatever this guessed.
                {"MultiCastSummonSpellButtonFlyoutButton_OnClick", lua_ReturnNothing2},
                {"MultiCastSummonSpellButtonFlyoutButton_OnLeave", lua_ReturnNothing2},
                {"PickupInventoryItem", lua_PickupInventoryItem},
                {"PickupBagFromSlot",   lua_PickupBagFromSlot},
                {"ClickSendMailItemButton", lua_ClickSendMailItemButton},
                {"ClickPetitionButton", lua_ClickPetitionButton},
                {"TurnInPetition",      lua_TurnInPetition},
                {"ClearCursor",         lua_ClearCursor},
                {"GetCursorInfo",       lua_GetCursorInfo},
                {"PickupPetAction",     lua_PickupPetAction},
                {"CursorHasItem",       lua_CursorHasItem},
                {"CursorHasSpell",      lua_CursorHasSpell},
                {"CursorHasMoney",      lua_CursorHasMoney},
                {"CursorHasMacro",      lua_CursorHasMacro},
                {"DeleteCursorItem",    lua_DeleteCursorItem},
                {"AutoEquipCursorItem", lua_AutoEquipCursorItem},
                {"EquipCursorItem",     lua_EquipCursorItem},
                {"IsModifiedClick",     lua_IsModifiedClick},
                {"GetModifiedClick",    lua_GetModifiedClick},
                {"SetModifiedClick",    lua_SetModifiedClick},
                {"GetNumModifiedClickActions", lua_GetNumModifiedClickActions},
                {"GetModifiedClickAction", lua_GetModifiedClickAction},
                {"GetMouseButtonName",  lua_GetMouseButtonName},
                // Macros. The client has stored their text all along and
                // persisted it per character; only the interface's way in was
                // missing, so GetNumMacros answered zero and the panel opened
                // empty over a full set.
                //
                // WoW splits the ids: 1-36 are account-wide, 37 and up belong
                // to the character. That split is what GetNumMacros reports and
                // what CreateMacro's perCharacter flag chooses between.
                {"GetNumMacros",        [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int global = 0, perChar = 0;
            if (gh) {
                for (uint32_t id : gh->getMacroIds()) {
                    if (id <= 36) ++global; else ++perChar;
                }
            }
            lua_pushnumber(L, global);
            lua_pushnumber(L, perChar);
            return 2;
        }},
                // SecureCmdOptionParse("options") (0x00564ae0) → the text of
                // the first clause whose conditions hold and the unit its
                // passing group names (nil for none); nothing when no clause
                // applies. The macro conditions as the client evaluates them
                // (game::macro_options, 0x005f0df0).
                {"SecureCmdOptionParse", [](lua_State* L) -> int {
            if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: SecureCmdOptionParse(\"options\")");
            auto* gh = getGameHandler(L);
            if (!gh) return 0;
            const std::string options = lua_tostring(L, 1);
            LuaMacroOptionState state(L, *gh);
            std::string result, unit;
            if (!game::macro_options::parse(options, state, result, unit)) return 0;
            lua_pushstring(L, result.c_str());
            if (unit.empty()) lua_pushnil(L); else lua_pushstring(L, unit.c_str());
            return 2;
        }},
                // GetMacroBody(index or name) (0x00566f40) → the text.
                {"GetMacroBody",        [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = macroArg(L, gh, 1);
            if (id == 0) return 0;
            lua_pushstring(L, gh->getMacroText(id).c_str());
            return 1;
        }},
                // GetMacroIndexByName(name) (0x005666a0) → the index of the
                // macro of that name without case, 0 for none.
                {"GetMacroIndexByName", [](lua_State* L) -> int {
            if (!lua_isstring(L, 1)) return luaL_error(L, "Usage: GetMacroIndexByName(name)");
            lua_pushnumber(L, macroArg(L, getGameHandler(L), 1));
            return 1;
        }},
                // GetMacroSpell(index or name) (0x00567450) → the name and
                // rank of the spell the macro shows, or nil, nil.
                {"GetMacroSpell",       [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const MacroShown* m = macroShown(L, gh, macroArg(L, gh, 1));
            if (!m) return 0;
            const auto it = m->result.spell > 0
                ? gh->spellNameCacheRef().find(static_cast<uint32_t>(m->result.spell))
                : gh->spellNameCacheRef().end();
            if (it == gh->spellNameCacheRef().end()) {
                lua_pushnil(L);
                lua_pushnil(L);
                return 2;
            }
            lua_pushstring(L, it->second.name.c_str());
            lua_pushstring(L, it->second.rank.c_str());
            return 2;
        }},
                // GetMacroItem(index or name) (0x00567200) → the name and link
                // of the item the macro shows, once the item is known.
                {"GetMacroItem",        [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const MacroShown* m = macroShown(L, gh, macroArg(L, gh, 1));
            if (!m || m->result.item == 0) return 0;
            const auto* info = gh->getItemInfo(m->result.item);
            if (!info || !info->valid) {
                gh->ensureItemInfo(m->result.item);
                return 0;
            }
            lua_pushstring(L, info->name.c_str());
            lua_pushstring(L, game::buildItemLink(m->result.item, info->quality, info->name,
                                                  gh->getPlayerLevel()).c_str());
            return 2;
        }},
                // SetMacroSpell(index or name, spell [, unit]) (0x00567320) and
                // SetMacroItem(index or name, item [, unit]) (0x005670d0): what
                // the macro shows, set rather than read from its text, and kept
                // until its text is read again - the macro stops following its
                // conditions. 1 when it names something, else nil.
                {"SetMacroSpell",       [](lua_State* L) -> int {
            return setMacroShown(L, false);
        }},
                {"SetMacroItem",        [](lua_State* L) -> int {
            return setMacroShown(L, true);
        }},
                // GetMacroInfo(index or name) (0x00566eb0) → name, icon, body;
                // nothing for no macro.
                {"GetMacroInfo",        [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const uint32_t id = macroArg(L, gh, 1);
            if (id == 0) return 0;
            std::string name = gh->getMacroName(id);
            if (name.empty()) name = "Macro";
            // The texture as 0x00566ac0 gives it: the question mark shows
            // what the macro casts.
            const std::string icon = macroTexture(L, gh, id);
            lua_pushstring(L, name.c_str());
            lua_pushstring(L, icon.c_str());
            lua_pushstring(L, gh->getMacroText(id).c_str());
            return 3;
        }},
                // CreateMacro(name, icon, body, perCharacter) → id
                {"CreateMacro",         [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (!gh) return luaReturnNil(L);
            const char* name = luaL_optstring(L, 1, "Macro");
            const char* icon = luaL_optstring(L, 2, "");
            const char* body = luaL_optstring(L, 3, "");
            const bool perChar = lua_toboolean(L, 4) != 0;
            // The macro verbs ask the taint check (0x00568160, 0x00566fa0,
            // 0x00566f70: kind 0xc - not insecure code in combat lockdown).
            if (!taint::allowed(L, 0xc)) return 0;
            // The first free id in the half that was asked for. WoW allows 18
            // of each; refusing past that is what stops a full list growing
            // ids the interface will not show.
            const uint32_t first = perChar ? 37u : 1u;
            const uint32_t last  = perChar ? 54u : 36u;
            uint32_t id = 0;
            for (uint32_t candidate = first; candidate <= last; ++candidate) {
                if (gh->getMacroText(candidate).empty()) { id = candidate; break; }
            }
            if (id == 0) return luaReturnNil(L);
            gh->setMacroText(id, body);
            gh->setMacroMeta(id, name, icon);
            lua_pushnumber(L, id);
            return 1;
        }},
                // EditMacro(id, name, icon, body) → id
                {"EditMacro",           [](lua_State* L) -> int {
            if (!taint::allowed(L, 0xc)) return 0;   // see CreateMacro
            auto* gh = getGameHandler(L);
            const uint32_t id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (!gh || id == 0) return luaReturnNil(L);
            const char* name = luaL_optstring(L, 2, nullptr);
            const char* icon = luaL_optstring(L, 3, nullptr);
            const char* body = luaL_optstring(L, 4, nullptr);
            // Each part is optional and nil means "leave it": EditMacro is
            // called with only a name when a macro is renamed, and writing the
            // other two as empty would erase the macro being renamed.
            if (body) gh->setMacroText(id, body);
            gh->setMacroMeta(id, name ? name : gh->getMacroName(id),
                                 icon ? icon : gh->getMacroIcon(id));
            lua_pushnumber(L, id);
            return 1;
        }},
                // DeleteMacro(index) - and the rest of the half shifts down.
                //
                // Every macro verb here takes what WoW calls an index and what
                // this client stores as an id, and the two are the same number
                // only while the slots are dense. CreateMacro fills the first
                // free slot, so they start dense; this emptied one and left a
                // hole, and from then on the two disagreed.
                //
                // What that costs: with macros in slots 1, 2 and 3, deleting
                // the second leaves 1 and 3. GetNumMacros counts two, so the
                // macro frame asks for slots 1 and 2 - one macro, one blank
                // row, and the macro in slot 3 invisible and unreachable.
                //
                // Compacting is also what WoW does. Deleting a macro there
                // shifts the ones after it down, which is why a macro on an
                // action bar can move when an earlier one is deleted; matching
                // that is more faithful than keeping a hole, and it is the
                // assumption getMacroIds already documents.
                {"DeleteMacro",         [](lua_State* L) -> int {
            if (!taint::allowed(L, 0xc)) return 0;   // see CreateMacro
            auto* gh = getGameHandler(L);
            const uint32_t id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (!gh || id == 0) return 0;
            // Which half this slot belongs to; the two do not shift into
            // each other.
            const uint32_t first = (id >= 37u) ? 37u : 1u;
            const uint32_t last  = (id >= 37u) ? 54u : 36u;
            if (id < first || id > last) return 0;
            for (uint32_t slot = id; slot < last; ++slot) {
                const std::string next = gh->getMacroText(slot + 1);
                if (next.empty()) {
                    gh->setMacroText(slot, "");
                    gh->setMacroMeta(slot, "", "");
                    break;
                }
                gh->setMacroText(slot, next);
                gh->setMacroMeta(slot, gh->getMacroName(slot + 1),
                                       gh->getMacroIcon(slot + 1));
            }
            // The last slot in the half is never written by the loop above.
            gh->setMacroText(last, "");
            gh->setMacroMeta(last, "", "");
            return 0;
        }},
                // Saved as each change is made, so this has nothing to do.
                {"SaveMacros",          [](lua_State* L) -> int { (void)L; return 0; }},
                // PickupMacro(id) - onto the cursor, so it can be dropped on
                // an action slot. The slot type already existed and the packet
                // that sets it already went out for spells and items; a macro
                // was simply never something the cursor could hold.
                {"PickupMacro",         [](lua_State* L) -> int {
            if (!taint::allowed(L, 0xe)) return 0;   // 0x005674f0
            auto* gh = getGameHandler(L);
            const uint32_t id = static_cast<uint32_t>(luaL_optnumber(L, 1, 0));
            if (!gh || id == 0 || gh->getMacroText(id).empty()) return 0;
            setCursorType(L, CursorType::MACRO);
            s_cursorId = id;
            std::string icon = gh->getMacroIcon(id);
            if (icon.empty()) icon = "Interface\\Icons\\INV_Misc_QuestionMark";
            wowee::ui::frameXmlSetCursorItem(icon);
            return 0;
        }},
                {"GetTradePlayerItemInfo", lua_GetTradePlayerItemInfo},
                {"GetTradeTargetItemInfo", lua_GetTradeTargetItemInfo},
                {"GetTradePlayerItemLink", lua_GetTradePlayerItemLink},
                {"GetTradeTargetItemLink", lua_GetTradeTargetItemLink},
                {"ClickTradeButton",    lua_ClickTradeButton},
                {"ClickTargetTradeButton", lua_ClickTargetTradeButton},
                {"SetTradeMoney",       lua_SetTradeMoney},
                // CancelTradeAccept() - take an acceptance back without
                // closing the trade. The cancel button does this instead of
                // closing while the player has already accepted, so without it
                // the only way out of an accepted trade was to end it.
                {"CancelTradeAccept", [](lua_State* L) -> int {
            // 0x00586730 asks the taint check first (kind 7: insecure code
            // only inside a hardware event).
            if (!taint::allowed(L, 7)) return 0;
            if (auto* gh = getGameHandler(L)) gh->unacceptTrade();
            return 0;
        }},
                {"CloseTrade",          lua_CloseTrade},
                {"BeginTrade",          lua_BeginTrade},
                {"GetBindingKey",       lua_GetBindingKey},
                {"GetBindingAction",    lua_GetBindingAction},
                {"GetNumBindings",      lua_GetNumBindings},
                {"GetBinding",          lua_GetBinding},
                {"SetBinding",          lua_SetBinding},
                {"SetBindingSpell",     lua_SetBindingSpell},
                {"SetBindingItem",      lua_SetBindingItem},
                {"SetBindingMacro",     lua_SetBindingMacro},
                {"SetBindingClick",     lua_SetBindingClick},
                // GetBindingByKey(key) - the binding a key runs, or nil.
                //
                // Reached from GetBindingFromClick, which StaticPopup_OnKeyDown
                // calls on *every* keypress while a popup is up - so with
                // dialogs handed over, pressing any key with one on screen hit
                // an unbound global and raised. Escape is how a popup is
                // dismissed, so it was the keypress most likely to hit it.
                //
                // Two names are all FrameXML compares against here, and both
                // are fixed in this client: escape opens the game menu and
                // print screen takes a screenshot, neither rebindable. Anything
                // else answers nil, which is what WoW answers for an unbound
                // key and which lets the dialog's own key handling run.
                {"GetBindingByKey", [](lua_State* L) -> int {
            std::string key(luaL_optstring(L, 1, ""));
            for (char& c : key) c = static_cast<char>(std::toupper(
                static_cast<unsigned char>(c)));
            if (key == "ESCAPE")           lua_pushstring(L, "TOGGLEGAMEMENU");
            else if (key == "PRINTSCREEN") lua_pushstring(L, "SCREENSHOT");
            else                           lua_pushnil(L);
            return 1;
        }},
                // GetQuestGreenRange() - how many levels below the player a
                // thing can be before it turns grey.
                //
                // This was left unbound on the grounds that a guessed constant
                // would mis-colour every quest and that nothing live called it.
                // The second half was wrong: TargetFrame_CheckLevel reaches it
                // through GetQuestDifficultyColor for any target the player can
                // attack, so it raised before levelText:Show() and an enemy's
                // level never appeared. A friendly target took the other branch
                // and showed its level, which is why it looked like enemies
                // specifically.
                //
                // No guess needed either. AzerothCore's Formulas.h gives the
                // grey level exactly, and the green range is the distance from
                // the player's level down to it.
                {"GetQuestGreenRange", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int lvl = gh ? static_cast<int>(gh->getPlayerLevel()) : 1;
            int gray;
            if (lvl <= 5)       gray = 0;
            else if (lvl <= 39) gray = lvl - 5 - lvl / 10;
            else if (lvl <= 59) gray = lvl - 1 - lvl / 5;
            else                gray = lvl - 9;
            lua_pushnumber(L, lvl - gray);
            return 1;
        }},
                // Two latent raises on the escape-key path, bound as no-ops
                // because doing nothing is the right behaviour rather than a
                // placeholder for it. SetUIVisibility is reached only from
                // ToggleGameMenu's first branch, which runs when UIParent is
                // hidden - out of the way until someone hides the interface,
                // and then on the only path that brings it back.
                // ConsoleAddMessage is the sink for FrameXML's debug print.
                {"SetUIVisibility",   [](lua_State* L) -> int { (void)L; return 0; }},
                {"ConsoleAddMessage", [](lua_State* L) -> int { (void)L; return 0; }},
                // GetCurrentBindingSet() - which set SaveBindings should write.
                //
                // 1 is account-wide and 2 is per character. This client keeps
                // one set of bindings, so it is always the first. It was
                // missing rather than stubbed, and interfaceoptionspanels.lua
                // calls SaveBindings(GetCurrentBindingSet()) from three
                // separate option handlers - so changing an option would have
                // thrown rather than saved.
                {"GetCurrentBindingSet", [](lua_State* L) -> int {
            lua_pushnumber(L, 1); return 1;
        }},
                {"SaveBindings",        lua_SaveBindings},
                {"LoadBindings",        lua_LoadBindings},
                {"RunBinding",          lua_RunBinding},
                {"SetOverrideBindingClick", lua_SetOverrideBindingClick},
                {"SetOverrideBinding",      lua_SetOverrideBinding},
                {"SetOverrideBindingSpell", lua_SetOverrideBindingSpell},
                {"SetOverrideBindingMacro", lua_SetOverrideBindingMacro},
                {"SetOverrideBindingItem",  lua_SetOverrideBindingItem},
                {"ClearOverrideBindings", lua_ClearOverrideBindings},
                // Paging lives here, and the getter with it. Both were
                // defined twice - this pair against __WoweeActionBarPage and a
                // bootstrap pair against a local - so whichever won,
                // ChangeActionBarPage could move one number while
                // GetActionBarPage read the other. Removing only the getter
                // made that worse rather than better, which is what happened
                // an hour ago.
                {"GetActionBarPage", [](lua_State* L) -> int {
            // 1 while the bonus bar holds the pet's actions (0x005a7fd0).
            if (auto* gh = getGameHandler(L); gh && gh->petActionsOnBonusBar()) {
                lua_pushnumber(L, 1);
                return 1;
            }
            // The page is the client's number (0x005a7fd0 pushes it fresh),
            // so whoever turned it last taints no action button reading it.
            lua_Number page = 1;
            {
                taint::Quiet quiet;
                lua_getglobal(L, "__WoweeActionBarPage");
                if (lua_isnumber(L, -1)) page = lua_tonumber(L, -1);
                lua_pop(L, 1);
            }
            lua_pushnumber(L, page);
            return 1;
        }},
                {"ChangeActionBarPage", [](lua_State* L) -> int {
            int page = static_cast<int>(luaL_checknumber(L, 1));
            if (!taint::allowed(L, 0xe)) return 0;   // 0x005a7f60
            if (page < 1) page = 1;
            if (page > 6) page = 6;
            lua_pushnumber(L, page);
            lua_setglobal(L, "__WoweeActionBarPage");
            // The number keys are handled client-side against a page of their
            // own, so it has to be told or they go on casting page one.
            if (auto* svc = getLuaServices(L); svc && svc->setActionBarPage) {
                svc->setActionBarPage(page);
            }
            // Through the engine, which delivers to both registries.
            //
            // This walked __WoweeEvents by hand and stopped there. That table
            // is where an addon's RegisterEvent lands; FrameXML registers
            // through frame:RegisterEvent, which fills __WoweeFrameEvents - so
            // the six frames that listen for this, including the one in
            // actionbutton.lua that redraws every button, were never told.
            //
            // The page number moved and nothing else did, which is exactly
            // what the arrows beside the bar looked like: they clicked, they
            // played their sound, and the icons stayed where they were.
            lua_getfield(L, LUA_REGISTRYINDEX, "wowee_lua_engine");
            auto* engine = static_cast<LuaEngine*>(lua_touserdata(L, -1));
            lua_pop(L, 1);
            if (engine) engine->fireEvent("ACTIONBAR_PAGE_CHANGED", {});
            return 0;
        }},
                // Two returns: whether there is a pet interface at all, and
                // whether it is a hunter's. PetFrame_SetHappiness reads the
                // second to decide whether to draw the happiness icon, and one
                // return left it nil.
                //
                // The second is that predicate on the pet handle, and the pet
                // handle is what 0x005d3960 asks it of - not the player.
                {"HasPetUI", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const bool has = gh && gh->hasPet();
            lua_pushboolean(L, has ? 1 : 0);
            const bool hunterPet = hunterPetOf(gh, resolveUnit(L, "pet"));
            lua_pushboolean(L, hunterPet ? 1 : 0);
            return 2;
        }},
                // Happiness, for a hunter's pet only. Nil is the honest answer
                // for everyone else and the one PetFrame_SetHappiness guards
                // for - the fallback answering with an object instead made
                // that guard pass and the branch index nothing.
                // GetMirrorTimerInfo(index) → timer, value, maxvalue, scale,
                // paused, label.
                //
                // "UNKNOWN" for a timer that is not running, which is exactly
                // what mirrortimer.lua tests for before using the rest. Absent,
                // the fallback answered with an object, that test passed, and
                // the next line divided a nil by a thousand.
                {"GetMirrorTimerInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 1)) - 1;
            // Shared with the packet handler that fires MIRROR_TIMER_START, so
            // that polling and event agree on the name MirrorTimer_Hide will
            // later be asked to match.
            const auto& kNames  = game::GameHandler::kMirrorTimerNames;
            const auto& kLabels = game::GameHandler::kMirrorTimerLabels;
            if (!gh || index < 0 || index > 2 || !gh->getMirrorTimer(index).active) {
                lua_pushstring(L, "UNKNOWN");
                lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
                lua_pushnumber(L, 0);
                lua_pushstring(L, "");
                return 6;
            }
            const auto& t = gh->getMirrorTimer(index);
            // Worded by the interface, as the event's label is: "Atem" on a
            // German client, the English only without that string. Read
            // before the answers are built, so nothing is popped among them.
            std::string label = kLabels[index];
            if (const char* key = game::GameHandler::kMirrorTimerLabelKeys[index]) {
                lua_getglobal(L, key);
                if (lua_isstring(L, -1)) label = lua_tostring(L, -1);
                lua_pop(L, 1);
            }
            lua_pushstring(L, kNames[index]);
            lua_pushnumber(L, t.value);
            lua_pushnumber(L, t.maxValue);
            lua_pushnumber(L, t.scale);
            // A number, as MirrorTimer_Show compares it: "if ( paused > 0 )".
            // A boolean there raised on every breath bar shown on entering
            // the world.
            lua_pushnumber(L, t.paused ? 1 : 0);
            lua_pushstring(L, label.c_str());
            return 6;
        }},
                // GetPetHappiness() → happiness 1..3, and what it does to damage
                //
                // Both values, and that is not tidiness. The pet frame does
                // format(PET_DAMAGE_PERCENTAGE, damagePercentage) - "Causes %d%%
                // of normal damage" - the moment happiness is anything at all,
                // and %d against nil raises. Answering nil, as this did before,
                // was safe only because the line above it returns early on a nil
                // happiness; filling in the first value alone would have turned
                // a blank indicator into a dead pet frame for every hunter.
                //
                // Happiness is a power like mana, at index 4, running from
                // nothing to its own maximum. The three faces are the thirds of
                // that range, and the damage each is worth - three quarters,
                // normal, a quarter more - is the game's, not a guess.
                //
                // And always two, even for the answer that has no happiness:
                // 0x005d3b00's one failure tail pushes nil and then the double
                // 100.0, so "no happiness" still carries a damage figure - the
                // 100% of an unmeasured pet. PetFrame_SetHappiness never gets
                // that far (it returns on the nil) but the second value is not
                // something the binding withholds.
                //
                // And only a hunter's pet has any: the row this is read out of
                // is fetched through 0x0071f390, which is the hunter-pet
                // predicate again, so a warlock pet - which does carry
                // happiness as a power, the realm sends it - gets the failure
                // answer rather than a level.
                {"GetPetHappiness", [](lua_State* L) -> int {
            auto* pet = resolveUnit(L, "pet");
            if (!pet || !hunterPetOf(getGameHandler(L), pet)) {
                lua_pushnil(L); lua_pushnumber(L, 100); return 2;
            }
            const uint32_t maxHappiness = pet->getMaxPowerByType(4);
            if (maxHappiness == 0) { lua_pushnil(L); lua_pushnumber(L, 100); return 2; }

            const uint32_t happiness = pet->getPowerByType(4);
            const int level = (happiness * 3 >= maxHappiness * 2) ? 3
                            : (happiness * 3 >= maxHappiness)     ? 2
                                                                  : 1;
            static const int kDamagePercent[4] = {0, 75, 100, 125};
            lua_pushnumber(L, level);
            lua_pushnumber(L, kDamagePercent[level]);
            return 2;
        }},
                // Only referenced from commented-out code in 3.3.5, but a
                // temporary pet's timer is nil when there is no timer.
                {"GetPetTimeRemaining", [](lua_State* L) -> int {
            lua_pushnil(L);
            return 1;
        }},
                // GetPetActionSlotUsable(index) → whether the pet can use it
                //
                // Read as "if usable then draw it normally, else grey it out",
                // so an absent answer greyed out every ability the pet has.
                //
                // What the slot holds decides the test (0x005d5400 ->
                // 0x005d3fd0): the type byte, not the low 24 bits, says whether
                // a slot is a spell at all, and a command or a stance is usable
                // as such as long as there is a pet. Taking the low bits instead
                // greyed out Stay and Passive, whose ids are zero - the same
                // mistake GetPetActionInfo made before it learned to ask
                // petSlotSpell.
                //
                // A slot that holds a spell then goes through the client's own
                // usability read, and the leg of that read this client can
                // answer for a pet is what it costs (0x008017e0), the same leg
                // IsUsableSpell uses for a pet caster. The client asks it of
                // every pet it has and only refuses when all of them fail the
                // same way; with the one pet this client tracks, that is the
                // pet's own answer. Usable is 1, otherwise nothing - the client
                // pushes 1.0 or nil, never false.
                // CancelItemTempEnchantment(hand) - drop a weapon imbue
                //
                // The interface counts the hands from one and the request from
                // zero, which is the only thing between them.
                {"CancelItemTempEnchantment", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int hand = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (gh && (hand == 1 || hand == 2)) {
                gh->cancelTempEnchantment(static_cast<uint8_t>(hand - 1));
            }
            return 0;
        }},
                {"GetPetActionSlotUsable", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            if (!gh || index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) {
                lua_pushboolean(L, 0);
                return 1;
            }
            const uint32_t packed = gh->getPetActionSlot(index - 1);
            auto* pet = resolveUnit(L, "pet");
            auto* sh = gh->getSpellHandler();
            bool usable;
            if (packed == 0)              usable = true;   // nothing in the slot to grey
            else if (pet == nullptr)      usable = false;
            else if (const uint32_t spell = petSlotSpell(packed); spell != 0)
                usable = !sh || sh->enoughPower(spell, *pet);
            else                          usable = true;   // a command or a stance
            if (usable) lua_pushnumber(L, 1); else lua_pushnil(L);
            return 1;
        }},
                // GetPetActionInfo(index) →
                //   name, subtext, texture, isToken, isActive,
                //   autoCastAllowed, autoCastEnabled
                //
                // Six of the pet bar's ten slots are not spells at all: three
                // commands (attack, follow, stay) and three stances. This read
                // every slot as a spell, so the low 24 bits - which for those
                // six are 0, 1 or 2 - were looked up as spell ids. The bar's
                // command buttons were drawn with whatever Spell.dbc has at
                // those numbers, or with "Unknown" and a question mark.
                //
                // isToken says which kind a slot is, and PetActionBar_Update
                // reads name and texture as *global names* when it is set:
                //     petActionIcon:SetTexture(_G[texture])
                // Answering a flat false meant even a correct path would have
                // been used the wrong way round.
                {"GetPetActionInfo", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) {
                return luaReturnNil(L);
            }
            const uint32_t packed = gh->getPetActionSlot(index - 1);
            if (packed == 0) { return luaReturnNil(L); }

            const uint32_t action = game::pet::petActionId(packed);
            const auto type = game::pet::petActionType(packed);

            const char* tokenName = nullptr;
            const char* tokenTexture = nullptr;
            bool active = false;

            if (type == game::pet::ActionType::Command) {
                switch (action) {
                    case game::pet::kStay:
                        tokenName = "PET_ACTION_WAIT";    tokenTexture = "PET_WAIT_TEXTURE";   break;
                    case game::pet::kFollow:
                        tokenName = "PET_ACTION_FOLLOW";  tokenTexture = "PET_FOLLOW_TEXTURE"; break;
                    case game::pet::kAttack:
                        tokenName = "PET_ACTION_ATTACK";  tokenTexture = "PET_ATTACK_TEXTURE"; break;
                    case game::pet::kAbandon:
                        tokenName = "PET_DISMISS";        tokenTexture = "PET_DISMISS_TEXTURE"; break;
                    default: break;
                }
            } else if (type == game::pet::ActionType::Reaction) {
                switch (action) {
                    case game::pet::kPassive:
                        tokenName = "PET_MODE_PASSIVE";    tokenTexture = "PET_PASSIVE_TEXTURE";    break;
                    case game::pet::kDefensive:
                        tokenName = "PET_MODE_DEFENSIVE";  tokenTexture = "PET_DEFENSIVE_TEXTURE";  break;
                    case game::pet::kAggressive:
                        tokenName = "PET_MODE_AGGRESSIVE"; tokenTexture = "PET_AGGRESSIVE_TEXTURE"; break;
                    default: break;
                }
                // The stance the pet is in is the one drawn as chosen.
                active = (action == gh->getPetReact());
            }

            if (tokenName) {
                lua_pushstring(L, tokenName);       // 1: name - a global's name
                lua_pushstring(L, "");              // 2: subtext
                lua_pushstring(L, tokenTexture);    // 3: texture - likewise
                lua_pushboolean(L, 1);              // 4: isToken
                lua_pushboolean(L, active ? 1 : 0); // 5: isActive
                // A command or a stance has nothing to cast automatically, and
                // saying otherwise puts the autocast ring on all six of them.
                lua_pushboolean(L, 0);              // 6: autoCastAllowed
                lua_pushboolean(L, 0);              // 7: autoCastEnabled
                return 7;
            }

            const uint32_t spellId = action;
            if (spellId == 0) { return luaReturnNil(L); }
            const std::string& name = gh->getSpellName(spellId);
            const std::string iconPath = gh->getSpellIconPath(spellId);
            lua_pushstring(L, name.empty() ? "Unknown" : name.c_str());
            lua_pushstring(L, "");
            lua_pushstring(L, iconPath.empty() ? "Interface\\Icons\\INV_Misc_QuestionMark"
                                               : iconPath.c_str());
            lua_pushboolean(L, 0);                                      // isToken
            // A pet spell is never "active" - that is the checked ring, which
            // belongs to the stance. Reading the autocast bits as active put
            // it on every spell the pet knows.
            lua_pushboolean(L, 0);                                      // isActive
            lua_pushboolean(L, 1);                                      // autoCastAllowed
            lua_pushboolean(L, gh->isPetSpellAutocast(spellId) ? 1 : 0);
            return 7;
        }},
                // IsPetAttackAction(index) - which slot is the attack command,
                // so the bar can flash it while the pet is on a target.
                {"IsPetAttackAction", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            bool isAttack = false;
            if (gh && index >= 1 && index <= game::GameHandler::PET_ACTION_BAR_SLOTS) {
                const uint32_t packed = gh->getPetActionSlot(index - 1);
                isAttack = game::pet::petActionType(packed) == game::pet::ActionType::Command &&
                           game::pet::petActionId(packed) == game::pet::kAttack;
            }
            lua_pushboolean(L, isAttack ? 1 : 0);
            return 1;
        }},
                // GetPetActionCooldown(index) → start, duration, enable
                //
                // A flat zero, so a pet ability on cooldown was drawn ready and
                // clickable. The client has tracked pet spell cooldowns all
                // along - they arrive on the same packet as the player's.
                //
                // Only the remaining time is kept, not the original duration,
                // so the cooldown is reported as starting now and lasting what
                // is left. That draws exactly the remaining arc, which is the
                // part anyone reads, and it is what the item cooldown beside it
                // does for the same reason.
                {"GetPetActionCooldown", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            const int index = static_cast<int>(luaL_optnumber(L, 1, 0));
            // Enable is one, not zero: zero switches the sweep off entirely,
            // and all three go straight into CooldownFrame_SetTimer.
            if (!gh || index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) {
                lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 1);
                return 3;
            }
            const uint32_t packed = gh->getPetActionSlot(index - 1);
            // A command or a stance has no cooldown of its own; a spell's is
            // the pet's history's (0x005d5280 -> 0x00809000).
            if (!game::pet::isPetSpellAction(packed)) {
                lua_pushnumber(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 1);
                return 3;
            }
            return pushHistoryCooldown(L, gh->petSpellCooldown(game::pet::petActionId(packed)));
        }},
                {"PetAttack", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            // Through CGPetInfo::SendPetAction (0x005d4210), kind 2.
            if (gh && gh->hasPet() && !taint::allowed(L, 2)) return 0;
            if (gh && gh->hasPet() && gh->hasTarget())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Command, game::pet::kAttack),
                    gh->getTargetGuid());
            return 0;
        }},
                // PetStopAttack() (0x005d5640 -> 0x005d4650): only while the
                // pet is held to be attacking (0x00c234dc).
                {"PetStopAttack", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->petStopAttack();
            return 0;
        }},
                {"PetFollow", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            // Through CGPetInfo::SendPetAction (0x005d4210), kind 2.
            if (gh && gh->hasPet() && !taint::allowed(L, 2)) return 0;
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Command, game::pet::kFollow), 0);
            return 0;
        }},
                {"PetWait", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            // Through CGPetInfo::SendPetAction (0x005d4210), kind 2.
            if (gh && gh->hasPet() && !taint::allowed(L, 2)) return 0;
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Command, game::pet::kStay), 0);
            return 0;
        }},
                {"PetPassiveMode", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            // Through CGPetInfo::SendPetAction (0x005d4210), kind 2.
            if (gh && gh->hasPet() && !taint::allowed(L, 2)) return 0;
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Reaction, game::pet::kPassive), 0);
            return 0;
        }},
                // CastPetAction(action, unit) - the unit is what a click-cast
                // binding on a unit frame passes, and dropping it sent the pet
                // at whatever was targeted instead of what was clicked.
                {"CastPetAction", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) return 0;
            castPetSlot(L, gh, gh->getPetActionSlot(index - 1), 2);
            return 0;
        }},
                {"TogglePetAutocast", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            int index = static_cast<int>(luaL_checknumber(L, 1));
            if (!gh || !gh->hasPet() || index < 1 || index > game::GameHandler::PET_ACTION_BAR_SLOTS) return 0;
            uint32_t packed = gh->getPetActionSlot(index - 1);
            uint32_t spellId = packed & 0x00FFFFFF;
            // 0x005d66f0 -> 0x005d5d20, which asks the taint check (kind 2).
            if (spellId != 0 && !taint::allowed(L, 2)) return 0;
            if (spellId != 0) gh->togglePetSpellAutocast(spellId);
            return 0;
        }},
                // CGPetInfo::PetDismiss (0x005d46f0).
                {"PetDismiss", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->dismissPet();
            return 0;
        }},
                // The other end of the ABANDON_PET dialog, which the pet's
                // unit menu opens. It was built, shown, and raised on accept.
                //
                // Not the same as PetDismiss above, which sends the pet away
                // and can be undone by calling it back. Abandon is permanent,
                // which is what the dialog in front of it is for.
                {"PetAbandon", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            if (gh && gh->hasPet()) gh->abandonPet();
            return 0;
        }},
                // CONFIRM_BUY_STABLE_SLOT's accept, from the stable panel.
                {"BuyStableSlot", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->buyStableSlot();
            return 0;
        }},
                // VOTE_BOOT_PLAYER's two buttons. The dialog is shown from
                // LFG_BOOT_PROPOSAL_UPDATE, which is fired, so a vote to kick
                // reached the player and neither answer went anywhere.
                //
                // Both buttons matter here: declining is a vote, not a
                // dismissal, and OnCancel sends it.
                {"SetLFGBootVote", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->lfgSetBootVote(lua_toboolean(L, 1) != 0);
            return 0;
        }},
                // CONFIRM_RESET_INSTANCES' accept, from /resetinstances and
                // the party menu.
                {"ResetInstances", [](lua_State* L) -> int {
            if (auto* gh = getGameHandler(L)) gh->resetInstances();
            return 0;
        }},
                // IsPetAttackActive() (0x005d3160): the attack flag
                // PetStopAttack clears (0x00c234dc).
                {"IsPetAttackActive", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            lua_pushboolean(L, gh && gh->isPetAttacking() ? 1 : 0);
            return 1;
        }},
                {"PetDefensiveMode", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            // Through CGPetInfo::SendPetAction (0x005d4210), kind 2.
            if (gh && gh->hasPet() && !taint::allowed(L, 2)) return 0;
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Reaction, game::pet::kDefensive), 0);
            return 0;
        }},
                // The third stance, which was a no-op in another file entirely
                // - so a hunter could set a pet passive or defensive and not
                // aggressive, and the button for it did nothing.
                {"PetAggressiveMode", [](lua_State* L) -> int {
            auto* gh = getGameHandler(L);
            // Through CGPetInfo::SendPetAction (0x005d4210), kind 2.
            if (gh && gh->hasPet() && !taint::allowed(L, 2)) return 0;
            if (gh && gh->hasPet())
                gh->sendPetAction(
                    game::pet::packPetAction(game::pet::ActionType::Reaction, game::pet::kAggressive), 0);
            return 0;
        }},
    };
    for (const auto& [name, func] : api) {
        lua_pushcfunction(L, func);
        lua_setglobal(L, name);
    }

    // The cursor, for this client's own windows to read. They keep a separate
    // held item of their own, so a drag out of a handed-over bag into a window
    // that is still this client's - the bank, the mailbox, a trade - had no way
    // to know anything had been picked up.
    //
    // Registered here rather than mirrored into the ui layer because this is
    // where the cursor is. Re-registered on every reload, which rebuilds the
    // Lua state, so the captured pointer is the current one; LuaEngine::shutdown
    // takes the bridge back down again.
    ui::frameXmlSetCursorBridge(
        [](uint8_t& bag, uint8_t& slot) { return cursorWireSlot(bag, slot); },
        [L]() { clearCursorItem(L); });
}

} // namespace wowee::addons
