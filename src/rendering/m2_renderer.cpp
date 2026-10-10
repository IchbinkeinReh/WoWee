#include <atomic>
#include "rendering/placement_transform.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/rt_bvh.hpp"
#include "rendering/rt_scene.hpp"
#include "core/env_flag.hpp"
#include "rendering/m2_renderer_internal.h"
#include "rendering/m2_blend_mode.hpp"
#include "rendering/m2_texture_combiner.hpp"
#include "rendering/m2_view_distance.hpp"
#include "pipeline/model_bounds.hpp"
#include "rendering/render_constants.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/bone_slots.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include <chrono>
#include <cctype>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <limits>
#include <future>
#include <thread>
#include <set>

#include <string_view>

namespace wowee {
namespace rendering {

void M2Instance::updateModelMatrix() {
    // Doodads and buildings compose this identically, in placement_transform.hpp
    // - the header records what it took to establish the order, and a test
    // pins it. Composing it here as well is how the two came to disagree.
    modelMatrix = placementModelMatrix(position, rotation, scale);
    invModelMatrix = glm::inverse(modelMatrix);
}

void M2Instance::recomputeCachedCullFactors() {
    // Matrix instances (notably ADT tree doodads) can have an offset pivot and
    // arbitrary scale. A sphere centered at the placement origin with the M2
    // header radius can therefore exclude much of the visible canopy. Derive
    // the render-cull sphere from transformed vertex bounds instead. The
    // separate worldBounds fields intentionally remain collision bounds.
    if (cachedModel) {
        glm::vec3 visualMin(std::numeric_limits<float>::max());
        glm::vec3 visualMax(std::numeric_limits<float>::lowest());
        for (int x = 0; x < 2; ++x) {
            for (int y = 0; y < 2; ++y) {
                for (int z = 0; z < 2; ++z) {
                    const glm::vec3 local(
                        x ? cachedModel->boundMax.x : cachedModel->boundMin.x,
                        y ? cachedModel->boundMax.y : cachedModel->boundMin.y,
                        z ? cachedModel->boundMax.z : cachedModel->boundMin.z);
                    const glm::vec3 world = glm::vec3(modelMatrix * glm::vec4(local, 1.0f));
                    visualMin = glm::min(visualMin, world);
                    visualMax = glm::max(visualMax, world);
                }
            }
        }
        cachedCullCenter = (visualMin + visualMax) * 0.5f;
        cachedVisualRadius = glm::length(visualMax - visualMin) * 0.5f;
        cachedVisualExtent = visualMax - visualMin;
    } else {
        cachedCullCenter = position;
        cachedVisualRadius = cachedBoundRadius * scale;
    }

    // The size class by the largest side of the header's vertex box in the
    // world (0x007bdb10); a model with no header box, by its own vertices.
    if (cachedModel && cachedModel->hasVertexBox) {
        cachedSizeClass = m2DoodadSizeClassOfBox(modelMatrix, cachedModel->vertexBoxMin,
                                                 cachedModel->vertexBoxMax);
    } else if (cachedModel) {
        const glm::vec3 ext = cachedVisualExtent;
        cachedSizeClass = m2DoodadSizeClass(std::max({ext.x, ext.y, ext.z}));
    } else {
        cachedSizeClass = m2DoodadSizeClass(cachedVisualRadius * 2.0f);
    }

    float worldRadius = cachedVisualRadius;
    float cullRadius = worldRadius;
    if (cachedDisableAnimation) cullRadius = std::max(cullRadius, 3.0f);
    cachedPaddedRadius = std::max(cullRadius * rendering::M2_PADDED_RADIUS_SCALE,
                                  cullRadius + rendering::M2_PADDED_RADIUS_MIN_MARGIN);
}

M2Renderer::M2Renderer() {
}

M2Renderer::~M2Renderer() {
    shutdown();
}

/// The nine main-pass pipelines, built once at startup and again after a
/// device loss.
///
/// Both paths used to build them: initialize() here and recreatePipelines() in
/// m2_renderer_instance.cpp, which was a copy of these 190 lines that had
/// drifted only in its comments. Eighty-seven overlapping twelve-line blocks -
/// the largest duplicate in the tree. A change to any blend state, depth mode
/// or vertex layout landed in whichever copy was in front of whoever made it,
/// and the one that did not get it only showed after a device loss.
///
/// The one thing that genuinely differed is the ribbon pipeline layout, which
/// is created here and is now created only when there is not one already: the
/// rebuild destroys pipelines and keeps layouts.
bool M2Renderer::buildMainPassPipelines(VkDescriptorSetLayout perFrameLayout) {
    VkDevice device = vkCtx_->getDevice();

    // --- Load shaders ---
    rendering::VkShaderModule m2Vert, m2Frag;
    rendering::VkShaderModule particleVert, particleFrag;

    (void)m2Vert.loadFromFile(device, "assets/shaders/m2.vert.spv");
    (void)m2Frag.loadFromFile(device, "assets/shaders/m2.frag.spv");
    (void)particleVert.loadFromFile(device, "assets/shaders/m2_particle.vert.spv");
    (void)particleFrag.loadFromFile(device, "assets/shaders/m2_particle.frag.spv");

    if (!m2Vert.isValid() || !m2Frag.isValid()) {
        LOG_ERROR("M2: Missing required shaders, cannot build pipelines");
        return false;
    }

    VkRenderPass mainPass = vkCtx_->getImGuiRenderPass();

    // --- Build M2 model pipelines ---
    // Vertex input: 18 floats = 72 bytes stride
    // loc 0: vec3 pos (0), loc 1: vec3 normal (12), loc 2: vec2 uv0 (24),
    // loc 5: vec2 uv1 (32), loc 3: vec4 boneWeights (40), loc 4: vec4 boneIndices (56)
    VkVertexInputBindingDescription m2Binding{};
    m2Binding.binding = 0;
    m2Binding.stride = 18 * sizeof(float);
    m2Binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> m2Attrs = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},                     // position
        {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 3 * sizeof(float)},     // normal
        {.location = 2, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 6 * sizeof(float)},        // texCoord0
        {.location = 5, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 8 * sizeof(float)},        // texCoord1
        {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 10 * sizeof(float)}, // boneWeights
        {.location = 4, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 14 * sizeof(float)}, // boneIndices (float)
    };

