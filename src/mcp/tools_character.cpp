// What the character has, knows and is at: bags, equipment, spells, action
// bars, quests, skills, standing, the map. Asked of the interface's own
// functions, as its windows ask.

#include <string>

#include "mcp/lua_json.hpp"
#include "mcp/schema.hpp"
#include "mcp/tool_context.hpp"

namespace wowee::mcp {

namespace {

const char* const kInventoryChunk = R"lua(
local out = {money = GetMoney(), bags = {}, equipment = {}, repair_cost = nil}
for bag = BAGS_FIRST, BAGS_LAST do
  local slots = GetContainerNumSlots(bag)
  if slots and slots > 0 then
    local free = GetContainerNumFreeSlots(bag)
    local b = {bag = bag, name = GetBagName(bag), slots = slots, free = free, items = {}}
    for slot = 1, slots do
      local texture, count, locked, quality, readable, lootable, link = GetContainerItemInfo(bag, slot)
      if texture then
        link = link or GetContainerItemLink(bag, slot)
        local name = link and string.match(link, "%[(.-)%]")
        local id = link and tonumber(string.match(link, "item:(%d+)"))
        local start, duration = GetContainerItemCooldown(bag, slot)
        b.items[#b.items + 1] = {slot = slot, name = name, id = id, count = count ~= 1 and count or nil,
          quality = quality, locked = locked and true or nil,
          usable = link and IsUsableItem(link) and true or nil,
          cooldown = duration and duration > 0 and start + duration - GetTime() or nil}
      end
    end
    out.bags[#out.bags + 1] = b
  end
end
local slots = {"Head","Neck","Shoulder","Shirt","Chest","Waist","Legs","Feet","Wrist","Hands","Finger0","Finger1",
  "Trinket0","Trinket1","Back","MainHand","SecondaryHand","Ranged","Tabard","Ammo","Bag0","Bag1","Bag2","Bag3"}
for _, s in ipairs(slots) do
  local id = GetInventorySlotInfo(s .. "Slot")
  local link = id and GetInventoryItemLink("player", id)
  if link then
    local cur, max = GetInventoryItemDurability(id)
    out.equipment[#out.equipment + 1] = {slot = s, name = string.match(link, "%[(.-)%]"),
      id = tonumber(string.match(link, "item:(%d+)")), durability = max and {cur, max} or nil}
  end
end
return out
)lua";

const char* const kSpellbookChunk = R"lua(
local out = {}
for tab = 1, GetNumSpellTabs() do
  local name, _, offset, count = GetSpellTabInfo(tab)
  local t = {tab = name, spells = {}}
  for i = offset + 1, offset + count do
    local spell, rank = GetSpellName(i, BOOKTYPE_SPELL or "spell")
    if spell and (FILTER == "" or string.find(string.lower(spell), FILTER, 1, true)) then
      local start, duration = GetSpellCooldown(i, BOOKTYPE_SPELL or "spell")
      local link = GetSpellLink(i, BOOKTYPE_SPELL or "spell")
      local usable, nomana = IsUsableSpell(i, BOOKTYPE_SPELL or "spell")
      t.spells[#t.spells + 1] = {name = spell, rank = rank ~= "" and rank or nil,
        id = link and tonumber(string.match(link, "spell:(%d+)")),
        passive = IsPassiveSpell(i, BOOKTYPE_SPELL or "spell") and true or nil,
        usable = usable and true or false, no_mana = nomana and true or nil,
        cooldown = duration and duration > 1.5 and start + duration - GetTime() or nil}
    end
  end
  if #t.spells > 0 then out[#out + 1] = t end
end
return out
)lua";

const char* const kActionsChunk = R"lua(
local out = {page = GetActionBarPage(), bonus_offset = GetBonusBarOffset(), bars = {}}
local function slotInfo(slot)
  if not HasAction(slot) then return nil end
  local kind, id, _, spell = GetActionInfo(slot)
  local usable, nomana = IsUsableAction(slot)
  local start, duration = GetActionCooldown(slot)
  local a = {slot = slot, kind = kind, id = spell or id, text = GetActionText(slot), usable = usable and true or false,
    no_mana = nomana and true or nil, in_range = IsActionInRange(slot),
    current = IsCurrentAction(slot) and true or nil, attack = IsAttackAction(slot) and true or nil,
    count = GetActionCount(slot) > 0 and GetActionCount(slot) or nil,
    cooldown = duration and duration > 1.5 and start + duration - GetTime() or nil}
  if spell then a.name = GetSpellInfo(spell) elseif kind == "item" then a.name = GetItemInfo(id) end
  return a
end
local function buttons(name, count, command)
  local b = {buttons = name, slots = {}}
  for i = 1, count do
    local button = getglobal(name .. i)
    local slot = button and (button.action or (ActionButton_GetPagedID and ActionButton_GetPagedID(button)))
    local info = slot and slotInfo(slot)
    if info then
      info.button = name .. i
      info.shown = button:IsVisible() and true or nil
      info.key = GetBindingKey(command .. i)
      b.slots[#b.slots + 1] = info
    end
  end
  if #b.slots > 0 then out.bars[#out.bars + 1] = b end
end
buttons("ActionButton", 12, "ACTIONBUTTON")
buttons("BonusActionButton", 12, "ACTIONBUTTON")
buttons("MultiBarBottomLeftButton", 12, "MULTIACTIONBAR1BUTTON")
buttons("MultiBarBottomRightButton", 12, "MULTIACTIONBAR2BUTTON")
buttons("MultiBarRightButton", 12, "MULTIACTIONBAR3BUTTON")
buttons("MultiBarLeftButton", 12, "MULTIACTIONBAR4BUTTON")
local forms = {}
for i = 1, GetNumShapeshiftForms() do
  local _, name, active, castable = GetShapeshiftFormInfo(i)
  forms[#forms + 1] = {index = i, name = name, active = active and true or nil, castable = castable and true or nil}
end
if #forms > 0 then out.forms = forms end
if HasPetUI() then
  out.pet = {}
  for i = 1, 10 do
    local name, sub, _, token, active = GetPetActionInfo(i)
    if name then out.pet[#out.pet + 1] = {slot = i, name = token and getglobal(name) or name, active = active and true or nil} end
  end
end
return out
)lua";

const char* const kQuestChunk = R"lua(
local out = {quests = {}}
local entries, quests = GetNumQuestLogEntries()
out.count = quests
local header
for i = 1, entries do
  local title, level, tag, group, isHeader, collapsed, complete, daily, id = GetQuestLogTitle(i)
  if isHeader then
    header = title
  else
    local q = {index = i, title = title, level = level, zone = header, id = id, tag = tag,
      complete = complete == 1 and true or nil, failed = complete == -1 and true or nil, daily = daily and true or nil,
      watched = IsQuestWatched(i) and true or nil, objectives = {}}
    for o = 1, GetNumQuestLeaderBoards(i) do
      local text, kind, done = GetQuestLogLeaderBoard(o, i)
      q.objectives[#q.objectives + 1] = {text = text, kind = kind, done = done and true or nil}
    end
    out.quests[#out.quests + 1] = q
  end
end
return out
)lua";

const char* const kCharacterChunk = R"lua(
local out = {}
out.skills = {}
local header
for i = 1, GetNumSkillLines() do
  local name, isHeader, _, rank, temp, modifier, max = GetSkillLineInfo(i)
  if isHeader then header = name else
    out.skills[#out.skills + 1] = {name = name, group = header, rank = {rank, max}}
  end
end
out.reputation = {}
for i = 1, GetNumFactions() do
  local name, _, standing, bottom, top, earned, atWar, _, isHeader = GetFactionInfo(i)
  if name and not isHeader then
    out.reputation[#out.reputation + 1] = {name = name, standing = standing, value = {earned - bottom, top - bottom},
      at_war = atWar and true or nil}
  end
end
out.stats = {}
for i = 1, 5 do
  local base, effective = UnitStat("player", i)
  out.stats[#out.stats + 1] = effective
end
local _, armor = UnitArmor("player")
out.armor = armor
out.talents = {}
for tab = 1, GetNumTalentTabs() do
  local name, _, points = GetTalentTabInfo(tab)
  out.talents[#out.talents + 1] = {tab = name, points = points}
end
out.unspent_talents = UnitCharacterPoints("player")
out.bind = GetBindLocation()
out.guild = GetGuildInfo("player")
out.title = GetCurrentTitle and GetTitleName and GetCurrentTitle() > 0 and GetTitleName(GetCurrentTitle()) or nil
out.companions = {}
for _, kind in ipairs({"MOUNT", "CRITTER"}) do
  for i = 1, GetNumCompanions(kind) do
    local _, name, spell, _, summoned = GetCompanionInfo(kind, i)
    out.companions[#out.companions + 1] = {kind = kind, index = i, name = name, spell = spell, out = summoned and true or nil}
  end
end
return out
)lua";

const char* const kMapChunk = R"lua(
local out = {zone = GetZoneText(), subzone = GetSubZoneText(), real_zone = GetRealZoneText(),
  minimap = GetMinimapZoneText(), pvp = GetZonePVPInfo()}
out.in_instance, out.instance_type = IsInInstance()
local shown = WorldMapFrame and WorldMapFrame:IsShown()
out.world_map_open = shown and true or false
if not shown then SetMapToCurrentZone() end
out.map = GetMapInfo()
out.continent, out.zone_index, out.level = GetCurrentMapContinent(), GetCurrentMapZone(), GetCurrentMapDungeonLevel()
local x, y = GetPlayerMapPosition("player")
out.player = {x * 100, y * 100}
local cx, cy = GetCorpseMapPosition()
if cx and (cx ~= 0 or cy ~= 0) then out.corpse = {cx * 100, cy * 100} end
out.landmarks = {}
for i = 1, GetNumMapLandmarks() do
  local name, text, icon, lx, ly = GetMapLandmarkInfo(i)
  out.landmarks[#out.landmarks + 1] = {name = name, text = text ~= "" and text or nil, at = {lx * 100, ly * 100}}
end
out.tracking = {}
for i = 1, GetNumTrackingTypes() do
  local name, _, active = GetTrackingInfo(i)
  if active then out.tracking[#out.tracking + 1] = name end
end
return out
)lua";

std::string replaced(std::string text, const std::string& from, const std::string& to) {
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

json inventory(ToolContext& context, const json& args) {
    const bool bank = args.value("bank", false);
    std::string chunk = replaced(kInventoryChunk, "BAGS_FIRST", bank ? "-1" : "0");
    chunk = replaced(chunk, "BAGS_LAST", bank ? "11" : "4");
    json out = queryLua(context, chunk);
    if (args.contains("name") && out.contains("bags")) {
        // Kept small: only what was asked for.
        for (auto& bag : out["bags"]) {
            json kept = json::array();
            for (auto& item : bag["items"]) {
                const std::string name = item.contains("name") && item["name"].is_string()
                                             ? item["name"].get<std::string>()
                                             : std::string();
                if (name.find(args["name"].get<std::string>()) != std::string::npos) kept.push_back(item);
            }
            bag["items"] = std::move(kept);
        }
    }
    return out;
}

json spellbook(ToolContext& context, const json& args) {
    return queryLua(context, "local FILTER = string.lower(" + luaQuote(args.value("filter", std::string())) + ")\n" +
                                 kSpellbookChunk);
}

}  // namespace

void addCharacterTools(Toolbox& box) {
    using namespace schema;
    box.push_back(lookTool("inventory", "Bags, worn equipment and money.",
                           object({optional("name", text("only items with this in their name", 64)),
                                   optional("bank", flag("the bank's bags as well, while it is open"))}),
                           inventory));
    box.push_back(lookTool("spellbook", "Known spells by tab: usable, cooldown.",
                           object({optional("filter", text("part of a name", 64))}), spellbook));
    box.push_back(lookTool(
        "action_bars", "What is on the action bars: spell or item, usable, in range, cooldown, the button and its key.",
        object({}), [](ToolContext& c, const json&) { return queryLua(c, kActionsChunk); }));
    box.push_back(lookTool("quest_log", "The quest log with objectives.", object({}),
                           [](ToolContext& c, const json&) { return queryLua(c, kQuestChunk); }));
    box.push_back(lookTool("character",
                           "Skills, reputation, stats, talents, mounts and companions, the inn.", object({}),
                           [](ToolContext& c, const json&) { return queryLua(c, kCharacterChunk); }));
    box.push_back(lookTool("map", "Zone texts, the map and the player's place on it, landmarks, the corpse, tracking.",
                           object({}), [](ToolContext& c, const json&) { return queryLua(c, kMapChunk); }));
}

}  // namespace wowee::mcp
