#pragma once

// What a cinematic needs of the client around it (game/cinematic.hpp): the
// rows from CinematicSequences and CinematicCamera, the camera model and its
// animated camera on the world camera (rendering/m2_camera_track.hpp), the
// world loaded around it, its sounds, its packets and its events.

#include "game/cinematic.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/m2_camera_track.hpp"

#include <cstdint>
#include <optional>

namespace wowee {
namespace game { class GameHandler; }
namespace pipeline { class AssetManager; }
namespace rendering { class Renderer; }

namespace core {

class CinematicPlayer final : public game::cinematic::Host {
public:
    CinematicPlayer(game::GameHandler& gameHandler, rendering::Renderer& renderer,
                    pipeline::AssetManager& assets);
    ~CinematicPlayer() override;

    CinematicPlayer(const CinematicPlayer&) = delete;
    CinematicPlayer& operator=(const CinematicPlayer&) = delete;

    /// SMSG_TRIGGER_CINEMATIC's sequence (0x00528af0).
    void start(uint32_t sequenceId);
    /// StopCinematic (0x00524980).
    void stop();
    /// Each frame, before the renderer's.
    void update();
    /// How black the screen is, 0..1.
    [[nodiscard]] float fadeAlpha() const { return director_.fadeAlpha(); }
    /// The world gone (a logout, a disconnect): its sounds and its camera
    /// with it, and nothing more said to anyone.
    void abandon();

    // game::cinematic::Host
    std::optional<game::cinematic::SequenceRow> sequenceRow(uint32_t id) override;
    std::optional<game::cinematic::CameraRow> cameraRow(uint32_t id) override;
    bool hasActivePlayer() override;
    void setInCinematic(bool in) override;
    void stopWorldSounds() override;
    void fireStart() override;
    void fireStop() override;
    void playSequenceSound(uint32_t soundId) override;
    void stopSequenceSound() override;
    void playCameraSound(uint32_t soundId) override;
    void stopCameraSound() override;
    std::optional<uint32_t> loadCamera(const game::cinematic::CameraRow& row) override;
    void poseCamera(uint32_t timeMs) override;
    bool cameraAreaReady() override;
    bool playerAreaReady() override;
    void releaseCamera() override;
    void sendNextCamera() override;
    void sendComplete() override;

private:
    uint32_t nowMs() const;
    /// A SoundEntries kit's file by its weights, streamed once; its handle.
    uint32_t playKit(uint32_t soundId);
    /// The world loaded around `at`, or waited for long enough.
    bool areaReady(const glm::vec3& at, uint32_t& waitStartMs);

    game::GameHandler& gameHandler_;
    rendering::Renderer& renderer_;
    pipeline::AssetManager& assets_;
    game::cinematic::Director director_;

    std::optional<pipeline::M2Model> model_;
    uint32_t sequenceIndex_ = 0;
    game::cinematic::CameraRow row_;
    rendering::m2_camera::Pose cache_;
    glm::vec3 cameraPosition_{0.0f};
    uint32_t sequenceSound_ = 0;
    uint32_t cameraSound_ = 0;
    uint32_t cameraWaitStartMs_ = 0;
    uint32_t playerWaitStartMs_ = 0;
};

}  // namespace core
}  // namespace wowee
