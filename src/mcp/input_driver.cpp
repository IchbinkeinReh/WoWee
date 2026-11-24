#include "mcp/input_driver.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <utility>

#include "core/input.hpp"
#include "mcp/origin.hpp"

namespace wowee::mcp {

namespace {

struct Named {
    std::string_view name;
    SDL_Scancode key;
};

/// The keys that are where they are on every keyboard, by the binding tables'
/// names for them.
constexpr std::array<Named, 37> kNamed = {{
    {"ESCAPE", SDL_SCANCODE_ESCAPE},     {"SPACE", SDL_SCANCODE_SPACE},
    {"ENTER", SDL_SCANCODE_RETURN},      {"TAB", SDL_SCANCODE_TAB},
    {"BACKSPACE", SDL_SCANCODE_BACKSPACE}, {"HOME", SDL_SCANCODE_HOME},
    {"END", SDL_SCANCODE_END},           {"PAGEUP", SDL_SCANCODE_PAGEUP},
    {"PAGEDOWN", SDL_SCANCODE_PAGEDOWN}, {"DELETE", SDL_SCANCODE_DELETE},
    {"INSERT", SDL_SCANCODE_INSERT},     {"LEFT", SDL_SCANCODE_LEFT},
    {"RIGHT", SDL_SCANCODE_RIGHT},       {"UP", SDL_SCANCODE_UP},
    {"DOWN", SDL_SCANCODE_DOWN},         {"NUMLOCK", SDL_SCANCODE_NUMLOCKCLEAR},
    {"NUMPADDIVIDE", SDL_SCANCODE_KP_DIVIDE}, {"NUMPADMULTIPLY", SDL_SCANCODE_KP_MULTIPLY},
    {"NUMPADMINUS", SDL_SCANCODE_KP_MINUS}, {"NUMPADPLUS", SDL_SCANCODE_KP_PLUS},
    {"NUMPADDECIMAL", SDL_SCANCODE_KP_PERIOD}, {"NUMPAD0", SDL_SCANCODE_KP_0},
    {"NUMPAD1", SDL_SCANCODE_KP_1},      {"NUMPAD2", SDL_SCANCODE_KP_2},
    {"NUMPAD3", SDL_SCANCODE_KP_3},      {"NUMPAD4", SDL_SCANCODE_KP_4},
    {"NUMPAD5", SDL_SCANCODE_KP_5},      {"NUMPAD6", SDL_SCANCODE_KP_6},
    {"NUMPAD7", SDL_SCANCODE_KP_7},      {"NUMPAD8", SDL_SCANCODE_KP_8},
    {"NUMPAD9", SDL_SCANCODE_KP_9},      {"LSHIFT", SDL_SCANCODE_LSHIFT},
    {"RSHIFT", SDL_SCANCODE_RSHIFT},     {"LCTRL", SDL_SCANCODE_LCTRL},
    {"RCTRL", SDL_SCANCODE_RCTRL},       {"LALT", SDL_SCANCODE_LALT},
    {"RALT", SDL_SCANCODE_RALT},
}};

}  // namespace

SDL_Scancode scancodeNamed(std::string_view wowName) {
    std::string name(wowName);
    for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const auto& n : kNamed) {
        if (n.name == name) return n.key;
    }
    if (name.size() >= 2 && name.size() <= 3 && name[0] == 'F' &&
        std::all_of(name.begin() + 1, name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        const int f = std::atoi(name.c_str() + 1);
        if (f >= 1 && f <= 12) return static_cast<SDL_Scancode>(SDL_SCANCODE_F1 + f - 1);
    }
    if (name.size() == 1) {
        // By the character on the key, which the layout places.
        const SDL_Keycode code = static_cast<SDL_Keycode>(std::tolower(static_cast<unsigned char>(name[0])));
        return SDL_GetScancodeFromKey(code, nullptr);
    }
    return SDL_SCANCODE_UNKNOWN;
}

bool parseChord(std::string_view chord, SDL_Scancode& key, SDL_Keymod& mods) {
    mods = SDL_KMOD_NONE;
    std::string rest(chord);
    for (char& c : rest) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    while (true) {
        if (rest.rfind("ALT-", 0) == 0 && rest.size() > 4) {
            mods |= SDL_KMOD_LALT;
            rest.erase(0, 4);
        } else if (rest.rfind("CTRL-", 0) == 0 && rest.size() > 5) {
            mods |= SDL_KMOD_LCTRL;
            rest.erase(0, 5);
        } else if (rest.rfind("SHIFT-", 0) == 0 && rest.size() > 6) {
            mods |= SDL_KMOD_LSHIFT;
            rest.erase(0, 6);
        } else {
            break;
        }
    }
    key = scancodeNamed(rest);
    return key != SDL_SCANCODE_UNKNOWN;
}

void InputDriver::push(SDL_Event& event) {
    if (origin_) origin_->actedByServer();
    SDL_PushEvent(&event);
}

bool InputDriver::keyHeld(SDL_Scancode key) const {
    return std::find(held_.begin(), held_.end(), key) != held_.end();
}

void InputDriver::keyDown(SDL_Scancode key, SDL_Keymod mods) {
    if (key == SDL_SCANCODE_UNKNOWN) return;
    const bool repeat = keyHeld(key);
    if (!repeat) held_.push_back(key);
    core::Input::getInstance().setVirtualKey(key, true);
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = window_ ? SDL_GetWindowID(window_) : 0;
    e.key.which = kDevice;
    e.key.scancode = key;
    e.key.key = SDL_GetKeyFromScancode(key, SDL_KMOD_NONE, false);
    e.key.mod = static_cast<SDL_Keymod>(mods_ | mods);
    e.key.down = true;
    e.key.repeat = repeat;
    push(e);
}

void InputDriver::keyUp(SDL_Scancode key, SDL_Keymod mods) {
    if (key == SDL_SCANCODE_UNKNOWN) return;
    held_.erase(std::remove(held_.begin(), held_.end(), key), held_.end());
    core::Input::getInstance().setVirtualKey(key, false);
    SDL_Event e{};
    e.type = SDL_EVENT_KEY_UP;
    e.key.windowID = window_ ? SDL_GetWindowID(window_) : 0;
    e.key.which = kDevice;
    e.key.scancode = key;
    e.key.key = SDL_GetKeyFromScancode(key, SDL_KMOD_NONE, false);
    e.key.mod = static_cast<SDL_Keymod>(mods_ | mods);
    e.key.down = false;
    push(e);
}

void InputDriver::modifiers(SDL_Keymod mods, bool down) {
    constexpr std::pair<SDL_Keymod, SDL_Scancode> kKeys[] = {
        {SDL_KMOD_LSHIFT, SDL_SCANCODE_LSHIFT},
        {SDL_KMOD_LCTRL, SDL_SCANCODE_LCTRL},
        {SDL_KMOD_LALT, SDL_SCANCODE_LALT},
    };
    for (const auto& [mod, key] : kKeys) {
        if (!(mods & mod)) continue;
        mods_ = static_cast<SDL_Keymod>(down ? (mods_ | mod) : (mods_ & ~mod));
        // What IsShiftKeyDown and the binding dispatch ask, which a pushed
        // event does not set.
        const SDL_Keymod now = SDL_GetModState();
        SDL_SetModState(static_cast<SDL_Keymod>(down ? (now | mod) : (now & ~mod)));
        if (down) keyDown(key);
        else keyUp(key);
    }
}

void InputDriver::type(std::string_view utf8) {
    if (utf8.empty()) return;
    typed_.push_back({.text = std::string(utf8), .frames = 0});
    SDL_Event e{};
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.reserved = kDevice;
    e.text.windowID = window_ ? SDL_GetWindowID(window_) : 0;
    e.text.text = typed_.back().text.c_str();
    push(e);
}

void InputDriver::moveTo(const glm::vec2& at) {
    const glm::vec2 from = pointerHeld_ ? pointer_ : core::Input::getInstance().getMousePosition();
    pointer_ = at;
    pointerHeld_ = true;
    core::Input::getInstance().setVirtualMousePosition(at);
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = window_ ? SDL_GetWindowID(window_) : 0;
    e.motion.which = kDevice;
    e.motion.x = at.x;
    e.motion.y = at.y;
    e.motion.xrel = at.x - from.x;
    e.motion.yrel = at.y - from.y;
    push(e);
}

void InputDriver::nudge(float dx, float dy) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.windowID = window_ ? SDL_GetWindowID(window_) : 0;
    e.motion.which = kDevice;
    e.motion.x = pointer_.x;
    e.motion.y = pointer_.y;
    e.motion.xrel = dx;
    e.motion.yrel = dy;
    push(e);
}

