#pragma once

#include <SDL3/SDL.h>
#include <array>
#include <optional>
#include <glm/glm.hpp>

namespace wowee {
namespace core {

class Input {
public:
    static Input& getInstance();

    void update();

    // Keyboard
    [[nodiscard]] bool isKeyPressed(SDL_Scancode key) const;

    /// Holds a key down from somewhere that is not a keyboard.
    ///
    /// The on-screen stick is the reason: movement, its animations and the
    /// packets that announce it are a long chain that starts at
    /// isKeyPressed(SDL_SCANCODE_W), and a phone has no W. Setting the key here
    /// drives all of it without any of it knowing where the press came from.
    /// Merged over the hardware state, so a keyboard still works alongside.
    void setVirtualKey(SDL_Scancode key, bool held);
    void clearVirtualKeys();

    /// Holds a mouse button down from somewhere that is not a mouse.
    ///
    /// The controller's pointer is the reason, and it is the same argument as
    /// the virtual keys above: targeting, looting and every click on the world
    /// is a chain that starts at isMouseButtonJustPressed, and a pad has no
    /// mouse. The pointer itself is the real one - it is warped, so everything
    /// that asks where it is gets the truth - and only the button is faked.
    void setVirtualMouseButton(int button, bool held);
    /// Reads a button as up even while the device holds it down. A touch
    /// screen's long press is a right click, and the finger is still pressing
    /// the left button SDL made of it - read as held, it would turn the right
    /// click into both buttons, which is walking forward.
    void setMouseButtonMasked(int button, bool masked);
    /// Reads a button as up, wherever it is held from, until it is let go of:
    /// its press has been spent on something that nothing else may hear.
    void setMouseButtonSpent(int button, bool spent);
    /// Reads a key as up, wherever it is held from, until it is let go of: a
    /// frame of the interface or an edit box has taken its press, and a key
    /// that one takes goes nowhere else (0x004943c0, with the release going to
    /// the same one, 0x00494490). Only the state read here - what is told of
    /// the press and of the release still is.
    void setKeySpent(SDL_Scancode key);
    /// Where a scripted test run puts the pointer (__WoweeTestMouse, only
    /// with WOWEE_AUTO_LUA), or the MCP server's mouse does, read in place of
    /// the device's; cleared to give the device's back.
    void setVirtualMousePosition(const glm::vec2& at) { virtualMousePos_ = at; hasVirtualMousePos_ = true; }
    void clearVirtualMousePosition() { hasVirtualMousePos_ = false; }
    [[nodiscard]] std::optional<glm::vec2> virtualMousePosition() const {
        return hasVirtualMousePos_ ? std::optional<glm::vec2>(virtualMousePos_) : std::nullopt;
    }
    [[nodiscard]] bool isKeyJustPressed(SDL_Scancode key) const;

    // Mouse
    [[nodiscard]] bool isMouseButtonPressed(int button) const;
    [[nodiscard]] bool isMouseButtonJustPressed(int button) const;
    [[nodiscard]] bool isMouseButtonJustReleased(int button) const;

    [[nodiscard]] glm::vec2 getMousePosition() const { return mousePosition; }
    [[nodiscard]] glm::vec2 getMouseDelta() const { return mouseDelta; }

    [[nodiscard]] bool isMouseLocked() const { return mouseLocked; }

    /// The last key, button, wheel or movement of the pointer (0x00b499a4).
    void noteHardwareEvent();
    [[nodiscard]] uint32_t idleMs() const;

private:
    Input() = default;
    ~Input() = default;
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;

    static constexpr int NUM_KEYS = SDL_SCANCODE_COUNT;
    static constexpr int NUM_MOUSE_BUTTONS = 8;

    std::array<bool, NUM_KEYS> currentKeyState = {};
    std::array<bool, NUM_KEYS> virtualKeyState = {};
    std::array<bool, NUM_KEYS> spentKeyState = {};
    std::array<bool, NUM_KEYS> previousKeyState = {};

    std::array<bool, NUM_MOUSE_BUTTONS> currentMouseState = {};
    std::array<bool, NUM_MOUSE_BUTTONS> virtualMouseState = {};
    std::array<bool, NUM_MOUSE_BUTTONS> maskedMouseState = {};
    std::array<bool, NUM_MOUSE_BUTTONS> spentMouseState = {};
    std::array<bool, NUM_MOUSE_BUTTONS> previousMouseState = {};

    glm::vec2 mousePosition = glm::vec2(0.0f);
    glm::vec2 virtualMousePos_ = glm::vec2(0.0f);
    bool hasVirtualMousePos_ = false;
    glm::vec2 previousMousePosition = glm::vec2(0.0f);
    glm::vec2 mouseDelta = glm::vec2(0.0f);
    bool mouseLocked = false;
    double lastHardwareEvent_ = 0.0;
};

} // namespace core
} // namespace wowee
