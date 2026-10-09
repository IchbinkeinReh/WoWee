#pragma once

#include <cstdint>

namespace wowee {
namespace ui {

/// The target's nameplate cast bar, as NamePlateFrame.cpp keeps it.
///
/// The unit hands it a cast (0x0098f040, from 0x00720e50 on each cast event)
/// and the frame runs the bar on by itself every frame (0x0098e9f0): a cast
/// fills up, a channel drains. When it gets there the bar turns green and
/// everything on it fades out over a second. A cast that fails or is
/// interrupted clears the unit's cast first (0x00726200), which hands the
/// plate no spell, and that hides the bar at once (0x0098e7e0).
struct PlateCastBar {
    enum class Colour : uint8_t { Orange, Green, Red };

    bool shown = false;
    float min = 0.0f, max = 0.0f, value = 0.0f;
    bool running = false;          // +0x2f4
    bool channel = false;          // +0x2f5
    float fade = 0.0f;             // +0x2fc
    bool notInterruptible = false; // the shield in place of the border
    uint32_t spellId = 0;
    Colour colour = Colour::Orange;
    /// Every texture's vertex alpha; the fade multiplies it frame by frame.
    uint8_t alpha = 255;

    /// 0x0098e7e0.
    void hide() {
        shown = false;
        running = false;
        fade = 0.0f;
    }

    /// 0x0098f040. `spellShown` is false for no spell, one not in Spell.dbc
    /// and a trade skill (Attributes 0x20), which all hide the bar.
    void set(float lo, float hi, float v, uint32_t spell, bool spellShown, bool isChannel, bool shield) {
        if (hi < v) return;  // past its end: left as it is
        min = lo;
        max = hi;
        value = v;
        channel = isChannel;
        if (spell == 0 || !spellShown) {
            hide();
            return;
        }
        spellId = spell;
        notInterruptible = shield;
        shown = true;
        alpha = 255;
        // Handed over already at its end: red, fading.
        if (isChannel ? lo == v : hi == v) {
            fade = 1.0f;
            colour = Colour::Red;
            running = false;
        } else {
            colour = Colour::Orange;
            running = true;
            fade = 0.0f;
        }
    }

    /// 0x0098e9f0, the cast bar's part. False while the bar is off.
    void update(float dt) {
        if (!shown) return;
        if (fade > 0.0f) {
            fade -= dt;
            if (fade > 0.0f) {
                alpha = static_cast<uint8_t>(static_cast<float>(alpha) * fade);
                return;
            }
            hide();
            return;
        }
        if (!running) {
            hide();
            return;
        }
        if (!channel) {
            value += dt;
            if (value > max) {
                value = max;
                fade = 1.0f;
                colour = Colour::Green;
                running = false;
            }
        } else {
            value -= dt;
            if (value < min) {
                value = min;
                fade = 1.0f;
                colour = Colour::Green;
                running = false;
            }
        }
    }

    /// How full the bar is drawn.
    [[nodiscard]] float filled() const {
        return max > min ? (value - min) / (max - min) : 0.0f;
    }
};

}  // namespace ui
}  // namespace wowee