    // Pipeline derivatives - opaque is the base, others derive from it for shared state optimization
    auto buildM2Pipeline = [&](VkPipelineColorBlendAttachmentState blendState, bool depthWrite,
                               VkPipelineCreateFlags flags = 0, VkPipeline basePipeline = VK_NULL_HANDLE,
                               bool alphaToCoverage = false,
                               VkCullModeFlags cullMode = VK_CULL_MODE_NONE,
                               bool depthTest = true) -> VkPipeline {
        auto builder = PipelineBuilder()
            .setShaders(m2Vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        m2Frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({m2Binding}, m2Attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, cullMode, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            // The sky model tests depth but never writes it. Its vertices are
            // pushed to the far plane, so the test is what lets ground drawn
            // before it occlude it, and a write would put the far plane over
            // everything drawn after.
            .setDepthTest(depthTest, skyMode_ ? false : depthWrite, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setColorBlendAttachment(blendState)
            .setMultisample(vkCtx_->getMsaaSamples());
        // MSAA alpha-to-coverage dithers the shader's sharpened cutout alpha
        // across samples for smooth foliage/leaf silhouettes.
        if (alphaToCoverage) builder.setAlphaToCoverage(true);
        return builder
            .setLayout(pipelineLayout_)
            .setRenderPass(mainPass)
            .setDynamicStates(viewportAndScissorDynamic())
            .setFlags(flags)
            .setBasePipeline(basePipeline)
            .build(device, vkCtx_->getPipelineCache());
    };

    opaquePipeline_ = buildM2Pipeline(PipelineBuilder::blendDisabled(), true,
                                      VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT);
    // Counter-clockwise is the front face for every M2 pipeline: the camera
    // flips Y for Vulkan, which turns the model's outward winding around. The
    // culled variants (see below) rely on it.
    alphaTestPipeline_ = buildM2Pipeline(PipelineBuilder::blendAlpha(), true,
                                         VK_PIPELINE_CREATE_DERIVATIVE_BIT, opaquePipeline_);
    // Every alpha-tested batch - a canopy, a fern, a tuft of clutter - is drawn
    // through this one. Alpha-to-coverage spreads the shader's sharpened alpha
    // across the samples, so a leaf edge is a coverage ramp rather than a
    // binary in-or-out, and the distance fade has somewhere to land: on the
    // opaque pipeline the cutout path used to bind, both were computed and
    // then thrown away, which is why a canopy read as one hard-edged blob and
    // a doodad popped rather than faded. Blending stays off, so it is still an
    // opaque pass: order-independent, depth written, no halo where a leaf
    // drawn early sits over the sky.
    cutoutPipeline_ = buildM2Pipeline(PipelineBuilder::blendDisabled(), true,
                                      VK_PIPELINE_CREATE_DERIVATIVE_BIT, opaquePipeline_,
                                      /*alphaToCoverage=*/true);
    alphaPipeline_ = buildM2Pipeline(PipelineBuilder::blendAlpha(), false,
                                     VK_PIPELINE_CREATE_DERIVATIVE_BIT, opaquePipeline_);
    additivePipeline_ = buildM2Pipeline(PipelineBuilder::blendAdditive(), false,
                                        VK_PIPELINE_CREATE_DERIVATIVE_BIT, opaquePipeline_);
    // Colour-only blends; the destination's alpha is left as it is.
    auto colourBlend = [](VkBlendFactor src, VkBlendFactor dst) {
        VkPipelineColorBlendAttachmentState st = PipelineBuilder::blendAdditive();
        st.srcColorBlendFactor = src;
        st.dstColorBlendFactor = dst;
        st.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        st.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        return st;
    };
    noAlphaAddPipeline_ = buildM2Pipeline(colourBlend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE), false,
                                          VK_PIPELINE_CREATE_DERIVATIVE_BIT, opaquePipeline_);
    modPipeline_ = buildM2Pipeline(colourBlend(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO), false,
                                   VK_PIPELINE_CREATE_DERIVATIVE_BIT, opaquePipeline_);
    mod2xPipeline_ = buildM2Pipeline(colourBlend(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_SRC_COLOR), false,
                                     VK_PIPELINE_CREATE_DERIVATIVE_BIT, opaquePipeline_);

    // The per-batch state the 3.3.5a client sets from an M2 material's flags
    // (FUN_0081fe90): back faces culled unless 0x4 (two-sided), depth test
    // off with 0x8, depth write off with 0x10 - for every blend mode alike.
    // Each base above gets one variant per combination; pipelineVariant()
    // picks it at draw time. The bases themselves stay as they were for the
    // paths that bind them directly.
    struct VariantSpec {
        VkPipeline base;
        VkPipelineColorBlendAttachmentState blend;
        bool alphaToCoverage;
    };
    const VariantSpec variantSpecs[] = {
        {.base = opaquePipeline_, .blend = PipelineBuilder::blendDisabled(), .alphaToCoverage = false},
        {.base = alphaTestPipeline_, .blend = PipelineBuilder::blendAlpha(), .alphaToCoverage = false},
        {.base = cutoutPipeline_, .blend = PipelineBuilder::blendDisabled(), .alphaToCoverage = true},
        {.base = alphaPipeline_, .blend = PipelineBuilder::blendAlpha(), .alphaToCoverage = false},
        {.base = additivePipeline_, .blend = PipelineBuilder::blendAdditive(), .alphaToCoverage = false},
        {.base = noAlphaAddPipeline_, .blend = colourBlend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE), .alphaToCoverage = false},
        {.base = modPipeline_, .blend = colourBlend(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO), .alphaToCoverage = false},
        {.base = mod2xPipeline_, .blend = colourBlend(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_SRC_COLOR), .alphaToCoverage = false},
    };
    for (const auto& spec : variantSpecs) {
        if (!spec.base) continue;
        std::array<VkPipeline, kPipelineVariantCount> variants{};
        for (uint32_t v = 0; v < kPipelineVariantCount; ++v) {
            const bool cull = (v & kVariantCull) != 0;
            const bool noTest = (v & kVariantNoDepthTest) != 0;
            const bool noWrite = (v & kVariantNoDepthWrite) != 0;
            variants[v] = buildM2Pipeline(spec.blend, !noWrite,
                                          VK_PIPELINE_CREATE_DERIVATIVE_BIT, opaquePipeline_,
                                          spec.alphaToCoverage,
                                          cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
                                          !noTest);
        }
        pipelineVariants_[spec.base] = variants;
    }

    // --- Build particle pipelines ---
    if (particleVert.isValid() && particleFrag.isValid()) {
        VkVertexInputBindingDescription pBind{};
        pBind.binding = 0;
        pBind.stride = 9 * sizeof(float); // pos3 + color4 + size1 + tile1
        // One record a particle, read per instance: the vertex shader draws each
        // as a camera-facing quad, the way the client does, not as a point
        // sprite - which a driver clamps in size and draws as a square.
        pBind.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        std::vector<VkVertexInputAttributeDescription> pAttrs = {
            {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},                    // position
            {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = 3 * sizeof(float)}, // color
            {.location = 2, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = 7 * sizeof(float)},          // size
            {.location = 3, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = 8 * sizeof(float)},          // tile
        };

        // WOWEE_PFX_NODEPTH=1: particles ignore the depth buffer (a diagnostic).
        const bool pfxNoDepth = std::getenv("WOWEE_PFX_NODEPTH") != nullptr;
        auto buildParticlePipeline = [&](VkPipelineColorBlendAttachmentState blend) -> VkPipeline {
            return PipelineBuilder()
                .setShaders(particleVert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                            particleFrag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
                .setVertexInput({pBind}, pAttrs)
                .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
                .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
                .setDepthTest(!pfxNoDepth, false, VK_COMPARE_OP_LESS_OR_EQUAL)
                .setColorBlendAttachment(blend)
                .setMultisample(vkCtx_->getMsaaSamples())
                .setLayout(particlePipelineLayout_)
                .setRenderPass(mainPass)
                .setDynamicStates(viewportAndScissorDynamic())
                .build(device, vkCtx_->getPipelineCache());
        };

        particlePipeline_ = buildParticlePipeline(PipelineBuilder::blendAlpha());
        particleAdditivePipeline_ = buildParticlePipeline(PipelineBuilder::blendAdditive());
        // The client's blend factors for the rest (Gx table at 0x00a2f964/94):
        // type 3 is (one, one); 5 is (destination colour, zero); 6 is
        // (destination colour, source colour).
        const auto customBlend = [](VkBlendFactor src, VkBlendFactor dst) {
            VkPipelineColorBlendAttachmentState state = PipelineBuilder::blendAlpha();
            state.srcColorBlendFactor = src;
            state.dstColorBlendFactor = dst;
            return state;
        };
        particleNoAlphaAddPipeline_ = buildParticlePipeline(
            customBlend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE));
        particleModPipeline_ = buildParticlePipeline(
            customBlend(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO));
        particleMod2xPipeline_ = buildParticlePipeline(
            customBlend(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_SRC_COLOR));
    }

    // --- Build ribbon pipelines ---
    // Vertex format: pos(3) + color(3) + alpha(1) + uv(2) = 9 floats = 36 bytes
    {
        rendering::VkShaderModule ribVert, ribFrag;
        (void)ribVert.loadFromFile(device, "assets/shaders/m2_ribbon.vert.spv");
        (void)ribFrag.loadFromFile(device, "assets/shaders/m2_ribbon.frag.spv");
        if (ribVert.isValid() && ribFrag.isValid()) {
            // Reuse particleTexLayout_ for set 1 (single texture sampler).
            // Only once: a pipeline layout outlives the pipelines built from
            // it, and a device-loss rebuild destroys the pipelines alone. The
            // rebuild path used to be a copy of this function that simply did
            // not have these six lines, which is the whole reason the two
            // could drift.
            if (ribbonPipelineLayout_ == VK_NULL_HANDLE) {
                VkDescriptorSetLayout ribLayouts[] = {perFrameLayout, particleTexLayout_};
                VkPipelineLayoutCreateInfo lci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
                lci.setLayoutCount = 2;
                lci.pSetLayouts = ribLayouts;
                // The material's alpha reference, lit and fogged bits.
                VkPushConstantRange pcr{};
                pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
                pcr.size = 4 * sizeof(float);
                lci.pushConstantRangeCount = 1;
                lci.pPushConstantRanges = &pcr;
                vkCreatePipelineLayout(device, &lci, nullptr, &ribbonPipelineLayout_);
            }

            VkVertexInputBindingDescription rBind{};
            rBind.binding = 0;
            rBind.stride = 9 * sizeof(float);
            rBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            std::vector<VkVertexInputAttributeDescription> rAttrs = {
                {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0},                    // pos
                {.location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 3 * sizeof(float)},    // color
                {.location = 2, .binding = 0, .format = VK_FORMAT_R32_SFLOAT,       .offset = 6 * sizeof(float)},    // alpha
                {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,    .offset = 7 * sizeof(float)},    // uv
            };

            auto buildRibbonPipeline = [&](VkPipelineColorBlendAttachmentState blend, VkCullModeFlags cull,
                                           bool depthTest, bool depthWrite) -> VkPipeline {
                return PipelineBuilder()
                    .setShaders(ribVert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                                ribFrag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
                    .setVertexInput({rBind}, rAttrs)
                    .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
                    .setRasterization(VK_POLYGON_MODE_FILL, cull, VK_FRONT_FACE_COUNTER_CLOCKWISE)
                    .setDepthTest(depthTest, depthWrite, VK_COMPARE_OP_LESS_OR_EQUAL)
                    .setColorBlendAttachment(blend)
                    .setMultisample(vkCtx_->getMsaaSamples())
                    .setLayout(ribbonPipelineLayout_)
                    .setRenderPass(mainPass)
                    .setDynamicStates(viewportAndScissorDynamic())
                    .build(device, vkCtx_->getPipelineCache());
            };
            // Each M2 blend's Gx blend (0x00a45570) and its factors (0x00a2f964,
            // 0x00a2f994): opaque and alpha key (one, zero), alpha, add by
            // alpha, no-alpha add (one, one), mod, mod2x.
            const VkPipelineColorBlendAttachmentState ribbonBlends[kRibbonBlends] = {
                PipelineBuilder::blendDisabled(),
                PipelineBuilder::blendDisabled(),
                PipelineBuilder::blendAlpha(),
                colourBlend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE),
                PipelineBuilder::blendAdditive(),
                colourBlend(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO),
                colourBlend(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_SRC_COLOR),
            };
            for (uint32_t b = 0; b < kRibbonBlends; ++b) {
                for (uint32_t v = 0; v < kPipelineVariantCount; ++v) {
                    ribbonPipelines_[b * kPipelineVariantCount + v] = buildRibbonPipeline(
                        ribbonBlends[b], (v & kVariantCull) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
                        (v & kVariantNoDepthTest) == 0, (v & kVariantNoDepthWrite) == 0);
                }
            }
        }
        ribVert.destroy(); ribFrag.destroy();
    }

    // Clean up shader modules
    m2Vert.destroy(); m2Frag.destroy();
    particleVert.destroy(); particleFrag.destroy();

    return true;
}

bool M2Renderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                            pipeline::AssetManager* assets) {
    if (initialized_) { assetManager = assets; return true; }
    vkCtx_ = ctx;
    assetManager = assets;

    // Announce the renderer diagnostics this build understands, and which of
    // them are active. A run that logs this line is definitely a build that has
    // them, which takes the guesswork out of "did that binary include the fix?".
    LOG_INFO("M2 render diagnostics available (NO_PARTICLES/NO_RIBBONS/NO_SKINNING): ",
             "particles=", core::envFlagEnabled("WOWEE_M2_NO_PARTICLES") ? "OFF" : "on",
             " ribbons=", core::envFlagEnabled("WOWEE_M2_NO_RIBBONS") ? "OFF" : "on",
             " skinning=", core::envFlagEnabled("WOWEE_M2_NO_SKINNING") ? "OFF" : "on",
             " maxBonesPerInstance=", kMaxBonesPerInstance);

    // Instance storage grows to tens of thousands as a session explores, and
    // each doubling reallocates the whole thing mid-frame: measured at 8.9ms
    // crossing 32k and 18.5ms crossing 64k, doubling again each time. Take that
    // allocation up front, where a stall is invisible.
    instances.reserve(65536);

    const unsigned hc = std::thread::hardware_concurrency();
    const size_t availableCores = (hc > 1u) ? static_cast<size_t>(hc - 1u) : 1ull;
    // Keep headroom for other frame tasks: M2 gets about half of non-main cores by default.
    const size_t defaultAnimThreads = std::max<size_t>(1, availableCores / 2);
    numAnimThreads_ = static_cast<uint32_t>(std::max<size_t>(
        1, envSizeOrDefault("WOWEE_M2_ANIM_THREADS", defaultAnimThreads)));
    LOG_INFO("Initializing M2 renderer (Vulkan, ", numAnimThreads_, " anim threads)...");

    VkDevice device = vkCtx_->getDevice();

    // --- Descriptor set layouts ---

    // Material set layout (set 1): binding 0 = sampler2D, binding 2 = M2Material UBO,
    // binding 3 = the second texture stage's sampler2D
    // (M2Params moved to push constants alongside model matrix)
    {
        VkDescriptorSetLayoutBinding bindings[3] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 2;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].binding = 3;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 3;
        ci.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &ci, nullptr, &materialSetLayout_);
    }

    // Bone set layout (set 2): binding 0 = STORAGE_BUFFER (bone matrices)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        vkCreateDescriptorSetLayout(device, &ci, nullptr, &boneSetLayout_);
    }

    // Instance data set layout (set 3): binding 0 = STORAGE_BUFFER (per-instance data)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        vkCreateDescriptorSetLayout(device, &ci, nullptr, &instanceSetLayout_);
    }

    // Particle texture set layout (set 1 for particles): binding 0 = sampler2D
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        vkCreateDescriptorSetLayout(device, &ci, nullptr, &particleTexLayout_);
    }

    // --- Descriptor pools ---
    {
        VkDescriptorPoolSize sizes[] = {
            {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = 2 * MAX_MATERIAL_SETS + 256},
            {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = MAX_MATERIAL_SETS + 256},
        };
        VkDescriptorPoolCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = MAX_MATERIAL_SETS + 256;
        ci.poolSizeCount = 2;
        ci.pPoolSizes = sizes;
        ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vkCreateDescriptorPool(device, &ci, nullptr, &materialDescPool_);
    }
    {
        VkDescriptorPoolSize sizes[] = {
            {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = MAX_BONE_SETS},
        };
        VkDescriptorPoolCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = MAX_BONE_SETS;
        ci.poolSizeCount = 1;
        ci.pPoolSizes = sizes;
        ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vkCreateDescriptorPool(device, &ci, nullptr, &boneDescPool_);
    }

    // Create a small identity-bone SSBO + descriptor set so that non-animated
    // draws always have a valid set 2 bound.  The Intel ANV driver segfaults
    // on vkCmdDrawIndexed when a declared descriptor set slot is unbound.
    {
        // Single identity matrix (bone 0 = identity)
        glm::mat4 identity(1.0f);
        VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = sizeof(glm::mat4);
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo allocInfo{};
        vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                        &dummyBoneBuffer_, &dummyBoneAlloc_, &allocInfo);
        if (allocInfo.pMappedData) {
            memcpy(allocInfo.pMappedData, &identity, sizeof(identity));
        }

        dummyBoneSet_ = allocateBoneSet();
        if (dummyBoneSet_) {
            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = dummyBoneBuffer_;
            bufInfo.offset = 0;
            bufInfo.range = sizeof(glm::mat4);
            VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = dummyBoneSet_;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &bufInfo;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
    }

    // Mega bone SSBO - consolidates all animated instance bones into one buffer per frame.
    // Slot 0 = identity matrix (for non-animated instances), slots 1..N = animated instances.
    {
        const VkDeviceSize megaSize = VkDeviceSize(MEGA_BONE_MATRIX_CAPACITY) * sizeof(glm::mat4);
        glm::mat4 identity(1.0f);
        for (int i = 0; i < 2; i++) {
            VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = megaSize;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo allocInfo{};
            vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                            &megaBoneBuffer_[i], &megaBoneAlloc_[i], &allocInfo);
            megaBoneMapped_[i] = allocInfo.pMappedData;

            // Slot 0: identity matrix (for non-animated instances)
            if (megaBoneMapped_[i]) {
                memcpy(megaBoneMapped_[i], &identity, sizeof(identity));
            }

            megaBoneSet_[i] = allocateBoneSet();
            if (megaBoneSet_[i]) {
                VkDescriptorBufferInfo bufInfo{};
                bufInfo.buffer = megaBoneBuffer_[i];
                bufInfo.offset = 0;
                bufInfo.range = megaSize;
                VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = megaBoneSet_[i];
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &bufInfo;
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            }
        }

        // Fresh buffers hold no bone data - invalidate any per-instance upload
        // tracking so prepareRender() re-uploads everything (defensive: instances
        // are normally empty when this runs, but re-init must not skip uploads).
        for (auto& inst : instances) {
            inst.megaBoneUploadedSlot[0] = inst.megaBoneUploadedSlot[1] = 0;
        }
    }

    // Instance data SSBO - per-frame buffer holding per-instance transforms, fade, bones.
    // Shader reads instanceData[push.instanceDataOffset + gl_InstanceIndex].
    {
        static_assert(sizeof(M2InstanceGPU) == 192, "M2InstanceGPU must be 192 bytes (std430)");
        const VkDeviceSize instBufSize = MAX_INSTANCE_DATA * sizeof(M2InstanceGPU);

        // Descriptor pool for 2 sets (double-buffered)
        VkDescriptorPoolSize poolSize{.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2};
        VkDescriptorPoolCreateInfo poolCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolCi.maxSets = 2;
        poolCi.poolSizeCount = 1;
        poolCi.pPoolSizes = &poolSize;
        vkCreateDescriptorPool(device, &poolCi, nullptr, &instanceDescPool_);

        for (int i = 0; i < 2; i++) {
            VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = instBufSize;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo allocInfo{};
            vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                            &instanceBuffer_[i], &instanceAlloc_[i], &allocInfo);
            instanceMapped_[i] = allocInfo.pMappedData;

            VkDescriptorSetAllocateInfo setAi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            setAi.descriptorPool = instanceDescPool_;
            setAi.descriptorSetCount = 1;
            setAi.pSetLayouts = &instanceSetLayout_;
            vkAllocateDescriptorSets(device, &setAi, &instanceSet_[i]);

            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = instanceBuffer_[i];
            bufInfo.offset = 0;
            bufInfo.range = instBufSize;
            VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = instanceSet_[i];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &bufInfo;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
    }

    // GPU frustum culling - compute pipeline, buffers, descriptors.
    // Compute shader tests each instance bounding sphere against 6 frustum planes + distance.
    // Output: uint visibility[] read back by CPU to skip culled instances in sortedVisible_ build.
    {
        static_assert(sizeof(CullInstanceGPU) == 32, "CullInstanceGPU must be 32 bytes (std430)");
        static_assert(sizeof(CullUniformsGPU) == 272, "CullUniformsGPU must be 272 bytes (std140)");

        // Descriptor set layout: binding 0 = UBO (frustum+camera), 1 = SSBO (input), 2 = SSBO (output)
        VkDescriptorSetLayoutBinding bindings[3] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutCi.bindingCount = 3;
        layoutCi.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &layoutCi, nullptr, &cullSetLayout_);

        // Pipeline layout (no push constants - everything via UBO)
        VkPipelineLayoutCreateInfo plCi{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plCi.setLayoutCount = 1;
        plCi.pSetLayouts = &cullSetLayout_;
        vkCreatePipelineLayout(device, &plCi, nullptr, &cullPipelineLayout_);

        // Load compute shader
        rendering::VkShaderModule cullComp;
        if (!cullComp.loadFromFile(device, "assets/shaders/m2_cull.comp.spv")) {
            LOG_ERROR("M2Renderer: failed to load m2_cull.comp.spv - GPU culling disabled");
        } else {
            VkComputePipelineCreateInfo cpCi{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpCi.stage = cullComp.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
            cpCi.layout = cullPipelineLayout_;
            if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCi, nullptr, &cullPipeline_) != VK_SUCCESS) {
                LOG_ERROR("M2Renderer: failed to create cull compute pipeline");
                cullPipeline_ = VK_NULL_HANDLE;
            }
            cullComp.destroy();
        }

        // HiZ-aware cull pipeline (Phase 6.3 Option B)
        // Uses set 0 (same as frustum-only) + set 1 (HiZ pyramid sampler from HiZSystem).
        // The HiZ descriptor set layout is created lazily when hizSystem_ is set, but the
        // pipeline layout and shader are created now if the shader is available.
        rendering::VkShaderModule cullHiZComp;
        if (cullHiZComp.loadFromFile(device, "assets/shaders/m2_cull_hiz.comp.spv")) {
            // HiZ cull set 1 layout: single combined image sampler (the HiZ pyramid)
            VkDescriptorSetLayoutBinding hizBinding{};
            hizBinding.binding = 0;
            hizBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            hizBinding.descriptorCount = 1;
            hizBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

            VkDescriptorSetLayout hizSamplerLayout = VK_NULL_HANDLE;
            VkDescriptorSetLayoutCreateInfo hizLayoutCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            hizLayoutCi.bindingCount = 1;
            hizLayoutCi.pBindings = &hizBinding;
            vkCreateDescriptorSetLayout(device, &hizLayoutCi, nullptr, &hizSamplerLayout);

            VkDescriptorSetLayout hizSetLayouts[2] = {cullSetLayout_, hizSamplerLayout};
            VkPipelineLayoutCreateInfo hizPlCi{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            hizPlCi.setLayoutCount = 2;
            hizPlCi.pSetLayouts = hizSetLayouts;
            vkCreatePipelineLayout(device, &hizPlCi, nullptr, &cullHiZPipelineLayout_);

            VkComputePipelineCreateInfo hizCpCi{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            hizCpCi.stage = cullHiZComp.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
            hizCpCi.layout = cullHiZPipelineLayout_;
            if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &hizCpCi, nullptr, &cullHiZPipeline_) != VK_SUCCESS) {
                LOG_WARNING("M2Renderer: failed to create HiZ cull compute pipeline - HiZ disabled");
                cullHiZPipeline_ = VK_NULL_HANDLE;
                vkDestroyPipelineLayout(device, cullHiZPipelineLayout_, nullptr);
                cullHiZPipelineLayout_ = VK_NULL_HANDLE;
            } else {
                LOG_INFO("M2Renderer: HiZ occlusion cull pipeline created");
            }

            // The hizSamplerLayout is now owned by the pipeline layout; we don't track it
            // separately because the pipeline layout keeps a ref. But actually Vulkan
            // requires us to keep it alive. Store it where HiZSystem will provide it.
            // For now, we can destroy it since the pipeline layout was already created.
            vkDestroyDescriptorSetLayout(device, hizSamplerLayout, nullptr);

            cullHiZComp.destroy();
        } else {
            LOG_INFO("M2Renderer: m2_cull_hiz.comp.spv not found - HiZ occlusion culling not available");
        }

        // Descriptor pool: 2 sets × 3 descriptors each (1 UBO + 2 SSBO)
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0] = {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .descriptorCount = 2};
        poolSizes[1] = {.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 4};  // 2 input + 2 output
        VkDescriptorPoolCreateInfo poolCi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolCi.maxSets = 2;
        poolCi.poolSizeCount = 2;
        poolCi.pPoolSizes = poolSizes;
        vkCreateDescriptorPool(device, &poolCi, nullptr, &cullDescPool_);

        const VkDeviceSize uniformSize = sizeof(CullUniformsGPU);
        const VkDeviceSize inputSize   = MAX_CULL_INSTANCES * sizeof(CullInstanceGPU);
        const VkDeviceSize outputSize  = MAX_CULL_INSTANCES * sizeof(uint32_t);

        for (int i = 0; i < 2; i++) {
            // Uniform buffer (frustum planes + camera)
            {
                VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bci.size = uniformSize;
                bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo ai{};
                vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                                &cullUniformBuffer_[i], &cullUniformAlloc_[i], &ai);
                cullUniformMapped_[i] = ai.pMappedData;
            }
            // Input SSBO (per-instance cull data)
            {
                VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bci.size = inputSize;
                bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo ai{};
                vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                                &cullInputBuffer_[i], &cullInputAlloc_[i], &ai);
                cullInputMapped_[i] = ai.pMappedData;
            }
            // Output SSBO (visibility flags - GPU writes, CPU reads)
            {
                VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bci.size = outputSize;
                bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
                aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo ai{};
                vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                                &cullOutputBuffer_[i], &cullOutputAlloc_[i], &ai);
                cullOutputMapped_[i] = ai.pMappedData;
            }

            // Allocate and write descriptor set
            VkDescriptorSetAllocateInfo setAi{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            setAi.descriptorPool = cullDescPool_;
            setAi.descriptorSetCount = 1;
            setAi.pSetLayouts = &cullSetLayout_;
            vkAllocateDescriptorSets(device, &setAi, &cullSet_[i]);

            VkDescriptorBufferInfo uboInfo{.buffer = cullUniformBuffer_[i], .offset = 0, .range = uniformSize};
            VkDescriptorBufferInfo inputInfo{.buffer = cullInputBuffer_[i], .offset = 0, .range = inputSize};
            VkDescriptorBufferInfo outputInfo{.buffer = cullOutputBuffer_[i], .offset = 0, .range = outputSize};

            VkWriteDescriptorSet writes[3] = {};
            writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[0].dstSet = cullSet_[i];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &uboInfo;

            writes[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[1].dstSet = cullSet_[i];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[1].pBufferInfo = &inputInfo;

            writes[2] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[2].dstSet = cullSet_[i];
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[2].pBufferInfo = &outputInfo;

            vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        }
    }

    // --- Pipeline layouts ---

    // Main M2 pipeline layout: set 0 = perFrame, set 1 = material, set 2 = bones, set 3 = instances
    // Push constant: int texCoordSet + int isFoliage + int instanceDataOffset
    //              + float swayRefHeight + float swayAmp (both unused) + float plantHeight (24 bytes)
    {
        VkDescriptorSetLayout setLayouts[] = {perFrameLayout, materialSetLayout_, boneSetLayout_, instanceSetLayout_};
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = 24;

        VkPipelineLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 4;
        ci.pSetLayouts = setLayouts;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pushRange;
        vkCreatePipelineLayout(device, &ci, nullptr, &pipelineLayout_);
    }

    // Particle pipeline layout: set 0 = perFrame, set 1 = particleTex
    // Push constant: vec2 tileCount + int alphaKey (12 bytes)
    {
        VkDescriptorSetLayout setLayouts[] = {perFrameLayout, particleTexLayout_};
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = 20; // vec2 tileCount + int alphaKey + int lit + int fogMode

        VkPipelineLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 2;
        ci.pSetLayouts = setLayouts;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pushRange;
        vkCreatePipelineLayout(device, &ci, nullptr, &particlePipelineLayout_);
    }

    perFrameLayout_ = perFrameLayout;
    if (!buildMainPassPipelines(perFrameLayout)) return false;

    // --- Create dynamic particle buffers (mapped for CPU writes) ---
    {
        VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocInfo{};

        // M2 particle buffer
        // One per frame in flight (see kDynamicVBSlots): with a single
        // buffer the CPU rewrote what the GPU was still drawing from, and the
        // particles flickered whenever the two overlapped.
        static_assert(kDynamicVBSlots == MAX_FRAMES_IN_FLIGHT);
        for (uint32_t i = 0; i < kDynamicVBSlots; ++i) {
            bci.size = MAX_M2_PARTICLE_VERTS * 9 * sizeof(float);
            vmaCreateBuffer(vkCtx_->getAllocator(), &bci, &aci, &m2ParticleVB_[i], &m2ParticleVBAlloc_[i], &allocInfo);
            m2ParticleVBMapped_[i] = allocInfo.pMappedData;

        // Ribbon vertex buffer - triangle strip: pos(3)+color(3)+alpha(1)+uv(2)=9 floats/vert
            bci.size = MAX_RIBBON_VERTS * 9 * sizeof(float);
            vmaCreateBuffer(vkCtx_->getAllocator(), &bci, &aci, &ribbonVB_[i], &ribbonVBAlloc_[i], &allocInfo);
            ribbonVBMapped_[i] = allocInfo.pMappedData;
        }
    }

    // --- Create white fallback texture ---
    {
        uint8_t white[] = {255, 255, 255, 255};
        whiteTexture_ = std::make_unique<VkTexture>();
        whiteTexture_->upload(*vkCtx_, white, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
        whiteTexture_->createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    }

    textureCacheBudgetBytes_ =
        envSizeMBOrDefault("WOWEE_M2_TEX_CACHE_MB", 4096) * 1024ull * 1024ull;
    modelCacheLimit_ = envSizeMBOrDefault("WOWEE_M2_MODEL_LIMIT", 6000);
    LOG_INFO("M2 texture cache budget: ", textureCacheBudgetBytes_ / (1024 * 1024), " MB");
    LOG_INFO("M2 model cache limit: ", modelCacheLimit_);

    LOG_INFO("M2 renderer initialized (Vulkan)");
    initialized_ = true;
    return true;
}

