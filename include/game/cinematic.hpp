#pragma once

// A cinematic, as the client plays one (SMSG_TRIGGER_CINEMATIC).
//
// 0x00528af0 starts it: the CinematicSequences row, its sound (a CINEMATIC
// sound, with Sound_EnableMusic), its first camera, the client in the
// cinematic (DAT_00bd07fc) - and, with an active player, the screen fading
// to black over a quarter of a second (0x005ed480). Black, 0x00527fe0 stops
// the world's music and ambience (0x00409550), says CINEMATIC_START and
// starts the camera (0x00526370): the CinematicCamera
// row's model, placed at its origin and facing (0x00606570); the world
// loaded around it (0x00781500, 0x004bae10); its clock from now (0x005fe320);
// CMSG_NEXT_CINEMATIC_CAMERA; the row's sound (RACIALCINEMATIC); and the
// screen back from black over a quarter second (0x005ed080). A camera that
// will not start ends the cinematic.
//
// The camera's animation run out (0x005249c0) holds its last frame and
// fades to black; then (0x00523530) its sound stops and the next of the
// sequence's eight cameras starts the same way, or - none left - the
// cinematic ends (0x0051d7c0): CINEMATIC_STOP, the world camera back to the
// player, the world loaded around them, the sequence's sound stopped,
// CMSG_COMPLETE_CINEMATIC, out of the cinematic, and back from black.
//
// StopCinematic (0x00524980), which CinematicFrame calls for the key bound
// to TOGGLEGAMEMENU, stops the camera's sound and fades to black, then ends
// it the same way.
//
// The fade (0x005ed120, each frame): its clock starts the first frame it is
// drawn; to black it is elapsed / duration black, back from black the rest;
// the frame it reaches its end it is fully there, and the frame after it
// calls whoever asked for it - a fade back from black then goes, a fade to
// black stays black until something fades it back. Asking for a fade to
// black anew starts it again from clear (0x005ed480); a fade back from black
// is only asked of a screen that is faded (0x005ed080).

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include <glm/glm.hpp>

namespace wowee::game::cinematic {

/// A CinematicSequences row (0x00ad3480): its sound and its eight cameras.
struct SequenceRow {
    uint32_t soundId = 0;
    std::array<uint32_t, 8> cameras{};
};

/// A CinematicCamera row (0x00ad345c): the camera model, its sound, and
/// where the model is placed and which way it faces.
struct CameraRow {
    uint32_t id = 0;
    std::string model;
    uint32_t soundId = 0;
    glm::vec3 origin{0.0f};
    float facing = 0.0f;
};

/// How long each of the cinematic's fades takes (0x009fe800).
constexpr float kFadeSeconds = 0.25f;

/// The black over the screen (0x005ed480, 0x005ed080, 0x005ed120).
class ScreenFade {
public:
    enum class Mode : uint8_t { Hold, FromBlack, ToBlack };
    /// What to do once a fade is done.
    enum class Then : uint8_t { Nothing, StartCamera, NextCamera, End };

    /// 0x005ed480: anew, from clear to black over `seconds`.
    void toBlack(float seconds, Then then);
    /// 0x005ed080: from black to clear over `seconds`, if the screen is
    /// faded at all; at once for none.
    /// Returns what to do now, for a fade over at once.
    Then fromBlack(float seconds, Then then);
    /// One frame at `nowMs`: how black the screen is (0..1), and - the frame
    /// after a fade reached its end - what its asker does now.
    Then frame(uint32_t nowMs);

    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] float alpha() const { return alpha_; }
    [[nodiscard]] Mode mode() const { return mode_; }

private:
    bool active_ = false;   ///< 0x00c24294: the black is there
    Mode mode_ = Mode::Hold; ///< 0x00c2428c
    float seconds_ = 0.0f;  ///< 0x00c24288
    bool started_ = false;  ///< 0x00c24295: its clock runs
    uint32_t startMs_ = 0;  ///< 0x00c24284
    bool reached_ = false;  ///< 0x00c24280: at its end, the asker next frame
    Then then_ = Then::Nothing;
    float alpha_ = 0.0f;
};

