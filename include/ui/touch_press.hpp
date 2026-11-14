#pragma once

// The left button SDL makes of the first finger, held back until it is known
// what the press is.
//
// A finger on the interface, or on the world's camera stick, is a tap, a push
// or a long press, and only time tells which. SDL presses the left button the
// moment the finger lands. Passed on at once, that press was in the interface
// and the world before the long press could turn into a right click, and
// letting it go again at the long press was a left click: a unit frame
// targeted its unit, then opened its menu. A desktop right click targets
// nothing, and a long press stands in for one.
//
// So the press is held: nothing sees a left button until the finger lifts (a
// tap - the press and the release are let through together, a click), moves
// (a push - the press is let through, and the finger drags), or stays put long
// enough (a long press - the press is dropped, and nobody ever saw it). A
// finger that is anything else - the movement stick, a press outside the safe
// area, any press outside the world - has its press let through as it came.
//
// Apart from the events, ImGui and core::Input that TouchControls drives, so
// that what a press becomes can be asked without them.

#include <cstdint>

namespace wowee::ui::touch {

/// What to do with the left button's events.
enum class PressStep : uint8_t {
    /// Nothing to do.
    None,
    /// Pass the event on as it came.
    Pass,
    /// Keep the event back: nothing sees it.
    Swallow,
    /// Let the press that was kept back through now, ahead of whatever comes
    /// after; the finger is still down.
    Deliver,
    /// The finger lifted: let the press that was kept back through, then this
    /// release - a click.
    DeliverClick,
};

class HeldPress {
public:
    /// SDL's left button went down for the finger that is the mouse. Kept
    /// back in the world, unless the finger has already been seen to be one
    /// that cannot be a long press.
    PressStep buttonDown(bool inWorld) {
        const Expect expect = expect_;
        expect_ = Expect::Unknown;
        if (!inWorld || expect == Expect::Pass) {
            state_ = State::Passed;
            return PressStep::Pass;
        }
        state_ = expect == Expect::Hold ? State::Held : State::Pending;
        return PressStep::Swallow;
    }

    /// The finger that is the mouse landed, on something a long press can be
    /// made of (the interface, the camera stick) or not. SDL sends the button
    /// ahead of the finger, but either order is taken.
    PressStep fingerDown(bool longPressable) {
        if (state_ == State::Pending) {
            if (longPressable) {
                state_ = State::Held;
                return PressStep::None;
            }
            state_ = State::Passed;
            return PressStep::Deliver;
        }
        if (state_ == State::Idle) expect_ = longPressable ? Expect::Hold : Expect::Pass;
        return PressStep::None;
    }

    /// The finger moved far enough to be a push, or became half of a pinch:
    /// the press is a press after all.
    PressStep moved() {
        if (state_ != State::Held && state_ != State::Pending) return PressStep::None;
        state_ = State::Passed;
        return PressStep::Deliver;
    }

    /// The finger has been still long enough. True when the press was being
    /// held, which it now never will be: the long press is a right click and
    /// nothing else.
    bool longPress() {
        if (state_ != State::Held) return false;
        state_ = State::Dropped;
        return true;
    }

    /// SDL's left button went up: the finger lifted.
    PressStep buttonUp() {
        const State was = state_;
        state_ = State::Idle;
        expect_ = Expect::Unknown;
        switch (was) {
        case State::Pending:
        case State::Held: return PressStep::DeliverClick;
        case State::Dropped: return PressStep::Swallow;
        case State::Idle:
        case State::Passed: break;
        }
        return PressStep::Pass;
    }

    /// Once a frame, after the events. A press whose finger never showed up
    /// is let through rather than kept for ever.
    PressStep update() {
        if (state_ != State::Pending) return PressStep::None;
        state_ = State::Passed;
        return PressStep::Deliver;
    }

    /// Forgets the press: the controls were reset with a finger down.
    void reset() {
        state_ = State::Idle;
        expect_ = Expect::Unknown;
    }

    /// Whether a press is being kept from everything - held, or dropped by a
    /// long press - so that core::Input must not read SDL's button either.
    [[nodiscard]] bool keptBack() const {
        return state_ == State::Pending || state_ == State::Held || state_ == State::Dropped;
    }
    /// Whether the press is held, still able to become a long press.
    [[nodiscard]] bool held() const { return state_ == State::Held; }

private:
    enum class State : uint8_t { Idle, Pending, Held, Passed, Dropped };
    enum class Expect : uint8_t { Unknown, Hold, Pass };
    State state_ = State::Idle;
    Expect expect_ = Expect::Unknown;
};

}  // namespace wowee::ui::touch
