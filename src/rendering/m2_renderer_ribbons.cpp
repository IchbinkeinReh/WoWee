// M2 ribbon emitters as the client drives and draws them: CM2Model's
// per-frame pass over its ribbons (0x00828a00) and CRibbonEmitter
// (rendering/client_ribbon.hpp, 0x0097f510 to 0x00980b70).
#include "rendering/m2_renderer.hpp"

#include "rendering/m2_renderer_internal.h"
#include "rendering/m2_track_sampler.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"

#include <glm/gtc/matrix_transform.hpp>

namespace wowee {
namespace rendering {

namespace {

/// Whether a track has keys to give (0x00828a00 sets a value only then, and
/// leaves the emitter's own otherwise).
bool trackHasKeys(const pipeline::M2AnimationTrack& track) {
    for (const auto& seq : track.sequences) {
        if (!seq.floatValues.empty() || !seq.vec3Values.empty()) return true;
    }
    return false;
}

}  // namespace

void M2Renderer::updateClientRibbons(M2Instance& inst, const M2ModelGPU& gpu, float dt) {
    const auto& emitters = gpu.ribbonEmitters;
    if (inst.clientRibbons.size() != emitters.size()) {
        // 0x00832ea0: each emitter made from its record, its gravity set,
        // hidden until the first frame's visibility.
        inst.clientRibbons.assign(emitters.size(), client_ribbon::Emitter{});
        for (size_t i = 0; i < emitters.size(); ++i) {
            const auto& em = emitters[i];
            auto& r = inst.clientRibbons[i];
            r.init(em.edgesPerSecond, em.edgeLifetime, em.textureRows, em.textureCols);
            r.setGravity(em.gravity);
            r.setVisible(false);
        }
    }

    const int seq = inst.currentSequenceIndex;
    const float t = inst.animTime;
    const float g = inst.globalSequenceTime;
    const auto& gs = gpu.globalSequenceDurations;
    for (size_t i = 0; i < emitters.size(); ++i) {
        const auto& em = emitters[i];
        auto& r = inst.clientRibbons[i];
        if (trackHasKeys(em.colorTrack)) {
            r.setColor(m2_track::sampleVec3(em.colorTrack, seq, t, g, gs, glm::vec3(1.0f)));
        }
        // The track's alpha times the model's.
        r.setAlpha(m2_track::sampleFloat(em.alphaTrack, seq, t, g, gs, 1.0f) * inst.fade);
        if (trackHasKeys(em.heightAboveTrack)) {
            r.setAbove(m2_track::sampleFloat(em.heightAboveTrack, seq, t, g, gs, 0.0f));
        }
        if (trackHasKeys(em.heightBelowTrack)) {
            r.setBelow(m2_track::sampleFloat(em.heightBelowTrack, seq, t, g, gs, 0.0f));
        }
        if (trackHasKeys(em.texSlotTrack)) {
            const float slot = m2_track::sampleFloat(em.texSlotTrack, seq, t, g, gs, 0.0f);
            r.setTexSlot(slot > 0.0f ? static_cast<uint32_t>(slot) : 0u);
        }
        const bool visible = m2_track::sampleFloat(em.visibilityTrack, seq, t, g, gs, 1.0f) != 0.0f;
        r.setVisible(visible);

        // The bone's matrix, moved to the emitter's place on it.
        glm::mat4 world = inst.modelMatrix;
        if (em.bone < inst.boneMatrices.size()) world = world * inst.boneMatrices[em.bone];
        world = glm::translate(world, em.position);
        r.setTransform(world);
        r.update(dt, !visible);
    }
}

void M2Renderer::renderClientRibbons(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    const uint32_t vbSlot = vkCtx_->getCurrentFrame() % kDynamicVBSlots;
    if (!ribbonPipelineLayout_ || !ribbonVB_[vbSlot] || !ribbonVBMapped_[vbSlot]) return;

    float* dst = static_cast<float*>(ribbonVBMapped_[vbSlot]);
    size_t written = 0;
    ribbonDraws_.clear();
    std::vector<client_ribbon::Vertex> strip;

    for (const auto& inst : instances) {
        if (!inst.cachedModel) continue;
        const auto& gpu = *inst.cachedModel;
        for (size_t ri = 0; ri < gpu.ribbonEmitters.size() && ri < inst.clientRibbons.size(); ++ri) {
            if (ri + 1 >= gpu.ribbonMaterialStart.size()) break;
            const uint32_t matBegin = gpu.ribbonMaterialStart[ri];
            const uint32_t matEnd = gpu.ribbonMaterialStart[ri + 1];
            if (matBegin == matEnd) continue;
            strip.clear();
            inst.clientRibbons[ri].appendStrip(strip);
            if (strip.size() < 4) continue;
            if (written + strip.size() > MAX_RIBBON_VERTS) break;

            const uint32_t first = static_cast<uint32_t>(written);
            for (const auto& v : strip) {
                float* o = dst + written * 9;
                o[0] = v.position.x;
                o[1] = v.position.y;
                o[2] = v.position.z;
                o[3] = static_cast<float>((v.color >> 16) & 0xFF) / 255.0f;
                o[4] = static_cast<float>((v.color >> 8) & 0xFF) / 255.0f;
                o[5] = static_cast<float>(v.color & 0xFF) / 255.0f;
                o[6] = static_cast<float>((v.color >> 24) & 0xFF) / 255.0f;
                o[7] = v.uv.x;
                o[8] = v.uv.y;
                ++written;
            }
            // 0x00980b70: the same strip once for each texture and material
            // pair, in order, each with its material's state.
            for (uint32_t m = matBegin; m < matEnd; ++m) {
                const VkDescriptorSet texSet = gpu.ribbonTexSets[m];
                const auto& mat = gpu.ribbonMaterials[m];
                const VkPipeline pipe = ribbonPipelineFor(mat);
                if (!texSet || !pipe) continue;
                ribbonDraws_.push_back({.texSet = texSet,
                                        .pipeline = pipe,
                                        .firstVertex = first,
                                        .vertexCount = static_cast<uint32_t>(strip.size()),
                                        .alphaRef = mat.alphaRef,
                                        .lit = mat.lit ? 1 : 0,
                                        .fogged = mat.fogged ? 1 : 0});
            }
        }
    }
    if (ribbonDraws_.empty()) return;

    VkExtent2D ext = vkCtx_->getSwapchainExtent();
    VkViewport vp{};
    vp.width = static_cast<float>(ext.width);
    vp.height = static_cast<float>(ext.height);
    vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.extent = ext;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    VkPipeline lastPipe = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    for (const auto& dc : ribbonDraws_) {
        if (dc.pipeline != lastPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dc.pipeline);
            if (lastPipe == VK_NULL_HANDLE) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ribbonPipelineLayout_, 0, 1,
                                        &perFrameSet, 0, nullptr);
                vkCmdBindVertexBuffers(cmd, 0, 1, &ribbonVB_[vbSlot], &offset);
            }
            lastPipe = dc.pipeline;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ribbonPipelineLayout_, 1, 1,
                                &dc.texSet, 0, nullptr);
        struct {
            float alphaRef;
            int32_t lit;
            int32_t fogged;
            int32_t pad;
        } pc{dc.alphaRef, dc.lit, dc.fogged, 0};
        vkCmdPushConstants(cmd, ribbonPipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, dc.vertexCount, 1, dc.firstVertex, 0);
    }
}

}  // namespace rendering
}  // namespace wowee
