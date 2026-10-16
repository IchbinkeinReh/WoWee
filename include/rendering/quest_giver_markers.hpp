#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class M2Renderer;

/// The mark's frame: where the name attachment is and as large, stood upright
/// and turned about the vertical so that its face - the model's +X, the
/// broad side of the TalkToMe mesh - looks at the camera. The attachment's
/// own turn is the questgiver's facing, and a mark that kept it showed its
/// edge or its back to anyone not standing in front of the NPC.
inline glm::mat4 markerFrameFacing(const glm::mat4& attachment, const glm::vec3& cameraPos) {
    const glm::vec3 position(attachment[3]);
    const float scale = glm::length(glm::vec3(attachment[0]));
    glm::vec2 toCamera(cameraPos.x - position.x, cameraPos.y - position.y);
    const float len = glm::length(toCamera);
    // Straight above or below it, any turn is as good as another.
    toCamera = len > 1e-4f ? toCamera / len : glm::vec2(1.0f, 0.0f);
    glm::mat4 frame(1.0f);
    frame[0] = glm::vec4(toCamera.x * scale, toCamera.y * scale, 0.0f, 0.0f);
    frame[1] = glm::vec4(-toCamera.y * scale, toCamera.x * scale, 0.0f, 0.0f);
    frame[2] = glm::vec4(0.0f, 0.0f, scale, 0.0f);
    frame[3] = glm::vec4(position, 1.0f);
    return frame;
}

/// The mark over a questgiver's head: one of the TalkToMe models
/// (game::questGiverMarkerModel) hung at the object's name attachment
/// (0x00719310 for a unit, 0x0070cf30 for a game object) and left to run its
/// own animation, turned to face the camera (markerFrameFacing). No sprite,
/// no bob, no fade of its own.
class QuestGiverMarkers {
public:
    struct Marker {
        uint64_t guid = 0;
        const char* model = nullptr;  ///< game::questGiverMarkerModel's path
        glm::mat4 frame{1.0f};        ///< the attachment's world frame
    };

    /// Once a frame with every mark there is now. Hangs new ones, moves the
    /// rest with their attachment, swaps a model whose status changed and
    /// takes down the marks that are gone.
    void update(M2Renderer* m2, pipeline::AssetManager* assets, const std::vector<Marker>& markers);

    /// Take every mark down, as on leaving a map.
    void clear();

private:
    /// Clear of the level-up effect (999900) and the loot sparkle (999901).
    static constexpr uint32_t kFirstModelId = 999902;

    /// The renderer's id for a model, loading it the first time; 0 if it
    /// cannot be loaded.
    uint32_t modelId(const char* path);

    struct Placed {
        uint32_t instance = 0;
        uint32_t modelId = 0;
    };

    M2Renderer* m2_ = nullptr;
    pipeline::AssetManager* assets_ = nullptr;
    std::unordered_map<std::string, uint32_t> modelIds_;  // path -> id; 0 when it failed
    std::unordered_map<uint64_t, Placed> placed_;         // questgiver guid -> its mark
};

} // namespace rendering
} // namespace wowee
