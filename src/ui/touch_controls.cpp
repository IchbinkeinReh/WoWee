#include "ui/touch_controls.hpp"

#include "core/input.hpp"
#include "core/logger.hpp"
#include "rendering/camera_controller.hpp"
#include "ui/framexml_takeover.hpp"
#include "ui/safe_area.hpp"
#include "ui/touch_zones.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>

namespace wowee {
namespace ui {

float TouchControls::stickRadius() const {
    if (cachedRadius_ > 0.0f) return cachedRadius_;
    // SDL3 dropped SDL_GetDisplayDPI for a content scale, which is the same
    // number this was deriving: dpi over Android's 160 baseline is 1x.
    float density = 2.0f;
    if (const float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        scale > 0.0f) {
        density = scale;
    }
    cachedRadius_ = kStickRadiusDp * std::max(density, 1.0f);
    return cachedRadius_;
}

bool TouchControls::held(float axis, float threshold, bool wasOn) {
    const float release = std::max(threshold - kReleaseHysteresis, 0.05f);
    return axis > (wasOn ? release : threshold);
}

TouchControls& touchControls() {
    static TouchControls instance;
    return instance;
}

void TouchControls::setInWorld(bool inWorld) {
    if (inWorld_ == inWorld) return;
    inWorld_ = inWorld;
    if (!inWorld_) reset();
}

void TouchControls::setMovementKeys(bool forward, bool back, bool left, bool right) const {
    auto& input = core::Input::getInstance();
    input.setVirtualKey(SDL_SCANCODE_W, forward);
    input.setVirtualKey(SDL_SCANCODE_S, back);
    // Q and E rather than A and D. With the right button up - which it always
    // is on a phone - this client turns the character on A and D and strafes on
    // Q and E. Turning meant the stick swung the view as well as the feet,
    // which is not what a stick pushed sideways should do.
    input.setVirtualKey(SDL_SCANCODE_Q, left);
    input.setVirtualKey(SDL_SCANCODE_E, right);
}

bool TouchControls::pointOverInterface(float x, float y) {
    // This client's own windows, asked the way ImGui itself finds the hovered
    // one, at the point rather than where the cursor last was: the finger has
    // only just gone down, and ImGui has not yet seen it there. Between frames
    // the window list is last frame's, which is what is on the screen.
    if (ImGui::GetCurrentContext()) {
        ImGuiWindow* hovered = nullptr;
        ImGui::FindHoveredWindowEx(ImVec2(x, y), false, &hovered, nullptr);
        if (hovered) return true;
    }
    // FrameXML draws into ImGui's background list, so ImGui knows nothing of
    // its frames; the widget tree is asked about the same point.
    return frameXmlPointOwnsMouse(x, y);
}

void TouchControls::handleEvent(const SDL_Event& event, int windowWidth, int windowHeight) {
    if (event.type != SDL_EVENT_FINGER_DOWN && event.type != SDL_EVENT_FINGER_MOTION &&
        event.type != SDL_EVENT_FINGER_UP && event.type != SDL_EVENT_FINGER_CANCELED) {
        return;
    }

    const float w = static_cast<float>(std::max(windowWidth, 1));
    const float h = static_cast<float>(std::max(windowHeight, 1));
    const float x = event.tfinger.x * w;
    const float y = event.tfinger.y * h;
    const SDL_FingerID id = event.tfinger.fingerID;
    const bool down = event.type == SDL_EVENT_FINGER_DOWN;
    const bool up = event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED;

    // Which finger SDL makes the mouse is tracked on every screen, so it is
    // right when the world appears with a finger already down.
    if (down && mouseFingerId_ == kNoFinger) mouseFingerId_ = id;
    if (up && id == mouseFingerId_) mouseFingerId_ = kNoFinger;

    if (!inWorld_) return;

    if (down) {
        // Each stick owns one half of the safe area, the full height of it.
        const safe_area::Insets& inset = safe_area::current();
        const touch::Area area =
            touch::usableArea(w, h, inset.left, inset.right, inset.top, inset.bottom);

        // A press outside the safe area - on the gesture bar, in the cutout's
        // corner - is the system's or nobody's. It is left to be a mouse, and
        // never becomes a stick, a tap the controls track or half of a pinch.
        if (!touch::inside(x, y, area)) {
            LOG_DEBUG("touch: finger down at ", x, ",", y, " outside the safe area");
            return;
        }

        // The interface is asked first, so a bag, an action button or the
        // minimap under the thumb still gets the press. A stick that ate those
        // would be maddening. The press stays a tap or a long press.
        if (pointOverInterface(x, y)) {
            LOG_DEBUG("touch: finger down at ", x, ",", y, " over the interface");
            if (lookFingerId_ == kNoFinger) {
                lookFingerId_ = id;
                lookIsStick_ = false;
                lookX_ = lookOriginX_ = x;
                lookY_ = lookOriginY_ = y;
                lookStickX_ = lookStickY_ = 0.0f;
                lookMoved_ = false;
                lookTravel_ = 0.0f;
                lookDownMs_ = SDL_GetTicks();
                longPressFired_ = false;
            }
            return;
        }

        const bool leftHalf = touch::halfAt(x, area) == touch::Half::Move;

        if (leftHalf && stickFingerId_ == kNoFinger) {
            LOG_DEBUG("touch: move stick at ", x, ",", y);
            stickFingerId_ = id;
            stickOriginX_ = stickAtX_ = x;
            stickOriginY_ = stickAtY_ = y;
            stickX_ = stickY_ = 0.0f;
            return;
        }
        if (!leftHalf && lookFingerId_ == kNoFinger) {
            LOG_DEBUG("touch: camera stick at ", x, ",", y);
            lookFingerId_ = id;
            lookIsStick_ = true;
            lookX_ = lookOriginX_ = x;
            lookY_ = lookOriginY_ = y;
            lookStickX_ = lookStickY_ = 0.0f;
            lookMoved_ = false;
            lookTravel_ = 0.0f;
            lookDownMs_ = SDL_GetTicks();
            longPressFired_ = false;
            return;
        }

        // A second finger in a half whose stick is held: a pinch with that
        // stick's finger. Two thumbs, one in each half, are walking and
        // looking, never a pinch.
        if (pinchA_ == kNoFinger) {
            const bool withMove = leftHalf && stickFingerId_ != kNoFinger;
            const bool withLook = !leftHalf && isLookStickHeld();
            if (withMove || withLook) {
                pinchA_ = withMove ? stickFingerId_ : lookFingerId_;
                pinchOfMoveStick_ = withMove;
                pinchB_ = id;
                pinchBX_ = x;
                pinchBY_ = y;
                const float ax = withMove ? stickAtX_ : lookX_;
                const float ay = withMove ? stickAtY_ : lookY_;
                lastPinchSpacing_ = std::hypot(ax - x, ay - y);
                LOG_DEBUG("touch: pinch with the ", withMove ? "move" : "camera", " stick");
            }
        }
        return;
    }

    if (up) {
        if (id == pinchA_ || id == pinchB_) endPinch();
        if (id == stickFingerId_) {
            stickFingerId_ = kNoFinger;
            stickX_ = stickY_ = 0.0f;
            setMovementKeys(false, false, false, false);
        }
        if (id == lookFingerId_) {
            lookFingerId_ = kNoFinger;
            lookIsStick_ = false;
            lookMoved_ = false;
            lookStickX_ = lookStickY_ = 0.0f;
            // The finger is off, so SDL has let go of its left button and
            // there is nothing left to read as up.
            if (leftMasked_) {
                core::Input::getInstance().setMouseButtonMasked(SDL_BUTTON_LEFT, false);
                leftMasked_ = false;
            }
        }
        return;
    }

    // SDL_EVENT_FINGER_MOTION
    const float radius = stickRadius();
    if (id == stickFingerId_) {
        stickAtX_ = x;
        stickAtY_ = y;
        if (pinchA_ != id) {
            stickX_ = std::clamp((x - stickOriginX_) / radius, -1.0f, 1.0f);
            stickY_ = std::clamp((y - stickOriginY_) / radius, -1.0f, 1.0f);
        }
    } else if (id == lookFingerId_) {
        // Distance travelled, not distance from where the finger went down: a
        // slow push arrives in many small steps and would never clear a
        // per-event threshold.
        if (!lookMoved_) {
            lookTravel_ += std::hypot(x - lookX_, y - lookY_);
            if (lookTravel_ >= kLookSlopPixels) lookMoved_ = true;
        }
        lookX_ = x;
        lookY_ = y;
        if (lookIsStick_ && pinchA_ != id) {
            lookStickX_ = std::clamp((x - lookOriginX_) / radius, -1.0f, 1.0f);
            lookStickY_ = std::clamp((y - lookOriginY_) / radius, -1.0f, 1.0f);
        }
    } else if (id == pinchB_) {
        pinchBX_ = x;
        pinchBY_ = y;
    } else {
        return;
    }

    if (pinchA_ == kNoFinger || pinchB_ == kNoFinger || (id != pinchA_ && id != pinchB_)) return;
    const float ax = pinchOfMoveStick_ ? stickAtX_ : lookX_;
    const float ay = pinchOfMoveStick_ ? stickAtY_ : lookY_;
    const float spacing = std::hypot(ax - pinchBX_, ay - pinchBY_);
    const float moved = spacing - lastPinchSpacing_;
    if (std::abs(moved) < kPinchPixelsPerNotch) return;
    lastPinchSpacing_ = spacing;

    // Fingers spreading pulls the camera in, the way a wheel forward does. Sent
    // as a wheel event so it goes through the same clamping and the same
    // setting as a wheel, rather than reaching into the camera here.
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.timestamp = SDL_GetTicks();
    // One float axis in SDL3: the integer y and the precise one were the
    // same measurement at two precisions, and only the finer one survived.
    wheel.wheel.y = moved / kPinchPixelsPerNotch;
    SDL_PushEvent(&wheel);
}

void TouchControls::endPinch() {
    if (pinchA_ == stickFingerId_ && stickFingerId_ != kNoFinger) {
        stickOriginX_ = stickAtX_;
        stickOriginY_ = stickAtY_;
        stickX_ = stickY_ = 0.0f;
    }
    if (pinchA_ == lookFingerId_ && lookFingerId_ != kNoFinger) {
        lookOriginX_ = lookX_;
        lookOriginY_ = lookY_;
        lookStickX_ = lookStickY_ = 0.0f;
    }
    pinchA_ = pinchB_ = kNoFinger;
}

void TouchControls::updateLongPress() {
    auto& input = core::Input::getInstance();
    // Pressed last frame, let go this one: a click, which is what both the
    // world's targeting and the interface's OnClick wait for the release of.
    if (rightHeld_) {
        input.setVirtualMouseButton(SDL_BUTTON_RIGHT, false);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        rightHeld_ = false;
    }
    // Only the finger SDL is reporting as the mouse: the click lands where the
    // mouse is, and with the other thumb on the movement stick that is under
    // the other thumb.
    if (!inWorld_ || longPressFired_ || lookFingerId_ == kNoFinger || lookMoved_ ||
        lookFingerId_ != mouseFingerId_ || pinchA_ != kNoFinger) {
        return;
    }
    if (SDL_GetTicks() - lookDownMs_ < kLongPressMs) return;
    longPressFired_ = true;
    LOG_DEBUG("touch: long press at ", lookX_, ",", lookY_, " - right click");

    // The left button first. The finger has held it since it went down, and
    // the world reads both buttons at once as walking forward - and the
    // world's right click only counts with the left one up.
    //
    // Both channels, as the gamepad presses its buttons: the world polls
    // core::Input, and every panel asks ImGui. ImGui hands the queued changes
    // out a frame at a time, so the left button lets go before the right one
    // goes down, and the interface sees a right click on its own.
    input.setMouseButtonMasked(SDL_BUTTON_LEFT, true);
    leftMasked_ = true;
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);

