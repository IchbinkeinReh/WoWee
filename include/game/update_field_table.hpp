#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace wowee {
namespace game {

/**
 * Logical update field identifiers (expansion-agnostic).
 * Wire indices are loaded at runtime from JSON.
 */
enum class UF : uint16_t {
    // Object fields
    OBJECT_FIELD_ENTRY,
    OBJECT_FIELD_SCALE_X,

    // Unit fields
    UNIT_FIELD_TARGET_LO,
    UNIT_FIELD_TARGET_HI,
    UNIT_FIELD_CHANNEL_OBJECT,  // the low word of the guid the unit channels at (0x0073a520)
    UNIT_CHANNEL_SPELL,
    UNIT_FIELD_BYTES_0,
    UNIT_FIELD_BYTES_1,  // byte2 = visibility flags; byte3 is expansion-dependent
    UNIT_FIELD_HEALTH,
    UNIT_FIELD_POWER1,
    UNIT_FIELD_MAXHEALTH,
    UNIT_FIELD_MAXPOWER1,
    UNIT_FIELD_LEVEL,
    UNIT_FIELD_FACTIONTEMPLATE,
    UNIT_FIELD_FLAGS,
    UNIT_FIELD_FLAGS_2,
    UNIT_FIELD_AURASTATE,      // Reactive combat opportunities (e.g. dodge/block/parry)
    /// How wide the unit is, and how far it can reach past that. Range is
    /// measured between the two units' edges rather than their centres, so a
    /// large creature is in reach from further away - which is the whole of
    /// why a charge at one read as out of range. Both are floats.
    UNIT_FIELD_BOUNDINGRADIUS,
    UNIT_FIELD_COMBATREACH,
    UNIT_FIELD_DISPLAYID,
    UNIT_FIELD_MOUNTDISPLAYID,
    UNIT_FIELD_AURAS,           // Start of aura spell ID array (48 consecutive uint32 slots, pre-WotLK clients)
    UNIT_FIELD_AURAFLAGS,       // Aura flags packed 4-per-uint32 (12 uint32 slots); 0x01=cancelable,0x02=harmful,0x04=helpful
    UNIT_NPC_FLAGS,
    UNIT_NPC_EMOTESTATE,       // Persistent NPC emote animation ID (uint32)
    UNIT_DYNAMIC_FLAGS,
    UNIT_FIELD_PETEXPERIENCE,   // A hunter pet's experience toward its next level
    UNIT_FIELD_PETNEXTLEVELEXP, // ...and what that level costs
    UNIT_FIELD_PETNUMBER,       // Non-zero on a hunter pet; such a pet always takes its family's size
    UNIT_FIELD_RESISTANCES,   // Physical armor (index 0 of the resistance array)
    UNIT_FIELD_STAT0,         // Strength (effective base, includes items)
    UNIT_FIELD_STAT1,         // Agility
    UNIT_FIELD_STAT2,         // Stamina
    UNIT_FIELD_STAT3,         // Intellect
    UNIT_FIELD_STAT4,         // Spirit
    UNIT_END,

    // Unit combat fields (WotLK: PRIVATE+OWNER - only visible for the player character)
    UNIT_FIELD_MINDAMAGE,            // Weapon damage low end (float bits)
    UNIT_FIELD_MAXDAMAGE,            // ...and high end
    UNIT_FIELD_ATTACK_POWER,         // Melee attack power (int32)
    UNIT_FIELD_RANGED_ATTACK_POWER,  // Ranged attack power (int32)
    UNIT_FIELD_POWER_REGEN_FLAT_MODIFIER,             // Power regen while not casting (float[7], mana=0); PRIVATE/OWNER
    UNIT_FIELD_POWER_REGEN_INTERRUPTED_FLAT_MODIFIER, // ...and the reduced rate during the five-second rule

