#pragma once

#include <SDL3/SDL.h>

#include "ui/touch_press.hpp"

#include <cstdint>

namespace wowee {
namespace rendering { class CameraController; }
namespace ui {

/**
 * The on-screen controls a phone needs, and nothing a desktop does.
 *
 * A touch screen already drives most of this client. SDL reports the first
 * finger as a mouse, so taps press buttons and select targets, and every panel,
 * slider and action button works without knowing a finger touched it. Three
 * things a finger cannot do are hold W, turn the view the way a held mouse
 * button does, and turn a wheel. Those are what this adds.
 *
 * It reads the finger events SDL sends alongside the mouse ones, and claims
 * only what it needs. The thumbs split the display's safe area down the
 * middle: a thumb landing in the left half becomes the movement stick, one in
 * the right half the camera stick, each the full height of its half. A press
 * on the interface - an action button, the minimap, a unit frame, one of this
 * client's own windows - is the interface's and never becomes a stick. A
 * second finger in the half whose stick is held makes a pinch with it, which is
 * the camera zoom.
 *
 * A finger held still is a right click, which a touch screen otherwise has no
 * way to make: the menu on a unit frame or a name in chat - where Invite is -
 * and interacting with whatever in the world is under the finger.
 *
 * The first finger is also the one SDL is reporting as a held mouse button,
 * which would orbit the camera the whole time it was down. So on a touch
 * screen the camera is told to ignore mouse motion, and turns only from here.
 */
class TouchControls {
public:
    /// The sticks and the pinch only apply in the world. On the login and
    /// character screens every touch is left to be a mouse.
    void setInWorld(bool inWorld);
    [[nodiscard]] bool isInWorld() const { return inWorld_; }

    /// True while a thumb is on the movement stick.
    [[nodiscard]] bool isStickHeld() const { return stickFingerId_ != kNoFinger; }

    /// True while a thumb is on the camera stick.
    [[nodiscard]] bool isLookStickHeld() const { return lookFingerId_ != kNoFinger && lookIsStick_; }

    /// The camera to steer. Without one the camera stick does nothing.
    void setCameraController(rendering::CameraController* camera) { camera_ = camera; }

    /// True while the camera stick is being pushed, which is when the
    /// character should face where the camera does. Not for a thumb resting on
    /// it, which may be a tap or a long press, nor for one that is half of a
    /// pinch, which zooms and does not turn.
    [[nodiscard]] bool isSteering() const {
        return isLookStickHeld() && lookMoved_ && pinchA_ != lookFingerId_;
    }

    /// The display changed under the controls: a fold, a rotation, the window
    /// moving to another screen, or the safe area changing. The stick's size is
    /// worked out again from the density, and a finger held through it is let
    /// go - it is no longer where it was.
    void displayChanged() { cachedRadius_ = 0.0f; reset(); }

    /// Reads the finger events, and the left button SDL makes of the first
    /// finger. True when the event is kept back: the caller passes it on to
    /// nothing else.
    [[nodiscard]] bool handleEvent(const SDL_Event& event, int windowWidth, int windowHeight);

    /// A left button event let through late (touch::HeldPress), to be handled
    /// ahead of anything SDL has queued since. False when there is none.
    bool takeReplay(SDL_Event& event);

    /// Applies the movement stick to the movement keys, turns the view by the
    /// camera stick, and turns a finger held still into a right click. Once a
    /// frame, after events.
    void update();

    /// Draws the sticks where the thumbs put them. Nothing when idle.
    void draw() const;

    /// Drops the sticks and releases the keys.
    void reset();

private:
    static constexpr SDL_FingerID kNoFinger = -1;

    /// How far the thumb travels for a full deflection, in Android's density
    /// independent pixels. A radius in raw pixels is a different size on every
    /// phone: 150 of them is a third of an inch on this one, which is why the
    /// first version was unusable.
    static constexpr float kStickRadiusDp = 130.0f;

    /// Below this the stick reads as centred, so a resting thumb does not walk.
    /// Sideways takes more deflection than forward, so that walking straight
    /// ahead does not need a steady hand.
    static constexpr float kWalkDeadzone = 0.30f;
    static constexpr float kStrafeDeadzone = 0.45f;
    /// The camera stick's centre, the inner ring both sticks draw: a thumb
    /// inside it does not turn the view.
    static constexpr float kLookDeadzone = kStrafeDeadzone;
    /// How fast a held camera stick turns the view at full deflection, in the
    /// pixels a mouse drag would have moved.
    static constexpr float kLookStickPixelsPerSecond = 900.0f;
    /// How far back inside the deadzone a direction has to fall before it lets
    /// go, so a thumb resting on the edge does not stutter.
    static constexpr float kReleaseHysteresis = 0.10f;
    /// What a pinch has to cover before it is worth a notch of the wheel.
    static constexpr float kPinchPixelsPerNotch = 90.0f;
    /// How far a finger travels before it counts as a push rather than a tap,
    /// so that selecting a target does not also swing the view.
    static constexpr float kLookSlopPixels = 16.0f;
    /// How long a finger stays put before it is a right click. Long enough that
    /// a tap is never one, short enough not to feel like waiting.
    static constexpr uint64_t kLongPressMs = 500;
    /// When the ring that shows a long press coming starts to draw, so a plain
    /// tap does not flash one.
    static constexpr uint64_t kLongPressHintMs = 150;

