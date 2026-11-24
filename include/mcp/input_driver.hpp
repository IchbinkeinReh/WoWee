#pragma once

// The MCP server's keyboard and mouse.
//
// What it presses goes the two ways a device's press goes: an event in SDL's
// queue, which the interface, the edit boxes, the key bindings and the camera
// take from there, and the held state core::Input reads once a frame, which is
// what moves the character and clicks the world. Both before the frame's
// events are read, so one press is one press to everything that looks at it.
// Nothing is called that a key would have called.

#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

namespace wowee::mcp {

class Origin;

/// A key by the name the key bindings give it ("W", "SPACE", "F1", "NUMPAD3",
/// "ESCAPE"); SDL_SCANCODE_UNKNOWN for none. By what is printed on the key,
/// whatever the layout.
SDL_Scancode scancodeNamed(std::string_view wowName);
/// "CTRL-SHIFT-F1" to its key and modifiers. False for what is none.
bool parseChord(std::string_view chord, SDL_Scancode& key, SDL_Keymod& mods);

class InputDriver {
public:
    /// Marks an event as this driver's: in `which`, and in a text event's
    /// reserved field.
    static constexpr Uint32 kDevice = 0x4D435057u;

    void setWindow(SDL_Window* window) { window_ = window; }
    void setOrigin(Origin* origin) { origin_ = origin; }

    void keyDown(SDL_Scancode key, SDL_Keymod mods = SDL_KMOD_NONE);
    void keyUp(SDL_Scancode key, SDL_Keymod mods = SDL_KMOD_NONE);
    /// Modifier keys pressed, or let go of, as keys of their own.
    void modifiers(SDL_Keymod mods, bool down);
    [[nodiscard]] bool keyHeld(SDL_Scancode key) const;
    /// Text as a keyboard's, one event.
    void type(std::string_view utf8);

    /// The pointer, in window coordinates from the top left.
    void moveTo(const glm::vec2& at);
    /// A step of the mouse with the pointer where it is, which is how the
    /// camera is turned by a drag.
    void nudge(float dx, float dy);
    void button(int sdlButton, bool down, int clicks = 1);
    void wheel(float clicks);
    [[nodiscard]] bool pointerHeld() const { return pointerHeld_; }
    [[nodiscard]] glm::vec2 pointer() const { return pointer_; }
    /// The pointer back to the device.
    void releasePointer();

    /// Every key and button let go of.
    void releaseAll();

    /// Looks at an event off the queue before anything else does. True when it
    /// is to be dropped: a real mouse while the pointer is the server's.
    /// `handsBusy`: a call that presses something is under way.
    bool filter(const SDL_Event& event, bool handsBusy);

    /// Once a frame: typed text that has been read is let go of.
    void tick();

private:
    void push(SDL_Event& event);

    SDL_Window* window_ = nullptr;
    Origin* origin_ = nullptr;
    std::vector<SDL_Scancode> held_;
    std::vector<int> buttons_;
    SDL_Keymod mods_ = SDL_KMOD_NONE;
    bool pointerHeld_ = false;
    glm::vec2 pointer_{0.0f};
    /// An SDL text event points at its text; kept until it has been read.
    struct Typed {
        std::string text;
        int frames = 0;
    };
    std::deque<Typed> typed_;
};

}  // namespace wowee::mcp