    // Player fields
    PLAYER_FLAGS,
    PLAYER_BYTES,
    PLAYER_BYTES_2,
    PLAYER_XP,
    PLAYER_NEXT_LEVEL_XP,
    PLAYER_REST_STATE_EXPERIENCE,
    PLAYER_FIELD_COINAGE,
    PLAYER_QUEST_LOG_START,
    PLAYER_FIELD_INV_SLOT_HEAD,
    PLAYER_FIELD_PACK_SLOT_1,
    PLAYER_FIELD_KEYRING_SLOT_1,
    PLAYER_FIELD_BANK_SLOT_1,
    PLAYER_FIELD_BANKBAG_SLOT_1,
    PLAYER_SKILL_INFO_START,
    PLAYER_EXPLORED_ZONES_START,
    PLAYER_CHOSEN_TITLE,         // Active title index (-1 = no title)
    // How drunk: PLAYER_BYTES_3's byte 1 (0..100), and the fake inebriation
    // an item can add (0x004f7290 reads +0x1d and +0x2b8 of the player's
    // fields). WotLK only here.
    PLAYER_BYTES_3,
    PLAYER_FAKE_INEBRIATION,
    // Byte 3, 0x40: the invisibility glow, which puts up ScreenEffect 0x51
    // (0x004f88b0 reads +0x10e7). WotLK only here.
    PLAYER_FIELD_BYTES2,

    // The honour counters the PvP and honour panels print.  All four are read
    // straight out of the fields, unsigned: 0x00611a20 (GetPVPSessionStats)
    // takes the low halfword of PLAYER_FIELD_KILLS and
    // PLAYER_FIELD_TODAY_CONTRIBUTION, 0x00611ad0 (GetPVPYesterdayStats) the
    // high halfword of the same dword and PLAYER_FIELD_YESTERDAY_CONTRIBUTION,
    // and 0x00611b80 (GetPVPLifetimeStats) PLAYER_FIELD_LIFETIME_HONORBALE_
    // KILLS - the client's own spelling of it, missing an O like its own header
    // does.  The lifetime dishonourable count is byte 3 of PLAYER_FIELD_BYTES
    // (+0x1067), and reads as zero below 5.  WotLK only here.
    PLAYER_FIELD_KILLS,
    PLAYER_FIELD_TODAY_CONTRIBUTION,
    PLAYER_FIELD_YESTERDAY_CONTRIBUTION,
    PLAYER_FIELD_LIFETIME_HONORBALE_KILLS,

    // Which faction the realm is showing under the experience bar, as a
    // reputation list index rather than a faction id.  GetWatchedFactionInfo
    // (0x005d1240) reads +0x10e8 into the field blob the player object keeps at
    // +0x1008 - blob dword 1082, which the client's own per-field descriptor
    // table names PLAYER_FIELD_WATCHED_FACTION_INDEX - rejects it below 0 or
    // above 0x7f, and looks up the reputation list entry at that index to get
    // the faction, its standing and the rank bounds.  The same index goes back
    // out in CMSG_SET_WATCHED_FACTION (0x005d0ba0 converts a faction id to it and
    // sends -1 for an untracked one, and only sends when the field would change),
    // so the choice belongs to the realm: it survives a relog and follows the
    // character to another client.  WotLK only here.
    PLAYER_FIELD_WATCHED_FACTION_INDEX,

