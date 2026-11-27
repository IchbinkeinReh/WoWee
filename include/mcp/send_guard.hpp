#pragma once

// What of the client's packets the MCP server does not let out.
//
// A tool can refuse the plain ways of speaking to somebody, but a click on the
// right button of the right window, a macro or an addon's answer to an event
// ends in the same packet, so the packet is where it is decided: WorldSocket
// asks here about each one it is about to send while the server runs and the
// last thing that acted on the client was the server (origin.hpp).
//
// By name rather than by number, and by the whole family of a name, with the
// harmless ones let through one by one: a packet of the guild's or the
// auction's that nobody thought of is refused, not missed. Free of the game so
// that every rule can be tried in a test.

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mcp/social.hpp"

namespace wowee::mcp {

struct GuardContext {
    /// The character's own name and guid, for a whisper or a spell at itself.
    std::string ownName;
    uint64_t ownGuid = 0;
    /// The guild the character is in, "" for none: the only one `.guild
    /// delete` may name.
    std::string ownGuildName;
    /// --mcp-allow.
    uint32_t allow = 0;
    /// The 3.3.5a wire, whose casts are read for their target. On another the
    /// selected target stands for it.
    bool wotlkWire = true;
    /// Who is selected, which is whom a game master's command is about.
    uint64_t targetGuid = 0;
    /// The last CMSG_SET_SELECTION that went out: whom the realm has selected,
    /// which is somebody still where the client let go without saying so.
    uint64_t selectedOnRealm = 0;
    /// The vehicle the character sits in, 0 for none.
    uint64_t vehicleGuid = 0;
    /// The creature the character charms (UNIT_FIELD_CHARM), 0 for none: let
    /// go of, it is not lost for good.
    uint64_t charmGuid = 0;
    /// The character's summoned pet where it is no hunter's (a warlock's
    /// demon, a death knight's ghoul, a mage's elemental), 0 otherwise: the
    /// realm only unsummons such a pet on COMMAND_ABANDON, a hunter's pet it
    /// deletes (AzerothCore HandlePetActionHelper: PET_SAVE_AS_DELETED for
    /// HUNTER_PET only).
    uint64_t summonedPetGuid = 0;
    /// Whether a guid is another player's pet, totem or guardian.
    std::function<bool(uint64_t)> belongsToOtherPlayer;
};

struct Verdict {
    enum class Kind { Allowed, Social, Never };
    Kind kind = Kind::Allowed;
    /// Of a Social one: the --mcp-allow that would let it through.
    Social social = Social::Chat;
    std::string why;

    [[nodiscard]] bool allowed() const { return kind == Kind::Allowed; }
};

/// Whether a packet may go out: its opcode by its canonical name
/// ("CMSG_MESSAGECHAT") and what is in it.
Verdict judgeSend(std::string_view opcode, std::span<const uint8_t> payload, const GuardContext& context);

/// Every opcode the rules name one by one: so that a test can hold them
/// against the ones there are, for a name misspelt here would rule nothing.
std::vector<std::string_view> namedOpcodes();

/// Whether a guid is a player's, by its high part.
bool isPlayerGuid(uint64_t guid);

/// Whether a game master's command may be given, as a whisper to oneself: the
/// ones that move, heal, equip or describe the character itself. Everything
/// that names another player, an account, the realm or the database is not
/// among them. `text` begins with '.' or '!'.
Verdict judgeGmCommand(std::string_view text, const GuardContext& context);

/// Whether a game master's command only tells (.npc info, .gps, .lookup ...):
/// the only kind given with anything selected but the character itself. Every
/// other is about whoever is selected, where it names nobody, and the
/// character itself is selected for it.
bool gmCommandOnlyTells(std::string_view text);

/// Why click_ui does not press `name`, a button of the dialog `popup` that shows
/// `which` now ("" for none), when `expected` is the dialog the caller named
/// ("" for none given); empty when it is pressed. A dialog's button is pressed
/// only for the dialog meant: another may have taken its place.
std::string popupPressRefusal(const std::string& name, const std::string& popup, const std::string& which,
                              const std::string& expected);

/// Whom a CMSG_SET_SELECTION selects, where the packet is one.
std::optional<uint64_t> selectionOf(std::string_view opcode, std::span<const uint8_t> payload);

/// Whether text given to the chat box may be entered: the slash commands that
/// run Lua are --mcp-lua's, and those that end the session have tools of their
/// own. What is said to somebody is decided by the packet.
Verdict judgeChatText(std::string_view text, bool luaAllowed);

}  // namespace wowee::mcp
