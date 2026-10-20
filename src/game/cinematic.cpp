// A cinematic's course and its fades, as the client's (game/cinematic.hpp).
#include "game/cinematic.hpp"

namespace wowee::game::cinematic {

void ScreenFade::toBlack(float seconds, Then then) {
    // 0x005ed480: any black there goes, and a new one starts from clear.
    active_ = true;
    mode_ = Mode::ToBlack;
    seconds_ = seconds;
    started_ = false;
    reached_ = false;
    then_ = then;
    alpha_ = 0.0f;
}

ScreenFade::Then ScreenFade::fromBlack(float seconds, Then then) {
    // 0x005ed080: only a screen that is faded.
    if (!active_) return Then::Nothing;
    seconds_ = seconds;
    then_ = then;
    started_ = false;
    mode_ = Mode::FromBlack;
    reached_ = false;
    if (seconds == 0.0f) {
        active_ = false;
        started_ = false;
        mode_ = Mode::Hold;
        alpha_ = 0.0f;
        const Then now = then_;
        then_ = Then::Nothing;
        return now;
    }
    return Then::Nothing;
}

ScreenFade::Then ScreenFade::frame(uint32_t nowMs) {
    if (!active_) {
        alpha_ = 0.0f;
        return Then::Nothing;
    }
    // 0x005ed120: black while it holds; else by the time since its first
    // frame, and fully there once that reaches its length.
    float alpha = 1.0f;
    bool justReached = false;
    if (mode_ != Mode::Hold) {
        float elapsed = 0.0f;
        if (!started_) {
            startMs_ = nowMs;
        } else {
            elapsed = static_cast<float>(nowMs - startMs_) * 0.001f;
        }
        if (seconds_ > elapsed) {
            const float part = elapsed / seconds_;
            alpha = mode_ == Mode::ToBlack ? part : 1.0f - part;
        } else {
            justReached = true;
            alpha = mode_ == Mode::ToBlack ? 1.0f : 0.0f;
        }
    }
    alpha_ = alpha;
    started_ = true;
    if (!reached_) {
        if (justReached) reached_ = true;
        return Then::Nothing;
    }
    // The frame after: a fade back from black goes; the asker's turn.
    const bool cleared = mode_ == Mode::FromBlack;
    if (cleared) active_ = false;
    started_ = !cleared;
    mode_ = Mode::Hold;
    reached_ = false;
    const Then then = then_;
    then_ = Then::Nothing;
    return then;
}

void Director::start(uint32_t sequenceId, uint32_t /*nowMs*/) {
    // 0x00528af0: the state anew; the row, its sound, its first camera.
    sequence_ = host_.sequenceRow(sequenceId);
    camera_.reset();
    cameraIndex_ = 0;
    if (sequence_) {
        host_.playSequenceSound(sequence_->soundId);
        if (sequence_->cameras[0] != 0) camera_ = host_.cameraRow(sequence_->cameras[0]);
    }
    host_.setInCinematic(true);
    if (host_.hasActivePlayer()) {
        fade_.toBlack(kFadeSeconds, ScreenFade::Then::StartCamera);
        if (state_ == State::Idle) state_ = State::FadingOut;
    }
}

void Director::reset() {
    state_ = State::Idle;
    fade_ = ScreenFade{};
    sequence_.reset();
    camera_.reset();
    cameraIndex_ = 0;
    cameraLoaded_ = false;
    cameraDurationMs_ = 0;
    cameraStartMs_ = 0;
    cameraTimeMs_ = 0;
    cameraEnded_ = false;
}

void Director::stop(uint32_t /*nowMs*/) {
    // 0x00524980: the camera's sound stops; black, then the end.
    host_.stopCameraSound();
    fade_.toBlack(kFadeSeconds, ScreenFade::Then::End);
}

bool Director::beginCamera(uint32_t /*nowMs*/) {
    // 0x00526370 up to the wait for the world around it.
    if (!camera_) return false;
    const std::optional<uint32_t> duration = host_.loadCamera(*camera_);
    if (!duration) return false;
    cameraLoaded_ = true;
    cameraDurationMs_ = *duration;
    cameraEnded_ = false;
    cameraTimeMs_ = 0;
    host_.poseCamera(0);
    state_ = State::LoadingCamera;
    return true;
}

void Director::end(uint32_t nowMs) {
    // 0x0051d7c0 up to the wait for the world around the player.
    host_.fireStop();
    host_.releaseCamera();
    cameraLoaded_ = false;
    state_ = State::LoadingPlayer;
    if (host_.playerAreaReady()) finishEnd(nowMs);
}

void Director::finishEnd(uint32_t nowMs) {
    // ... and the rest of it, the world there.
    host_.stopSequenceSound();
    host_.sendComplete();
    host_.setInCinematic(false);
    state_ = State::Idle;
    camera_.reset();
    act(fade_.fromBlack(kFadeSeconds, ScreenFade::Then::Nothing), nowMs);
}

void Director::act(ScreenFade::Then then, uint32_t nowMs) {
    switch (then) {
    case ScreenFade::Then::StartCamera:
        // 0x00527fe0.
        host_.stopWorldSounds();
        host_.fireStart();
        if (!beginCamera(nowMs)) end(nowMs);
        break;
    case ScreenFade::Then::NextCamera:
        // 0x00523530: the camera's sound stops; the sequence's next camera.
        host_.stopCameraSound();
        camera_.reset();
        if (sequence_) {
            ++cameraIndex_;
            if (cameraIndex_ < sequence_->cameras.size() && sequence_->cameras[cameraIndex_] != 0) {
                camera_ = host_.cameraRow(sequence_->cameras[cameraIndex_]);
            }
        }
        if (!beginCamera(nowMs)) end(nowMs);
        break;
    case ScreenFade::Then::End:
        end(nowMs);
        break;
    case ScreenFade::Then::Nothing:
        break;
    }
}

void Director::update(uint32_t nowMs) {
    switch (state_) {
    case State::LoadingCamera:
        host_.poseCamera(0);
        if (host_.cameraAreaReady()) {
            // The world is there: the camera's clock from now (0x005fe320),
            // the server told, its sound, and back from black.
            cameraStartMs_ = nowMs;
            cameraTimeMs_ = 0;
            host_.sendNextCamera();
            host_.playCameraSound(camera_ ? camera_->soundId : 0);
            state_ = State::Playing;
            act(fade_.fromBlack(kFadeSeconds, ScreenFade::Then::Nothing), nowMs);
        }
        break;
    case State::Playing:
        if (cameraLoaded_) {
            cameraTimeMs_ = nowMs - cameraStartMs_;
            if (cameraTimeMs_ >= cameraDurationMs_) {
                // 0x005249c0, once: its last frame held, and to black.
                cameraTimeMs_ = cameraDurationMs_ > 0 ? cameraDurationMs_ - 1 : 0;
                if (!cameraEnded_) {
                    cameraEnded_ = true;
                    fade_.toBlack(kFadeSeconds, ScreenFade::Then::NextCamera);
                }
            }
            host_.poseCamera(cameraTimeMs_);
        }
        break;
    case State::LoadingPlayer:
        if (host_.playerAreaReady()) finishEnd(nowMs);
        break;
    case State::Idle:
    case State::FadingOut:
        break;
    }
    // The fade is drawn after the world's update, and its asker called
    // from there (0x005ed120).
    if (fade_.active()) act(fade_.frame(nowMs), nowMs);
}

}  // namespace wowee::game::cinematic