    // Player spell power / healing bonus (WotLK: PRIVATE - int32 per school)
    PLAYER_FIELD_MOD_DAMAGE_DONE_POS,  // Spell damage bonus (first of 7 schools)
    // The other half of the same seven-school run - a damage *reduction*, sent
    // negative.  0x0060e310 (GetSpellBonusDamage) answers one school as
    // PLAYER_FIELD_MOD_DAMAGE_DONE_POS[school] + this one[school], the two
    // accessors being +0xffc and +0x1018 into the field blob the player object
    // keeps at +0x1008 (0x00578210, 0x00578250), and pushes that sum without
    // clamping, so a curse shows as a negative spell damage.  The descriptor
    // table names both runs seven dwords wide, POS at blob 1023 and NEG at 1030,
    // with MOD_DAMAGE_DONE_PCT (float) after that at 1037.  WotLK only here.
    PLAYER_FIELD_MOD_DAMAGE_DONE_NEG,
    // The third seven-school run, a float: UnitDamage (0x00610860) pushes it as
    // its seventh answer and the character sheet **divides** the damage bounds
    // by it (paperdollframe.lua:544-545), so it is a multiplier rather than a
    // percentage.  Read with the pair above through 0x00578290 (+0x1034 into the
    // field blob the player object keeps at +0x1008 - blob dword 1037), always at
    // index 0: the call sites push 0, visible in the code bytes as `6a 00` before
    // each of the three helper calls, which is the argument Ghidra drops.  WotLK
    // only here.
    PLAYER_FIELD_MOD_DAMAGE_DONE_PCT,
    PLAYER_FIELD_MOD_HEALING_DONE_POS, // Healing bonus
    // What the player's gear takes off the target's resistances, sent already
    // summed: 0x0060e470 reads +0x105c into the field blob the player object
    // keeps at +0x1008 - dword 1047 of it, PLAYER_FIELD_MOD_TARGET_RESISTANCE
    // in the client's own per-field descriptor table - and pushes its negation,
    // which is what GetSpellPenetration answers with and what the character
    // sheet prints as SPELL_PENETRATION (paperdollframe.lua:988).  Negative in
    // the field, positive out of the binding.  WotLK only here.
    PLAYER_FIELD_MOD_TARGET_RESISTANCE,

    // Player combat stats (WotLK: PRIVATE - float values)
    PLAYER_BLOCK_PERCENTAGE,         // Block chance %
    PLAYER_DODGE_PERCENTAGE,         // Dodge chance %
    PLAYER_PARRY_PERCENTAGE,         // Parry chance %
    PLAYER_CRIT_PERCENTAGE,          // Melee crit chance %
    PLAYER_RANGED_CRIT_PERCENTAGE,   // Ranged crit chance %
    PLAYER_SPELL_CRIT_PERCENTAGE1,   // Spell crit chance % (first school; 7 consecutive float fields)
    // How much a block takes off, which GetShieldBlock() answers with straight
    // out of the field: 0x0060dff0 reads +0xdec into the field blob the player
    // object keeps at +0x1008 - dword 891 of it, the dword the client's own
    // per-field descriptor table names PLAYER_SHIELD_BLOCK - and hands the
    // character sheet's block tooltip that number as the damage blocked.  An
    // int, not one of the percent floats either side of it, and the client
    // reads it unsigned: it adds 2^32 when the int comes out negative.  WotLK
    // only here.
    PLAYER_SHIELD_BLOCK,
    PLAYER_FIELD_COMBAT_RATING_1,    // First of 25 int32 combat rating slots (CR_* indices)
    // What the player's tracking finds, as bit masks: creature types from
    // 1, and LockType ids from 1 (herbs, minerals, treasure, fish). The
    // minimap blips what they match (0x006dca00, 0x006dca90 read them at
    // 0xda8 and 0xdac past UNIT_END, which is 1022 and 1023). WotLK only.
    // The skill points the old rank purchase spends (0xda4 past UNIT_END,
    // 1021; 0x005cd970(1), 0x005ce540). WotLK only.
    PLAYER_CHARACTER_POINTS2,
    PLAYER_TRACK_CREATURES,
    PLAYER_TRACK_RESOURCES,
    // Expertise, in expertise *points* rather than percent - the character
    // sheet prints the points and works the percent out at a quarter of one
    // each. Ints, not floats, unlike the percentages above. WotLK only here:
    // the indices were read off AzerothCore's UpdateFields.h and checked by
    // reproducing PLAYER_BLOCK_PERCENTAGE's known 1024 from the same UNIT_END.
    PLAYER_EXPERTISE,
    PLAYER_OFFHAND_EXPERTISE,

