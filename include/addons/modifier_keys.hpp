#pragma once

#include "addons/binding_lookup.hpp"

#include <SDL3/SDL_keyboard.h>

#include <cstdint>
#include <optional>
#include <string_view>

// The modifier keys as the interface is told of them: IsShiftKeyDown and its
// like, IsModifiedClick, a macro's [mod].
namespace wowee::addons {

/// The modifiers of the binding whose script runs, none outside one.
inline std::optional<uint32_t> bindingModifiers;

/// The six held on the keyboard, as 0x00512d20 numbers them.
inline uint32_t heldModifiers() {
    const SDL_Keymod m = SDL_GetModState();
    uint32_t held = 0;
    if ((m & SDL_KMOD_LSHIFT) != 0) held |= kLeftShift;
    if ((m & SDL_KMOD_RSHIFT) != 0) held |= kRightShift;
    if ((m & SDL_KMOD_LCTRL) != 0) held |= kLeftCtrl;
    if ((m & SDL_KMOD_RCTRL) != 0) held |= kRightCtrl;
    if ((m & SDL_KMOD_LALT) != 0) held |= kLeftAlt;
    if ((m & SDL_KMOD_RALT) != 0) held |= kRightAlt;
    return held;
}

/// 0x00512d20: those held, but in a binding's script only the ones the
/// binding's own key did not take.
inline uint32_t interfaceModifiers() { return bindingModifiers ? *bindingModifiers : heldModifiers(); }

/// Around the script of the binding a key with these modifiers ran
/// (0x00563150: +0x12c, +0x130).
class BindingModifiers {
public:
    explicit BindingModifiers(std::string_view key) : before_(bindingModifiers) {
        bindingModifiers = modifiersLeftBy(heldModifiers(), key);
    }
    ~BindingModifiers() { bindingModifiers = before_; }
    BindingModifiers(const BindingModifiers&) = delete;
    BindingModifiers& operator=(const BindingModifiers&) = delete;

private:
    std::optional<uint32_t> before_;
};

}  // namespace wowee::addons
