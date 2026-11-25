#pragma once

namespace wowee::addons {

/// The sets of key bindings: the default keys 0, the account's 1 and the
/// character's 2.

/// Which is read where `asked` is (0x00562b80): it or, while nothing is saved in
/// it, the one below.
[[nodiscard]] constexpr int bindingSetRead(int asked, bool accountSaved, bool characterSaved) {
    if (asked >= 2 && characterSaved) return 2;
    return asked >= 1 && accountSaved ? 1 : 0;
}

/// The one a character begins with (0x00562d60): its own where it has one.
[[nodiscard]] constexpr int bindingSetAtLogin(bool characterSaved) { return characterSaved ? 2 : 1; }

/// The one SaveBindings makes the current (0x005640c0); the account's does away
/// with the character's. 0 for an argument that is neither.
[[nodiscard]] constexpr int bindingSetSaved(double which) {
    const int set = static_cast<int>(which);
    return set == 1 || set == 2 ? set : 0;
}

}  // namespace wowee::addons