    // Player PvP currency (TBC/WotLK only - Classic uses the old weekly honor system)
    PLAYER_FIELD_HONOR_CURRENCY,     // Accumulated honor points (uint32)
    PLAYER_FIELD_ARENA_CURRENCY,     // Accumulated arena points (uint32)
    // Which currencies the realm says this character has met. WotLK only: no
    // earlier layout this client's tables were read from names it, so on those
    // layouts fieldIndex answers 0xFFFF and the token tab falls back to
    // counting the bags. One field the client reads as
    // two dwords (blob +0x790 and +0x794, wire 632 and 633); known_currencies.hpp
    // says which bit of which half a currency id uses, and 0x005b0030 is the
    // client's own token-list builder reading both.
    PLAYER_FIELD_KNOWN_CURRENCIES,
    // The titles the realm says this character has earned, six dwords holding
    // three 64-bit groups (wire 626..631, blob +0x778..+0x78f - the run ends
    // exactly where the currency pair above begins). The client's own field-name
    // table names the run three times, two dwords each
    // (PLAYER__FIELD_KNOWN_TITLES, _TITLES1, _TITLES2, with its own doubled
    // underscore), and it installs one eight-byte watcher per pair, all three of
    // which raise KNOWN_TITLES_UPDATE (498) with no arguments (0x006e5180). This
    // layout names the run once, at its first dword; known_titles.hpp says which
    // group and which bit a title id addresses, and IsTitleKnown (0x0051b410) is
    // the client's reader.
    PLAYER_FIELD_KNOWN_TITLES,
    // The three arena teams, seven fields each with the team id first
    // (0x1150 past UNIT_END's player block, 0x005bc240). WotLK only.
    PLAYER_FIELD_ARENA_TEAM_INFO_1_1,

    // The player's active non-combat companion, as a 64-bit guid (low word at
    // this index, high word at the next). WotLK only - no earlier expansion
    // published it, and none has CMSG_DISMISS_CRITTER to act on it either.
    UNIT_FIELD_CRITTER,
    // Who this unit is controlling, and who is controlling it. Both are guid
    // pairs. WotLK only for now: the indices below were read off AzerothCore's
    // UpdateFields.h and checked against UNIT_FIELD_CRITTER, which that header
    // puts at OBJECT_END + 4 and this client's table already has at 10 - so
    // OBJECT_END is 6 and the table holds absolute indices. No such check was
    // possible for the other three expansions, and an update field index
    // guessed is an arbitrary field read.
    UNIT_FIELD_CHARM,
    /// The pet it has summoned, the guid pair after UNIT_FIELD_CHARM (WotLK
    /// 8): where 0x0052d310 looks for a pet when nothing is charmed.
    UNIT_FIELD_SUMMON,
    UNIT_FIELD_CHARMEDBY,
    /// Who summoned this unit, if anyone. Two fields, low half first, like
    /// every other guid here. A pet, a guardian and a totem all carry it; what
    /// separates them is UNIT_FLAG_PLAYER_CONTROLLED and the creature type.
    UNIT_FIELD_SUMMONEDBY_LO,
    UNIT_FIELD_SUMMONEDBY_HI,
    /// Who made this unit (low half; the high half is the next index). Two
    /// below the target field in every layout. With CHARMEDBY, whose owner
    /// the client asks about (0x004f5f20), and a unit made but neither
    /// charmed nor summoned is a guardian to it (0x0071b600).
    UNIT_FIELD_CREATEDBY,
    /// The spell that made the unit (0x0061e830 reads it for the line under
    /// a pet's or minion's name). WotLK: 0x51.
    UNIT_CREATED_BY_SPELL,