    void setMovementKeys(bool forward, bool back, bool left, bool right) const;
    /// A finger on the interface or on the camera stick, held still long
    /// enough, becomes a right click.
    void updateLongPress();
    /// Lets go of what a long press is holding.
    void endLongPress();
    /// Ends a pinch. The stick it was made with starts again from where its
    /// finger is now, rather than jumping by however far the pinch moved it.
    void endPinch();
    /// Whether the interface has something at this point that takes the press:
    /// one of this client's windows, or a FrameXML frame that takes the mouse.
    [[nodiscard]] static bool pointOverInterface(float x, float y);
    /// Full deflection in real pixels, from the display's density.
    [[nodiscard]] float stickRadius() const;
    /// Held with hysteresis: a direction already on lets go later than it came on.
    [[nodiscard]] static bool held(float axis, float threshold, bool wasOn);

    bool inWorld_ = false;
    rendering::CameraController* camera_ = nullptr;

    // The finger SDL reports as the mouse: the first one down while no other
    // is. A long press clicks where the mouse is, so only that finger's can.
    SDL_FingerID mouseFingerId_ = kNoFinger;

    // The movement stick: the left thumb.
    SDL_FingerID stickFingerId_ = kNoFinger;
    float stickOriginX_ = 0.0f, stickOriginY_ = 0.0f;
    float stickAtX_ = 0.0f, stickAtY_ = 0.0f;     // where the thumb is now
    float stickX_ = 0.0f, stickY_ = 0.0f;         // -1..1

    // The right thumb: the camera stick when it landed on the world, a tap or
    // a long press when it landed on the interface.
    SDL_FingerID lookFingerId_ = kNoFinger;
    bool lookIsStick_ = false;
    float lookOriginX_ = 0.0f, lookOriginY_ = 0.0f;
    float lookX_ = 0.0f, lookY_ = 0.0f;           // where the finger is now
    float lookStickX_ = 0.0f, lookStickY_ = 0.0f; // -1..1
    bool lookMoved_ = false;
    float lookTravel_ = 0.0f;
    uint64_t lookDownMs_ = 0;
    uint64_t lastUpdateNs_ = 0;
    // The long press: whether it has fired, whether the right button it
    // pressed is still down (it is let go the next frame), and whether the
    // left button the finger is holding is being read as up.
    bool longPressFired_ = false;
    bool rightHeld_ = false;
    bool leftMasked_ = false;

    // The first finger's left button, kept back until the press is a tap, a
    // push or a long press: the press SDL sent, the events to let through
    // late (at most a press and its release), and whether the one being
    // handled is one of those. A tap's press for core::Input, which reads
    // SDL's buttons and not its events: held for the one frame.
    touch::HeldPress leftPress_;
    SDL_Event heldDown_{};
    SDL_Event replay_[2]{};
    int replayCount_ = 0;
    int replayNext_ = 0;
    bool replaying_ = false;
    bool tapPressed_ = false;

    /// Reads the touch mouse's left button events. True to keep the event back.
    bool handleTouchButton(const SDL_Event& event);
    /// Queues the press kept back, and the release with it for a tap.
    void replayHeldPress(const SDL_Event* release);
    /// The press kept back is a press after all: queued, and core::Input
    /// reads SDL's button again.
    void letHeldPressThrough();
    /// Lets core::Input read SDL's left button again.
    void unmaskLeft();
    /// A finger just landed and has been made whatever it is: if it is the
    /// mouse's, its press is held for a long press or let through.
    void settleMousePress(SDL_FingerID id);

    // The pinch: a stick's finger and a second one in the same half. While it
    // lasts that stick is held centred, since both fingers are zooming.
    SDL_FingerID pinchA_ = kNoFinger, pinchB_ = kNoFinger;
    bool pinchOfMoveStick_ = false;
    float pinchBX_ = 0.0f, pinchBY_ = 0.0f;
    float lastPinchSpacing_ = 0.0f;

    // What the stick asked for last frame, for the hysteresis.
    mutable bool wasForward_ = false, wasBack_ = false, wasLeft_ = false, wasRight_ = false;

    mutable float cachedRadius_ = 0.0f;
};

/// The one the client uses. One screen, one pair of thumbs.
TouchControls& touchControls();

}  // namespace ui
}  // namespace wowee
