#pragma once

// The steps the pressing tools are made of, and what they report back.

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

#include "mcp/script.hpp"
#include "mcp/tool_context.hpp"

namespace wowee::mcp {

using Steps = std::deque<Script::Fn>;

/// What has come of a call: the interface's events, errors, dialogs, refused
/// packets and the chat's system lines since it began, short.
struct Aftermath {
    uint64_t events = 0;
    uint64_t refusals = 0;
    uint64_t chat = 0;
    uint64_t log = 0;
    explicit Aftermath(const ToolContext& context);
    [[nodiscard]] json report(ToolContext& context) const;
    /// Whether one of these events has been fired since.
    [[nodiscard]] bool heard(ToolContext& context, std::initializer_list<const char*> names) const;
};

/// A step that puts `value()` in the script's result and ends it, with the
/// aftermath beside it.
Script::Fn finishWith(ToolContext& context, Aftermath after, std::function<json()> value);

/// Fails the call while an edit box or a dialog of the client's own takes the
/// keys: what is pressed would be typed.
Script::Fn wantKeysFree(ToolContext& context);

/// A key down, held, up.
Steps pressKey(ToolContext& context, SDL_Scancode key, SDL_Keymod mods, double seconds);
/// The pointer to a place and a moment for the interface to notice.
Steps movePointer(ToolContext& context, std::function<std::optional<glm::vec2>()> where);
/// A click where the pointer is.
Steps pressButton(ToolContext& context, int button, SDL_Keymod mods, int clicks);
/// The pointer there, and a click.
Steps clickAt(ToolContext& context, std::function<std::optional<glm::vec2>()> where, int button, SDL_Keymod mods,
              int clicks);
/// From one place to another with the button held, and with the modifiers.
Steps dragFromTo(ToolContext& context, glm::vec2 from, glm::vec2 to, int button, SDL_Keymod mods = SDL_KMOD_NONE);

/// A point of the window that is the world's and not the interface's, for a
/// drag to begin at.
std::optional<glm::vec2> worldPoint(ToolContext& context);

/// The camera turned by dragging with `button` until `yawLeft` and `pitchLeft`
/// - degrees still to go, read anew each frame - are within a degree; gives up
/// after a while.
Steps dragView(ToolContext& context, int button, std::function<glm::vec2()> degreesLeft);

/// The character turned until it looks at a place, with the turn keys or by
/// the mouse. `target` is asked each frame.
Steps turnToward(ToolContext& context, std::function<std::optional<glm::vec3>()> target, bool byMouse);

/// A key held while a condition is, let go of at once when it is not; the
/// steering's hands.
class HeldKey {
public:
    HeldKey(ToolContext& context, SDL_Scancode key) : context_(context), key_(key) {}
    ~HeldKey() { set(false); }
    HeldKey(const HeldKey&) = delete;
    HeldKey& operator=(const HeldKey&) = delete;
    void set(bool down);
    [[nodiscard]] bool isDown() const { return down_; }

private:
    ToolContext& context_;
    SDL_Scancode key_;
    bool down_ = false;
};

/// A walk to a place, by keys. `goal` is asked each frame, so that it can be
/// something that moves; nothing ends the walk.
struct WalkOptions {
    float stopWithin = 2.0f;
    double timeout = 60.0;
    bool stopInCombat = false;
};
Steps walkTo(ToolContext& context, std::function<std::optional<glm::vec3>()> goal, WalkOptions options,
             std::shared_ptr<json> told);

/// Text typed into the chat box and sent, from opening it on.
Steps chatLine(ToolContext& context, std::string text);

void append(Steps& to, Steps more);

}  // namespace wowee::mcp