    // GameObject fields
    /// Who made the object (low half; the high half is the next index):
    /// the controller the combat log names for a trap or a totem's object
    /// (0x0074dcb0 reads it as +0 of the object's own fields). WotLK: 6.
    GAMEOBJECT_FIELD_CREATED_BY,
    GAMEOBJECT_DISPLAYID,
    // UNIT_FIELD_BYTES_2: byte 0 is the sheath state in every layout (164 in
    // Classic, 208 in TBC). In WotLK byte 1 is the unit's PvP flags (1 PvP, 4
    // free-for-all, 8 sanctuary; game::unitPvpFlags), and PLAYER_DUEL_ARBITER, a guid, is the duel flag
    // both duellists name - what the client reads to tell whether two players
    // may attack each other.
    UNIT_FIELD_BYTES_2,
    /// WotLK: three item entries, main hand, off hand, ranged - what a
    /// creature holds (0x0072dbc0).
    UNIT_VIRTUAL_ITEM_SLOT_ID,
    /// Classic and TBC: three display ids, then six UNIT_VIRTUAL_ITEM_INFO
    /// fields, two a slot (between BYTES_0 and FLAGS in their tables).
    UNIT_VIRTUAL_ITEM_SLOT_DISPLAY,
    UNIT_VIRTUAL_ITEM_INFO,
    /// WotLK: a corpse's flags; 1 is bones (0x00743760 gives those no blob).
    CORPSE_FIELD_FLAGS,
    /// WotLK: what a corpse looks like (0x00705670, 0x00705b20) - its
    /// display id, its 19 item display ids (inventory type in the top
    /// byte), race, sex and skin in BYTES_1 bytes 1-3, face, hair style,
    /// hair colour and facial hair in BYTES_2.
    CORPSE_FIELD_DISPLAY_ID,
    CORPSE_FIELD_ITEM,
    CORPSE_FIELD_BYTES_1,
    CORPSE_FIELD_BYTES_2,
    /// WotLK: the guild whose emblem a corpse's guild tabard carries
    /// (0x007059a0).
    CORPSE_FIELD_GUILD,
    /// WotLK: a corpse's dynamic flags; 1 is lootable, which draws the loot
    /// sparkle (0x00705900).
    CORPSE_FIELD_DYNAMIC_FLAGS,
    PLAYER_DUEL_ARBITER,
    /// WotLK: the side of the duel a player is on (+0x1008 +0x20), which
    /// with the arbiter decides who may help and attack whom (0x007293d0,
    /// 0x00729740).
    PLAYER_DUEL_TEAM,
    /// WotLK: a unit's casting speed as a factor, which shortens a pet's
    /// global cooldown (+0x128, 0x00805d70).
    UNIT_MOD_CAST_SPEED,
    /// WotLK: what 0x00739650 and the power cost (0x008012f0) read - the
    /// base mana and health (+0x1c8, +0x1cc), the cost modifier and
    /// multiplier per school (+500, +0x210: UNIT_END-relative 131 and 138),
    /// and PLAYER_FIELD_BYTES (+0x1008 +0x1064).
    UNIT_FIELD_BASE_MANA,
    UNIT_FIELD_BASE_HEALTH,
    UNIT_FIELD_POWER_COST_MODIFIER,
    UNIT_FIELD_POWER_COST_MULTIPLIER,
    PLAYER_FIELD_BYTES,
    /// WotLK: the ranged attack time (+0xe8), which a ranged spell's
    /// category cooldown runs longer by (0x007fef60).
    UNIT_FIELD_RANGEDATTACKTIME,
    /// WotLK: the runes' regen rate by type, four floats (+0x1008 +0x1214).
    PLAYER_RUNE_REGEN_1,
    /// WotLK: the unit the player looks through - a vehicle it drives, a
    /// possessed unit (+0x1008 +0x770, 0x004f7310).
    PLAYER_FARSIGHT,
    /// WotLK: the ammunition the player has put in (+0x1008 +0x1068,
    /// 0x005e8300) and the class spells it needs no reagents for, three
    /// mask words (+0x1008 +0x1224, 0x00800d60).
    PLAYER_AMMO_ID,
    /// WotLK: the spell the realm means to resurrect this client with while a
    /// soulstone is on it (+0x1008 +0x106c, blob dword 1051).  Nothing else
    /// announces a soulstone: the death popup asks this field directly
    /// (0x0051acd0), and its own second button comes and goes with it.
    PLAYER_SELF_RES_SPELL,
    PLAYER_NO_REAGENT_COST_1,
    /// The six glyph sockets' GlyphSlot.dbc ids (+0x1008 +0x1230), the
    /// GlyphProperties ids in them (+0x1248), and the bitmask of the sockets
    /// unlocked (+0x1260) - what PlaceGlyphInSocket checks (0x0080c3e0).
    PLAYER_FIELD_GLYPH_SLOTS_1,
    PLAYER_FIELD_GLYPHS_1,
    PLAYER_GLYPHS_ENABLED,
    /// WotLK: the spell power the pet is given, the dword straight after the
    /// unlocked glyph mask (+0x1264). GetPetSpellBonusDamage reads exactly this
    /// (0x0060e410), and the watcher on it (0x006d1cb0) is what the pet sheet's
    /// Spell Bonus Damage line waits for.
    PLAYER_FIELD_PET_SPELL_POWER,
    GAMEOBJECT_FLAGS,
    GAMEOBJECT_BYTES_1,
    // MO_TRANSPORT route clock. LEVEL carries the route's period in ms; the high
    // int16 of DYNAMIC carries how far through that period the transport is, as a
    // fraction of 65535. Both are WotLK-only - nothing before it published a
    // transport's phase, so those expansions keep animating on their own clock.
    GAMEOBJECT_DYNAMIC,
    GAMEOBJECT_LEVEL,
    /// The object's faction template, how it regards the player (0x0070edd0).
    GAMEOBJECT_FACTION,
    /// WotLK: four floats, x y z w (+0xd0 +0x10), what a transport turning
    /// by TransportRotation.dbc is turned by besides (0x0070dc10).
    GAMEOBJECT_PARENTROTATION,
    /// WotLK: a dynamic object's fields past OBJECT_END (+0xd0): BYTES' low
    /// byte its type (+8), its spell (+0xc) and radius (+0x10), which pick
    /// and size its model (0x00705100, 0x00704d90); its CASTER (+0, a
    /// guid) and CASTTIME (+0x14), which with its spell name the cast whose
    /// missile may still fly (0x00804cc0).
    DYNAMICOBJECT_CASTER,
    DYNAMICOBJECT_BYTES,
    DYNAMICOBJECT_SPELLID,
    DYNAMICOBJECT_RADIUS,
    DYNAMICOBJECT_CASTTIME,