    input.setVirtualMouseButton(SDL_BUTTON_RIGHT, true);
    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, true);
    rightHeld_ = true;
}

void TouchControls::endLongPress() {
    auto& input = core::Input::getInstance();
    if (rightHeld_) {
        input.setVirtualMouseButton(SDL_BUTTON_RIGHT, false);
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Right, false);
        rightHeld_ = false;
    }
    if (leftMasked_) {
        input.setMouseButtonMasked(SDL_BUTTON_LEFT, false);
        leftMasked_ = false;
    }
    longPressFired_ = false;
}

void TouchControls::update() {
    updateLongPress();

    // The camera stick turns at a rate, so it needs the time since the last
    // update; a frame held up by a load is not a second and a half of turning.
    const uint64_t nowNs = SDL_GetTicksNS();
    const float seconds = lastUpdateNs_ == 0
        ? 0.0f
        : std::min(static_cast<float>(nowNs - lastUpdateNs_) * 1e-9f, 0.1f);
    lastUpdateNs_ = nowNs;

    if (inWorld_ && isLookStickHeld() && camera_ &&
        !(pinchA_ != kNoFinger && !pinchOfMoveStick_)) {
        const touch::Turn turn = touch::lookStickTurn(lookStickX_, lookStickY_, kLookDeadzone,
                                                      kLookStickPixelsPerSecond, seconds);
        if (turn.dx != 0.0f || turn.dy != 0.0f) camera_->applyLookDelta(turn.dx, turn.dy);
    }

    if (!inWorld_ || stickFingerId_ == kNoFinger ||
        (pinchA_ != kNoFinger && pinchOfMoveStick_)) {
        wasForward_ = wasBack_ = wasLeft_ = wasRight_ = false;
        setMovementKeys(false, false, false, false);
        return;
    }
    // Up is forward, sideways is a strafe: the stick moves the character and
    // never the view, which is the other thumb's job.
    //
    // Sideways asks for more of the stick than forward, and each direction lets
    // go later than it took hold, so that holding a heading does not need the
    // thumb kept still to the pixel.
    wasForward_ = held(-stickY_, kWalkDeadzone, wasForward_);
    wasBack_    = held( stickY_, kWalkDeadzone, wasBack_);
    wasLeft_    = held(-stickX_, kStrafeDeadzone, wasLeft_);
    wasRight_   = held( stickX_, kStrafeDeadzone, wasRight_);
    setMovementKeys(wasForward_, wasBack_, wasLeft_, wasRight_);
}

