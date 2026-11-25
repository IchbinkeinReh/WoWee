#pragma once

#include <array>
#include <cstddef>

namespace wowee::rendering {

/// The keys that move the figure, with a line being typed. What is pressed then is the line's
/// (0x004943c0), but a key is let go of by what took its press (0x00494490): one that went down
/// before is the game's until it comes up, so a figure walks on while its player types.
class KeysHeldIntoTyping {
public:
    static constexpr size_t kKeys = 12;

    /// Whether `key` is the game's and down. Asked of each key once a frame.
    bool down(size_t key, bool pressed, bool typing) {
        bool& carried = carried_[key];
        carried = pressed && (!typing || carried);
        return carried;
    }

private:
    std::array<bool, kKeys> carried_{};
};

}  // namespace wowee::rendering