    // Item fields
    ITEM_FIELD_STACK_COUNT,
    ITEM_FIELD_DURABILITY,
    ITEM_FIELD_MAXDURABILITY,

    // Container fields
    CONTAINER_FIELD_NUM_SLOTS,
    CONTAINER_FIELD_SLOT_1,

    /// WotLK: the unit's own attack times, main hand then off hand. The
    /// client's UnitAttackSpeed (0x00610a00) reads them at +0xe0 and +0xe4 of
    /// the field blob, both a millisecond count it divides by 1000, and the
    /// ranged one at +0xe8 - which is UNIT_FIELD_RANGEDATTACKTIME, already in
    /// this table at 64. So the pair is 62 and 63: the blob runs six dwords
    /// behind the wire index across the unit fields (UnitLevel reads +0xc0 for
    /// LEVEL's 54, BYTES_1's 74 sits at +0x110). A druid's cat form and every
    /// pet answer their swing time from these; a weapon's delay is only what
    /// the realm wrote the field from. WotLK only - the other three layouts
    /// number their fields by their own client's table, and none of those
    /// places this pair. The client's own field-name table names the two
    /// together, as one UNIT_FIELD_BASEATTACKTIME entry two fields wide, which
    /// is why 63 needs a name of its own here for anything to ask for it.
    UNIT_FIELD_BASEATTACKTIME,
    UNIT_FIELD_OFFHANDATTACKTIME,