void TouchControls::draw() const {
    if (!inWorld_) return;

    // A ring closing round a finger held still, so a long press can be seen
    // coming and the finger lifted in time if a right click was not meant. In
    // front of everything, because the finger is usually on a frame.
    if (lookFingerId_ != kNoFinger && lookFingerId_ == mouseFingerId_ && !lookMoved_ &&
        !longPressFired_ && pinchA_ == kNoFinger) {
        const uint64_t heldMs = SDL_GetTicks() - lookDownMs_;
        if (heldMs >= kLongPressHintMs) {
            if (ImDrawList* fg = ImGui::GetForegroundDrawList()) {
                const float t = std::min(1.0f, static_cast<float>(heldMs - kLongPressHintMs) /
                                                   static_cast<float>(kLongPressMs - kLongPressHintMs));
                const float r = stickRadius() * 0.35f;
                const ImVec2 c(lookX_, lookY_);
                constexpr float kTop = -1.5707963f;
                fg->PathArcTo(c, r, kTop, kTop + t * 6.2831853f, 40);
                fg->PathStroke(IM_COL32(255, 255, 255, 170), 0, 4.0f);
            }
        }
    }

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;
    const float radius = stickRadius();
    // Both sticks get the same two rings and knob, so the pair reads as one
    // control. The inner ring is where sideways starts on the movement stick
    // and where turning starts on the camera stick.
    auto ring = [&](float ox, float oy, float sx, float sy) {
        const ImVec2 origin(ox, oy);
        const ImVec2 knob(ox + sx * radius, oy + sy * radius);
        dl->AddCircle(origin, radius, IM_COL32(255, 255, 255, 60), 48, 3.0f);
        dl->AddCircle(origin, radius * kStrafeDeadzone, IM_COL32(255, 255, 255, 35), 40, 2.0f);
        dl->AddCircleFilled(knob, radius * 0.28f, IM_COL32(255, 255, 255, 95), 32);
    };
    if (stickFingerId_ != kNoFinger) ring(stickOriginX_, stickOriginY_, stickX_, stickY_);
    // The camera stick once it is being pushed: a finger resting there may be
    // a tap or a long press, which have their own ring.
    if (isLookStickHeld() && lookMoved_) ring(lookOriginX_, lookOriginY_, lookStickX_, lookStickY_);
}

void TouchControls::reset() {
    stickFingerId_ = kNoFinger;
    lookFingerId_ = kNoFinger;
    lookIsStick_ = false;
    lookMoved_ = false;
    lookTravel_ = 0.0f;
    lookStickX_ = lookStickY_ = 0.0f;
    pinchA_ = pinchB_ = kNoFinger;
    stickX_ = stickY_ = 0.0f;
    wasForward_ = wasBack_ = wasLeft_ = wasRight_ = false;
    endLongPress();
    // Everything the stick could be holding, not just the four it uses now.
    core::Input::getInstance().clearVirtualKeys();
}

}  // namespace ui
}  // namespace wowee
