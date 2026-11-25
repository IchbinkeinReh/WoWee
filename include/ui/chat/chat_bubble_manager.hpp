#pragma once

#include "ui/ui_services.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace wowee {
namespace game { class GameHandler; }
namespace ui {

/// The chat bubbles over the units that speak: how long each is up, how it fades and
/// where it is. They are the interface's frames (chat_bubble_lua.hpp).
class ChatBubbleManager {
public:
    /** Add a bubble, in place of the one the unit has; `own` for the player's. */
    void addBubble(uint64_t senderGuid, const std::string& message, uint8_t chatType, bool own = false);

    /** Tick the bubbles and put their frames where their units are on the screen. */
    void render(game::GameHandler& gameHandler, const UIServices& services);

    /** Register the chat-bubble callback on GameHandler (call once per session). */
    void setupCallback(game::GameHandler& gameHandler);

    [[nodiscard]] bool empty() const { return bubbles_.empty(); }

    /// The chatBubbles and chatBubblesParty CVars. Each is asked when a
    /// bubble is made (0x00720010) - say and yell lines by the first, party
    /// lines by the second - and not afterwards: a bubble already up stays
    /// for its time when either is turned off.
    [[nodiscard]] bool bubblesShown() const { return show_; }
    void setBubblesShown(bool shown) { show_ = shown; }
    void setPartyBubblesShown(bool shown) { partyShown_ = shown; }

private:
    struct ChatBubble {
        uint64_t senderGuid = 0;
        std::string message;
        float elapsed = 0.0f;    ///< seconds since it was said
        float duration = 0.0f;   ///< seconds up before the fade out (chat_bubble::durationMs)
        float alpha = 0.0f;      ///< 0..1, faded in and out over 250 ms
        uint8_t chatType = 1;
        uint32_t id = 0;
    };
    std::vector<ChatBubble> bubbles_;
    uint32_t lastId_ = 0;
    bool callbackSet_ = false;
    bool show_ = true;
    bool partyShown_ = true;

    static constexpr size_t kMaxBubbles = 10;
};

} // namespace ui
} // namespace wowee