    /// WotLK: the rest of the damage family, under the names the client's own
    /// field-name table gives them. UnitDamage (0x00610860) pushes four of them
    /// straight out of the field blob - +0x100 and +0x104, which are
    /// MINDAMAGE's 70 and MAXDAMAGE's 71, then +0x108 and +0x10c, the off-hand
    /// pair at 72 and 73 - each stored as float bits and added to by nothing,
    /// because the realm has already put the weapon's damage and the unit's
    /// attack power into them. UnitRangedDamage (0x00610550) reads the ranged
    /// pair below beside the ranged attack time at +0xe8, which is
    /// UNIT_FIELD_RANGEDATTACKTIME: +0x1ec and +0x1f0, which the drift puts at
    /// 129 and 130. Both pairs sit where the client's table says they do - the
    /// off-hand pair directly below BYTES_1's 74, the ranged pair directly
    /// below POWER_COST_MODIFIER's 131 - and a field this table already has
    /// bounds each of them on the other side (MAXDAMAGE 71, RANGED_ATTACK_POWER
    /// 126), so neither rests on the drift alone.
    UNIT_FIELD_MINOFFHANDDAMAGE,
    UNIT_FIELD_MAXOFFHANDDAMAGE,
    UNIT_FIELD_MINRANGEDDAMAGE,
    UNIT_FIELD_MAXRANGEDDAMAGE,

    /// WotLK: the two attack-power mod fields beside each base. UnitAttackPower
    /// (0x00610b60) reads three dwords of the blob - +0x1d4 the base (WoWee's
    /// ATTACK_POWER, 123), +0x1d8 the mods, +0x1dc a float multiplier - and
    /// multiplies every answer it pushes by (multiplier + 1). The mods dword is
    /// two signed halves: +0x1d8 low for the positive buff, +0x1da high for the
    /// negative one, which is why one field carries both. UnitRangedAttackPower
    /// (0x00610ca0) does the same with +0x1e0/+0x1e4/+0x1e8, bases 126, mods
    /// 127, multiplier 128. Each pair is wedged between two fields this table
    /// already has - 123 and 126 on one side, 126 and MINRANGEDDAMAGE's 129 on
    /// the other - and the client's field-name table lists them in that order
    /// (ATTACK_POWER_MODS at 0x009f8b74, …MULTIPLIER at 0x009f8b50).
    UNIT_FIELD_ATTACK_POWER_MODS,
    UNIT_FIELD_ATTACK_POWER_MULTIPLIER,
    UNIT_FIELD_RANGED_ATTACK_POWER_MODS,
    UNIT_FIELD_RANGED_ATTACK_POWER_MULTIPLIER,