/// What a cinematic needs of the client around it.
class Host {
public:
    virtual ~Host() = default;
    virtual std::optional<SequenceRow> sequenceRow(uint32_t id) = 0;
    virtual std::optional<CameraRow> cameraRow(uint32_t id) = 0;
    /// The active player exists (0x00528af0 fades only then).
    virtual bool hasActivePlayer() = 0;
    /// DAT_00bd07fc, which InCinematic answers.
    virtual void setInCinematic(bool in) = 0;
    /// 0x00409550, as the screen is black: the world's music and ambience
    /// stop.
    virtual void stopWorldSounds() = 0;
    virtual void fireStart() = 0;  ///< CINEMATIC_START
    virtual void fireStop() = 0;   ///< CINEMATIC_STOP
    /// The sequence's sound, a CINEMATIC one, where Sound_EnableMusic is on -
    /// the music stopped for it (0x004c93b0).
    virtual void playSequenceSound(uint32_t soundId) = 0;
    virtual void stopSequenceSound() = 0;
    /// The camera's sound (RACIALCINEMATIC), the music stopped first
    /// (0x004c93b0).
    virtual void playCameraSound(uint32_t soundId) = 0;
    virtual void stopCameraSound() = 0;
    /// 0x00606570: the camera model loaded, placed, and with a camera; the
    /// length of the animation it plays in ms, or none.
    virtual std::optional<uint32_t> loadCamera(const CameraRow& row) = 0;
    /// The world camera where the camera is `timeMs` into its animation.
    virtual void poseCamera(uint32_t timeMs) = 0;
    /// The world loaded around the camera, and around the player once it
    /// is back on them (0x00781500 and 0x004bae10, which wait for it).
    virtual bool cameraAreaReady() = 0;
    virtual bool playerAreaReady() = 0;
    /// 0x005fe360: the world camera back on the player.
    virtual void releaseCamera() = 0;
    virtual void sendNextCamera() = 0;  ///< CMSG_NEXT_CINEMATIC_CAMERA
    virtual void sendComplete() = 0;    ///< CMSG_COMPLETE_CINEMATIC
};

/// The cinematic's course (0x00528af0 .. 0x0051d7c0).
class Director {
public:
    explicit Director(Host& host) : host_(host) {}

    /// SMSG_TRIGGER_CINEMATIC's sequence (0x00528af0).
    void start(uint32_t sequenceId, uint32_t nowMs);
    /// StopCinematic (0x00524980).
    void stop(uint32_t nowMs);
    /// One frame.
    void update(uint32_t nowMs);
    /// Back to nothing at all, saying nothing to the host (the world gone).
    void reset();

    enum class State : uint8_t {
        Idle,
        FadingOut,     ///< to black before the first camera
        LoadingCamera, ///< black, the world loading around the camera
        Playing,       ///< the camera flies
        LoadingPlayer, ///< ended, black, the world loading around the player
    };
    [[nodiscard]] State state() const { return state_; }
    /// How black the screen is, 0..1.
    [[nodiscard]] float fadeAlpha() const { return fade_.active() ? fade_.alpha() : 0.0f; }
    /// Whether the world camera is the cinematic's.
    [[nodiscard]] bool cameraActive() const { return cameraLoaded_; }
    /// Where the camera is in its animation, ms.
    [[nodiscard]] uint32_t cameraTimeMs() const { return cameraTimeMs_; }
    [[nodiscard]] const std::optional<CameraRow>& camera() const { return camera_; }
    [[nodiscard]] uint32_t cameraIndex() const { return cameraIndex_; }

private:
    bool beginCamera(uint32_t nowMs);
    void end(uint32_t nowMs);
    void finishEnd(uint32_t nowMs);
    void act(ScreenFade::Then then, uint32_t nowMs);

    Host& host_;
    State state_ = State::Idle;
    ScreenFade fade_;
    std::optional<SequenceRow> sequence_;  ///< 0x00bd0ac8
    std::optional<CameraRow> camera_;      ///< 0x00bd0ae4
    uint32_t cameraIndex_ = 0;             ///< 0x00bd0ae0
    bool cameraLoaded_ = false;
    uint32_t cameraDurationMs_ = 0;
    uint32_t cameraStartMs_ = 0;
    uint32_t cameraTimeMs_ = 0;
    bool cameraEnded_ = false;  ///< 0x005249c0 came for this camera
};

}  // namespace wowee::game::cinematic
