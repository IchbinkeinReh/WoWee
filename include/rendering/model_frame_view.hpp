#pragma once

// A UI Model frame that shows a model file (CSimpleModel, 0x0095fc30): the
// M2 drawn - particles and ribbons too - into an off-screen image of the
// frame's own size, which the interface draws over the frame.
//
// The client draws it in the frame's own viewport. With no camera chosen it
// looks straight at the frame through an orthographic projection over the
// frame's rect in DDC (the screen with a diagonal of 1, 0x0047bf90), z from
// -500 to 500, with the model's origin at the frame's bottom-left corner
// (0x004bee60): the model's X runs right and its Y up. Its pose (0x0095fba0)
// is the frame's SetPosition times its effective scale, turned by its facing
// and sized by ModelScale * effective scale * DDC height * 1.6666. With a
// camera (SetCamera) it looks through that M2 camera (0x004bf0c0).

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vector>

#include "pipeline/m2_loader.hpp"
#include "rendering/built_beside.hpp"
#include "rendering/model_lights.hpp"
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace wowee::pipeline { class AssetManager; }

namespace wowee::rendering {

class VkContext;
class VkRenderTarget;
class M2Renderer;
class Camera;

/// The client's pose and projection for a frame, as pure arithmetic.
namespace model_frame {

/// The screen's height in DDC for an aspect (0x0047bf90).
inline float ddcHeight(float aspect) { return 1.0f / std::sqrt(aspect * aspect + 1.0f); }

/// What a frame asks of its model.
struct Pose {
    glm::vec3 position{0.0f};   ///< SetPosition (+0x31c)
    float facing = 0.0f;        ///< SetFacing (+0x328)
    float modelScale = 1.0f;    ///< SetModelScale / scale="" (+0x32c)
    float effectiveScale = 1.0f;///< the frame's (+0x7c)
};

/// 0x0095fba0: the model's matrix in the frame's DDC space - SetPosition
/// times the effective scale, the facing about Z, and ModelScale * effective
/// scale * the DDC height (0x0047c080(1)) * 1.6666.
inline glm::mat4 modelMatrix(const Pose& pose, float ddcScreenHeight) {
    const glm::vec3 at = pose.position * pose.effectiveScale;
    const float scale = pose.modelScale * pose.effectiveScale * ddcScreenHeight * (5.0f / 3.0f);
    glm::mat4 m = glm::translate(glm::mat4(1.0f), at);
    m = glm::rotate(m, pose.facing, glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::scale(m, glm::vec3(scale));
}

/// Model:SetLight's light (the M2Light at +0x2b0): its ambient and diffuse,
/// from a point (omni) or along a direction, in the frame's space.
struct Light {
    bool enabled = false;
    bool omni = true;
    glm::vec3 vec{0.0f};
    glm::vec3 ambient{0.0f};
    glm::vec3 diffuse{0.0f};
};

/// The frame's fog, set into the model's lighting by its callback
/// (0x00960070 -> 0x00834940): colour, near and far, while it is on.
struct Fog {
    bool enabled = false;
    glm::vec3 color{0.0f};
    float nearDist = 0.0f, farDist = 1.0f;
};

template <typename Frame>
[[nodiscard]] Light lightOf(const Frame& f) {
    const auto vec = [](const float (&v)[3]) { return glm::vec3(v[0], v[1], v[2]); };
    return {.enabled = f.modelLightEnabled, .omni = f.modelLightOmni, .vec = vec(f.modelLightVec),
            .ambient = vec(f.modelLightAmbient), .diffuse = vec(f.modelLightDiffuse)};
}

/// `unit`: what one of the view's own units is in the frame's.
template <typename Frame>
[[nodiscard]] Fog fogOf(const Frame& f, float unit = 1.0f) {
    const float per = unit > 1e-6f ? 1.0f / unit : 1.0f;
    return {.enabled = f.modelFogEnabled,
            .color = glm::vec3(f.modelFogColor[0], f.modelFogColor[1], f.modelFogColor[2]),
            .nearDist = f.modelFogNear * per, .farDist = f.modelFogFar * per};
}

/// What a lit surface gets: a directional light is merged as any is (0x00834f60), a point
/// light adds no ambient and is taken as it reaches the model's origin. Off, the model's
/// lighting starts at nothing each frame (0x00831af0 -> 0x00834900) and the frame's callback
/// adds none (0x00960070), so that only unlit batches and particles show; the direction is the
/// one 0x00835280 falls back to.
using Lighting = model_lights::Lighting;
[[nodiscard]] inline Lighting lighting(const Light& light) {
    if (!light.enabled) return {};
    model_lights::Sum sum;
    if (light.omni) {
        const float d = glm::length(light.vec);
        if (d >= 1e-5f) sum.addDirectional(light.diffuse * model_lights::pointReach(d), -light.vec / d);
    } else {
        sum.addAmbient(light.ambient);
        sum.addDirectional(light.diffuse, light.vec);
    }
    return sum.merged();
}

/// The same for a frame's data, as the shaders take them.
template <typename PerFrame>
void put(const Lighting& l, const Fog& fog, PerFrame& ubo) {
    ubo.lightDir = glm::vec4(l.direction, 0.0f);
    ubo.lightColor = glm::vec4(l.diffuse, 0.0f);
    ubo.ambientColor = glm::vec4(l.ambient, 0.0f);
    if (fog.enabled && fog.farDist > fog.nearDist) {
        ubo.fogColor = glm::vec4(fog.color, 1.0f);
        ubo.fogParams = glm::vec4(fog.nearDist, fog.farDist, 0.0f, 0.0f);
    } else {
        ubo.fogColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        ubo.fogParams = glm::vec4(99999.0f, 100000.0f, 0.0f, 0.0f);
    }
}
template <typename PerFrame>
void put(const Light& light, const Fog& fog, PerFrame& ubo) {
    put(lighting(light), fog, ubo);
}

}  // namespace model_frame

class ModelFrameView {
public:
    ModelFrameView();
    ~ModelFrameView();
    ModelFrameView(const ModelFrameView&) = delete;
    ModelFrameView& operator=(const ModelFrameView&) = delete;

