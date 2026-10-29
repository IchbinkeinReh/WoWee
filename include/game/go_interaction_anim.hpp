#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "game/protocol_constants.hpp"

namespace wowee::game::go_interaction {

/// A game object's animation states, CGGameObject_C's index into the table
/// at 0x00ada938: Spawn, Closed, Open, Opened, Close, Destroy, Destroyed,
/// Rebuild, then Custom0..3 and Despawn (AnimationData 145..157).
inline constexpr uint8_t kStateOpen = 2;
inline constexpr uint8_t kStateOpened = 3;
inline constexpr uint8_t kStateClose = 4;
inline constexpr uint8_t kStateDestroy = 5;
inline constexpr uint8_t kStateRebuild = 7;
inline constexpr uint8_t kStateCustom0 = 8;
inline constexpr uint8_t kStateDespawn = 12;

/// What the type's handler does with its animation.
enum class Action : uint8_t {
    None,
    /// Enter `state` (slot 0xb0, 0x0070d510: nothing if already in it).
    Enter,
    /// Enter `state` only if the model has its animation (0x0070c480).
    EnterIfModelHas,
    /// A custom animation (0x0070bde0): as EnterIfModelHas, or, while the
    /// model is still loading, noted (GameObject_C +0x208) and played as
    /// it arrives (0x00713f50).
    PlayCustom,
    /// Enter `state` only if the model is not showing its Open already -
    /// Open, or what AnimationData falls back to without one (0x0070fe10).
    EnterUnlessShowingOpen,
};

struct Step {
    Action action = Action::None;
    uint8_t state = 0;
};

/// A QUESTGIVER's customAnim: of type 2's data fields, the one of kind 0x16
/// (0x00746190 over the table at 0x00a36f34) - data4. 0 when the template is
/// not cached (GameObject_C +0x1a4).
[[nodiscard]] inline uint32_t questGiverCustomAnim(const uint32_t* data) {
    return data ? data[4] : 0u;
}

/// The object became the interaction object (0x0051fce0 -> 0x0070bab0, the
/// type handler's slot 0x98).
///   type 2   0x0070fe10: with no customAnim, Open unless it shows it already;
///            with one, the custom animation 0x0070bde0 picks -
///            0x00a335e8[customAnim + 1], Custom0..3 and Despawn for 1..5 -
///            which a model still loading gets when it arrives
///   type 9   0x0070b560: Open
///   19, 34   0x0070c9f0 (mailbox, guild bank): Custom0
/// Every other type's slot is 0x005eeb70, which does nothing.
[[nodiscard]] inline Step onOpen(uint32_t goType, const uint32_t* data) {
    switch (goType) {
        case 2: {
            const uint32_t custom = questGiverCustomAnim(data);
            if (custom == 0) return {Action::EnterUnlessShowingOpen, kStateOpen};
            // Past 5 the table holds something that is not a state.
            if (custom > 5) return {};
            return {Action::PlayCustom, static_cast<uint8_t>(kStateCustom0 + custom - 1)};
        }
        case 9:
            return {Action::Enter, kStateOpen};
        case 19:
        case 34:
            return {Action::EnterIfModelHas, kStateCustom0};
        default:
            return {};
    }
}

/// Whether the type handler's slot 0x4c, through which a custom animation is
/// played (0x0070bde0, 0x0070bef0, 0x00713f50), is 0x0070c480: enter the
/// state if the model has its animation. The handlers 0x00714250 builds for
/// types 4, 7, 11, 13, 14, 15, 31, 32, 33 and 35, and the base one for a
/// type it has no case for (0x00a33618: 20, 21, 28 and past 35), have
/// 0x00632050 there, which does nothing.
[[nodiscard]] inline bool playsCustomAnimation(uint32_t goType) {
    switch (goType) {
        case 4: case 7: case 11: case 13: case 14: case 15:
        case 20: case 21: case 28:
        case 31: case 32: case 33: case 35:
            return false;
        default:
            return goType < 35;
    }
}

/// A door's (type 0) state entry, 0x0070d8d0 - its handler's slot 0xb0 -
/// also sets whether it collides (GameObject_C +0x20c, which 0x0070f550
/// gives the world's collision queries, 0x004f6560): only in Closed (1), so
/// not while it plays a custom animation or Despawn. Other types' entry
/// leaves it as it is (nullopt).
[[nodiscard]] inline std::optional<bool> collidesAfterEntering(uint32_t goType, uint8_t state) {
    if (goType != 0) return std::nullopt;
    return state == 1;
}

/// SMSG_GAMEOBJECT_CUSTOM_ANIM (0x0070be90): an index under 4 plays custom
/// animation 0..3 (0x0070bde0, 0x00a335e8[index + 2]: Custom0..3); any other
/// is ignored.
[[nodiscard]] inline std::optional<uint8_t> customAnimState(uint32_t index) {
    if (index >= 4) return std::nullopt;
    return static_cast<uint8_t>(kStateCustom0 + index);
}

/// 0x0070d7e0, as a custom animation ends: Custom0..3 take the server
/// state's pose again (slot 0x30, unless taken away meanwhile). Despawn has
/// no case - the model goes on with its sequence as the M2 plays it on.
[[nodiscard]] inline bool customAnimEndRestoresPose(uint8_t state) {
    return state >= kStateCustom0 && state < kStateDespawn;
}

/// The interaction object was cleared (0x00512e60 -> 0x0070bac0, slot 0x9c).
///   type 2   0x0070ff20: Close, with no customAnim and the object in Open
///            or Opened
///   type 9   0x0070b570: Close
/// Every other type's slot does nothing.
[[nodiscard]] inline Step onClose(uint32_t goType, const uint32_t* data, uint8_t current) {
    switch (goType) {
        case 2:
            if (questGiverCustomAnim(data) == 0 &&
                (current == kStateOpen || current == kStateOpened)) {
                return {Action::Enter, kStateClose};
            }
            return {};
        case 9:
            return {Action::Enter, kStateClose};
        default:
            return {};
    }
}

/// 0x0070d510: entering `to` from `from` reverses a transition - Open and
/// Close, Destroy and Rebuild - which then starts where the other got to.
[[nodiscard]] inline bool reversesTransition(uint8_t from, uint8_t to) {
    return (from == kStateOpen && to == kStateClose) || (from == kStateClose && to == kStateOpen) ||
           (from == kStateDestroy && to == kStateRebuild) || (from == kStateRebuild && to == kStateDestroy);
}

/// 0x0070d510 -> 0x0070d010: how far into the reversed transition it starts,
/// in 65535ths (written to GAMEOBJECT_DYNAMIC's high half, +0x22): what is
/// left of the one playing, 1 - elapsed / length over 0x008266b0's start and
/// end. Those span the sequence's length at its speed, its time counted
/// from the start, so elapsed / length is time / duration. None when that
/// has no length (a speed of 0 starts and ends it at once, 0x00826ed0) or
/// is past its end.
[[nodiscard]] inline std::optional<uint16_t> reversedStartProgress(float timeMs, float durationMs,
                                                                   float speed) {
    if (speed == 0.0f || !(durationMs > 0.0f) || timeMs < 0.0f) return std::nullopt;
    const float done = timeMs / durationMs;
    if (done > 1.0f) return std::nullopt;
    return static_cast<uint16_t>(std::lrint((1.0f - done) * 65535.0f));
}

/// 0x0070d1e0: the sequence time (ms) a transition starts at for a progress
/// in 65535ths - duration * progress / 65535, rounded.
[[nodiscard]] inline uint32_t progressStartTime(uint16_t progress, uint32_t durationMs) {
    return static_cast<uint32_t>(std::lrint(static_cast<float>(durationMs) *
                                            static_cast<float>(progress) * 1.5259022e-05f));
}

/// 0x0070d1e0: entering a state takes a hold on the object (0x004d4d60)
/// that 0x0070d7e0 lets go as the state ends - any state but Closed, Opened
/// and Destroyed. One the server takes away meanwhile is kept, marked with
/// GameObject_C +0xbc 0x100000, until the last hold goes (0x004d7610,
/// 0x004d7230, 0x004d77e0).
[[nodiscard]] inline bool stateHoldsObject(uint8_t state) {
    return state != 1 && state != kStateOpened && state != 6;
}

/// Whether a window opening with `guid` makes it the interaction object
/// (0x0051fce0). Most windows always do; the trainer's (0x005940e0) and the
/// stable's (0x005a11a0) not while already open with it (`openWith`), and
/// the trainer's not for the player itself (`self`, 0 for any).
[[nodiscard]] inline bool windowSetsInteraction(uint64_t guid, uint64_t openWith, uint64_t self = 0) {
    if (guid == openWith) return false;
    return self == 0 || guid != self;
}

/// The windows that set the interaction object, in the order 0x00512e60
/// asks them as it lets the object go: the first open with it closes.
///   Trade         0xbfa658, closed by 0x005870a0(0)
///   Book          0xbfc948, 0x0058a1a0(0)
///   Gossip        0xc016f0, 0x0058a550(0)
///   Quest         0xc0d648, 0x0058ca70
///   Merchant      0xbfa3e8, 0x00584600
///   Taxi          0xc0dbf0, 0x00590ba0
///   Trainer       0xc0e490, 0x005940e0(0)
///   Bank          0xbeb9a0, 0x0057b8d0(0)
///   GuildBank     0xc1dc10, 0x005a4270(0)
///   Registrar     0xc0e570, 0x00599e90 (the guild's; the arena's, 0xc0f7c8,
///                 comes after the stable - wowee keeps one for both)
///   Tabard        0xc0e518, 0x00598ec0
///   Mailbox       0xbeb030, 0x00571730
///   AuctionHouse  0xc0f3e8, 0x0059f150
///   Stable        0xc0f6c0, 0x005a0f10
/// The battlemaster (0xbea568), which wowee does not open, is left out.
enum class Window : uint8_t {
    Trade, Book, Gossip, Quest, Merchant, Taxi, Trainer, Bank, GuildBank,
    Registrar, Tabard, Mailbox, AuctionHouse, Stable, Count
};
inline constexpr size_t kWindowCount = static_cast<size_t>(Window::Count);

/// 0x00512e60 as 0x0051fce0 replaces `released`: the first window in its
/// order open with it (`openWith`, 0 for one not open), which closes. Not
/// the window being opened (`opening`): the client asks before that one
/// takes its object, wowee after.
[[nodiscard]] inline std::optional<Window> windowClosedWith(const std::array<uint64_t, kWindowCount>& openWith,
                                                            uint64_t released,
                                                            std::optional<Window> opening = std::nullopt) {
    if (released == 0) return std::nullopt;
    for (size_t i = 0; i < kWindowCount; ++i) {
        const auto window = static_cast<Window>(i);
        if (opening && *opening == window) continue;
        if (openWith[i] == released) return window;
    }
    return std::nullopt;
}

/// The windows UNIT_NPC_FLAGS opens, each with the flag that keeps it open, in
/// the order 0x0071a0b0 asks them: gossip 0x1, the quest giver's 0x2, the flight
/// master's 0x2000, the trainer's 0x10, the banker's 0x20000, the guild banker's
/// 0x00800000 and the postmaster's 0x04000000.  The flight master comes before
/// the trainer here, where the letting-go order above has them the other way
/// round, and no other window is in it: a merchant, an auction house, a stable,
/// a registrar or a tabard vendor whose flag goes away keeps its window open.
inline constexpr std::pair<uint32_t, Window> kWindowKeptByNpcFlag[] = {
    {NPC_FLAG_GOSSIP,        Window::Gossip},
    {NPC_FLAG_QUESTGIVER,    Window::Quest},
    {NPC_FLAG_FLIGHT_MASTER, Window::Taxi},
    {NPC_FLAG_TRAINER,       Window::Trainer},
    {NPC_FLAG_BANKER,        Window::Bank},
    {NPC_FLAG_GUILD_BANKER,  Window::GuildBank},
    {NPC_FLAG_POSTMASTER,    Window::Mailbox},
};

/// 0x0071a0b0 on a unit whose UNIT_NPC_FLAGS went from `before` to `after`:
/// every window whose flag went away with the bit, and only one open with this
/// very unit (`with` - the same per-window guids as the list above, 0 for a
/// window not open, and `guid` 0 for a unit no window can be open with).
///
/// More than one can answer for one change, and the client closes everything
/// that matches rather than stopping at the first: a quest giver who stops being
/// a gossip as well loses the gossip frame and then the quest frame, in that
/// order.
///
/// Not its vtable +0x50, which the client also runs whenever the quest giver's
/// or the flight master's bit changes either way: that redraws the unit, and the
/// marks over a quest giver come from the status query wowee already sends when
/// a unit becomes one (quest_status_requery).
[[nodiscard]] inline std::vector<Window> npcFlagWindowsLost(uint32_t before, uint32_t after,
                                                            uint64_t guid,
                                                            const std::array<uint64_t, kWindowCount>& with) {
    std::vector<Window> lost;
    if (guid == 0) return lost;
    for (const auto& [flag, window] : kWindowKeptByNpcFlag) {
        if ((before & flag) != 0 && (after & flag) == 0 && with[static_cast<size_t>(window)] == guid)
            lost.push_back(window);
    }
    return lost;
}

/// SMSG_TRADE_STATUS codes after which the trade manager forgets its
/// partner and who asked (0x007044a0: 0xca0fe0 and 0xca0fe8 cleared) - the
/// partner a trade window sets as the interaction object. Not 1, 2, 4, 7, 9
/// or 22 and on.
[[nodiscard]] inline bool tradeStatusEndsTrade(uint32_t status) {
    switch (status) {
        case 0: case 3: case 5: case 6: case 8: case 10: case 11: case 12: case 13:
            return true;
        default:
            return status >= 14 && status <= 21;
    }
}

/// What 0x00703cf0 does with a trade request (CMSG_INITIATE_TRADE).
enum class TradeInitiate : uint8_t {
    /// A partner is set (0xca0fe8): ERR_ALREADY_TRADING, nothing sent.
    AlreadyTrading,
    /// Within a second of the last request sent (0xca0ff8): nothing.
    TooSoon,
    /// Sent, and the next one allowed a second on.
    Send,
};

/// `partner` is the trade manager's partner; `resendAtMs` when the next
/// request may go (0 before the first), compared with `nowMs` as the client
/// compares its millisecond clock.
[[nodiscard]] inline TradeInitiate tradeInitiate(uint64_t partner, uint32_t nowMs, uint32_t resendAtMs) {
    if (partner != 0) return TradeInitiate::AlreadyTrading;
    if (resendAtMs != 0 && static_cast<int32_t>(nowMs - resendAtMs) < 0) return TradeInitiate::TooSoon;
    return TradeInitiate::Send;
}

} // namespace wowee::game::go_interaction