void InputDriver::button(int sdlButton, bool down, int clicks) {
    buttons_.erase(std::remove(buttons_.begin(), buttons_.end(), sdlButton), buttons_.end());
    if (down) buttons_.push_back(sdlButton);
    core::Input::getInstance().setVirtualMouseButton(sdlButton, down);
    SDL_Event e{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.windowID = window_ ? SDL_GetWindowID(window_) : 0;
    e.button.which = kDevice;
    e.button.button = static_cast<Uint8>(sdlButton);
    e.button.down = down;
    e.button.clicks = static_cast<Uint8>(clicks);
    e.button.x = pointer_.x;
    e.button.y = pointer_.y;
    push(e);
}

void InputDriver::wheel(float clicks) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.windowID = window_ ? SDL_GetWindowID(window_) : 0;
    e.wheel.which = kDevice;
    e.wheel.y = clicks;
    e.wheel.mouse_x = pointer_.x;
    e.wheel.mouse_y = pointer_.y;
    push(e);
}

void InputDriver::releasePointer() {
    pointerHeld_ = false;
    core::Input::getInstance().clearVirtualMousePosition();
}

void InputDriver::releaseAll() {
    while (!buttons_.empty()) button(buttons_.back(), false);
    if (mods_ != SDL_KMOD_NONE) modifiers(mods_, false);
    while (!held_.empty()) keyUp(held_.back());
}

bool InputDriver::filter(const SDL_Event& event, bool handsBusy) {
    const auto byHand = [&] {
        if (origin_) origin_->actedByHand();
    };
    switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
            if (event.motion.which == kDevice || !pointerHeld_) return false;
            if (handsBusy || !window_ || SDL_GetKeyboardFocus() != window_) return true;
            // Somebody plays along: the pointer is theirs again.
            releasePointer();
            return false;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.which == kDevice) return false;
            // A press is kept out of the call's own clicking; the release of
            // one from before it is let through, or it would stay pressed.
            if (handsBusy && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) return true;
            if (pointerHeld_) releasePointer();
            byHand();
            return false;
        case SDL_EVENT_MOUSE_WHEEL:
            if (event.wheel.which != kDevice && handsBusy) return true;
            return false;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.which != kDevice) byHand();
            return false;
        case SDL_EVENT_TEXT_INPUT:
            if (event.text.reserved != kDevice) byHand();
            return false;
        default:
            return false;
    }
}

void InputDriver::tick() {
    constexpr int kKeptFrames = 8;
    for (auto& t : typed_) ++t.frames;
    while (!typed_.empty() && typed_.front().frames > kKeptFrames) typed_.pop_front();
}

}  // namespace wowee::mcp