    /// Built on first use at this many pixels; rebuilt when they change.
    bool ensureSize(pipeline::AssetManager* assets, int width, int height);
    /// The model shown, by file (.mdx/.mdl read as .m2); empty clears it.
    /// False when it will not load.
    /// `iconTexture`, where given, is ReplaceIconTexture's: the model's
    /// textures of replaceable type 14 drawn with it (0x00960320).
    bool setModel(const std::string& path, const std::string& iconTexture = std::string());
    /// The frame's rect in DDC (its width and height) and its pose; the
    /// camera index (-1 none) and sequence (-1 the model's own).
    using Light = model_frame::Light;
    void setLight(const Light& light) { light_ = light; }
    using Fog = model_frame::Fog;
    void setFog(const Fog& fog) { fog_ = fog; }
    /// The pose, projection and animation: `sequence` (-1 the model's own)
    /// started at `sequenceTimeMs` each time `sequenceSerial` changes
    /// (SetSequence, SetSequenceTime: 0x0095f5e0, 0x0095f610). `came` is how a
    /// PlayerModel stood as its model came, for the camera that stays there
    /// (figure_camera.hpp).
    void setFrame(float widthDdc, float heightDdc, const model_frame::Pose& pose, float ddcScreenHeight,
                  int camera, int sequence, int sequenceTimeMs = 0, uint32_t sequenceSerial = 0,
                  const model_frame::Pose* came = nullptr);
    void update(float deltaTime);
    /// Records the draw; Renderer calls it outside the scene's passes.
    void compositePass(VkCommandBuffer cmd, uint32_t frameIndex);
    /// Asked by the frame loop each frame it is drawn.
    void requestComposite() { compositeRequested_ = true; }
    [[nodiscard]] bool hasModel() const { return instanceId_ != 0; }
    /// The image as an ImGui texture id, 0 before the first draw.
    [[nodiscard]] uint64_t textureId() const;
    void shutdown();

private:
    bool createTarget(int width, int height);
    void destroyTarget();

    static constexpr uint32_t kFrames = 2;
    VkContext* vkCtx_ = nullptr;
    pipeline::AssetManager* assets_ = nullptr;
    int width_ = 0, height_ = 0;
    std::unique_ptr<VkRenderTarget> target_;
    std::unique_ptr<M2Renderer> m2_;
    /// m2_ while it is made, on a thread of its own: some hundred pipelines,
    /// seconds with the driver's cache empty (BuiltBeside). Not again once
    /// it failed.
    BuiltBeside<M2Renderer> m2Building_;
    bool m2Failed_ = false;
    std::unique_ptr<Camera> camera_;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    std::array<VkBuffer, kFrames> ubo_{};
    std::array<VmaAllocation, kFrames> uboAlloc_{};
    std::array<void*, kFrames> uboMapped_{};
    std::array<VkDescriptorSet, kFrames> perFrameSet_{};
    VkImage dummyShadowImage_ = VK_NULL_HANDLE;
    VmaAllocation dummyShadowAlloc_ = VK_NULL_HANDLE;
    VkImageView dummyShadowView_ = VK_NULL_HANDLE;
    VkDescriptorSet imguiTexture_ = VK_NULL_HANDLE;
    std::string modelPath_;
    std::vector<pipeline::M2Camera> cameras_;  ///< the model's, for SetCamera
    glm::vec3 boxMiddle_{0.0f};
    uint32_t modelId_ = 0;
    uint32_t instanceId_ = 0;
    int sequence_ = -1;
    uint32_t sequenceSerial_ = 0;
    std::string iconTexture_;
    std::vector<std::pair<uint32_t, uint32_t>> sequenceDurations_;  ///< animation id, ms
    bool compositeRequested_ = false;
    Light light_;
    Fog fog_;
    bool composited_ = false;
    bool registered_ = false;
};

}  // namespace wowee::rendering