void M2Renderer::invalidateCullOutput(uint32_t frameIndex) {
    // On non-HOST_COHERENT memory, VMA-mapped GPU→CPU buffers need explicit
    // invalidation so the CPU cache sees the latest GPU writes.
    if (frameIndex < 2 && cullOutputAlloc_[frameIndex]) {
        vmaInvalidateAllocation(vkCtx_->getAllocator(), cullOutputAlloc_[frameIndex], 0, VK_WHOLE_SIZE);
    }
}

void M2Renderer::destroyPipelineVariants() {
    if (vkCtx_) rendering::destroyPipelineVariants(vkCtx_->getDevice(), pipelineVariants_);
}

void M2Renderer::destroyRibbonPipelines() {
    if (!vkCtx_) return;
    for (VkPipeline& p : ribbonPipelines_) {
        if (p) vkDestroyPipeline(vkCtx_->getDevice(), p, nullptr);
        p = VK_NULL_HANDLE;
    }
}

void M2Renderer::shutdown() {
    LOG_INFO("Shutting down M2 renderer...");
    if (!vkCtx_) return;

    vkDeviceWaitIdle(vkCtx_->getDevice());
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator alloc = vkCtx_->getAllocator();

    // Delete model GPU resources
    for (auto& [id, model] : models) {
        destroyModelGPU(model);
    }
    models.clear();
    pinnedModelIds_.clear();

    // Destroy instance bone buffers
    for (auto& inst : instances) {
        destroyInstanceBones(inst);
    }
    instances.clear();
    shadowCullDirty_ = true;
    spatialGrid.clear();
    instanceIndexById.clear();
    instanceDedupMap_.clear();
    // Model and bone destruction above is deferred; drain it now while the
    // descriptor pools are still alive.
    vkCtx_->flushDeferredCleanup();

    // Delete cached textures. ~VkTexture is empty by design -- it has no device
    // or allocator to free with -- so clearing the map on its own drops the
    // unique_ptrs and leaks every image, view and allocation behind them.
    for (auto& [path, entry] : textureCache) {
        if (entry.texture) entry.texture->destroy(device, alloc);
    }
    textureCache.clear();
    stripTextures_.clear();
    clientStrips_.clear();
    clientStripSets_.clear();
    // The singletons the cache never held. Same reason as above: a
    // unique_ptr<VkTexture> releases nothing on its own.
    if (whiteTexture_) { whiteTexture_->destroy(device, alloc); whiteTexture_.reset(); }
    textureCacheBytes_ = 0;
    textureCacheCounter_ = 0;
    texturePropsByPtr_.clear();
    failedTextureCache_.clear();
    failedTextureRetryAt_.clear();
    loggedTextureLoadFails_.clear();
    textureLookupSerial_ = 0;
    textureBudgetRejectWarnings_ = 0;
    whiteTexture_.reset();

    // Clean up particle/ribbon buffers
    for (uint32_t i = 0; i < kDynamicVBSlots; ++i) {
        destroy(alloc, m2ParticleVB_[i], m2ParticleVBAlloc_[i]);
        destroy(alloc, ribbonVB_[i], ribbonVBAlloc_[i]);
    }

    // Destroy pipelines
    auto destroyPipeline = [&](VkPipeline& p) { if (p) { vkDestroyPipeline(device, p, nullptr); p = VK_NULL_HANDLE; } };
    destroyPipeline(opaquePipeline_);
    destroyPipeline(cutoutPipeline_);
    destroyPipeline(alphaTestPipeline_);
    destroyPipeline(alphaPipeline_);
    destroyPipeline(additivePipeline_);
    destroyPipelineVariants();
    destroyPipeline(noAlphaAddPipeline_);
    destroyPipeline(modPipeline_);
    destroyPipeline(mod2xPipeline_);
    destroyPipeline(particlePipeline_);
    destroyPipeline(particleAdditivePipeline_);
    destroyPipeline(particleNoAlphaAddPipeline_);
    destroyPipeline(particleModPipeline_);
    destroyPipeline(particleMod2xPipeline_);
    destroyRibbonPipelines();

    destroy(device, pipelineLayout_);
    destroy(device, particlePipelineLayout_);
    destroy(device, ribbonPipelineLayout_);

    // Destroy descriptor pools and layouts
    destroy(alloc, dummyBoneBuffer_, dummyBoneAlloc_);
    // dummyBoneSet_ is freed implicitly when boneDescPool_ is destroyed
    dummyBoneSet_ = VK_NULL_HANDLE;
    // Mega bone SSBO cleanup (sets freed implicitly with boneDescPool_)
    for (int i = 0; i < 2; i++) {
        destroy(alloc, megaBoneBuffer_[i], megaBoneAlloc_[i]);
        megaBoneMapped_[i] = nullptr;
        megaBoneSet_[i] = VK_NULL_HANDLE;
    }
    destroy(device, materialDescPool_);
    if (boneDescPool_) {
        if (boneDescPoolGeneration_) boneDescPoolGeneration_->fetch_add(1, std::memory_order_relaxed);
        vkDestroyDescriptorPool(device, boneDescPool_, nullptr);
        boneDescPool_ = VK_NULL_HANDLE;
    }
    // Instance data SSBO cleanup (sets freed with instanceDescPool_)
    for (int i = 0; i < 2; i++) {
        destroy(alloc, instanceBuffer_[i], instanceAlloc_[i]);
        instanceMapped_[i] = nullptr;
        instanceSet_[i] = VK_NULL_HANDLE;
    }
    destroy(device, instanceDescPool_);

    // GPU frustum culling compute pipeline + buffers cleanup
    destroy(device, cullHiZPipeline_);
    destroy(device, cullHiZPipelineLayout_);
    destroy(device, cullPipeline_);
    destroy(device, cullPipelineLayout_);
    for (int i = 0; i < 2; i++) {
        destroy(alloc, cullUniformBuffer_[i], cullUniformAlloc_[i]);
        destroy(alloc, cullInputBuffer_[i], cullInputAlloc_[i]);
        destroy(alloc, cullOutputBuffer_[i], cullOutputAlloc_[i]);
        cullUniformMapped_[i] = cullInputMapped_[i] = cullOutputMapped_[i] = nullptr;
        cullSet_[i] = VK_NULL_HANDLE;
    }
    destroy(device, cullDescPool_);
    destroy(device, cullSetLayout_);

    destroy(device, materialSetLayout_);
    destroy(device, boneSetLayout_);
    destroy(device, instanceSetLayout_);
    destroy(device, particleTexLayout_);

    // Destroy shadow resources
    destroyPipeline(shadowPipeline_);
    destroy(device, shadowPipelineLayout_);
    destroyPipeline(shadowInstancedPipeline_);
    destroyPipeline(shadowInstancedDepthOnlyPipeline_);
    destroy(device, shadowInstancedLayout_);
    for (int i = 0; i < 2; i++) {
        destroy(alloc, shadowInstanceBuffer_[i], shadowInstanceAlloc_[i]);
        shadowInstanceMapped_[i] = nullptr;
        shadowInstanceSet_[i] = VK_NULL_HANDLE;  // freed with the pool
    }
    destroy(device, shadowInstanceDescPool_);
    for (auto& pool : shadowTexPool_) { if (pool) { vkDestroyDescriptorPool(device, pool, nullptr); pool = VK_NULL_HANDLE; } }
    destroyShadowParamsSet(device, alloc, shadowParams_);

    initialized_ = false;
}

