#pragma once

// The player character's spoken errors.
//
// A game error the client shows (0x005216f0) also speaks where its row in
// the client's table of them (0x00ac8af0: key, kind, sound, vocal, chat
// type; 0x44 for no vocal) names one of VocalUISounds' VocalUIEnum values,
// handing it to 0x00986d00. That plays the sound the character's race and
// gender has for it - 0x00986c90 fills the 0x44 of them from
// VocalUISounds.dbc as the player is made: each row's VocalUIEnum, for its
// RaceID, its NormalSoundID for the gender - a SoundEntries kit, as type 7,
// one at a time (audio/play_limit.hpp), with Sound_EnableErrorSpeech on.
//
// PlayerErrorSpeech is that enum, by its value, for those the game errors
// reach; named for the files the kits play.

#include <cstdint>
#include <optional>
#include <string_view>

namespace wowee::audio {

enum class PlayerErrorSpeech : uint8_t {
    INVENTORY_FULL = 0,
    NO_AMMO = 1,
    CANT_EQUIP_LEVEL = 2,
    CANT_EQUIP_EVER = 3,
    CANT_DROP_SOULBOUND = 4,
    ITEM_COOLDOWN = 5,
    FOOD_COOLDOWN = 7,
    PARTY_FULL = 8,
    OUT_OF_RANGE = 10,
    INVALID_ATTACK_TARGET = 11,
    SPELL_COOLDOWN = 12,
    CANT_LEARN_SPELL = 13,
    NO_MANA = 15,
    ALREADY_IN_GROUP = 20,
    BANK_SLOT_NOT_ENOUGH_MONEY = 22,
    NOT_A_BAG = 25,
    BAG_IN_BAG = 26,
    WRONG_SLOT = 27,
    AMMO_ONLY = 28,
    BAG_FULL = 29,
    ITEM_MAX_COUNT = 30,
    LOOT_DIDNT_KILL = 31,
    LOOT_BAD_FACING = 32,
    LOOT_LOCKED = 33,
    LOOT_TOO_FAR = 35,
    NO_ATTACK_TARGET = 38,
    NOT_ENOUGH_MONEY = 40,
    CANT_EQUIP_SKILL = 41,
    TWO_HANDED_EQUIPPED = 42,
    TWO_HAND_SKILL_NOT_FOUND = 43,
    NOT_EQUIPPABLE = 44,
    GENERIC_NO_TARGET = 45,
    SPELL_OUT_OF_RANGE = 46,
    POTION_COOLDOWN = 47,
    PROFICIENCY_NEEDED = 48,
    MUST_EQUIP_ITEM = 49,
    ABILITY_COOLDOWN = 50,
    CANT_USE_ITEM = 51,
    CHEST_IN_USE = 52,
    TAXI_NOT_ENOUGH_MONEY = 54,
    USE_TOO_FAR = 57,
    CANT_TRADE_SOULBOUND = 59,
    ITEM_LOCKED = 61,
    GUILD_PERMISSIONS = 62,
    NO_RAGE = 63,
    NO_ENERGY = 64,
};

namespace vocal_error {

/// How many VocalUIEnum values the client keeps a sound for (0x00986c90).
constexpr uint32_t kCount = 0x44;

/// The spoken error of a game error, by its GlobalStrings key - the
/// table's vocal column, for the 64 rows that have one; none for the rest.
inline std::optional<PlayerErrorSpeech> forGameError(std::string_view key) {
    using S = PlayerErrorSpeech;
    struct Row {
        std::string_view key;
        S speech;
    };
    static constexpr Row kRows[] = {
        {"ERR_INV_FULL", S::INVENTORY_FULL},
        {"ERR_CANT_EQUIP_LEVEL_I", S::CANT_EQUIP_LEVEL},
        {"ERR_CANT_EQUIP_SKILL", S::CANT_EQUIP_SKILL},
        {"ERR_CANT_EQUIP_EVER", S::CANT_EQUIP_EVER},
        {"ERR_PROFICIENCY_NEEDED", S::PROFICIENCY_NEEDED},
        {"ERR_WRONG_SLOT", S::WRONG_SLOT},
        {"ERR_CANT_EQUIP_NEED_TALENT", S::CANT_EQUIP_SKILL},
        {"ERR_BAG_FULL", S::BAG_FULL},
        {"ERR_BAG_IN_BAG", S::BAG_IN_BAG},
        {"ERR_AMMO_ONLY", S::AMMO_ONLY},
        {"ERR_ITEM_MAX_COUNT", S::ITEM_MAX_COUNT},
        {"ERR_NOT_EQUIPPABLE", S::NOT_EQUIPPABLE},
        {"ERR_NOT_A_BAG", S::NOT_A_BAG},
        {"ERR_ITEM_LOCKED", S::ITEM_LOCKED},
        {"ERR_2HANDED_EQUIPPED", S::TWO_HANDED_EQUIPPED},
        {"ERR_NOT_ENOUGH_MONEY", S::NOT_ENOUGH_MONEY},
        {"ERR_DROP_BOUND_ITEM", S::CANT_DROP_SOULBOUND},
        {"ERR_TRADE_BOUND_ITEM", S::CANT_TRADE_SOULBOUND},
        {"ERR_TRADE_QUEST_ITEM", S::CANT_TRADE_SOULBOUND},
        {"ERR_TRADE_TEMP_ENCHANT_BOUND", S::CANT_TRADE_SOULBOUND},
        {"ERR_ITEM_COOLDOWN", S::ITEM_COOLDOWN},
        {"ERR_POTION_COOLDOWN", S::POTION_COOLDOWN},
        {"ERR_FOOD_COOLDOWN", S::FOOD_COOLDOWN},
        {"ERR_SPELL_COOLDOWN", S::SPELL_COOLDOWN},
        {"ERR_ABILITY_COOLDOWN", S::ABILITY_COOLDOWN},
        {"ERR_SPELL_ALREADY_KNOWN_S", S::CANT_LEARN_SPELL},
        {"ERR_INVITED_ALREADY_IN_GROUP_SS", S::ALREADY_IN_GROUP},
        {"ERR_ALREADY_IN_GROUP_S", S::ALREADY_IN_GROUP},
        {"ERR_GROUP_FULL", S::PARTY_FULL},
        {"ERR_GUILD_PERMISSIONS", S::GUILD_PERMISSIONS},
        {"ERR_OUT_OF_RANGE", S::OUT_OF_RANGE},
        {"ERR_LOOT_LOCKED", S::LOOT_LOCKED},
        {"ERR_LOOT_TOO_FAR", S::LOOT_TOO_FAR},
        {"ERR_LOOT_DIDNT_KILL", S::LOOT_DIDNT_KILL},
        {"ERR_LOOT_BAD_FACING", S::LOOT_BAD_FACING},
        {"ERR_CANT_USE_ITEM", S::CANT_USE_ITEM},
        {"ERR_CANT_USE_ITEM_IN_ARENA", S::CANT_USE_ITEM},
        {"ERR_MUST_EQUIP_ITEM", S::MUST_EQUIP_ITEM},
        {"ERR_2HSKILLNOTFOUND", S::TWO_HAND_SKILL_NOT_FOUND},
        {"ERR_NO_ATTACK_TARGET", S::NO_ATTACK_TARGET},
        {"ERR_INVALID_ATTACK_TARGET", S::INVALID_ATTACK_TARGET},
        {"ERR_TAXINOTENOUGHMONEY", S::TAXI_NOT_ENOUGH_MONEY},
        {"ERR_GENERIC_NO_TARGET", S::GENERIC_NO_TARGET},
        {"ERR_CHEST_IN_USE", S::CHEST_IN_USE},
        {"ERR_USE_LOCKED", S::ITEM_LOCKED},
        {"ERR_DOOR_LOCKED", S::ITEM_LOCKED},
        {"ERR_BUTTON_LOCKED", S::ITEM_LOCKED},
        {"ERR_USE_TOO_FAR", S::USE_TOO_FAR},
        {"ERR_BANKSLOT_INSUFFICIENT_FUNDS", S::BANK_SLOT_NOT_ENOUGH_MONEY},
        {"ERR_WRONG_BAG_TYPE_SUBCLASS", S::AMMO_ONLY},
        {"ERR_OUT_OF_MANA", S::NO_MANA},
        {"ERR_OUT_OF_RAGE", S::NO_RAGE},
        {"ERR_OUT_OF_ENERGY", S::NO_ENERGY},
        {"ERR_GUILDEMBLEM_NOTENOUGHMONEY", S::NOT_ENOUGH_MONEY},
        {"ERR_SPELL_OUT_OF_RANGE", S::SPELL_OUT_OF_RANGE},
        {"ERR_NOAMMO_S", S::NO_AMMO},
        {"ERR_INVALID_ITEM_TARGET", S::CANT_USE_ITEM},
        {"ERR_MAIL_QUEST_ITEM", S::CANT_TRADE_SOULBOUND},
        {"ERR_MAIL_BOUND_ITEM", S::CANT_TRADE_SOULBOUND},
        {"ERR_AUCTION_QUEST_ITEM", S::CANT_TRADE_SOULBOUND},
        {"ERR_AUCTION_BOUND_ITEM", S::CANT_TRADE_SOULBOUND},
        {"ERR_NOT_SAME_ACCOUNT", S::CANT_TRADE_SOULBOUND},
        {"ERR_GENERIC_NO_VALID_TARGETS", S::GENERIC_NO_TARGET},
        {"ERR_CURRENCY_FULL", S::INVENTORY_FULL},
    };
    for (const Row& row : kRows) {
        if (row.key == key) return row.speech;
    }
    return std::nullopt;
}

/// The SoundEntries row, by name, a game error with no line plays - the
/// table's sound column (0x004c74a0 for any but "NONE"), for the 40 rows
/// that name one.
inline std::optional<std::string_view> namedSound(std::string_view key) {
    struct Row {
        std::string_view key;
        std::string_view sound;
    };
    static constexpr Row kRows[] = {
        {"ERR_RECEIVE_ITEM_S", "ITEMGENERICSOUND"},
        {"ERR_DECLINE_GROUP_S", "igPlayerInviteDecline"},
        {"ERR_GUILD_CREATE_S", "LEVELUP"},
        {"ERR_INVITED_TO_GUILD_SSS", "LEVELUP"},
        {"ERR_QUEST_ACCEPTED_S", "QUESTADDED"},
        {"ERR_QUEST_FAILED_S", "igQuestFailed"},
        {"ERR_QUEST_FAILED_BAG_FULL_S", "igQuestFailed"},
        {"ERR_QUEST_FAILED_MAX_COUNT_S", "igQuestFailed"},
        {"ERR_QUEST_FAILED_LOW_LEVEL", "igQuestFailed"},
        {"ERR_QUEST_FAILED_MISSING_ITEMS", "igQuestFailed"},
        {"ERR_QUEST_FAILED_WRONG_RACE", "igQuestFailed"},
        {"ERR_QUEST_FAILED_NOT_ENOUGH_MONEY", "igQuestFailed"},
        {"ERR_QUEST_FAILED_EXPANSION", "igQuestFailed"},
        {"ERR_QUEST_ONLY_ONE_TIMED", "igQuestFailed"},
        {"ERR_QUEST_NEED_PREREQS", "igQuestFailed"},
        {"ERR_QUEST_ALREADY_ON", "igQuestFailed"},
        {"ERR_QUEST_ALREADY_DONE", "igQuestFailed"},
        {"ERR_QUEST_ALREADY_DONE_DAILY", "igQuestFailed"},
        {"ERR_QUEST_MUST_CHOOSE", "igQuestFailed"},
        {"ERR_QUEST_LOG_FULL", "igQuestFailed"},
        {"ERR_TRADE_REQUEST_S", "LEVELUP"},
        {"ERR_NEWTAXIPATH", "TaxiNodeDiscovered"},
        {"ERR_FRIEND_ONLINE_SS", "FRIENDJOINGAME"},
        {"ERR_DUEL_REQUESTED", "LEVELUP"},
        {"ERR_PETITION_SIGNED", "igPlayerInviteAccept"},
        {"ERR_PETITION_SIGNED_S", "igPlayerInviteAccept"},
        {"ERR_PETITION_DECLINED_S", "igPlayerInviteDecline"},
        {"ERR_PETITION_ALREADY_SIGNED", "igPlayerInviteDecline"},
        {"ERR_PETITION_RESTRICTED_ACCOUNT", "igPlayerInviteDecline"},
        {"ERR_PETITION_ALREADY_SIGNED_OTHER", "igPlayerInviteDecline"},
        {"ERR_PETITION_IN_GUILD", "igPlayerInviteDecline"},
        {"ERR_PETITION_CREATOR", "igPlayerInviteDecline"},
        {"ERR_PETITION_NOT_ENOUGH_SIGNATURES", "igPlayerInviteDecline"},
        {"ERR_PETITION_NOT_SAME_SERVER", "igPlayerInviteDecline"},
        {"ERR_PETITION_FULL", "igPlayerInviteDecline"},
        {"ERR_MEETING_STONE_IN_QUEUE_S", "PVPENTERQUEUE"},
        {"ERR_ARENA_TEAM_CREATE_S", "LEVELUP"},
        {"ERR_INVITED_TO_ARENA_TEAM", "LEVELUP"},
        {"ERR_QUEST_FAILED_TOO_MANY_DAILY_QUESTS_I", "igQuestFailed"},
        {"ERR_QUEST_FAILED_CAIS", "igQuestFailed"},
    };
    for (const Row& row : kRows) {
        if (row.key == key) return row.sound;
    }
    return std::nullopt;
}

}  // namespace vocal_error
}  // namespace wowee::audio
