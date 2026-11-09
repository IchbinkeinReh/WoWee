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
    /// Model:SetLight's light (the M2Light at +0x2b0): off, the view's own
    /// stand-in; on, its ambient and diffuse, from a point (omni) or along a
    /// direction, in the frame's space.
    struct Light {
        bool enabled = false;
        bool omni = true;
        glm::vec3 vec{0.0f};
        glm::vec3 ambient{0.0f};
        glm::vec3 diffuse{0.0f};
    };
    void setLight(const Light& light) { light_ = light; }
    /// The frame's fog, set into the model's lighting by its callback
    /// (0x00960070 -> 0x00834940): colour, near and far, while it is on.
    struct Fog {
        bool enabled = false;
        glm::vec3 color{0.0f};
        float nearDist = 0.0f, farDist = 1.0f;
    };
    void setFog(const Fog& fog) { fog_ = fog; }
    /// The pose, projection and animation: `sequence` (-1 the model's own)
    /// started at `sequenceTimeMs` each time `sequenceSerial` changes
    /// (SetSequence, SetSequenceTime: 0x0095f5e0, 0x0095f610).
    void setFrame(float widthDdc, float heightDdc, const model_frame::Pose& pose, float ddcScreenHeight,
                  int camera, int sequence, int sequenceTimeMs = 0, uint32_t sequenceSerial = 0);
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