void M2Renderer::destroyModelGPU(M2ModelGPU& model) {
    if (!vkCtx_) return;
    releaseRtModel(model);
    VmaAllocator alloc = vkCtx_->getAllocator();
    destroy(alloc, model.vertexBuffer, model.vertexAlloc);
    destroy(alloc, model.indexBuffer, model.indexAlloc);
    VkDevice device = vkCtx_->getDevice();
    for (auto& batch : model.batches) {
        if (batch.materialSet) { vkFreeDescriptorSets(device, materialDescPool_, 1, &batch.materialSet); batch.materialSet = VK_NULL_HANDLE; }
        destroy(alloc, batch.materialUBO, batch.materialUBOAlloc);
    }
    // Free pre-allocated particle texture descriptor sets
    for (auto& pSet : model.particleTexSets) {
        if (pSet) { vkFreeDescriptorSets(device, materialDescPool_, 1, &pSet); pSet = VK_NULL_HANDLE; }
    }
    model.particleTexSets.clear();
    // Free ribbon texture descriptor sets
    for (auto& rSet : model.ribbonTexSets) {
        if (rSet) { vkFreeDescriptorSets(device, materialDescPool_, 1, &rSet); rSet = VK_NULL_HANDLE; }
    }
    model.ribbonTexSets.clear();
}

void M2Renderer::destroyInstanceBones(M2Instance& inst, bool defer) {
    if (!vkCtx_) return;
    releaseInstanceBones(*vkCtx_, boneDescPool_, boneDescPoolGeneration_, inst, defer);
}

VkDescriptorSet M2Renderer::allocateMaterialSet() {
    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = materialDescPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &materialSetLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult result = vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set);
    if (result != VK_SUCCESS) {
        LOG_ERROR("M2Renderer: material descriptor set allocation failed (", result, ")");
        return VK_NULL_HANDLE;
    }
    return set;
}

VkDescriptorSet M2Renderer::allocateBoneSet() {
    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = boneDescPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &boneSetLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult result = vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set);
    if (result != VK_SUCCESS) {
        LOG_ERROR("M2Renderer: bone descriptor set allocation failed (", result, ")");
        return VK_NULL_HANDLE;
    }
    return set;
}