    /// WotLK: the buff halves of the five primary stats. UnitStat (0x00610300)
    /// answers four values, three of them straight out of the field blob and all
    /// four gated on the unit being player-controlled: STAT0..4 (84-88, blob
    /// +0x138) as it stands, that same dword clamped at zero (0x005774b0, the
    /// `uVar & ((int)uVar < 0) - 1` trick) as the figure the sheet prints, then
    /// POSSTAT0..4 (89-93, blob +0x14c) and NEGSTAT0..4 (94-98, blob +0x160).
    /// So STAT is the *total*, not the base: paperdollframe.lua:246 gets the
    /// unbuffed number by subtracting both halves back out of it, and colours
    /// the line red when a negative half is in play.
    ///
    /// The client names all fifteen of these itself. Its field-name table holds
    /// UNIT_FIELD_NEGSTAT4 at 0x009f8c58 stepping up through NEGSTAT0,
    /// POSSTAT4..0, STAT4..0 to UNIT_FIELD_STAT0 at 0x009f8d70 - one name per
    /// element, in descending index order, with WoWee's own RESISTANCES 99
    /// directly above and STAT0 84 directly below. The blob reads agree
    /// dword for dword with that count, drift rule included.
    UNIT_FIELD_POSSTAT0,
    UNIT_FIELD_POSSTAT1,
    UNIT_FIELD_POSSTAT2,
    UNIT_FIELD_POSSTAT3,
    UNIT_FIELD_POSSTAT4,
    UNIT_FIELD_NEGSTAT0,
    UNIT_FIELD_NEGSTAT1,
    UNIT_FIELD_NEGSTAT2,
    UNIT_FIELD_NEGSTAT3,
    UNIT_FIELD_NEGSTAT4,

    /// WotLK: the buff halves of armour and the six schools, seven each in the
    /// same school order as RESISTANCES (99-105). 0x004f54d0 is the whole of it:
    /// effective = RESISTANCES[i], positive = this +i, negative = the other +i,
    /// and the base is effective - positive - negative, with the effective pair
    /// - and only those - clamped away when negative. UnitArmor (0x00610ec0)
    /// calls it for any player-controlled unit and pushes five values;
    /// UnitResistance (0x006101a0) calls it only for the local player and takes
    /// four, answering the pet from RESISTANCES alone with no parts. The two
    /// bindings disagree with each other about the pet; that is the client's own
    /// behaviour, mirrored here rather than tidied up, because it is what the
    /// shipped interface shows for a hunter's pet.
    ///
    /// Bases rather than one name per element: the client's field-name table has
    /// a single string for each run (RESISTANCEBUFFMODSNEGATIVE at 0x009f8bf0,
    /// POSITIVE at 0x009f8c18, RESISTANCES at 0x009f8c40 - descending index
    /// order, so NEGATIVE is the 113 end and POSITIVE the 106 end, which
    /// 0x004f54d0's own blob reads at +0x190 and +0x1ac confirm). Each run is
    /// then pinned between two keys this table has always had: RESISTANCES' 99
    /// and BASE_MANA's 120.
    UNIT_FIELD_RESISTANCEBUFFMODSPOSITIVE,
    UNIT_FIELD_RESISTANCEBUFFMODSNEGATIVE,

    COUNT
};

/**
 * Maps logical update field names to expansion-specific wire indices.
 * Loaded from JSON (e.g. Data/expansions/wotlk/update_fields.json).
 */
class UpdateFieldTable {
public:
    /** Load from JSON file. Returns true if successful. */
    bool loadFromJson(const std::string& path);

    /** Get the wire index for a logical field. Returns 0xFFFF if unknown. */
    [[nodiscard]] uint16_t index(UF field) const;

    /** Override a wire index at runtime (used for auto-detecting custom field layouts). */
    void setIndex(UF field, uint16_t idx) { fieldMap_[static_cast<uint16_t>(field)] = idx; }

    /** Check if a field is mapped. */
    [[nodiscard]] bool hasField(UF field) const;

    /** Number of mapped fields. */
    [[nodiscard]] size_t size() const { return fieldMap_.size(); }

private:
    std::unordered_map<uint16_t, uint16_t> fieldMap_;  // UF enum → wire index
};

/**
 * Global active update field table (set by Application at startup).
 */
void setActiveUpdateFieldTable(const UpdateFieldTable* table);
const UpdateFieldTable* getActiveUpdateFieldTable();

/** Convenience: get wire index for a logical field. */
inline uint16_t fieldIndex(UF field) {
    const auto* t = getActiveUpdateFieldTable();
    return t ? t->index(field) : 0xFFFF;
}

} // namespace game
} // namespace wowee