// ---------------------------------------------------------------------------
// M2 collision mesh: build spatial grid + classify triangles
// ---------------------------------------------------------------------------
void M2ModelGPU::CollisionMesh::build() {
    if (indices.size() < 3 || vertices.empty()) return;
    triCount = static_cast<uint32_t>(indices.size() / 3);

    // Bounding box for grid
    glm::vec3 bmin(std::numeric_limits<float>::max());
    glm::vec3 bmax(-std::numeric_limits<float>::max());
    for (const auto& v : vertices) {
        bmin = glm::min(bmin, v);
        bmax = glm::max(bmax, v);
    }

    gridOrigin = glm::vec2(bmin.x, bmin.y);
    gridCellsX = std::max(1, std::min(32, static_cast<int>(std::ceil((bmax.x - bmin.x) / CELL_SIZE))));
    gridCellsY = std::max(1, std::min(32, static_cast<int>(std::ceil((bmax.y - bmin.y) / CELL_SIZE))));

    cellFloorTris.resize(static_cast<size_t>(gridCellsX) * static_cast<size_t>(gridCellsY));
    cellWallTris.resize(static_cast<size_t>(gridCellsX) * static_cast<size_t>(gridCellsY));
    triBounds.resize(triCount);

    for (uint32_t ti = 0; ti < triCount; ti++) {
        uint16_t i0 = indices[ti * 3];
        uint16_t i1 = indices[ti * 3 + 1];
        uint16_t i2 = indices[ti * 3 + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;

        const auto& v0 = vertices[i0];
        const auto& v1 = vertices[i1];
        const auto& v2 = vertices[i2];

        triBounds[ti].minZ = std::min({v0.z, v1.z, v2.z});
        triBounds[ti].maxZ = std::max({v0.z, v1.z, v2.z});

        glm::vec3 normal = glm::cross(v1 - v0, v2 - v0);
        float normalLen = glm::length(normal);
        float absNz = (normalLen > 0.001f) ? std::abs(normal.z / normalLen) : 0.0f;
        bool isFloor = (absNz >= 0.35f);  // ~70° max slope (relaxed for steep stairs)
        bool isWall  = (absNz < 0.65f);

        float triMinX = std::min({v0.x, v1.x, v2.x});
        float triMaxX = std::max({v0.x, v1.x, v2.x});
        float triMinY = std::min({v0.y, v1.y, v2.y});
        float triMaxY = std::max({v0.y, v1.y, v2.y});

        int cxMin = std::clamp(static_cast<int>((triMinX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
        int cxMax = std::clamp(static_cast<int>((triMaxX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
        int cyMin = std::clamp(static_cast<int>((triMinY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);
        int cyMax = std::clamp(static_cast<int>((triMaxY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);

        for (int cy = cyMin; cy <= cyMax; cy++) {
            for (int cx = cxMin; cx <= cxMax; cx++) {
                int ci = cy * gridCellsX + cx;
                if (isFloor) cellFloorTris[ci].push_back(ti);
                if (isWall)  cellWallTris[ci].push_back(ti);
            }
        }
    }
}

/// The triangles of one cell array that a query box reaches, deduplicated.
///
/// Floors and walls are asked separately and the two queries differed in one
/// token: which array they read. A triangle spanning several cells is filed
/// under each, so the sort and unique are not tidiness - a caller that tests
/// the same triangle twice counts two hits, and a raycast then reports an even
/// number of crossings where there was one surface.
void M2ModelGPU::CollisionMesh::gatherTrisInRange(
        const std::vector<std::vector<uint32_t>>& cells,
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    out.clear();
    if (gridCellsX == 0 || gridCellsY == 0) return;

    const int cxMin = std::clamp(static_cast<int>((minX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
    const int cxMax = std::clamp(static_cast<int>((maxX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
    const int cyMin = std::clamp(static_cast<int>((minY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);
    const int cyMax = std::clamp(static_cast<int>((maxY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);

    const size_t cellCount = static_cast<size_t>(cxMax - cxMin + 1) *
                             static_cast<size_t>(cyMax - cyMin + 1);
    out.reserve(cellCount * 8);
    for (int cy = cyMin; cy <= cyMax; cy++) {
        for (int cx = cxMin; cx <= cxMax; cx++) {
            const auto& cell = cells[cy * gridCellsX + cx];
            out.insert(out.end(), cell.begin(), cell.end());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

void M2ModelGPU::CollisionMesh::getFloorTrisInRange(
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    gatherTrisInRange(cellFloorTris, minX, minY, maxX, maxY, out);
}

void M2ModelGPU::CollisionMesh::getWallTrisInRange(
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    gatherTrisInRange(cellWallTris, minX, minY, maxX, maxY, out);
}

bool M2Renderer::hasModel(uint32_t modelId) const {
    return models.find(modelId) != models.end();
}

namespace {

} // namespace

void M2Renderer::markModelAsSpellEffect(uint32_t modelId) {
    auto it = models.find(modelId);
    if (it != models.end()) {
        it->second.isSpellEffect = true;
        // Spell effects MUST have bone animation for ribbons/particles to work.
        if (it->second.disableAnimation && it->second.hasAnimation) {
            it->second.disableAnimation = false;
            LOG_INFO("SpellEffect: re-enabled animation for '", it->second.name, "'");
        }
    }
}

void M2Renderer::censusInstance(const M2Instance& instance) {
    static const bool kCensus = std::getenv("WOWEE_M2_CENSUS") != nullptr;
    if (!kCensus) return;
    auto it = models.find(instance.modelId);
    if (it == models.end()) return;
    const M2ModelGPU& gpu = it->second;
    static std::set<std::string> said;
    if (!said.insert(gpu.name).second) return;
    const float authored = gpu.boundMax.z - gpu.boundMin.z;
    LOG_WARNING("M2 census instance: '", gpu.name, "' scale=", instance.scale,
                " authoredH=", authored, " drawnH=", authored * instance.scale,
                " top=", instance.position.z + gpu.boundMax.z * instance.scale);
}

bool M2Renderer::loadModel(const pipeline::M2Model& model, uint32_t modelId) {
    if (models.find(modelId) != models.end()) {
        // Already loaded
        return true;
    }
    if (models.size() >= modelCacheLimit_) {
        if (modelLimitRejectWarnings_ < 3) {
            LOG_WARNING("M2 model cache full (", models.size(), "/", modelCacheLimit_,
                        "), skipping model load: id=", modelId, " name=", model.name);
        }
        ++modelLimitRejectWarnings_;
        return false;
    }

    // Every model this renderer takes on, named once.
    //
    // Creatures say what they draw through the spawner, and doodads are placed
    // from ADTs that can be read offline - but a spell visual, an attached
    // effect or anything else spawned at runtime appears in no list at all.
    // The Elemental Slave's white sheets are the case: its own model, skins,
    // particles, ribbons, bones and vertex weights were each measured and
    // found correct, which leaves something drawn beside it that nothing names.
    //
    // Cheap enough to leave on - once per model, and a session loads a few
    // hundred - and it is the list every "what is that thing" question starts
    // from.
    // Off unless asked for. This is an inventory, not a fault: it names every
    // model the renderer takes on, which is 197 lines of a 884-line log and
    // the largest single source in it. Worth having - it is what finally
    // showed that an imported override, not the shipped model, was what the
    // client drew - and not worth carrying every session.
    //
    // Two budgets, because one is eaten by the other. A zone's terrain streams
    // in hundreds of doodads within a second or two of arriving, and on the
    // first run of this the four hundred were spent before the creature that
    // prompted it had even spawned. Doodads are placed from ADTs and can be
    // enumerated offline; what cannot is anything spawned at runtime, so that
    // gets the larger share and its own allowance.
    static const bool kLoadDiag = core::envFlagEnabled("WOWEE_M2_LOAD_DIAG", false);
    if (kLoadDiag) {
        const bool placedDoodad = model.name.rfind("WORLD\\", 0) == 0 ||
                                  model.name.rfind("world\\", 0) == 0;
        static core::LogBudget doodadLoadBudget(120, "placed doodad models named at load");
        static core::LogBudget spawnedLoadBudget(600, "spawned models named at load");
        core::LogBudget& budget = placedDoodad ? doodadLoadBudget : spawnedLoadBudget;
        if (budget.take()) {
            LOG_WARNING("M2 load: '", model.name.empty() ? "<unnamed>" : model.name,
                        "' id=", modelId,
                        " verts=", model.vertices.size(),
                        " emitters=", model.particleEmitters.size(),
                        " ribbons=", model.ribbonEmitters.size());
        }
    }

    bool hasGeometry = !model.vertices.empty() && !model.indices.empty();
    bool hasParticles = !model.particleEmitters.empty();
    bool hasRibbons   = !model.ribbonEmitters.empty();
    if (!hasGeometry && !hasParticles && !hasRibbons) {
        LOG_WARNING("M2 model has no renderable content: id=", modelId,
                    " name=", model.name.empty() ? "<unnamed>" : model.name);
        return false;
    }

    M2ModelGPU gpuModel;
    gpuModel.name = model.name;

    // Use tight bounds from actual vertices for collision/camera occlusion.
    // Header bounds in some M2s are overly conservative.
    glm::vec3 tightMin(0.0f);
    glm::vec3 tightMax(0.0f);
    if (hasGeometry) {
        tightMin = glm::vec3(std::numeric_limits<float>::max());
        tightMax = glm::vec3(-std::numeric_limits<float>::max());
        for (const auto& v : model.vertices) {
            // Skip NaN-positioned vertices - would corrupt the bounds
            // (glm::min on NaN is implementation-defined) and feed NaN
            // into the camera-occlusion / culling AABB.
            if (!std::isfinite(v.position.x) || !std::isfinite(v.position.y) ||
                !std::isfinite(v.position.z)) continue;
            tightMin = glm::min(tightMin, v.position);
            tightMax = glm::max(tightMax, v.position);
        }
        // If all vertices were NaN (very unlikely after the loader scrub
        // but defense in depth), fall back to a unit box around origin.
        if (tightMin.x > tightMax.x) {
            tightMin = glm::vec3(-1.0f);
            tightMax = glm::vec3(1.0f);
        }
    }

    // Classify model from name and geometry - pure function, no GPU dependencies.
    auto cls = classifyM2Model(model.name, tightMin, tightMax,
                                model.vertices.size(),
                                model.particleEmitters.size());
    const bool isInvisibleTrap   = cls.isInvisibleTrap;
    if (isInvisibleTrap) {
        LOG_INFO("Loading InvisibleTrap model: ", model.name, " (will be invisible, no collision)");
    }

    gpuModel.isInvisibleTrap             = cls.isInvisibleTrap;
    gpuModel.collisionSteppedFountain    = cls.collisionSteppedFountain;
    gpuModel.collisionSteppedLowPlatform = cls.collisionSteppedLowPlatform;
    gpuModel.collisionBridge             = cls.collisionBridge;
    gpuModel.collisionPlanter            = cls.collisionPlanter;
    gpuModel.collisionStatue             = cls.collisionStatue;
    gpuModel.collisionTreeTrunk          = cls.collisionTreeTrunk;
    gpuModel.collisionNarrowVerticalProp = cls.collisionNarrowVerticalProp;
    gpuModel.collisionSmallSolidProp     = cls.collisionSmallSolidProp;
    gpuModel.collisionNoBlock            = cls.collisionNoBlock;
    gpuModel.isGroundDetail              = cls.isGroundDetail;
    gpuModel.isFoliageLike               = cls.isFoliageLike;
    gpuModel.shadowWindFoliage           = cls.shadowWindFoliage;
    gpuModel.isFireflyEffect             = cls.isFireflyEffect;
    gpuModel.isSmallFoliage              = cls.isSmallFoliage;
    gpuModel.isSpellEffect               = cls.isSpellEffect;
    gpuModel.isWaterVegetation           = cls.isWaterVegetation;
    gpuModel.isLanternLike               = cls.isLanternLike;
    gpuModel.isKoboldFlame               = cls.isKoboldFlame;
    gpuModel.isWaterfall                 = cls.isWaterfall;
    gpuModel.isBrazierOrFire             = cls.isBrazierOrFire;
    gpuModel.isTorch                     = cls.isTorch;
    // Data-driven flight-path detection: name tokens miss many flying doodads
    // (buzzards, swallows, bird swarms, ...). A small mesh whose bone animation
    // translates it tens of units is a flight-path doodad - it visibly freezes
    // mid-air whenever distance culling stops its bone updates, so give it the
    // same treatment as named sky birds.
    bool flightPathDoodad = cls.isSkyBird;
    if (!flightPathDoodad) {
        glm::vec3 meshExtent = tightMax - tightMin;
        const bool smallMesh = meshExtent.x < 6.0f && meshExtent.y < 6.0f &&
                               meshExtent.z < 6.0f;
        if (smallMesh) {
            constexpr float kFlightPathRange = 15.0f;
            for (const auto& bone : model.bones) {
                for (const auto& seq : bone.translation.sequences) {
                    for (const auto& v : seq.vec3Values) {
                        if (std::abs(v.x) > kFlightPathRange ||
                            std::abs(v.y) > kFlightPathRange ||
                            std::abs(v.z) > kFlightPathRange) {
                            flightPathDoodad = true;
                            break;
                        }
                    }
                    if (flightPathDoodad) break;
                }
                if (flightPathDoodad) break;
            }
            if (flightPathDoodad) {
                LOG_DEBUG("Flight-path doodad detected (unnamed sky bird): ", model.name);
            }
        }
    }
    gpuModel.isSkyBird                   = flightPathDoodad;
    gpuModel.isLightBeam                 = cls.isLightBeam;
    // WOWEE_M2_CENSUS=1: every model that loads, once, with what it is made
    // of and what the classifier made of it.
    //
    // Gated on the fire classification, this said nothing at all - and that
    // was the answer: hasWord(n, "bonfire") wants "bonfire" delimited, so
    // ORCPVPBONFIRELARGE is not a fire as far as the classifier is concerned,
    // and neither is anything else whose name runs its words together. A
    // screenshot cannot be grepped and guessing at the model has cost two
    // rounds, so this just lists them.
    static const bool kCensus = std::getenv("WOWEE_M2_CENSUS") != nullptr;
    if (kCensus) {
        LOG_WARNING("M2 census: '", gpuModel.name,
                    "' h=", tightMax.z - tightMin.z,
                    " top=", tightMax.z,
                    " verts=", model.vertices.size(),
                    " batches=", model.batches.size(),
                    " emitters=", model.particleEmitters.size(),
                    " ribbons=", model.ribbonEmitters.size(),
                    " bones=", model.bones.size(),
                    " fire=", cls.isBrazierOrFire ? 1 : 0,
                    " torch=", cls.isTorch ? 1 : 0,
                    " spellFx=", cls.isSpellEffect ? 1 : 0);
    }
    gpuModel.isTransportDoodad           = cls.isTransportDoodad;
    gpuModel.ambientEmitterType          = cls.ambientEmitterType;
    gpuModel.boundMin = tightMin;
    gpuModel.boundMax = tightMax;
    gpuModel.vertexBoxMin = model.vertexBoxMin;
    gpuModel.vertexBoxMax = model.vertexBoxMax;
    gpuModel.hasVertexBox = model.hasVertexBox;
    gpuModel.boundRadius = model.boundRadius;
    // Fallback when the M2 header reports 0. Measured from the model origin,
    // like the header value it stands in for: the sphere this feeds is centred
    // there, not on the box. See pipeline/model_bounds.hpp.
    if (gpuModel.boundRadius < 0.01f && !model.vertices.empty()) {
        gpuModel.boundRadius =
            pipeline::modelBoundsOf(model.vertices,
                                    [](const pipeline::M2Vertex& v) {
                                        return v.position;
                                    })
                .radius;
    }
    gpuModel.indexCount = static_cast<uint32_t>(model.indices.size());
    gpuModel.vertexCount = static_cast<uint32_t>(model.vertices.size());

    // Store bone/sequence data for animation
    gpuModel.bones = model.bones;
    gpuModel.sequences = model.sequences;
    gpuModel.globalSequenceDurations = model.globalSequenceDurations;
    gpuModel.hasAnimation = false;
    for (const auto& bone : model.bones) {
        // A billboard bone turns toward the camera every frame, so a model
        // that has one is animated even when no track moves anything. Without
        // this the bones were posed once at spawn, and a glow card or a face
        // such as the demon crystal's eyes stayed turned to wherever the
        // camera was then - edge-on, and so invisible, from everywhere else.
        if (bone.translation.hasData() || bone.rotation.hasData() || bone.scale.hasData() ||
            (bone.flags & kM2BoneSphericalBillboard) != 0) {
            gpuModel.hasAnimation = true;
            break;
        }
    }

    // Build collision mesh + spatial grid from M2 bounding geometry
    gpuModel.collision.vertices = model.collisionVertices;
    gpuModel.collision.indices = model.collisionIndices;
    gpuModel.collision.build();
    if (gpuModel.collision.valid()) {
        core::Logger::getInstance().debug("  M2 collision mesh: ", gpuModel.collision.triCount,
            " tris, grid ", gpuModel.collision.gridCellsX, "x", gpuModel.collision.gridCellsY);
    }


    // Batch all GPU uploads (VB, IB, textures) into a single command buffer
    // submission with one fence wait, instead of one fence wait per upload.
    vkCtx_->beginUploadBatch();

    if (hasGeometry) {
        // Create VBO with interleaved vertex data
        // Format: position (3), normal (3), texcoord0 (2), texcoord1 (2), boneWeights (4), boneIndices (4 as float)
        const size_t floatsPerVertex = 18;
        std::vector<float> vertexData;
        vertexData.reserve(model.vertices.size() * floatsPerVertex);

        for (const auto& v : model.vertices) {
            vertexData.push_back(v.position.x);
            vertexData.push_back(v.position.y);
            vertexData.push_back(v.position.z);
            vertexData.push_back(v.normal.x);
            vertexData.push_back(v.normal.y);
            vertexData.push_back(v.normal.z);
            vertexData.push_back(v.texCoords[0].x);
            vertexData.push_back(v.texCoords[0].y);
            vertexData.push_back(v.texCoords[1].x);
            vertexData.push_back(v.texCoords[1].y);
            float w0 = v.boneWeights[0] / 255.0f;
            float w1 = v.boneWeights[1] / 255.0f;
            float w2 = v.boneWeights[2] / 255.0f;
            float w3 = v.boneWeights[3] / 255.0f;
            vertexData.push_back(w0);
            vertexData.push_back(w1);
            vertexData.push_back(w2);
            vertexData.push_back(w3);
            vertexData.push_back(static_cast<float>(std::min(v.boneIndices[0], uint8_t(127))));
            vertexData.push_back(static_cast<float>(std::min(v.boneIndices[1], uint8_t(127))));
            vertexData.push_back(static_cast<float>(std::min(v.boneIndices[2], uint8_t(127))));
            vertexData.push_back(static_cast<float>(std::min(v.boneIndices[3], uint8_t(127))));
        }

        // Upload vertex buffer to GPU
        {
            auto buf = uploadBuffer(*vkCtx_,
                vertexData.data(), vertexData.size() * sizeof(float),
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            gpuModel.vertexBuffer = buf.buffer;
            gpuModel.vertexAlloc = buf.allocation;
        }

        // Upload index buffer to GPU
        {
            auto buf = uploadBuffer(*vkCtx_,
                model.indices.data(), model.indices.size() * sizeof(uint16_t),
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            gpuModel.indexBuffer = buf.buffer;
            gpuModel.indexAlloc = buf.allocation;
        }

        if (!gpuModel.vertexBuffer || !gpuModel.indexBuffer) {
            LOG_ERROR("M2Renderer::loadModel: GPU buffer upload failed for model ", modelId);
        }
    }

    // Load ALL textures from the model into a local vector.
    // textureLoadFailed[i] is true if texture[i] had a named path that failed to load.
    // Such batches are hidden (batchOpacity=0) rather than rendered white.
    std::vector<VkTexture*> allTextures;
    std::vector<bool> textureLoadFailed;
    std::vector<std::string> textureKeysLower;
    if (assetManager) {
        for (size_t ti = 0; ti < model.textures.size(); ti++) {
            const auto& tex = model.textures[ti];
            std::string texPath = tex.filename;
            // Some extracted M2 texture strings contain embedded NUL + garbage suffix.
            // Truncate at first NUL so valid paths like "...foo.blp\0junk" still resolve.
            size_t nul = texPath.find('\0');
            if (nul != std::string::npos) {
                texPath.resize(nul);
            }
            if (!texPath.empty()) {
                std::string texKey = texPath;
                std::replace(texKey.begin(), texKey.end(), '/', '\\');
                std::transform(texKey.begin(), texKey.end(), texKey.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                VkTexture* texPtr = loadTexture(texPath, tex.flags);
                bool failed = (texPtr == whiteTexture_.get());
                if (failed) {
                    static uint32_t loggedModelTextureFails = 0;
                    static bool loggedModelTextureFailSuppressed = false;
                    if (loggedModelTextureFails < 250) {
                        LOG_WARNING("M2 model ", model.name, " texture[", ti, "] failed to load: ", texPath);
                        ++loggedModelTextureFails;
                    } else if (!loggedModelTextureFailSuppressed) {
                        LOG_WARNING("M2 model texture-failure warnings suppressed after ",
                                    loggedModelTextureFails, " entries");
                        loggedModelTextureFailSuppressed = true;
                    }
                }
                if (isInvisibleTrap) {
                    LOG_INFO("  InvisibleTrap texture[", ti, "]: ", texPath, " -> ", (failed ? "WHITE" : "OK"));
                }
                allTextures.push_back(texPtr);
                textureLoadFailed.push_back(failed);
                textureKeysLower.push_back(std::move(texKey));
            } else {
                if (isInvisibleTrap) {
                    LOG_INFO("  InvisibleTrap texture[", ti, "]: EMPTY (using white fallback)");
                }
                // A slot with no filename is one the model expects someone
                // else to fill: a creature skin from CreatureDisplayInfo
                // (types 11-13), a character component, an item texture. When
                // nothing fills it the batch draws the white fallback, flat
                // and unlit, and the only sign is on screen - which is what
                // "glow cards rendered as white 2D meshes" turned out to be
                // every previous time it was reported.
                //
                // Not an error: plenty of these are filled a moment later by
                // setModelTexture or setTextureSlotOverride, and the renderer
                // cannot see that from here. It is worth naming anyway,
                // because when it is not filled nothing else says so and the
                // model has to be guessed at from a screenshot.
                // Type 0 means the model names its own texture, so an empty
                // name is a broken model - and it draws flat white, which
                // reads as a missing texture rather than as a bad file.
                //
                // This was written the other way round at first, warning for
                // types 11-13, where an empty name is how a creature says its
                // skin comes from CreatureDisplayInfo. It never fired once.
                // Meanwhile the Elemental Slave's white sheets were an
                // imported override with three type-0 slots and no names in
                // them, which this would have found in a single run.
                if (tex.type == 0) {
                    static core::LogBudget unnamedSlotBudget(
                        24, "M2 models with an unnamed texture of their own");
                    if (unnamedSlotBudget.take()) {
                        LOG_WARNING("M2 '", model.name, "' texture[", ti,
                                    "] names no file but is type 0, which means it"
                                    " should - it draws white");
                    }
                }
                allTextures.push_back(whiteTexture_.get());
                textureLoadFailed.push_back(false);  // Empty filename = intentional white (type!=0)
                textureKeysLower.emplace_back();
            }
        }
    }

    // Copy particle emitter data and resolve textures
    gpuModel.particleEmitters = model.particleEmitters;
    gpuModel.particleTextures.resize(model.particleEmitters.size(), whiteTexture_.get());
    for (size_t ei = 0; ei < model.particleEmitters.size(); ei++) {
        uint16_t texIdx = model.particleEmitters[ei].texture;
        if (texIdx < allTextures.size() && allTextures[texIdx] != nullptr) {
            gpuModel.particleTextures[ei] = allTextures[texIdx];
        } else {
            LOG_WARNING("M2 '", model.name, "' particle emitter[", ei,
                        "] texture index ", texIdx, " out of range (", allTextures.size(),
                        " textures) - using white fallback");
        }
    }

    // Pre-allocate one stable descriptor set per particle emitter to avoid per-frame allocation.
    // This prevents materialDescPool_ exhaustion when many emitters are active each frame.
    if (particleTexLayout_ && materialDescPool_ && !model.particleEmitters.empty()) {
        VkDevice device = vkCtx_->getDevice();
        gpuModel.particleTexSets.resize(model.particleEmitters.size(), VK_NULL_HANDLE);
        for (size_t ei = 0; ei < model.particleEmitters.size(); ei++) {
            VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = materialDescPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &particleTexLayout_;
            if (vkAllocateDescriptorSets(device, &ai, &gpuModel.particleTexSets[ei]) == VK_SUCCESS) {
                // Valid, not merely non-null: descriptorInfo() returns the
                // texture's handles as they are, so one whose upload failed
                // writes a null view and sampler into a live descriptor and
                // declares SHADER_READ_ONLY_OPTIMAL over it.
                VkTexture* tex = gpuModel.particleTextures[ei];
                if (!tex || !tex->isValid()) tex = whiteTexture_.get();
                if (!tex || !tex->isValid()) continue;
                VkDescriptorImageInfo imgInfo = tex->descriptorInfo();
                VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = gpuModel.particleTexSets[ei];
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &imgInfo;
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            }
        }
    }

    // Copy ribbon emitter data and resolve each texture and material pair
    // (0x00832ea0): the texture a direct index into the model's textures.
    gpuModel.ribbonEmitters = model.ribbonEmitters;
    if (!model.ribbonEmitters.empty()) {
        VkDevice device = vkCtx_->getDevice();
        gpuModel.ribbonMaterialStart.assign(1, 0u);
        for (size_t ri = 0; ri < model.ribbonEmitters.size(); ri++) {
            for (const auto& rm : model.ribbonEmitters[ri].materials) {
                VkTexture* tex = rm.textureIndex < allTextures.size() ? allTextures[rm.textureIndex] : nullptr;
                if (!tex) {
                    LOG_WARNING("M2 '", model.name, "' ribbon emitter[", ri, "] texture ", rm.textureIndex,
                                " of ", allTextures.size(), " missing - using white");
                    tex = whiteTexture_.get();
                }
                VkDescriptorSet set = VK_NULL_HANDLE;
                // A set of its own (particleTexLayout_: a single sampler).
                if (particleTexLayout_ && materialDescPool_) {
                    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                    ai.descriptorPool = materialDescPool_;
                    ai.descriptorSetCount = 1;
                    ai.pSetLayouts = &particleTexLayout_;
                    VkTexture* bound = (tex && tex->isValid()) ? tex : whiteTexture_.get();
                    if (bound && bound->isValid() &&
                        vkAllocateDescriptorSets(device, &ai, &set) == VK_SUCCESS) {
                        VkDescriptorImageInfo imgInfo = bound->descriptorInfo();
                        VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                        write.dstSet = set;
                        write.dstBinding = 0;
                        write.descriptorCount = 1;
                        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                        write.pImageInfo = &imgInfo;
                        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
                    }
                }
                gpuModel.ribbonTextures.push_back(tex);
                gpuModel.ribbonTexSets.push_back(set);
                gpuModel.ribbonMaterials.push_back(client_ribbon::materialState(rm.flags, rm.blendMode));
            }
            gpuModel.ribbonMaterialStart.push_back(static_cast<uint32_t>(gpuModel.ribbonTexSets.size()));
        }
        LOG_DEBUG("  Ribbon emitters loaded: ", model.ribbonEmitters.size());
    }

    // Copy texture transform data for UV animation
    gpuModel.textureTransforms = model.textureTransforms;
    gpuModel.textureTransformLookup = model.textureTransformLookup;
    gpuModel.colorRGBTracks = model.colorRGBTracks;
    gpuModel.colorAlphaTracks = model.colorAlphaTracks;
    gpuModel.textureWeightTracks = model.textureWeightTracks;
    gpuModel.hasTextureAnimation = false;

    // Build per-batch GPU entries
    if (!model.batches.empty()) {
        for (const auto& batch : model.batches) {
            // A submesh that reaches past the model's own indices is not drawn.
            //
            // vkCmdDrawIndexed does not check this and the GPU does not survive
            // it: validation caught a batch starting at index 6,290,784 of a
            // 768-index buffer - an ending offset 25 MB past the end - repeated
            // 46 times over twelve seconds before the device was lost. The same
            // start every time, so it is a submesh read from the wrong offset
            // rather than memory going bad, and this client parses two M2
            // layouts whose skin data does not sit in the same place.
            //
            // Dropping the submesh loses a piece of one doodad. Drawing it
            // loses the device.
            const uint64_t end = static_cast<uint64_t>(batch.indexStart) + batch.indexCount;
            if (end > gpuModel.indexCount) {
                LOG_WARNING("M2 '", gpuModel.name, "': submesh indices ",
                            batch.indexStart, "..", end, " lie past the model's ",
                            gpuModel.indexCount, " - not drawn");
                continue;
            }
            M2ModelGPU::BatchGPU bgpu;
            bgpu.indexStart = batch.indexStart;
            bgpu.indexCount = batch.indexCount;

            // Store texture animation index from batch
            bgpu.textureAnimIndex = batch.textureAnimIndex;
            if (bgpu.textureAnimIndex != 0xFFFF) {
                gpuModel.hasTextureAnimation = true;
            }

            // Store blend mode and flags from material
            if (batch.materialIndex < model.materials.size()) {
                bgpu.blendMode = model.materials[batch.materialIndex].blendMode;
                bgpu.materialFlags = model.materials[batch.materialIndex].flags;
                if (bgpu.blendMode >= 2) gpuModel.hasTransparentBatches = true;
            }

            // Copy LOD level from batch
            bgpu.submeshLevel = batch.submeshLevel;

            // Resolve texture: batch.textureIndex → textureLookup → allTextures
            VkTexture* tex = whiteTexture_.get();
            bool texFailed = false;
            std::string batchTexKeyLower;
            if (batch.textureIndex < model.textureLookup.size()) {
                uint16_t texIdx = model.textureLookup[batch.textureIndex];
                if (texIdx < allTextures.size()) {
                    tex = allTextures[texIdx];
                    texFailed = (texIdx < textureLoadFailed.size()) && textureLoadFailed[texIdx];
                    if (texIdx < textureKeysLower.size()) {
                        batchTexKeyLower = textureKeysLower[texIdx];
                    }
                }
                if (texIdx < model.textures.size()) {
                    bgpu.texFlags = static_cast<uint8_t>(model.textures[texIdx].flags & 0x3);
                }
            } else if (!allTextures.empty()) {
                LOG_WARNING("M2 '", model.name, "' batch textureIndex ", batch.textureIndex,
                            " out of range (textureLookup size=", model.textureLookup.size(),
                            ") - falling back to texture[0]");
                tex = allTextures[0];
                texFailed = !textureLoadFailed.empty() && textureLoadFailed[0];
                if (!textureKeysLower.empty()) {
                    batchTexKeyLower = textureKeysLower[0];
                }
            }

            bgpu.texture = tex;
            const auto tcls = classifyBatchTexture(batchTexKeyLower);
            bgpu.starLayer = tcls.starPointLayer;
            // A flat emissive card: a lantern's glow, or the GLOW32 card a
            // torch, fire pit or brazier pairs with its flame mesh. Both are
            // drawn by their material; this only keeps them out of the ray
            // traced scene, where a card would cast a shadow. The client
            // places no light at them - its WMO light is baked into the
            // vertex colours.
            const bool modelLanternFamily = gpuModel.isLanternLike;
            const bool torchGlowCard = gpuModel.isTorch &&
                tcls.hasGlowToken && tcls.hasGlowCardToken;
            const bool fireGlowCard = gpuModel.isBrazierOrFire &&
                tcls.hasGlowToken && tcls.hasGlowCardToken;
            const bool lanternGlowHint =
                tcls.softGlowSurface ||
                tcls.exactLanternGlowTex ||
                torchGlowCard ||
                fireGlowCard ||
                ((tcls.hasGlowToken || (modelLanternFamily && tcls.hasFlameToken)) &&
                 (tcls.lanternFamily || modelLanternFamily) &&
                 (!tcls.likelyFlame || modelLanternFamily));
            bgpu.glowCardLike = lanternGlowHint &&
                (tcls.hasGlowCardToken || tcls.softGlowSurface);
            if (tex != nullptr && tex != whiteTexture_.get()) {
                auto pit = texturePropsByPtr_.find(tex);
                if (pit != texturePropsByPtr_.end()) {
                    bgpu.hasAlpha = pit->second.hasAlpha;
                    bgpu.alphaIsSilhouette = pit->second.alphaIsSilhouette;
                }
            }
            // The batch's shader (0x00836980, 0x00836c90): how many textures
            // it combines, by what, and where each one's coordinates come from.
            bool shaderless = false;
            {
                const uint16_t matBlend = batch.materialIndex < model.materials.size()
                    ? model.materials[batch.materialIndex].blendMode : 0;
                const uint16_t shaderId = m2BatchShaderId(
                    batch.shader, matBlend, batch.textureCount, batch.textureUnit,
                    model.globalFlags, model.textureCoordCombos, model.textureCombinerCombos);
                const uint16_t firstCoord = batch.textureUnit < model.textureCoordCombos.size()
                    ? model.textureCoordCombos[batch.textureUnit] : 0;
                const M2BatchCombiner comb = m2ResolveCombiner(shaderId, batch.textureCount, firstCoord);
                shaderless = !comb.drawn;
                bgpu.combinerModes = m2PackCombinerModes(comb);
                bgpu.coordSources = m2PackCoordSources(comb);
                if (comb.stages > 1) {
                    // The second texture, through the same lookup (0x0081f450).
                    bgpu.texture2 = whiteTexture_.get();
                    const uint32_t li = static_cast<uint32_t>(batch.textureIndex) + 1;
                    if (li < model.textureLookup.size()) {
                        const uint16_t texIdx2 = model.textureLookup[li];
                        if (texIdx2 < allTextures.size() && allTextures[texIdx2]) {
                            bgpu.texture2 = allTextures[texIdx2];
                        }
                    }
                }
            }

            // Start at full opacity; hide only if texture failed to load.
            bgpu.batchOpacity = texFailed ? 0.0f : 1.0f;
            bgpu.priorityPlane = batch.priorityPlane;
            // Shader id 0x8000 gets no shader, and a batch with none is not
            // drawn (0x00836c90, 0x00821e97).
            if (shaderless) bgpu.batchOpacity = 0.0f;

            // And say so, because invisible is indistinguishable from absent.
            //
            // A tree reported as missing its inner bark, with the top floating
            // over a transparent gap, is one batch of two: the trunk drawn at
            // zero opacity while the canopy draws normally. Nothing named it.
            // The texture loader logs where a file fails, but a batch turned
            // invisible three steps later on the strength of that flag said
            // nothing at all, so the search went to the texture files - which
            // were all present and correct - instead of to the batch.
            if (texFailed) {
                static core::LogBudget hiddenBatchBudget(
                    16, "M2 batches hidden because their texture did not load");
                if (hiddenBatchBudget.take()) {
                    LOG_WARNING("M2 batch drawn invisible: '", model.name, "' batch ",
                                gpuModel.batches.size(), " wanted '", batchTexKeyLower,
                                "' and did not get it");
                }
            }

            // The batch's colour and transparency, which the client takes on
            // every draw (FUN_0081fe90): a track that moves is sampled per
            // frame at draw time, one that does not is its one value here -
            // zero included, which the client draws as nothing.
            if (bgpu.batchOpacity > 0.0f) {
                if (batch.colorIndex < model.colorRGB.size()) {
                    // The batch's authored colour. A glow card is painted
                    // white and coloured here, so without it every fire in the
                    // world burns white: Orgrimmar's carries (1.0, 0.329, 0.0).
                    bgpu.tint = model.colorRGB[batch.colorIndex];
                }
                // Through the lookup, as the client resolves it: the batch's
                // index names a lookup entry, which names the track.
                uint16_t weightIdx = 0xFFFF;
                if (batch.transparencyIndex < model.textureWeightLookup.size()) {
                    weightIdx = model.textureWeightLookup[batch.transparencyIndex];
                }
                if (batch.colorIndex < model.colorAlphaTracks.size()) bgpu.colorTrackIndex = batch.colorIndex;
                if (weightIdx < model.textureWeightTracks.size()) bgpu.weightTrackIndex = weightIdx;

                // A track moves if a sequence has more than one key, two
                // sequences hold keys (each its own value), it runs on a
                // global clock - or some sequences hold a key and others none,
                // since an empty one is the default (1) rather than the key.
                // Dalaran's fountain is that: its water, basin and crystals
                // are keyed to 0 in sequence 149 alone and so visible in
                // Stand, its Tirion statue keyed to 0 in Stand and visible in
                // 149. Taken as still, each batch got the one key there was,
                // 0 for nearly all of it, and the fountain drew as nothing but
                // its particles.
                auto animates = [](const std::vector<pipeline::M2AnimationTrack>& tracks, uint16_t idx) {
                    if (idx >= tracks.size()) return false;
                    const auto& t = tracks[idx];
                    if (t.globalSequence >= 0) return true;
                    int keyed = 0;
                    int empty = 0;
                    for (const auto& seq : t.sequences) {
                        if (seq.timestamps.size() > 1) return true;
                        if (!seq.timestamps.empty()) ++keyed; else ++empty;
                    }
                    return keyed > 1 || (keyed > 0 && empty > 0);
                };
                bgpu.colorAnimated = animates(model.colorAlphaTracks, bgpu.colorTrackIndex) ||
                                     animates(model.colorRGBTracks, bgpu.colorTrackIndex) ||
                                     animates(model.textureWeightTracks, bgpu.weightTrackIndex);

                float staticAlpha = 1.0f;
                if (batch.colorIndex < model.colorAlphas.size()) staticAlpha *= model.colorAlphas[batch.colorIndex];
                if (weightIdx < model.textureWeights.size()) staticAlpha *= model.textureWeights[weightIdx];
                bgpu.staticAlpha = std::clamp(staticAlpha, 0.0f, 1.0f);
                if (!bgpu.colorAnimated && bgpu.staticAlpha < 0.004f) bgpu.batchOpacity = 0.0f;
            }

            // Why a batch of a named model came out the way it did.
            //
            // Set WOWEE_M2_BATCH_DIAG to a substring of the model's name and
            // every batch of every model matching it is printed once, as it is
            // built. A batch that never reaches the screen leaves no other
            // trace: the values that decide its fate are read here and then
            // only compared, so a mesh that vanishes looks the same from the
            // outside as one that was never in the file.
            static const std::string kBatchDiag = [] {
                const char* v = std::getenv("WOWEE_M2_BATCH_DIAG");
                std::string s = v ? v : "";
                std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                return s;
            }();
            if (!kBatchDiag.empty()) {
                std::string lowerName = model.name;
                std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lowerName.find(kBatchDiag) != std::string::npos) {
                    // At warning, because the log carries nothing below it.
                    // Setting WOWEE_M2_BATCH_DIAG is asking for these lines,
                    // and they were being written where nobody could read
                    // them - which is the same as not writing them.
                    LOG_WARNING("M2 BATCH '", model.name, "' #", gpuModel.batches.size(),
                             ": tex='", batchTexKeyLower,
                             "' blend=", static_cast<int>(bgpu.blendMode),
                             " matFlags=0x", std::hex, bgpu.materialFlags, std::dec,
                             " alphaTestWillBe=",
                             m2BatchNeedsAlphaTest(bgpu.blendMode, bgpu.hasAlpha) ? 1 : 0,
                             " hasAlpha=", bgpu.hasAlpha ? "Y" : "N",
                             " alphaIsSilhouette=", bgpu.alphaIsSilhouette ? "Y" : "N",
                             " glowCardLike=", bgpu.glowCardLike ? "Y" : "N",
                             " opacity=", bgpu.batchOpacity,
                             " idxCount=", bgpu.indexCount,
                             " texFailed=", texFailed ? "Y" : "N");
                }
            }

            gpuModel.batches.push_back(bgpu);
        }
    } else {
        // Fallback: single batch covering all indices with first texture
        M2ModelGPU::BatchGPU bgpu;
        bgpu.indexStart = 0;
        bgpu.indexCount = gpuModel.indexCount;
        bgpu.texture = allTextures.empty() ? whiteTexture_.get() : allTextures[0];
        if (bgpu.texture != nullptr && bgpu.texture != whiteTexture_.get()) {
            auto pit = texturePropsByPtr_.find(bgpu.texture);
            if (pit != texturePropsByPtr_.end()) {
                bgpu.hasAlpha = pit->second.hasAlpha;
                bgpu.alphaIsSilhouette = pit->second.alphaIsSilhouette;
            }
        }
        gpuModel.batches.push_back(bgpu);
    }

    vkCtx_->endUploadBatch();

    // Allocate Vulkan descriptor sets and UBOs for each batch
    for (auto& bgpu : gpuModel.batches) {
        // Create combined UBO for M2Params (binding 1) + M2Material (binding 2)
        // We allocate them as separate buffers for clarity
        VmaAllocationInfo matAllocInfo{};
        {
            VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = sizeof(M2MaterialUBO);
            bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            vmaCreateBuffer(vkCtx_->getAllocator(), &bci, &aci, &bgpu.materialUBO, &bgpu.materialUBOAlloc, &matAllocInfo);

            // Write initial material data (static per-batch - fadeAlpha updated at draw time)
            M2MaterialUBO mat{};
            mat.hasTexture = (bgpu.texture != nullptr && bgpu.texture != whiteTexture_.get()) ? 1 : 0;
            mat.combiners = bgpu.combinerModes;
            mat.alphaTest = m2BatchNeedsAlphaTest(bgpu.blendMode, bgpu.hasAlpha) ? 1 : 0;
            mat.tintR = bgpu.tint.r;
            mat.tintG = bgpu.tint.g;
            mat.tintB = bgpu.tint.b;
            mat.unlit = (bgpu.materialFlags & 0x01) ? 1 : 0;
            mat.unfogged = (bgpu.materialFlags & 0x02) ? 1 : 0;
            mat.blendMode = bgpu.blendMode;
            mat.fadeAlpha = bgpu.staticAlpha;
            mat.specularIntensity = 0.5f;
            mat.emissiveBoost = 1.0f;
            memcpy(matAllocInfo.pMappedData, &mat, sizeof(mat));
            bgpu.materialUBOMapped = matAllocInfo.pMappedData;

            // What the sky model's layers are actually being given, once each.
            //
            // Three fixes have been aimed at this by reading - the colour key,
            // the alpha test's screen-space rescale, the order of the sky
            // early-out - and the flicker survived all three, which means the
            // reading was wrong about which of these values the sky's blended
            // layers carry. Twenty-three of them is too many to hold in the
            // head, and only this says what they are.
            if (skyMode_) {
                LOG_INFO("skyM2 batch ", &bgpu - gpuModel.batches.data(),
                         ": blend=", static_cast<int>(bgpu.blendMode),
                         " alphaTest=", mat.alphaTest,
                         " hasAlpha=", bgpu.hasAlpha ? 1 : 0,
                         " unlit=", mat.unlit,
                         " glowCardLike=", bgpu.glowCardLike ? 1 : 0,
                         " texAnim=", bgpu.textureAnimIndex,
                         " tint=(", bgpu.tint.r, ",", bgpu.tint.g, ",", bgpu.tint.b, ")");
            }
        }

        // Allocate descriptor set and write all bindings
        bgpu.materialSet = allocateMaterialSet();
        // Valid, not merely non-null - the same shape as the particle and
        // ribbon sets below. descriptorInfo() returns the texture's handles as
        // they are, so one whose upload or view creation failed writes
        // VK_NULL_HANDLE into a live descriptor and declares
        // SHADER_READ_ONLY_OPTIMAL over it, which reaches an NVIDIA driver as a
        // graphics engine exception and a lost device. See #123.
        //
        // The set is dropped when even the white fallback is unsampleable, and
        // the render passes already skip a batch with no set: losing a batch
        // costs part of a model, and a null image view costs the device.
        VkTexture* batchTex = (bgpu.texture && bgpu.texture->isValid())
            ? bgpu.texture : whiteTexture_.get();
        if (!batchTex || !batchTex->isValid()) bgpu.materialSet = VK_NULL_HANDLE;
        if (bgpu.materialSet) {
            VkDescriptorImageInfo imgInfo = batchTex->descriptorInfo();

            VkDescriptorBufferInfo matBufInfo{};
            matBufInfo.buffer = bgpu.materialUBO;
            matBufInfo.offset = 0;
            matBufInfo.range = sizeof(M2MaterialUBO);

            // The second stage's texture, white for a batch of one (it is not
            // sampled then) or one that would not load.
            VkTexture* batchTex2 = (bgpu.texture2 && bgpu.texture2->isValid())
                ? bgpu.texture2 : batchTex;
            if (!bgpu.texture2 && whiteTexture_ && whiteTexture_->isValid()) {
                batchTex2 = whiteTexture_.get();
            }
            VkDescriptorImageInfo imgInfo2 = batchTex2->descriptorInfo();

            VkWriteDescriptorSet writes[3] = {};
            // binding 0: texture
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = bgpu.materialSet;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &imgInfo;
            // binding 2: M2Material UBO
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = bgpu.materialSet;
            writes[1].dstBinding = 2;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[1].pBufferInfo = &matBufInfo;
            // binding 3: the second stage's texture
            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = bgpu.materialSet;
            writes[2].dstBinding = 3;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[2].pImageInfo = &imgInfo2;

            vkUpdateDescriptorSets(vkCtx_->getDevice(), 3, writes, 0, nullptr);
        }
    }


    registerRtModel(gpuModel, model);
    models[modelId] = std::move(gpuModel);
    spatialIndexDirty_ = true;  // Map may have rehashed - refresh cachedModel pointers

    LOG_DEBUG("Loaded M2 model: ", model.name, " (", models[modelId].vertexCount, " vertices, ",
              models[modelId].indexCount / 3, " triangles, ", models[modelId].batches.size(), " batches)");


    return true;
}

} // namespace rendering
} // namespace wowee

namespace wowee {
namespace rendering {

void M2Renderer::registerRtModel(M2ModelGPU& gpuModel, const pipeline::M2Model& model) {
    if (!rtScene_ || model.vertices.empty() || model.indices.empty()) return;
    // What the renderer never draws, what is too small and numerous to be
    // worth its triangles (ground clutter), and what moves: an animated model
    // would cast its bind pose, not the pose on screen.
    if (gpuModel.isInvisibleTrap || gpuModel.isSpellEffect ||
        gpuModel.isGroundDetail || gpuModel.isSkyBird || gpuModel.isLightBeam) {
        return;
    }
    if (gpuModel.hasAnimation && !gpuModel.disableAnimation && !gpuModel.isTransportDoodad) return;

    RtScene::MeshSource src;
    src.positions.reserve(model.vertices.size());
    for (const auto& v : model.vertices) src.positions.push_back(v.position);
    for (const auto& batch : gpuModel.batches) {
        if (batch.blendMode >= 2 || batch.batchOpacity < 0.01f ||
            batch.starLayer || batch.glowCardLike) {
            continue;
        }
        glm::vec3 albedo = batch.tint;
        float opacity = 1.0f;
        if (batch.texture) {
            albedo *= batch.texture->averageColor();
            if (m2BatchNeedsAlphaTest(static_cast<uint8_t>(batch.blendMode), batch.hasAlpha)) {
                opacity = batch.texture->alphaCoverage();
            }
        }
        const float surface = packRtSurface(albedo, opacity);
        const size_t end = std::min<size_t>(size_t(batch.indexStart) + batch.indexCount,
                                            model.indices.size());
        for (size_t i = batch.indexStart; i + 2 < end; i += 3) {
            src.indices.push_back(model.indices[i]);
            src.indices.push_back(model.indices[i + 1]);
            src.indices.push_back(model.indices[i + 2]);
            src.surfaces.push_back(surface);
        }
    }
    gpuModel.rtMesh = rtScene_->addMesh(std::move(src));
}

void M2Renderer::releaseRtModel(M2ModelGPU& gpuModel) {
    if (!rtScene_ || gpuModel.rtMesh == RtScene::kInvalid) return;
    // Instances of it are normally gone already; any left are dropped here,
    // and the M2Instance that still names one re-registers on the next sync
    // if its model comes back.
    for (size_t i = 0; i < rtOwned_.size();) {
        const uint32_t id = rtOwned_[i];
        if (rtMeshOf_[id] == gpuModel.rtMesh) {
            rtScene_->removeInstance(id);
            rtSeen_[id] = 0;
            rtOwned_[i] = rtOwned_.back();
            rtOwned_.pop_back();
        } else {
            ++i;
        }
    }
    rtScene_->removeMesh(gpuModel.rtMesh);
    gpuModel.rtMesh = RtScene::kInvalid;
}

void M2Renderer::syncRtScene() {
    if (!rtScene_ || !rtScene_->isActive()) return;
    const uint64_t gen = ++rtSyncGeneration_;
    auto track = [&](uint32_t id, uint32_t mesh) {
        if (id >= rtSeen_.size()) {
            rtSeen_.resize(id + 1, 0);
            rtMeshOf_.resize(id + 1, RtScene::kInvalid);
        }
        rtSeen_[id] = gen;
        rtMeshOf_[id] = mesh;
    };
    for (auto& inst : instances) {
        auto it = models.find(inst.modelId);
        const uint32_t mesh = it != models.end() ? it->second.rtMesh : RtScene::kInvalid;
        const bool wanted = mesh != RtScene::kInvalid && inst.fade >= 1.0f;
        // Still ours, not claimed by another instance this pass (a copied
        // M2Instance carries its original's id), and placing the same mesh.
        const bool valid = inst.rtInstance != RtScene::kInvalid &&
                           inst.rtInstance < rtSeen_.size() && rtSeen_[inst.rtInstance] != 0 &&
                           rtSeen_[inst.rtInstance] != gen && rtMeshOf_[inst.rtInstance] == mesh;
        if (!wanted) {
            inst.rtInstance = RtScene::kInvalid;  // an unseen id is removed below
            continue;
        }
        if (!valid) {
            inst.rtInstance = rtScene_->addInstance(mesh, inst.modelMatrix);
            if (inst.rtInstance == RtScene::kInvalid) continue;
            inst.rtMatrix = inst.modelMatrix;
            rtOwned_.push_back(inst.rtInstance);
            track(inst.rtInstance, mesh);
            continue;
        }
        if (inst.rtMatrix != inst.modelMatrix) {
            rtScene_->setInstanceTransform(inst.rtInstance, inst.modelMatrix);
            inst.rtMatrix = inst.modelMatrix;
        }
        track(inst.rtInstance, mesh);
    }
    for (size_t i = 0; i < rtOwned_.size();) {
        const uint32_t id = rtOwned_[i];
        if (rtSeen_[id] != gen) {
            rtScene_->removeInstance(id);
            rtSeen_[id] = 0;
            rtOwned_[i] = rtOwned_.back();
            rtOwned_.pop_back();
        } else {
            ++i;
        }
    }
}

} // namespace rendering
} // namespace wowee
