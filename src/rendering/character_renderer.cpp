/**
 * CharacterRenderer - GPU rendering of M2 character models with skeletal animation (Vulkan)
 *
 * Handles:
 *  - Uploading M2 vertex/index data to Vulkan buffers via VMA
 *  - Per-frame bone matrix computation (hierarchical, with keyframe interpolation)
 *  - GPU vertex skinning via a bone-matrix SSBO in the vertex shader
 *  - Per-batch texture binding through the M2 texture-lookup indirection
 *  - Geoset filtering (activeGeosets) to show/hide body part groups
 *  - CPU texture compositing for character skins (base skin + underwear overlays)
 *
 * The character texture compositing uses the WoW CharComponentTextureSections
 * layout, placing region overlays (pelvis, torso, etc.) at their correct pixel
 * positions on the 512x512 body skin atlas. Region coordinates sourced from
 * the original WoW Model Viewer (charcontrol.h, REGION_FAC=2).
 */
#include <atomic>
#include "rendering/depth_convention.hpp"
#include "core/frame_profiler.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/shadow_params.hpp"
#include "rendering/normal_map.hpp"
#include "rendering/m2_track_sampler.hpp"
#include "rendering/m2_texture_combiner.hpp"
#include "rendering/m2_event_sounds.hpp"
#include "rendering/unit_model_events.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "core/thread_pool.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/bone_slots.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/env_flag.hpp"
#include "core/geoset_rules.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <climits>
#include <cmath>
#include <filesystem>
#include <future>
#include <numeric>
#include <thread>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <cstring>

namespace wowee {
namespace rendering {

namespace {
size_t approxTextureBytesWithMips(int w, int h) {
    if (w <= 0 || h <= 0) return 0;
    size_t base = static_cast<size_t>(w) * static_cast<size_t>(h) * 4ull;
    return base + (base / 3);  // ~4/3 for mip chain
}

std::string normalizeTexturePathKey(std::string key) {
    std::replace(key.begin(), key.end(), '/', '\\');
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
}

bool isMagentaKeyCandidate(const uint8_t* rgba) {
    const int r = rgba[0];
    const int g = rgba[1];
    const int b = rgba[2];
    const int rbDelta = (r > b) ? (r - b) : (b - r);
    return r >= 170 && b >= 170 && g <= 120 &&
           r >= g + 70 && b >= g + 70 && rbDelta <= 96;
}

bool shouldApplyMagentaKey(const std::string& normalizedPath) {
    return normalizedPath.find("character\\") == 0 ||
           normalizedPath.find("item\\texturecomponents\\") == 0 ||
           normalizedPath.find("item\\objectcomponents\\") == 0 ||
           normalizedPath.find("\\hair") != std::string::npos ||
           normalizedPath.find("hair") == 0 ||
           normalizedPath.find("\\cape\\") != std::string::npos ||
           normalizedPath.find("cape\\") == 0;
}

size_t bleedAndStripMagentaKey(std::vector<uint8_t>& rgba, int width, int height) {
    if (width <= 0 || height <= 0 || rgba.size() < static_cast<size_t>(width) * height * 4) {
        return 0;
    }

    const size_t pixelCount = static_cast<size_t>(width) * height;
    std::vector<uint8_t> source = rgba;
    std::vector<uint8_t> mask(pixelCount, 0);
    size_t stripped = 0;

    for (size_t p = 0; p < pixelCount; ++p) {
        size_t i = p * 4;
        if (!isMagentaKeyCandidate(&source[i])) continue;
        mask[p] = 1;
        ++stripped;
    }

    if (stripped == 0) return 0;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            size_t p = static_cast<size_t>(y) * width + x;
            if (!mask[p]) continue;

            uint32_t rSum = 0;
            uint32_t gSum = 0;
            uint32_t bSum = 0;
            uint32_t samples = 0;

            for (int radius = 1; radius <= 8 && samples == 0; ++radius) {
                for (int dy = -radius; dy <= radius; ++dy) {
                    int ny = y + dy;
                    if (ny < 0 || ny >= height) continue;
                    for (int dx = -radius; dx <= radius; ++dx) {
                        if (std::abs(dx) != radius && std::abs(dy) != radius) continue;
                        int nx = x + dx;
                        if (nx < 0 || nx >= width) continue;

                        size_t np = static_cast<size_t>(ny) * width + nx;
                        if (mask[np]) continue;
                        size_t ni = np * 4;
                        if (source[ni + 3] == 0) continue;
                        rSum += source[ni + 0];
                        gSum += source[ni + 1];
                        bSum += source[ni + 2];
                        ++samples;
                    }
                }
            }

            size_t i = p * 4;
            if (samples > 0) {
                rgba[i + 0] = static_cast<uint8_t>(rSum / samples);
                rgba[i + 1] = static_cast<uint8_t>(gSum / samples);
                rgba[i + 2] = static_cast<uint8_t>(bSum / samples);
            } else {
                rgba[i + 0] = 0;
                rgba[i + 1] = 0;
                rgba[i + 2] = 0;
            }
            rgba[i + 3] = 0;
        }
    }
    return stripped;
}

bool hasNonOpaqueAlpha(const std::vector<uint8_t>& rgba) {
    for (size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] != 255) {
            return true;
        }
    }
    return false;
}

void applyMagentaKeyIfNeeded(pipeline::BLPImage& image, const std::string& path) {
    if (!image.isValid()) return;
    std::string key = normalizeTexturePathKey(path);
    if (!shouldApplyMagentaKey(key)) return;
    bleedAndStripMagentaKey(image.data, image.width, image.height);
}
} // namespace

// Descriptor pool sizing
static constexpr uint32_t MAX_MATERIAL_SETS = 4096;
// Two a skinned instance (CharacterInstance::kBoneRing).
static constexpr uint32_t MAX_BONE_SETS = 8192;

// Texture compositing sizes (NPC skin upscale)
static constexpr int kBaseTexSize    = 256;  // NPC baked texture default
static constexpr int kUpscaleTexSize = 512;  // Target size for region compositing

// WOWEE_SCENE_DIAG=1 - dump what each glue-scene backdrop batch is handed at draw
// time. The scene renders through the character path, so when it comes out wrong
// the question is always which texture, blend mode and shader path it actually got.
static bool sceneDiagEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("WOWEE_SCENE_DIAG");
        return v && v[0] != '\0' && v[0] != '0';
    }();
    return enabled;
}

// Evaluate a batch's M2 color-alpha track at the given animation sequence/time.
// Returns 1.0 when the batch has no color slot or the track has no usable data.
// Global-sequence-timed tracks are not culled (they run on a different clock and
// typically drive pulsing glows, not visibility gating).
static float evalBatchColorAlpha(const pipeline::M2Model& model,
                                 const pipeline::M2Batch& batch,
                                 int sequenceIndex, float animationTimeMs,
                                 float globalTimeMs) {
    if (batch.colorIndex == 0xFFFF ||
        batch.colorIndex >= model.colorAlphaTracks.size()) {
        return 1.0f;
    }
    return m2_track::sampleFloat(model.colorAlphaTracks[batch.colorIndex],
                                 sequenceIndex, animationTimeMs, globalTimeMs,
                                 model.globalSequenceDurations, 1.0f);
}

// The batch's colour, its colour track's rgb (white without one), which the
// client takes every draw with the alpha (FUN_0081fe90).
static glm::vec3 evalBatchColorRGB(const pipeline::M2Model& model,
                                   const pipeline::M2Batch& batch,
                                   int sequenceIndex, float animationTimeMs,
                                   float globalTimeMs) {
    if (batch.colorIndex == 0xFFFF || batch.colorIndex >= model.colorRGBTracks.size()) {
        return glm::vec3(1.0f);
    }
    return m2_track::sampleVec3(model.colorRGBTracks[batch.colorIndex],
                                sequenceIndex, animationTimeMs, globalTimeMs,
                                model.globalSequenceDurations, glm::vec3(1.0f));
}

// Evaluate the material transparency track selected through the skin batch's
// lookup table. Enchant cards depend on this track for their authored duty
// cycle and strength; treating the first key as a constant makes every pulse
// both brighter and several times more frequent than intended.
static float evalBatchTextureWeight(const pipeline::M2Model& model,
                                    const pipeline::M2Batch& batch,
                                    int sequenceIndex, float animationTimeMs,
                                    float globalTimeMs) {
    if (batch.transparencyIndex == 0xFFFF ||
        batch.transparencyIndex >= model.textureWeightLookup.size()) {
        return 1.0f;
    }
    const uint16_t trackIndex = model.textureWeightLookup[batch.transparencyIndex];
    if (trackIndex == 0xFFFF || trackIndex >= model.textureWeightTracks.size()) {
        return 1.0f;
    }
    return m2_track::sampleFloat(model.textureWeightTracks[trackIndex],
                                 sequenceIndex, animationTimeMs, globalTimeMs,
                                 model.globalSequenceDurations, 1.0f);
}

// Whether any texture the batch combines is of the given type.
static bool batchUsesTextureType(const pipeline::M2Model& model, const pipeline::M2Batch& b,
                                 uint32_t wantedType) {
    if (b.textureIndex == 0xFFFF || model.textureLookup.empty()) return false;
    uint32_t comboCount = b.textureCount ? static_cast<uint32_t>(b.textureCount) : 1u;
    comboCount = std::min<uint32_t>(comboCount, 8u);
    for (uint32_t i = 0; i < comboCount; ++i) {
        const uint32_t lookupPos = static_cast<uint32_t>(b.textureIndex) + i;
        if (lookupPos >= model.textureLookup.size()) break;
        const uint16_t texSlot = model.textureLookup[lookupPos];
        if (texSlot < model.textures.size() && model.textures[texSlot].type == wantedType) return true;
    }
    return false;
}

// Whether a batch is drawn as alpha-cut hair cards whatever its blend mode
// says. The main pass and the shadow pass both ask, so that what casts is the
// silhouette that is drawn: hair cut out on screen but cast as solid quads put
// a dark slab on the character's own back below the hair. The ray traced
// lighting's proxy asks too (character_renderer_rt.cpp).
bool batchIsHairCard(const pipeline::M2Model& model, const pipeline::M2Batch& batch,
                     uint16_t blendMode, bool isSceneModel) {
    // Scene models have no hair, and their submesh ids are all 0, which
    // would otherwise satisfy the hair-geoset guess for every batch.
    // The geoset-id guess is for player-style models, whose hair has
    // a texture slot of its own. A creature has none, and its
    // submesh ids land in the same ranges by accident: a translucent
    // body part (Mana Wyrm) was taken for hair, alpha-cut, and drawn
    // solid.
    if (isSceneModel) return false;
    if (batchUsesTextureType(model, batch, 6)) return true;
    const uint16_t submeshGroup = static_cast<uint16_t>(batch.submeshId / 100);
    const bool hairGeoset = (submeshGroup >= 1 && submeshGroup <= 3) ||
                            (submeshGroup == 0 && batch.submeshId > 0 && batch.submeshId <= 99);
    if (!hairGeoset || (blendMode == 0 && batch.textureCount <= 1)) return false;
    return std::any_of(model.textures.begin(), model.textures.end(),
                       [](const auto& t) { return t.type == 6; });
}

// CharMaterial UBO layout (matches character.frag.glsl set=1 binding=1)
struct CharMaterialUBO {
    float opacity;
    int32_t alphaTest;
    // The whole unit's alpha - its own, a spell kit's, the camera's - which
    // the batch's alpha carries with its transparency in `opacity`. A Mod or
    // Mod2x layer fades by it toward the colour that leaves the scene as it
    // is (colourBlend). Was the black colour key's slot.
    float instanceFade;
    int32_t unlit;
    float emissiveBoost;
    float emissiveTintR, emissiveTintG, emissiveTintB;
    float specularIntensity;
    int32_t enableNormalMap;
    // 1: the character preview's main model, drawn with its texture alone.
    int32_t previewSimpleTexture;
    float normalMapStrength;
    int32_t hairMaterial;
    // How the shader hands its colour to a colour-only blend: 0 normally,
    // 1 Mod and 2 Mod2x (faded toward their neutral 1.0 and 0.5 by the
    // unit's alpha, instanceFade), 3 NoAlphaAdd (scaled by the batch's
    // opacity, not the texture's alpha).
    int32_t colourBlend;
    // The client's fog for this batch: 0 none, 1 the world's fog, 2 black,
    // 3 white, 4 grey. See the table at the batch's material setup.
    int32_t fogMode;
    // The batch's texture matrix (see m2_track::textureTransformMatrix): the
    // linear part's rows and the translation. Scalars, so std140 adds no
    // padding; identity unless the batch animates its texture.
    float uvM00 = 1.0f, uvM01 = 0.0f, uvM10 = 0.0f, uvM11 = 1.0f;
    float uvTx = 0.0f, uvTy = 0.0f;
    // The batch's shader (0x00836980, 0x00836c90), as M2Material.combiners
    // and the coordinate sources packed as m2PackCoordSources.
    int32_t combiners = 0x100;
    int32_t coordSources = 0;
    // The batch's colour (its colour track's rgb, white without one): the
    // diffuse the stages combine over, with the light (0x0081fe90).
    float tintR = 1.0f, tintG = 1.0f, tintB = 1.0f;
    // The second stage's texture matrix, as uvM00..uvTy.
    float uv2M00 = 1.0f, uv2M01 = 0.0f, uv2M10 = 0.0f, uv2M11 = 1.0f;
    float uv2Tx = 0.0f, uv2Ty = 0.0f;
};

// GPU vertex struct with tangent (expanded from M2Vertex for normal mapping)
struct CharVertexGPU {
    glm::vec3 position;      // 12 bytes, offset 0
    uint8_t boneWeights[4];  // 4 bytes,  offset 12
    uint8_t boneIndices[4];  // 4 bytes,  offset 16
    glm::vec3 normal;        // 12 bytes, offset 20
    glm::vec2 texCoords;     // 8 bytes,  offset 32
    glm::vec4 tangent;       // 16 bytes, offset 40 (xyz=dir, w=handedness)
    glm::vec2 texCoords2;    // 8 bytes,  offset 56: the second UV set
};  // 64 bytes total

CharacterRenderer::CharacterRenderer() {
}

CharacterRenderer::~CharacterRenderer() {
    shutdown();
}

/// Builds the five main-pass pipelines from an already-loaded shader pair.
///
/// initialize() and recreatePipelines() both need exactly these five, blend
/// state apart, and each described the vertex layout and the builder for
/// itself. The layout is the part worth having once: the bone weights and
/// indices are packed bytes, so a description that drifts from the struct
/// feeds the skinning garbage rather than failing to build.
void CharacterRenderer::buildMainPassPipelines(VkDevice device, VkRenderPass mainPass,
                                               VkSampleCountFlagBits samples,
                                               wowee::rendering::VkShaderModule& charVert,
                                               wowee::rendering::VkShaderModule& charFrag) {
    // --- Vertex input ---
    // CharVertexGPU: vec3 pos(12) + uint8[4] boneWeights(4) + uint8[4] boneIndices(4) +
    //               vec3 normal(12) + vec2 texCoords(8) + vec4 tangent(16) + vec2 texCoords2(8) = 64 bytes
    VkVertexInputBindingDescription charBinding{};
    charBinding.binding = 0;
    charBinding.stride = sizeof(CharVertexGPU);
    charBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> charAttrs = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, position))},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,   .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, boneWeights))},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UINT,     .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, boneIndices))},
        {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT,  .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, normal))},
        {.location = 4, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,     .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, texCoords))},
        {.location = 5, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, tangent))},
        {.location = 6, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,     .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, texCoords2))},
    };

    // --- Build pipelines ---
    auto buildCharPipeline = [&](VkPipelineColorBlendAttachmentState blendState,
                                  bool depthWrite, bool alphaToCoverage = false,
                                  VkCullModeFlags cullMode = VK_CULL_MODE_NONE,
                                  bool depthTest = true, bool gbuffer = false) -> VkPipeline {
        auto builder = PipelineBuilder()
            .setShaders(charVert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        charFrag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({charBinding}, charAttrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            // Counter-clockwise front, as the M2 renderer's: the camera flips
            // Y for Vulkan, which turns the model's outward winding around.
            .setRasterization(VK_POLYGON_MODE_FILL, cullMode, VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .setDepthTest(depthTest, depthWrite, depth::kNearer)
            .setDepthBias(0.0f, 0.0f)
            .setColorBlendAttachment(blendState)
            .setMultisample(samples);
        if (alphaToCoverage)
            builder.setAlphaToCoverage(true);
        if (gbuffer) builder.setGbufferOutputs();
        return builder
            .setLayout(pipelineLayout_)
            .setRenderPass(gbuffer ? vkCtx_->getGbufferRenderPass() : mainPass)
            .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS})
            .build(device, vkCtx_->getPipelineCache());
    };

    opaquePipeline_ = buildCharPipeline(PipelineBuilder::blendDisabled(), true);
    alphaTestPipeline_ = buildCharPipeline(PipelineBuilder::blendDisabled(), true, true);
    alphaPipeline_ = buildCharPipeline(PipelineBuilder::blendAlpha(), false);
    additivePipeline_ = buildCharPipeline(PipelineBuilder::blendAdditive(), false);
    translucentPipeline_ = buildCharPipeline(PipelineBuilder::blendAlpha(), true);
    // Colour-only blends; the destination's alpha is left as it is.
    auto colourBlend = [](VkBlendFactor src, VkBlendFactor dst) {
        VkPipelineColorBlendAttachmentState st = PipelineBuilder::blendAdditive();
        st.srcColorBlendFactor = src;
        st.dstColorBlendFactor = dst;
        st.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        st.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        return st;
    };
    noAlphaAddPipeline_ = buildCharPipeline(colourBlend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE), false);
    modPipeline_ = buildCharPipeline(colourBlend(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO), false);
    mod2xPipeline_ = buildCharPipeline(colourBlend(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_SRC_COLOR), false);

    // The per-batch state the 3.3.5a client sets from an M2 material's flags
    // (FUN_0081fe90): back faces culled unless 0x4 (two-sided), depth test
    // off with 0x8, depth write off with 0x10 - for every blend mode alike.
    // One variant per combination for each base; a mirrored instance culls
    // front faces instead, its winding being turned around.
    struct VariantSpec {
        VkPipeline base;
        VkPipelineColorBlendAttachmentState blend;
        bool alphaToCoverage;
    };
    const VariantSpec variantSpecs[] = {
        {.base = opaquePipeline_, .blend = PipelineBuilder::blendDisabled(), .alphaToCoverage = false},
        {.base = alphaTestPipeline_, .blend = PipelineBuilder::blendDisabled(), .alphaToCoverage = true},
        {.base = alphaPipeline_, .blend = PipelineBuilder::blendAlpha(), .alphaToCoverage = false},
        {.base = additivePipeline_, .blend = PipelineBuilder::blendAdditive(), .alphaToCoverage = false},
        {.base = noAlphaAddPipeline_, .blend = colourBlend(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE), .alphaToCoverage = false},
        {.base = modPipeline_, .blend = colourBlend(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ZERO), .alphaToCoverage = false},
        {.base = mod2xPipeline_, .blend = colourBlend(VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_SRC_COLOR), .alphaToCoverage = false},
    };
    constexpr VkCullModeFlags kCullModes[3] = {VK_CULL_MODE_NONE, VK_CULL_MODE_BACK_BIT,
                                               VK_CULL_MODE_FRONT_BIT};
    for (const auto& spec : variantSpecs) {
        if (!spec.base) continue;
        std::array<VkPipeline, kPipelineVariantCount> variants{};
        for (uint32_t v = 0; v < kPipelineVariantCount; ++v) {
            const uint32_t cull = v % 3;
            const bool noTest = ((v / 3) & 1) != 0;
            const bool noWrite = ((v / 6) & 1) != 0;
            variants[v] = buildCharPipeline(spec.blend, !noWrite, spec.alphaToCoverage,
                                            kCullModes[cull], !noTest);
        }
        pipelineVariants_[spec.base] = variants;
    }

    // What the opaque phase draws with - opaque, alpha test, and the alpha
    // blend a batch whose own alpha track is below one takes - once more with
    // all their variants, for the ray traced lighting's G-buffer pass
    // (gbufferVariant). Not for a preview's own pass.
    if (renderPassOverride_ == VK_NULL_HANDLE && vkCtx_->getGbufferRenderPass()) {
        for (const auto& spec : variantSpecs) {
            if (!spec.base || (spec.base != opaquePipeline_ && spec.base != alphaTestPipeline_ &&
                               spec.base != alphaPipeline_)) {
                continue;
            }
            std::array<VkPipeline, kPipelineVariantCount> variants{};
            for (uint32_t v = 0; v < kPipelineVariantCount; ++v) {
                variants[v] = buildCharPipeline(spec.blend, ((v / 6) & 1) == 0, spec.alphaToCoverage,
                                                kCullModes[v % 3], ((v / 3) & 1) == 0, true);
            }
            gbufferVariants_[spec.base] = variants;
        }
    }
}

void CharacterRenderer::destroyPipelineVariants() {
    if (!vkCtx_) return;
    rendering::destroyPipelineVariants(vkCtx_->getDevice(), pipelineVariants_);
    rendering::destroyPipelineVariants(vkCtx_->getDevice(), gbufferVariants_);
}

// A variant's index: cull (none, back, front) + 3 * no-depth-test + 6 * no-depth-write.
static uint32_t charVariantIndex(uint16_t materialFlags, bool mirrored) {
    const uint32_t cull = (materialFlags & 0x04) ? 0u : (mirrored ? 2u : 1u);
    const uint32_t noTest = (materialFlags & 0x08) ? 1u : 0u;
    const uint32_t noWrite = (materialFlags & 0x10) ? 1u : 0u;
    return cull + 3u * noTest + 6u * noWrite;
}

VkPipeline CharacterRenderer::pipelineVariant(VkPipeline base, uint16_t materialFlags,
                                              bool mirrored) const {
    auto it = pipelineVariants_.find(base);
    if (it == pipelineVariants_.end()) return base;
    return it->second[charVariantIndex(materialFlags, mirrored)];
}

VkPipeline CharacterRenderer::gbufferVariant(VkPipeline base, uint16_t materialFlags,
                                             bool mirrored) const {
    auto it = gbufferVariants_.find(base);
    if (it == gbufferVariants_.end()) return VK_NULL_HANDLE;
    return it->second[charVariantIndex(materialFlags, mirrored)];
}

bool CharacterRenderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                                    pipeline::AssetManager* am,
                                    VkRenderPass renderPassOverride,
                                    VkSampleCountFlagBits msaaSamples) {
    core::Logger::getInstance().info("Initializing character renderer (Vulkan)...");

    vkCtx_ = ctx;
    assetManager = am;
    perFrameLayout_ = perFrameLayout;
    renderPassOverride_ = renderPassOverride;
    msaaSamplesOverride_ = msaaSamples;
    const unsigned hc = std::thread::hardware_concurrency();
    const size_t availableCores = (hc > 1u) ? static_cast<size_t>(hc - 1u) : 1ull;
    // Character updates run alongside M2/WMO work; default to a smaller share.
    const size_t defaultAnimThreads = std::max<size_t>(1, availableCores / 4);
    numAnimThreads_ = static_cast<uint32_t>(std::max<size_t>(
        1, envSizeOrDefault("WOWEE_CHAR_ANIM_THREADS", defaultAnimThreads)));
    core::Logger::getInstance().info("Character anim threads: ", numAnimThreads_);

    VkDevice device = vkCtx_->getDevice();

    // --- Descriptor set layouts ---

    // Material set layout (set 1): binding 0 = sampler2D, binding 1 = CharMaterial UBO,
    // binding 2 = normal map, binding 3 = the second stage's texture
    {
        VkDescriptorSetLayoutBinding bindings[4] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[3].binding = 3;
        bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[3].descriptorCount = 1;
        bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 4;
        ci.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &ci, nullptr, &materialSetLayout_);
    }

    // Bone set layout (set 2): binding 0 = the frame's bone matrices
    // (CharacterInstance::boneBuffer)
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

    // --- Descriptor pools ---
    // Material descriptors are transient and allocated every draw; keep per-frame
    // pools so we can reset safely each frame slot without exhausting descriptors.
    for (auto& materialDescPool : materialDescPools_) {
        VkDescriptorPoolSize sizes[] = {
            {.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = MAX_MATERIAL_SETS * 3},  // diffuse, normal, second stage
            {.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = MAX_MATERIAL_SETS},
        };
        VkDescriptorPoolCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = MAX_MATERIAL_SETS;
        ci.poolSizeCount = 2;
        ci.pPoolSizes = sizes;
        ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vkCreateDescriptorPool(device, &ci, nullptr, &materialDescPool);
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

    // --- Material UBO ring buffers (one per frame slot) ---
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(ctx->getPhysicalDevice(), &props);
        materialUboAlignment_ = static_cast<uint32_t>(props.limits.minUniformBufferOffsetAlignment);
        if (materialUboAlignment_ < 1) materialUboAlignment_ = 1;
        // Round up UBO size to alignment
        uint32_t alignedUboSize = (sizeof(CharMaterialUBO) + materialUboAlignment_ - 1) & ~(materialUboAlignment_ - 1);
        uint32_t ringSize = alignedUboSize * MATERIAL_RING_CAPACITY;
        for (int i = 0; i < 2; i++) {
            VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = ringSize;
            bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo allocInfo{};
            vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                            &materialRingBuffer_[i], &materialRingAlloc_[i], &allocInfo);
            materialRingMapped_[i] = allocInfo.pMappedData;
        }
    }

    // --- Pipeline layout ---
    // set 0 = perFrame, set 1 = material, set 2 = bones
    // Push constant: CharPushConstants, the model matrix, an interior
    // floor's light and the unit's colour = 128 bytes
    {
        VkDescriptorSetLayout setLayouts[] = {perFrameLayout, materialSetLayout_, boneSetLayout_};
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = sizeof(CharPushConstants);

        VkPipelineLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 3;
        ci.pSetLayouts = setLayouts;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pushRange;
        vkCreatePipelineLayout(device, &ci, nullptr, &pipelineLayout_);
    }

    // --- Load shaders ---
    rendering::VkShaderModule charVert, charFrag;
    if (!charVert.loadFromFile(device, "assets/shaders/character.vert.spv") ||
        !charFrag.loadFromFile(device, "assets/shaders/character.frag.spv")) {
        LOG_ERROR("Character: Missing required shaders, cannot initialize");
        return false;
    }

    VkRenderPass mainPass = renderPassOverride_ ? renderPassOverride_ : vkCtx_->getImGuiRenderPass();
    VkSampleCountFlagBits samples = renderPassOverride_ ? msaaSamplesOverride_ : vkCtx_->getMsaaSamples();

    buildMainPassPipelines(device, mainPass, samples, charVert, charFrag);

    // Clean up shader modules
    charVert.destroy();
    charFrag.destroy();

    createFallbackTextures(device);

    // Diagnostics-only: cache lifetime is currently tied to renderer lifetime.
    textureCacheBudgetBytes_ = envSizeMBOrDefault("WOWEE_CHARACTER_TEX_CACHE_MB", 4096) * 1024ull * 1024ull;
    LOG_INFO("Character texture cache budget: ", textureCacheBudgetBytes_ / (1024 * 1024), " MB");

    core::Logger::getInstance().info("Character renderer initialized (Vulkan)");
    return true;
}

void CharacterRenderer::shutdown() {
    if (!vkCtx_) return;

    LOG_INFO("CharacterRenderer::shutdown instances=", instances.size(),
             " models=", models.size(), " override=", (void*)renderPassOverride_);

    // Wait for any in-flight background normal map generation threads
    {
        std::unique_lock<std::mutex> lock(normalMapResultsMutex_);
        normalMapDoneCV_.wait(lock, [this] {
            return pendingNormalMapCount_.load(std::memory_order_acquire) == 0;
        });
    }

    vkDeviceWaitIdle(vkCtx_->getDevice());
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator alloc = vkCtx_->getAllocator();

    // Clean up GPU resources for models
    for (auto& pair : models) {
        destroyModelGPU(pair.second);
    }

    // Clean up instance bone buffers
    for (auto& pair : instances) {
        destroyInstanceBones(pair.second);
    }

    // Model and bone destruction above is deferred; drain it now while the
    // descriptor pools are still alive, since no further frames will run.
    vkCtx_->flushDeferredCleanup();

    // ~VkTexture is empty by design -- it has no device or allocator to free
    // with -- so clearing the map drops the unique_ptrs without destroying the
    // image, view or allocation behind each one. Both textures in an entry are
    // owned here.
    for (auto& [path, entry] : textureCache) {
        if (entry.texture) entry.texture->destroy(device, alloc);
        if (entry.normalHeightMap) entry.normalHeightMap->destroy(device, alloc);
    }
    textureCache.clear();
    // The singletons the cache never held. Same reason as above.
    if (whiteTexture_)       { whiteTexture_->destroy(device, alloc);       whiteTexture_.reset(); }
    if (transparentTexture_) { transparentTexture_->destroy(device, alloc); transparentTexture_.reset(); }
    if (flatNormalTexture_)  { flatNormalTexture_->destroy(device, alloc);  flatNormalTexture_.reset(); }
    texturePropsByPtr_.clear();
    normalMapByTexPtr_.clear();
    textureCacheBytes_ = 0;
    textureCacheCounter_ = 0;

    // Clean up composite cache
    compositeCache_.clear();
    failedTextureCache_.clear();
    failedTextureRetryAt_.clear();
    textureLookupSerial_ = 0;

    whiteTexture_.reset();
    transparentTexture_.reset();
    flatNormalTexture_.reset();

    models.clear();
    sharedItemModels_.clear();
    sharedItemModelIds_.clear();
    instances.clear();

    // Destroy pipelines
    auto destroyPipeline = [&](VkPipeline& p) {
        destroy(device, p);
    };
    destroyPipeline(opaquePipeline_);
    destroyPipeline(alphaTestPipeline_);
    destroyPipeline(alphaPipeline_);
    destroyPipeline(additivePipeline_);
    destroyPipeline(noAlphaAddPipeline_);
    destroyPipeline(modPipeline_);
    destroyPipeline(mod2xPipeline_);
    destroyPipelineVariants();
    destroyPipeline(translucentPipeline_);

    destroy(device, pipelineLayout_);

    // Destroy material ring buffers
    for (int i = 0; i < 2; i++) {
        if (materialRingBuffer_[i]) {
            vmaDestroyBuffer(alloc, materialRingBuffer_[i], materialRingAlloc_[i]);
            materialRingBuffer_[i] = VK_NULL_HANDLE;
            materialRingAlloc_[i] = VK_NULL_HANDLE;
            materialRingMapped_[i] = nullptr;
        }
        materialRingOffset_[i] = 0;
    }

    // Destroy descriptor pools and layouts
    for (auto& materialDescPool : materialDescPools_) {
        destroy(device, materialDescPool);
    }
    if (boneDescPool_) {
        if (boneDescPoolGeneration_) boneDescPoolGeneration_->fetch_add(1, std::memory_order_relaxed);
        vkDestroyDescriptorPool(device, boneDescPool_, nullptr);
        boneDescPool_ = VK_NULL_HANDLE;
    }
    destroy(device, materialSetLayout_);
    destroy(device, boneSetLayout_);

    // Shadow resources
    destroy(device, shadowPipeline_);
    destroy(device, shadowPipelineLayout_);
    destroyShadowParamsSet(device, alloc, shadowParams_);
    shadowTexSetCache_.clear();
    for (auto& pool : shadowTexPool_) {
        if (pool) { vkDestroyDescriptorPool(device, pool, nullptr); pool = VK_NULL_HANDLE; }
    }

    vkCtx_ = nullptr;
}

void CharacterRenderer::clear() {
    if (!vkCtx_) return;

    LOG_INFO("CharacterRenderer::clear instances=", instances.size(),
             " models=", models.size());

    // Wait for any in-flight background normal map generation threads
    {
        std::unique_lock<std::mutex> lock(normalMapResultsMutex_);
        normalMapDoneCV_.wait(lock, [this] {
            return pendingNormalMapCount_.load(std::memory_order_acquire) == 0;
        });
    }
    // Discard any completed results that haven't been uploaded
    {
        std::lock_guard<std::mutex> lock(normalMapResultsMutex_);
        completedNormalMaps_.clear();
    }

    vkDeviceWaitIdle(vkCtx_->getDevice());
    VkDevice device = vkCtx_->getDevice();

    // Destroy GPU resources for all models
    for (auto& pair : models) {
        destroyModelGPU(pair.second);
    }

    // Destroy bone buffers for all instances
    for (auto& pair : instances) {
        destroyInstanceBones(pair.second);
    }

    // See CharacterRenderer::shutdown: ~VkTexture frees nothing on its own.
    if (vkCtx_) {
        VkDevice dev = vkCtx_->getDevice();
        VmaAllocator vma = vkCtx_->getAllocator();
        for (auto& [path, entry] : textureCache) {
            if (entry.texture) entry.texture->destroy(dev, vma);
            if (entry.normalHeightMap) entry.normalHeightMap->destroy(dev, vma);
        }
    }
    textureCache.clear();
    texturePropsByPtr_.clear();
    normalMapByTexPtr_.clear();
    textureCacheBytes_ = 0;
    textureCacheCounter_ = 0;
    loggedTextureLoadFails_.clear();
    failedTextureRetryAt_.clear();
    textureLookupSerial_ = 0;

    // Clear composite and failed caches
    compositeCache_.clear();
    failedTextureCache_.clear();

    // Recreate default textures (needed by loadModel/loadTexture fallbacks)
    whiteTexture_.reset();
    transparentTexture_.reset();
    flatNormalTexture_.reset();
    createFallbackTextures(device);

    models.clear();
    sharedItemModels_.clear();
    sharedItemModelIds_.clear();
    instances.clear();

    // Reset material ring buffer offsets (buffers persist, just reset write position)
    for (uint32_t& offset : materialRingOffset_) {
        offset = 0;
    }

    // Reset descriptor pools (don't destroy - reuse for new allocations),
    // once what is still queued to free sets from them has run (see
    // WMORenderer::clearAll).
    vkCtx_->flushDeferredCleanup();
    for (auto& materialDescPool : materialDescPools_) {
        if (materialDescPool) {
            vkResetDescriptorPool(device, materialDescPool, 0);
        }
    }
    if (boneDescPool_) {
        if (boneDescPoolGeneration_) boneDescPoolGeneration_->fetch_add(1, std::memory_order_relaxed);
        vkResetDescriptorPool(device, boneDescPool_, 0);
    }
}

void CharacterRenderer::createFallbackTextures(VkDevice device) {
    // White: default diffuse when no texture is assigned
    {
        uint8_t white[] = {255, 255, 255, 255};
        whiteTexture_ = std::make_unique<VkTexture>();
        whiteTexture_->upload(*vkCtx_, white, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
        whiteTexture_->createSampler(device, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    }
    // Transparent: placeholder for optional overlay layers (e.g. hair highlights)
    {
        uint8_t transparent[] = {0, 0, 0, 0};
        transparentTexture_ = std::make_unique<VkTexture>();
        transparentTexture_->upload(*vkCtx_, transparent, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
        transparentTexture_->createSampler(device, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    }
    // Flat normal: neutral normal map (128,128,255) + 0.5 height in alpha channel
    {
        uint8_t flatNormal[] = {128, 128, 255, 128};
        flatNormalTexture_ = std::make_unique<VkTexture>();
        flatNormalTexture_->upload(*vkCtx_, flatNormal, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
        flatNormalTexture_->createSampler(device, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    }
}

void CharacterRenderer::destroyModelGPU(M2ModelGPU& gpuModel, bool defer) {
    if (!vkCtx_) return;
    VmaAllocator alloc = vkCtx_->getAllocator();

    // Snapshot raw handles and null the model fields immediately
    ::VkBuffer vb = gpuModel.vertexBuffer;
    VmaAllocation vbAlloc = gpuModel.vertexAlloc;
    ::VkBuffer ib = gpuModel.indexBuffer;
    VmaAllocation ibAlloc = gpuModel.indexAlloc;
    gpuModel.vertexBuffer = VK_NULL_HANDLE;
    gpuModel.vertexAlloc = VK_NULL_HANDLE;
    gpuModel.indexBuffer = VK_NULL_HANDLE;
    gpuModel.indexAlloc = VK_NULL_HANDLE;

    if (!defer) {
        // Safe after vkDeviceWaitIdle (shutdown / clear paths)
        if (vb) vmaDestroyBuffer(alloc, vb, vbAlloc);
        if (ib) vmaDestroyBuffer(alloc, ib, ibAlloc);
    } else if (vb || ib) {
        // Streaming path: in-flight command buffers may still reference these
        vkCtx_->deferAfterAllFrameFences([alloc, vb, vbAlloc, ib, ibAlloc]() {
            if (vb) vmaDestroyBuffer(alloc, vb, vbAlloc);
            if (ib) vmaDestroyBuffer(alloc, ib, ibAlloc);
        });
    }
}

void CharacterRenderer::destroyInstanceBones(CharacterInstance& inst, bool defer) {
    if (!vkCtx_) return;
    releaseInstanceBones(*vkCtx_, boneDescPool_, boneDescPoolGeneration_, inst, defer);
}

std::unique_ptr<VkTexture> CharacterRenderer::generateNormalMap(
        const uint8_t* pixels, uint32_t width, uint32_t height) {
    if (!vkCtx_ || width == 0 || height == 0) return nullptr;

    // Use the CPU-only static method, then upload to GPU
    std::vector<uint8_t> dummy(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    std::memcpy(dummy.data(), pixels, dummy.size());
    auto result = generateNormalMapCPU("", std::move(dummy), width, height);

    auto tex = std::make_unique<VkTexture>();
    if (!tex->upload(*vkCtx_, result.pixels.data(), width, height, VK_FORMAT_R8G8B8A8_UNORM, true)) {
        return nullptr;
    }
    tex->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                        VK_SAMPLER_ADDRESS_MODE_REPEAT);
    return tex;
}

CharacterRenderer::NormalMapResult CharacterRenderer::generateNormalMapCPU(
        std::string cacheKey, std::vector<uint8_t> srcPixels, uint32_t width, uint32_t height) {
    NormalMapResult result;
    result.cacheKey = std::move(cacheKey);
    result.width = width;
    result.height = height;
    // Five, where the WMO renderer asks for two: this is skin and cloth seen
    // close up, and it wants the gradient exaggerated.
    result.pixels = rendering::generateNormalMap(
        srcPixels.data(), width, height, /*strength=*/5.0f);
    return result;
}

VkTexture* CharacterRenderer::loadTexture(const std::string& path, uint32_t texFlags) {
    constexpr uint64_t kFailedTextureRetryLookups = 512;
    // Skip empty or whitespace-only paths (type-0 textures have no filename)
    if (path.empty()) return whiteTexture_.get();
    bool allWhitespace = true;
    for (char c : path) {
        if (c != ' ' && c != '\t' && c != '\0' && c != '\n') { allWhitespace = false; break; }
    }
    if (allWhitespace) return whiteTexture_.get();

    std::string key = normalizeTexturePathKey(path);
    const uint64_t lookupSerial = ++textureLookupSerial_;
    // The wrap mode belongs to the sampler, so a file loaded with other wrap
    // flags is a texture of its own. Repeat-both keeps the plain path.
    texFlags &= 0x3;
    const std::string cacheKey = texFlags == 0x3 ? key : key + "#wrap" + std::to_string(texFlags);

    // Check cache
    auto it = textureCache.find(cacheKey);
    if (it != textureCache.end()) {
        it->second.lastUse = ++textureCacheCounter_;
        return it->second.texture.get();
    }
    auto failIt = failedTextureRetryAt_.find(key);
    if (failIt != failedTextureRetryAt_.end() && lookupSerial < failIt->second) {
        return whiteTexture_.get();
    }

    if (!assetManager || !assetManager->isInitialized()) {
        return whiteTexture_.get();
    }

    // Check pre-decoded BLP cache first (populated by background threads)
    pipeline::BLPImage blpImage;
    if (predecodedBLPCache_) {
        auto pit = predecodedBLPCache_->find(key);
        if (pit != predecodedBLPCache_->end()) {
            blpImage = std::move(pit->second);
            predecodedBLPCache_->erase(pit);
        }
    }
    if (!blpImage.isValid()) {
        blpImage = assetManager->loadTexture(key);
    }
    if (!blpImage.isValid()) {
        // Cache misses briefly to avoid repeated expensive MPQ/disk probes.
        failedTextureCache_.insert(key);
        failedTextureRetryAt_[key] = lookupSerial + kFailedTextureRetryLookups;
        if (loggedTextureLoadFails_.insert(key).second) {
            core::Logger::getInstance().warning("Failed to load texture: ", path);
        }
        return whiteTexture_.get();
    }

    applyMagentaKeyIfNeeded(blpImage, key);

    size_t approxBytes = approxTextureBytesWithMips(blpImage.width, blpImage.height);
    if (textureCacheBytes_ + approxBytes > textureCacheBudgetBytes_) {
        static constexpr size_t kMaxFailedTextureCache = 200000;
        if (failedTextureCache_.size() < kMaxFailedTextureCache) {
            // Budget is saturated; avoid repeatedly decoding/uploading this texture.
            failedTextureCache_.insert(key);
            failedTextureRetryAt_[key] = lookupSerial + kFailedTextureRetryLookups;
        }
        if (textureBudgetRejectWarnings_ < 3) {
            core::Logger::getInstance().warning(
                "Character texture cache full (",
                textureCacheBytes_ / (1024 * 1024), " MB / ",
                textureCacheBudgetBytes_ / (1024 * 1024), " MB), rejecting texture: ",
                path);
        }
        ++textureBudgetRejectWarnings_;
        return whiteTexture_.get();
    }

    bool hasAlpha = false;
    for (size_t i = 3; i < blpImage.data.size(); i += 4) {
        if (blpImage.data[i] != 255) {
            hasAlpha = true;
            break;
        }
    }

    auto tex = std::make_unique<VkTexture>();
    tex->upload(*vkCtx_, blpImage.data.data(), blpImage.width, blpImage.height,
                VK_FORMAT_R8G8B8A8_UNORM, true);
    // M2Texture flags: bit 0 repeats U, bit 1 repeats V; otherwise clamped,
    // as the client samples them. Characters always repeated.
    tex->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR,
                       (texFlags & 0x1) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                       (texFlags & 0x2) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);

    VkTexture* texPtr = tex.get();

    TextureCacheEntry e;
    e.texture = std::move(tex);
    e.approxBytes = approxBytes;
    e.lastUse = ++textureCacheCounter_;
    e.hasAlpha = hasAlpha;
    e.wrapFlags = static_cast<uint8_t>(texFlags);

    // No normal map is derived for a character texture any more: the surface
    // bumps are for buildings only (skin and cloth carry painted folds and
    // shading, which a map made from the colour turns into bumps that are not
    // there), so each would have been a thread and an upload for nothing - a
    // few hundred of them as a crowd loads. The flat map stands in.
    e.normalMapPending = false;

    textureCacheBytes_ += e.approxBytes;
    texturePropsByPtr_[texPtr] = {.hasAlpha = hasAlpha};
    textureCache[cacheKey] = std::move(e);
    failedTextureCache_.erase(key);
    failedTextureRetryAt_.erase(key);

    core::Logger::getInstance().debug("Loaded character texture: ", path, " (", blpImage.width, "x", blpImage.height, ")");
    return texPtr;
}

void CharacterRenderer::processPendingNormalMaps(int budget) {
    if (!vkCtx_) return;

    // Collect completed results from background threads
    std::deque<NormalMapResult> ready;
    {
        std::lock_guard<std::mutex> lock(normalMapResultsMutex_);
        if (completedNormalMaps_.empty()) return;
        int count = std::min(budget, static_cast<int>(completedNormalMaps_.size()));
        for (int i = 0; i < count; i++) {
            ready.push_back(std::move(completedNormalMaps_.front()));
            completedNormalMaps_.pop_front();
        }
    }

    // GPU upload only (~1-2ms each) - CPU work already done on background thread
    for (auto& result : ready) {
        auto it = textureCache.find(result.cacheKey);
        if (it == textureCache.end()) continue;  // texture was evicted

        vkCtx_->beginUploadBatch();
        auto tex = std::make_unique<VkTexture>();
        bool ok = tex->upload(*vkCtx_, result.pixels.data(), result.width, result.height,
                              VK_FORMAT_R8G8B8A8_UNORM, true);
        if (ok) {
            // Wrapped as the texture it belongs to is.
            const uint8_t wrap = it->second.wrapFlags;
            tex->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR,
                               (wrap & 0x1) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                               (wrap & 0x2) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
            it->second.approxBytes += approxTextureBytesWithMips(result.width, result.height);
            textureCacheBytes_ += approxTextureBytesWithMips(result.width, result.height);
            it->second.normalHeightMap = std::move(tex);
            if (it->second.texture) {
                normalMapByTexPtr_[it->second.texture.get()] = {.normalMap = it->second.normalHeightMap.get()};
            }
        }
        vkCtx_->endUploadBatch();
        it->second.normalMapPending = false;
    }
}

// Alpha-blend overlay onto composite at (dstX, dstY)
static void blitOverlay(std::vector<uint8_t>& composite, int compW, int compH,
                         const pipeline::BLPImage& overlay, int dstX, int dstY) {
    for (int sy = 0; sy < overlay.height; sy++) {
        int dy = dstY + sy;
        if (dy < 0 || dy >= compH) continue;
        for (int sx = 0; sx < overlay.width; sx++) {
            int dx = dstX + sx;
            if (dx < 0 || dx >= compW) continue;

            size_t srcIdx = (static_cast<size_t>(sy) * overlay.width + sx) * 4;
            size_t dstIdx = (static_cast<size_t>(dy) * compW + dx) * 4;

            uint8_t srcA = overlay.data[srcIdx + 3];
            if (srcA == 0) continue;

            if (srcA == 255) {
                composite[dstIdx + 0] = overlay.data[srcIdx + 0];
                composite[dstIdx + 1] = overlay.data[srcIdx + 1];
                composite[dstIdx + 2] = overlay.data[srcIdx + 2];
                composite[dstIdx + 3] = 255;
            } else {
                float alpha = srcA / 255.0f;
                float invAlpha = 1.0f - alpha;
                composite[dstIdx + 0] = static_cast<uint8_t>(overlay.data[srcIdx + 0] * alpha + composite[dstIdx + 0] * invAlpha);
                composite[dstIdx + 1] = static_cast<uint8_t>(overlay.data[srcIdx + 1] * alpha + composite[dstIdx + 1] * invAlpha);
                composite[dstIdx + 2] = static_cast<uint8_t>(overlay.data[srcIdx + 2] * alpha + composite[dstIdx + 2] * invAlpha);
                composite[dstIdx + 3] = std::max(composite[dstIdx + 3], srcA);
            }
        }
    }
}

// A composite channel value rounded to its byte. Every value here is at or
// above zero, where adding a half and truncating rounds as std::lround does
// (bar a float a hair under some half, which may go up) - but lround is a
// library call on MinGW, not an instruction, and the interpolated composite
// makes four of them for each of a 512 atlas's quarter million pixels, layer
// after layer. With them, a creature's skin took about three times as long
// to build as the nearest copy it replaced, on the main thread, for every NPC
// a crowded city spawns.
static inline uint8_t compositeByte(float v) {
    return static_cast<uint8_t>(static_cast<int>(v + 0.5f));
}

// One overlay texel blended over a composite pixel, by the overlay's alpha.
static void blendCompositePixel(std::vector<uint8_t>& composite, size_t dstIdx, const float rgba[4]) {
    const float a = rgba[3];
    if (a <= 0.0f) return;
    if (a >= 255.0f) {
        for (int c = 0; c < 3; ++c) composite[dstIdx + c] = compositeByte(rgba[c]);
        composite[dstIdx + 3] = 255;
        return;
    }
    const float alpha = a / 255.0f;
    const float inv = 1.0f - alpha;
    for (int c = 0; c < 3; ++c) {
        composite[dstIdx + c] = compositeByte(rgba[c] * alpha + composite[dstIdx + c] * inv);
    }
    composite[dstIdx + 3] = std::max(composite[dstIdx + 3], compositeByte(a));
}

// The overlay at a texel-space point, bilinear, clamped to its edges. The
// colour is interpolated premultiplied, so a texel's transparent neighbours
// do not darken the edge it fades out on.
static void sampleImageBilinear(const uint8_t* data, int w, int h, float fx, float fy, float out[4]) {
    fx = std::clamp(fx, 0.0f, static_cast<float>(w - 1));
    fy = std::clamp(fy, 0.0f, static_cast<float>(h - 1));
    const int x0 = static_cast<int>(fx);
    const int y0 = static_cast<int>(fy);
    const int x1 = std::min(x0 + 1, w - 1);
    const int y1 = std::min(y0 + 1, h - 1);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const float wts[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
    const size_t idx[4] = {(static_cast<size_t>(y0) * w + x0) * 4, (static_cast<size_t>(y0) * w + x1) * 4,
                           (static_cast<size_t>(y1) * w + x0) * 4, (static_cast<size_t>(y1) * w + x1) * 4};
    float pre[3] = {0.0f, 0.0f, 0.0f};
    float alpha = 0.0f;
    for (int k = 0; k < 4; ++k) {
        const float a = data[idx[k] + 3] * wts[k];
        alpha += a;
        for (int c = 0; c < 3; ++c) pre[c] += data[idx[k] + c] * a;
    }
    for (int c = 0; c < 3; ++c) out[c] = alpha > 0.0f ? pre[c] / alpha : 0.0f;
    out[3] = alpha;
}

// Blit an overlay resampled to an explicit destination size.
//
// Grown, it is interpolated: the nearest-neighbour copy this was made every
// texel of a 256 atlas patch a hard 2x2 (or bigger) block in the 512
// composite, and no filter on the GPU can take those steps back out - a
// close-up of any armour showed them. Shrunk, it keeps the plain pick.
static void blitOverlayResampled(std::vector<uint8_t>& composite, int compW, int compH,
                                 const pipeline::BLPImage& overlay,
                                 int dstX, int dstY, int dstW, int dstH) {
    if (dstW <= 0 || dstH <= 0 || overlay.width <= 0 || overlay.height <= 0) return;
    const bool grows = dstW > overlay.width || dstH > overlay.height;
    const float sxScale = static_cast<float>(overlay.width) / static_cast<float>(dstW);
    const float syScale = static_cast<float>(overlay.height) / static_cast<float>(dstH);
    for (int y = 0; y < dstH; ++y) {
        const int dy = dstY + y;
        if (dy < 0 || dy >= compH) continue;
        for (int x = 0; x < dstW; ++x) {
            const int dx = dstX + x;
            if (dx < 0 || dx >= compW) continue;
            float rgba[4];
            if (grows) {
                sampleImageBilinear(overlay.data.data(), overlay.width, overlay.height,
                                    (static_cast<float>(x) + 0.5f) * sxScale - 0.5f,
                                    (static_cast<float>(y) + 0.5f) * syScale - 0.5f, rgba);
            } else {
                const int sx = std::min(overlay.width - 1, x * overlay.width / dstW);
                const int sy = std::min(overlay.height - 1, y * overlay.height / dstH);
                const size_t srcIdx = (static_cast<size_t>(sy) * overlay.width + sx) * 4;
                for (int c = 0; c < 4; ++c) rgba[c] = overlay.data[srcIdx + c];
            }
            blendCompositePixel(composite, (static_cast<size_t>(dy) * compW + dx) * 4, rgba);
        }
    }
}

// An overlay grown by a whole factor, interpolated (see blitOverlayResampled).
static void blitOverlayScaledN(std::vector<uint8_t>& composite, int compW, int compH,
                                const pipeline::BLPImage& overlay, int dstX, int dstY, int scale) {
    if (scale < 1) scale = 1;
    blitOverlayResampled(composite, compW, compH, overlay, dstX, dstY,
                         overlay.width * scale, overlay.height * scale);
}

// Legacy 2x wrapper
static void blitOverlayScaled2x(std::vector<uint8_t>& composite, int compW, int compH,
                                 const pipeline::BLPImage& overlay, int dstX, int dstY) {
    blitOverlayScaledN(composite, compW, compH, overlay, dstX, dstY, 2);
}

// Nearest-neighbor downscale blit: sample every Nth pixel from overlay
static void blitOverlayDownscaleN(std::vector<uint8_t>& composite, int compW, int compH,
                                   const pipeline::BLPImage& overlay, int dstX, int dstY, int scale) {
    if (scale < 2) { blitOverlay(composite, compW, compH, overlay, dstX, dstY); return; }
    int outW = overlay.width / scale;
    int outH = overlay.height / scale;
    for (int oy = 0; oy < outH; oy++) {
        int dy = dstY + oy;
        if (dy < 0 || dy >= compH) continue;
        for (int ox = 0; ox < outW; ox++) {
            int dx = dstX + ox;
            if (dx < 0 || dx >= compW) continue;

            int sx = ox * scale;
            int sy = oy * scale;
            size_t srcIdx = (static_cast<size_t>(sy) * overlay.width + sx) * 4;
            size_t dstIdx = (static_cast<size_t>(dy) * compW + dx) * 4;

            uint8_t srcA = overlay.data[srcIdx + 3];
            if (srcA == 0) continue;

            if (srcA == 255) {
                composite[dstIdx + 0] = overlay.data[srcIdx + 0];
                composite[dstIdx + 1] = overlay.data[srcIdx + 1];
                composite[dstIdx + 2] = overlay.data[srcIdx + 2];
                composite[dstIdx + 3] = 255;
            } else {
                float alpha = srcA / 255.0f;
                float invAlpha = 1.0f - alpha;
                composite[dstIdx + 0] = static_cast<uint8_t>(overlay.data[srcIdx + 0] * alpha + composite[dstIdx + 0] * invAlpha);
                composite[dstIdx + 1] = static_cast<uint8_t>(overlay.data[srcIdx + 1] * alpha + composite[dstIdx + 1] * invAlpha);
                composite[dstIdx + 2] = static_cast<uint8_t>(overlay.data[srcIdx + 2] * alpha + composite[dstIdx + 2] * invAlpha);
                composite[dstIdx + 3] = std::max(composite[dstIdx + 3], srcA);
            }
        }
    }
}

namespace {

/// Where a character-texture overlay belongs on the body atlas, in the
/// coordinates of the 256x256 reference atlas everything is expressed in.
///
/// One table, consulted twice: once to work out how big the canvas has to be
/// for the art being laid on it, and once to place each layer. It was written
/// out inline at the placement site alone, and then the size question could
/// only be answered by guessing.
struct AtlasRegion256 { int x, y, w, h; bool known; };

AtlasRegion256 regionFor(const std::string& pathLower) {
    if (pathLower.find("faceupper") != std::string::npos) return {  .x = 0, .y = 160, .w = 128, .h = 32, .known = true};
    if (pathLower.find("facelower") != std::string::npos) return {  .x = 0, .y = 192, .w = 128, .h = 64, .known = true};
    if (pathLower.find("pelvis")    != std::string::npos) return {.x = 128,  .y = 96, .w = 128, .h = 64, .known = true};
    if (pathLower.find("torso")     != std::string::npos) return {.x = 128,   .y = 0, .w = 128, .h = 64, .known = true};
    if (pathLower.find("armupper")  != std::string::npos) return {  .x = 0,   .y = 0, .w = 128, .h = 64, .known = true};
    if (pathLower.find("armlower")  != std::string::npos) return {  .x = 0,  .y = 64, .w = 128, .h = 64, .known = true};
    if (pathLower.find("hand")      != std::string::npos) return {  .x = 0, .y = 128, .w = 128, .h = 32, .known = true};
    if (pathLower.find("foot")      != std::string::npos ||
        pathLower.find("feet")      != std::string::npos) return {.x = 128, .y = 224, .w = 128, .h = 32, .known = true};
    if (pathLower.find("legupper")  != std::string::npos ||
        pathLower.find("leg")       != std::string::npos) return {.x = 128, .y = 160, .w = 128, .h = 64, .known = true};
    return {.x = 0, .y = 0, .w = 0, .h = 0, .known = false};
}

std::string lowerPath(const std::string& s) {
    std::string out = s;
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// The atlas scale a layer of this size implies for its region. A face authored
/// at 512x256 belongs in a 128x64 region, so it is asking for a 1024 atlas.
int impliedScale(const AtlasRegion256& region, int overlayWidth) {
    if (!region.known || region.w <= 0 || overlayWidth <= 0) return 1;
    int scale = overlayWidth / region.w;
    return scale < 1 ? 1 : scale;
}

}  // namespace

void CharacterRenderer::setFaceRegionLayer(const std::string& path, bool lower) {
    if (!path.empty()) faceRegionLayers_[lowerPath(path)] = lower;
}

VkTexture* CharacterRenderer::compositeTextures(const std::vector<std::string>& layerPaths) {
    if (layerPaths.empty() || !assetManager || !assetManager->isInitialized()) {
        return whiteTexture_.get();
    }
    // A layer's region: the head's where a caller has said so, else by name.
    auto layerRegion = [this](const std::string& path) {
        const std::string lower = lowerPath(path);
        if (auto it = faceRegionLayers_.find(lower); it != faceRegionLayers_.end())
            return regionFor(it->second ? "facelower" : "faceupper");
        return regionFor(lower);
    };

    // Composite key is deterministic from layer set; if we've already built it,
    // reuse the existing GPU texture to keep live instance pointers valid.
    std::string cacheKey = "__composite__";
    for (const auto& lp : layerPaths) { cacheKey += '|'; cacheKey += lp; }
    auto cachedComposite = textureCache.find(cacheKey);
    if (cachedComposite != textureCache.end()) {
        cachedComposite->second.lastUse = ++textureCacheCounter_;
        return cachedComposite->second.texture.get();
    }

    // Load base layer
    pipeline::BLPImage base;
    if (predecodedBLPCache_) {
        std::string key = layerPaths[0];
        std::replace(key.begin(), key.end(), '/', '\\');
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto pit = predecodedBLPCache_->find(key);
        if (pit != predecodedBLPCache_->end()) {
            base = std::move(pit->second);
            predecodedBLPCache_->erase(pit);
        }
    }
    if (!base.isValid()) base = assetManager->loadTexture(layerPaths[0]);
    if (!base.isValid()) {
        core::Logger::getInstance().warning("Composite: failed to load base layer: ", layerPaths[0]);
        return whiteTexture_.get();
    }
    applyMagentaKeyIfNeeded(base, layerPaths[0]);

    // Copy base pixel data as our working buffer
    std::vector<uint8_t> composite = base.data;
    int width = base.width;
    int height = base.height;

    core::Logger::getInstance().info("Composite: base layer ", width, "x", height, " from ", layerPaths[0]);

    // WoW character texture atlas regions (from WoW Model Viewer / CharComponentTextureSections)
    // Coordinates at 256x256 base resolution:
    // Region          X    Y    W    H
    // Base            0    0    256  256
    // Arm Upper       0    0    128  64
    // Arm Lower       0    64   128  64
    // Hand            0    128  128  32
    // Face Upper      0    160  128  32
    // Face Lower      0    192  128  64
    // Torso Upper     128  0    128  64
    // Torso Lower     128  64   128  32
    // Pelvis Upper    128  96   128  64
    // Pelvis Lower    128  160  128  64
    // Foot            128  224  128  32

    // Scale factor: base texture may be larger than the 256x256 reference atlas
    int coordScale = width / 256;
    if (coordScale < 1) coordScale = 1;

    // Load every overlay before deciding how big the canvas is.
    //
    // The body decided the atlas size on its own, and each overlay was then
    // squeezed into whatever region that gave it. An HD art set ships a body at
    // the size the stock one uses and a face at twice it - so a 512x256 face was
    // resampled down to 256x128 to fit a 512 body, which throws away exactly the
    // detail the art exists for and lands it soft next to a crisp body.
    //
    // The canvas is sized to the most demanding layer instead. Nothing changes
    // for art that agrees with its body, which is every set that shipped with
    // the game; a set that asks for more gets a bigger atlas and is placed at
    // its own resolution.
    struct LoadedOverlay { std::string path; pipeline::BLPImage image; };
    std::vector<LoadedOverlay> overlays;
    overlays.reserve(layerPaths.size());
    int requiredScale = coordScale;
    int largestRegionScale = 1;
    bool sawRegionLayer = false;
    for (size_t layer = 1; layer < layerPaths.size(); layer++) {
        if (layerPaths[layer].empty()) continue;
        pipeline::BLPImage overlay;
        if (predecodedBLPCache_) {
            std::string key = layerPaths[layer];
            std::replace(key.begin(), key.end(), '/', '\\');
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            auto pit = predecodedBLPCache_->find(key);
            if (pit != predecodedBLPCache_->end()) {
                overlay = std::move(pit->second);
                predecodedBLPCache_->erase(pit);
            }
        }
        if (!overlay.isValid()) overlay = assetManager->loadTexture(layerPaths[layer]);
        if (!overlay.isValid()) {
            core::Logger::getInstance().warning("Composite: FAILED to load overlay: ",
                                                layerPaths[layer]);
            continue;
        }
        applyMagentaKeyIfNeeded(overlay, layerPaths[layer]);
        // A full-atlas layer speaks for itself and is not a region at all.
        if (overlay.width != width || overlay.height != height) {
            const AtlasRegion256 region = layerRegion(layerPaths[layer]);
            if (region.known) {
                const int want = impliedScale(region, overlay.width);
                if (want > largestRegionScale) largestRegionScale = want;
                sawRegionLayer = true;
            }
        }
        overlays.push_back({.path = layerPaths[layer], .image = std::move(overlay)});
    }

    // Grow to the most demanding layer.
    //
    // A layer that does not fit its region is not merely higher resolution. A
    // draenei's HD faceLower is a front-facing head where the stock one is a
    // side profile, and a tauren's is a muzzle seen head on where the stock one
    // is the side of a head - different pictures, not larger ones, and shrinking
    // them into a region sized for the stock art puts the wrong thing on the
    // face. At 512x256 they are exactly their region on a 1024 atlas, which is
    // the size their scale is asking for.
    //
    // The cost is the layers that do fit: a body, a pelvis and a torso authored
    // for a 512 atlas get upscaled to sit beside them. That is softer, and
    // softer in the right place beats sharp in the wrong one. It applies only
    // to the four race and sex pairs that ship such a layer; every other set
    // agrees with its body and is untouched.
    if (sawRegionLayer && largestRegionScale > coordScale) {
        requiredScale = largestRegionScale;
    }

    if (requiredScale > coordScale) {
        const int newSize = 256 * requiredScale;
        std::vector<uint8_t> grown(static_cast<size_t>(newSize) * newSize * 4);
        const int factor = newSize / width;
        for (int y = 0; y < newSize; y++) {
            const int srcY = y / factor;
            for (int x = 0; x < newSize; x++) {
                const int srcIdx = (srcY * width + x / factor) * 4;
                const int dstIdx = (y * newSize + x) * 4;
                grown[dstIdx + 0] = composite[srcIdx + 0];
                grown[dstIdx + 1] = composite[srcIdx + 1];
                grown[dstIdx + 2] = composite[srcIdx + 2];
                grown[dstIdx + 3] = composite[srcIdx + 3];
            }
        }
        core::Logger::getInstance().info("Composite: body is ", width, "x", height,
                                         " but its art asks for ", newSize, "x", newSize,
                                         " - growing the atlas to keep the detail");
        composite = std::move(grown);
        width = height = newSize;
        coordScale = requiredScale;
    }

    // Alpha-blend each overlay onto the composite
    for (auto& loaded : overlays) {
        const pipeline::BLPImage& overlay = loaded.image;

        core::Logger::getInstance().info("Composite: overlay ", loaded.path,
            " (", overlay.width, "x", overlay.height, ")");

        if (overlay.width == width && overlay.height == height) {
            // Same size: full alpha-blend
            blitOverlay(composite, width, height, overlay, 0, 0);
        } else {
            // Where this layer belongs, from the one table that also sized the
            // canvas above.
            const AtlasRegion256 region = layerRegion(loaded.path);
            if (!region.known) {
                // Unknown -- center placement as fallback
                const int cx = (width - overlay.width) / 2;
                const int cy = (height - overlay.height) / 2;
                core::Logger::getInstance().info("Composite: UNKNOWN region for '",
                    loaded.path, "', centering at (", cx, ",", cy, ")");
                blitOverlay(composite, width, height, overlay, cx, cy);
                continue;
            }
            int dstX = region.x, dstY = region.y;
            const int expectedW256 = region.w, expectedH256 = region.h;

            // Scale coordinates from 256-base to actual canvas
            dstX *= coordScale;
            dstY *= coordScale;

            // The region is a fixed fraction of the atlas, so the overlay has to
            // land at exactly this size whatever resolution it was authored at.
            // Growing it by a whole factor was not enough, because it only ever
            // grew: an overlay arriving larger than its region was pasted at its
            // own size, spilling across neighbouring regions and dragging every
            // feature on the head to the wrong scale. These assets ship at two
            // resolutions - a 256-wide face belongs in a 128-wide slot on a
            // 256-wide atlas - so the two can meet whenever a lookup resolves
            // the body and the face from different sets.
            const int expectedW = expectedW256 * coordScale;
            const int expectedH = expectedH256 * coordScale;
            const bool needsResample =
                (overlay.width != expectedW || overlay.height != expectedH);

            if (needsResample) {
                // Resampling here means this overlay was authored for a different
                // atlas size than the body it is going onto - the two came from
                // different art sets. It will be placed correctly, but a quarter
                // resolution face stretched over an HD head is soft and muddy
                // next to a crisp body, and that reads as the face not fitting.
                core::Logger::getInstance().warning(
                    "Composite: '", loaded.path, "' is ", overlay.width, "x",
                    overlay.height, " but its region on this ", width, "x", height,
                    " body is ", expectedW, "x", expectedH,
                    " - mismatched art sets; resampling to fit");
            } else {
                core::Logger::getInstance().info("Composite: placing '", loaded.path,
                    "' (", overlay.width, "x", overlay.height,
                    ") at (", dstX, ",", dstY, ") on ", width, "x", height);
            }

            if (needsResample) {
                blitOverlayResampled(composite, width, height, overlay,
                                     dstX, dstY, expectedW, expectedH);
            } else {
                blitOverlay(composite, width, height, overlay, dstX, dstY);
            }
        }
    }

    bleedAndStripMagentaKey(composite, width, height);
    const bool hasAlpha = hasNonOpaqueAlpha(composite);

    // Upload composite to GPU via VkTexture
    auto tex = std::make_unique<VkTexture>();
    tex->upload(*vkCtx_, composite.data(), width, height, VK_FORMAT_R8G8B8A8_UNORM, true);
    tex->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                       VK_SAMPLER_ADDRESS_MODE_REPEAT);

    VkTexture* texPtr = tex.get();

    // Store in texture cache with deterministic key.
    // Keep the first allocation for a key to avoid invalidating raw pointers
    // held by active render instances.
    TextureCacheEntry e;
    e.texture = std::move(tex);
    e.approxBytes = approxTextureBytesWithMips(width, height);
    e.lastUse = ++textureCacheCounter_;
    e.hasAlpha = hasAlpha;
    texturePropsByPtr_[texPtr] = {.hasAlpha = hasAlpha};
    // No derived normal map for a composited body, and this is why: the
    // derivation reads luminance as height, which holds for stone and bark and
    // does not hold for skin. Every freckle, every painted shadow under a
    // collarbone, becomes a ridge - and on a character that reads as stretch
    // marks. Art authored as a surface gets one; art authored as a person does
    // not.
    // Checked before emplacing, not after. emplace builds its node from the
    // arguments and then destroys that node if the key is already present --
    // and ~VkTexture frees nothing, so the texture goes in, comes back out
    // destroyed-but-not-freed, and there is no longer a pointer to free it by.
    if (auto existing = textureCache.find(cacheKey); existing != textureCache.end()) {
        e.texture->destroy(vkCtx_->getDevice(), vkCtx_->getAllocator());
        existing->second.lastUse = ++textureCacheCounter_;
        return existing->second.texture.get();
    }
    textureCache.emplace(cacheKey, std::move(e));

    core::Logger::getInstance().info("Composite texture created: ", width, "x", height, " from ", layerPaths.size(), " layers");
    return texPtr;
}

void CharacterRenderer::clearCompositeCache() {
    // Just clear the lookup map so next compositeWithRegions() creates fresh textures.
    // Don't delete GPU textures -- they may still be referenced by models or instances.
    // Orphaned textures will be cleaned up when their model/instance is destroyed.
    compositeCache_.clear();
}

VkTexture* CharacterRenderer::compositeWithRegions(const std::string& basePath,
                                                const std::vector<std::string>& baseLayers,
                                                const std::vector<std::pair<int, std::string>>& regionLayers) {
    // Build cache key from all inputs to avoid redundant compositing
    std::string cacheKey = basePath;
    for (const auto& bl : baseLayers) { cacheKey += '|'; cacheKey += bl; }
    cacheKey += '#';
    for (const auto& rl : regionLayers) {
        cacheKey += std::to_string(rl.first);
        cacheKey += ':';
        cacheKey += rl.second;
        cacheKey += ',';
    }
    auto cacheIt = compositeCache_.find(cacheKey);
    if (cacheIt != compositeCache_.end() && cacheIt->second != nullptr) {
        return cacheIt->second;
    }

    // If the lookup map was cleared, recover from the texture cache without
    // regenerating/replacing the underlying GPU texture.
    std::string storageKey = "__compositeRegions__" + cacheKey;
    auto cachedComposite = textureCache.find(storageKey);
    if (cachedComposite != textureCache.end()) {
        cachedComposite->second.lastUse = ++textureCacheCounter_;
        VkTexture* texPtr = cachedComposite->second.texture.get();
        compositeCache_[cacheKey] = texPtr;
        return texPtr;
    }

    // Region index -> pixel coordinates on the 256x256 base atlas
    // These are scaled up by (width/256, height/256) for larger textures (512x512, 1024x1024)
    static constexpr int regionCoords256[][2] = {
        {   0,   0 },  // 0 = ArmUpper
        {   0,  64 },  // 1 = ArmLower
        {   0, 128 },  // 2 = Hand
        { 128,   0 },  // 3 = TorsoUpper
        { 128,  64 },  // 4 = TorsoLower
        { 128,  96 },  // 5 = LegUpper
        { 128, 160 },  // 6 = LegLower
        { 128, 224 },  // 7 = Foot
    };

    // First, build base skin + underwear using existing compositeTextures
    std::vector<std::string> layers;
    layers.push_back(basePath);
    for (const auto& ul : baseLayers) {
        layers.push_back(ul);
    }
    // Load base composite into CPU buffer
    if (!assetManager || !assetManager->isInitialized()) {
        return whiteTexture_.get();
    }

    pipeline::BLPImage base;
    if (predecodedBLPCache_) {
        std::string key = basePath;
        std::replace(key.begin(), key.end(), '/', '\\');
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto pit = predecodedBLPCache_->find(key);
        if (pit != predecodedBLPCache_->end()) {
            base = std::move(pit->second);
            predecodedBLPCache_->erase(pit);
        }
    }
    if (!base.isValid()) base = assetManager->loadTexture(basePath);
    if (!base.isValid()) {
        return whiteTexture_.get();
    }
    applyMagentaKeyIfNeeded(base, basePath);

    std::vector<uint8_t> composite;
    int width = base.width;
    int height = base.height;

    // No atlas growth on this path, and the reason is the equipment.
    //
    // Growing it here scales the region coordinates but not the art placed into
    // them: the equipment layers are authored for a 512 atlas, so on a 1024 one
    // they land at quarter size in the right corner of the right region, and a
    // fully armoured NPC comes out naked with fragments of armour scattered
    // over it. That is what growing this path did, and it is worse than what it
    // was meant to fix.
    //
    // The face it was meant to fix is a dwarf's, and a dwarf's HD faceLower is
    // a faithful 2x of the stock one - same parts in the same places - so
    // resampling it down gives back the stock picture. Softer, and right.
    // compositeTextures still grows, because it has no equipment to misplace.

    // If base texture is 256x256 (e.g., baked NPC texture), upscale to 512x512
    // so equipment regions can be composited at correct coordinates
    if (width == kBaseTexSize && height == kBaseTexSize && !regionLayers.empty()) {
        width = kUpscaleTexSize;
        height = kUpscaleTexSize;
        composite.resize(width * height * 4);
        // 2x, interpolated rather than each texel doubled into a block -
        // among opaque texels only: next to a cut-out one (alpha 0, or the
        // magenta key) the plain pick keeps the key and the edge exact.
        for (int y = 0; y < kUpscaleTexSize; y++) {
            for (int x = 0; x < kUpscaleTexSize; x++) {
                const float fx = (static_cast<float>(x) + 0.5f) * 0.5f - 0.5f;
                const float fy = (static_cast<float>(y) + 0.5f) * 0.5f - 0.5f;
                // The floor, without std::floor's call: fx and fy are never
                // below -0.25, so shifting by one truncates the same way.
                const int x0 = std::clamp(static_cast<int>(fx + 1.0f) - 1, 0, kBaseTexSize - 1);
                const int y0 = std::clamp(static_cast<int>(fy + 1.0f) - 1, 0, kBaseTexSize - 1);
                const int x1 = std::min(x0 + 1, kBaseTexSize - 1);
                const int y1 = std::min(y0 + 1, kBaseTexSize - 1);
                const auto alphaAt = [&](int sx, int sy) {
                    return base.data[(static_cast<size_t>(sy) * kBaseTexSize + sx) * 4 + 3];
                };
                const int dstIdx = (y * kUpscaleTexSize + x) * 4;
                if (alphaAt(x0, y0) == 255 && alphaAt(x1, y0) == 255 &&
                    alphaAt(x0, y1) == 255 && alphaAt(x1, y1) == 255) {
                    float rgba[4];
                    sampleImageBilinear(base.data.data(), kBaseTexSize, kBaseTexSize, fx, fy, rgba);
                    for (int c = 0; c < 4; ++c) {
                        composite[dstIdx + c] = compositeByte(std::clamp(rgba[c], 0.0f, 255.0f));
                    }
                } else {
                    const int srcIdx = ((y / 2) * kBaseTexSize + (x / 2)) * 4;
                    for (int c = 0; c < 4; ++c) composite[dstIdx + c] = base.data[srcIdx + c];
                }
            }
        }
        core::Logger::getInstance().debug("compositeWithRegions: upscaled 256x256 to 512x512");
    } else {
        composite = base.data;
    }

    // Blend face + underwear overlays
    // If we upscaled from 256->512, scale coords and texels with blitOverlayScaled2x.
    // For native 512/1024 textures, face overlays are full atlas size (hit width==width branch).
    bool upscaled = (base.width == kBaseTexSize && base.height == kBaseTexSize && width == kUpscaleTexSize);
    for (const auto& ul : baseLayers) {
        if (ul.empty()) continue;
        pipeline::BLPImage overlay;
        if (predecodedBLPCache_) {
            std::string key = ul;
            std::replace(key.begin(), key.end(), '/', '\\');
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            auto pit = predecodedBLPCache_->find(key);
            if (pit != predecodedBLPCache_->end()) {
                overlay = std::move(pit->second);
                predecodedBLPCache_->erase(pit);
            }
        }
        if (!overlay.isValid()) overlay = assetManager->loadTexture(ul);
        if (!overlay.isValid()) continue;
        applyMagentaKeyIfNeeded(overlay, ul);

        if (overlay.width == width && overlay.height == height) {
            blitOverlay(composite, width, height, overlay, 0, 0);
        } else {
            // WoW 256-scale atlas coordinates (from CharComponentTextureSections)
            int dstX = 0, dstY = 0;
            std::string pathLower = ul;
            for (auto& c : pathLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            // Scale factor from 256-base coordinates to actual canvas size
            int coordScale = width / 256;
            if (coordScale < 1) coordScale = 1;
            bool useScale = true;

            if (pathLower.find("faceupper") != std::string::npos) {
                dstX = 0; dstY = 160;
            } else if (pathLower.find("facelower") != std::string::npos) {
                dstX = 0; dstY = 192;
            } else if (pathLower.find("pelvis") != std::string::npos) {
                dstX = 128; dstY = 96;
            } else if (pathLower.find("torso") != std::string::npos) {
                dstX = 128; dstY = 0;
            } else if (pathLower.find("armupper") != std::string::npos) {
                dstX = 0; dstY = 0;
            } else if (pathLower.find("armlower") != std::string::npos) {
                dstX = 0; dstY = 64;
            } else if (pathLower.find("hand") != std::string::npos) {
                dstX = 0; dstY = 128;
            } else if (pathLower.find("foot") != std::string::npos || pathLower.find("feet") != std::string::npos) {
                dstX = 128; dstY = 224;
            } else if (pathLower.find("legupper") != std::string::npos || pathLower.find("leg") != std::string::npos) {
                dstX = 128; dstY = 160;
            } else {
                // Fallback: center overlay on canvas (already in canvas coords)
                dstX = (width - overlay.width) / 2;
                dstY = (height - overlay.height) / 2;
                useScale = false;
            }

            if (useScale) {
                dstX *= coordScale;
                dstY *= coordScale;
            }

            if (upscaled) {
                // Overlay is 256-base sized, needs 2x texel scaling for 512 canvas
                blitOverlayScaled2x(composite, width, height, overlay, dstX, dstY);
            } else {
                blitOverlay(composite, width, height, overlay, dstX, dstY);
            }
        }
    }

    // Expected region sizes on the 256x256 base atlas (scaled like coords)
    static constexpr int regionSizes256[][2] = {
        { 128,  64 },  // 0 = ArmUpper
        { 128,  64 },  // 1 = ArmLower
        { 128,  32 },  // 2 = Hand
        { 128,  64 },  // 3 = TorsoUpper
        { 128,  32 },  // 4 = TorsoLower
        { 128,  64 },  // 5 = LegUpper
        { 128,  64 },  // 6 = LegLower
        { 128,  32 },  // 7 = Foot
    };

    // Scale factor from 256-base to actual texture size
    int scaleX = width / 256;
    int scaleY = height / 256;
    if (scaleX < 1) scaleX = 1;
    if (scaleY < 1) scaleY = 1;

    // Now blit equipment region textures at explicit coordinates
    for (const auto& rl : regionLayers) {
        int regionIdx = rl.first;
        if (regionIdx < 0 || regionIdx >= 8) continue;

        pipeline::BLPImage overlay;
        if (predecodedBLPCache_) {
            std::string key = rl.second;
            std::replace(key.begin(), key.end(), '/', '\\');
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            auto pit = predecodedBLPCache_->find(key);
            if (pit != predecodedBLPCache_->end()) {
                overlay = std::move(pit->second);
                predecodedBLPCache_->erase(pit);
            }
        }
        if (!overlay.isValid()) overlay = assetManager->loadTexture(rl.second);
        if (!overlay.isValid()) {
            core::Logger::getInstance().warning("compositeWithRegions: failed to load ", rl.second);
            continue;
        }
        applyMagentaKeyIfNeeded(overlay, rl.second);

        int dstX = regionCoords256[regionIdx][0] * scaleX;
        int dstY = regionCoords256[regionIdx][1] * scaleY;

        // Expected full-resolution size for this region at current atlas scale
        int expectedW = regionSizes256[regionIdx][0] * scaleX;
        int expectedH = regionSizes256[regionIdx][1] * scaleY;
        if (overlay.width == expectedW && overlay.height == expectedH) {
            // Exact match - blit 1:1
            blitOverlay(composite, width, height, overlay, dstX, dstY);
        } else if (overlay.width * 2 == expectedW && overlay.height * 2 == expectedH) {
            // Overlay is half size - upscale 2x
            blitOverlayScaled2x(composite, width, height, overlay, dstX, dstY);
        } else if (overlay.width > expectedW && overlay.height > expectedH &&
                   expectedW > 0 && expectedH > 0) {
            // Overlay is larger than region (e.g. HD textures for 1024 atlas on 512 canvas)
            // Downscale to fit
            int dsX = overlay.width / expectedW;
            int dsY = overlay.height / expectedH;
            int ds = std::min(dsX, dsY);
            if (ds >= 2) {
                blitOverlayDownscaleN(composite, width, height, overlay, dstX, dstY, ds);
            } else {
                blitOverlay(composite, width, height, overlay, dstX, dstY);
            }
        } else {
            // Size mismatch - blit at natural size (may clip or leave gap)
            core::Logger::getInstance().warning("compositeWithRegions: region ", regionIdx,
                " at (", dstX, ",", dstY, ") overlay=", overlay.width, "x", overlay.height,
                " expected=", expectedW, "x", expectedH, " from ", rl.second);
            blitOverlay(composite, width, height, overlay, dstX, dstY);
        }
    }

    bleedAndStripMagentaKey(composite, width, height);
    const bool hasAlpha = hasNonOpaqueAlpha(composite);

    // Upload to GPU via VkTexture
    auto tex = std::make_unique<VkTexture>();
    tex->upload(*vkCtx_, composite.data(), width, height, VK_FORMAT_R8G8B8A8_UNORM, true);
    tex->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                       VK_SAMPLER_ADDRESS_MODE_REPEAT);

    VkTexture* texPtr = tex.get();

    // Store in texture cache.
    // Use emplace to avoid replacing an existing texture for this key; replacing
    // would invalidate pointers currently bound to active instances.
    TextureCacheEntry entry;
    entry.texture = std::move(tex);
    entry.approxBytes = approxTextureBytesWithMips(width, height);
    entry.lastUse = ++textureCacheCounter_;
    entry.hasAlpha = hasAlpha;
    texturePropsByPtr_[texPtr] = {.hasAlpha = hasAlpha};
    // Skin again, with armour composited onto it. Same reason as above.
    // Checked before emplacing: see the note in compositeTexture. Testing
    // ins.second afterwards is too late -- by then the texture has been moved
    // into a node that emplace already destroyed, leaking it with no handle
    // left to free.
    if (auto existing = textureCache.find(storageKey); existing != textureCache.end()) {
        entry.texture->destroy(vkCtx_->getDevice(), vkCtx_->getAllocator());
        existing->second.lastUse = ++textureCacheCounter_;
        compositeCache_[cacheKey] = existing->second.texture.get();
        return existing->second.texture.get();
    }
    textureCache.emplace(storageKey, std::move(entry));

    core::Logger::getInstance().debug("compositeWithRegions: created ", width, "x", height,
        " texture with ", regionLayers.size(), " equipment regions");
    compositeCache_[cacheKey] = texPtr;
    return texPtr;
}

void CharacterRenderer::setModelTexture(uint32_t modelId, uint32_t textureSlot, VkTexture* texture) {
    auto it = models.find(modelId);
    if (it == models.end()) {
        core::Logger::getInstance().warning("setModelTexture: model ", modelId, " not found");
        return;
    }

    auto& gpuModel = it->second;
    if (textureSlot >= gpuModel.textureIds.size()) {
        core::Logger::getInstance().warning("setModelTexture: slot ", textureSlot, " out of range (", gpuModel.textureIds.size(), " textures)");
        return;
    }

    gpuModel.textureIds[textureSlot] = texture;
    core::Logger::getInstance().debug("Replaced model ", modelId, " texture slot ", textureSlot, " with composited texture");
}
bool CharacterRenderer::loadModel(const pipeline::M2Model& model, uint32_t id) {
    if (!model.isValid()) {
        core::Logger::getInstance().error("Cannot load invalid M2 model");
        return false;
    }

    auto existingIt = models.find(id);
    if (existingIt != models.end()) {
        core::Logger::getInstance().warning("Model ID ", id, " already loaded, replacing");
        destroyModelGPU(existingIt->second, /*defer=*/true);
        models.erase(existingIt);
    }

    M2ModelGPU gpuModel;
    gpuModel.data = model;
    if (!model.vertices.empty()) {
        gpuModel.visualBoundMin = glm::vec3(std::numeric_limits<float>::max());
        gpuModel.visualBoundMax = glm::vec3(-std::numeric_limits<float>::max());
        for (const auto& vertex : model.vertices) {
            gpuModel.visualBoundMin = glm::min(gpuModel.visualBoundMin, vertex.position);
            gpuModel.visualBoundMax = glm::max(gpuModel.visualBoundMax, vertex.position);
        }
        gpuModel.visualBoundRadius =
            glm::length(gpuModel.visualBoundMax - gpuModel.visualBoundMin) * 0.5f;
    }
    const auto classification = classifyM2Model(
        model.name, model.boundMin, model.boundMax,
        model.vertices.size(), model.particleEmitters.size());
    gpuModel.isSkyBird = classification.isSkyBird;

    // Every model this renderer takes on, named once.
    //
    // The same line M2Renderer::loadModel carries, and it has to be in both:
    // creatures and players are drawn here, not there, so a diagnostic that
    // only named M2Renderer's models reported four hundred doodads and not one
    // creature. Three rounds of the Elemental Slave's white sheets were spent
    // reading M2Renderer's particle, glow and bone code before it was noticed
    // that the creature never goes through any of it.
    //
    // The emitter count is worth having here for its own reason: this renderer
    // reads it to classify and then draws none of them, so a creature whose
    // effects are missing entirely says so on this line.
    // Behind the same switch as M2Renderer's: an inventory rather than a
    // fault, and 41 more lines on top of that one's 197.
    static const bool kLoadDiag = core::envFlagEnabled("WOWEE_M2_LOAD_DIAG", false);
    if (kLoadDiag) {
        static core::LogBudget characterLoadBudget(300, "character models named at load");
        if (characterLoadBudget.take()) {
            LOG_WARNING("Character M2 load: '",
                        model.name.empty() ? "<unnamed>" : model.name,
                        "' id=", id,
                        " verts=", model.vertices.size(),
                        " emitters=", model.particleEmitters.size(),
                        " (not drawn)",
                        " ribbons=", model.ribbonEmitters.size());
        }
    }

    // Batch all GPU uploads (VB, IB, textures) into a single command buffer
    // submission with one fence wait, instead of one fence wait per upload.
    vkCtx_->beginUploadBatch();

    // Setup GPU buffers
    setupModelBuffers(gpuModel);

    // Calculate bind pose
    calculateBindPose(gpuModel);

    // Load textures from model.
    //
    // Only type 0 names its own file. Every other type is a slot the client
    // fills: the body and hair from CharSections, the monster skins from
    // CreatureDisplayInfo, the tabard from the guild. Whatever name a model
    // carries on one of those is leftover from whoever built it, and loading
    // it is loading the wrong thing.
    //
    // It reads as nothing on the shipped data, where those names are empty -
    // of 946 creature models and 38 character models, not one names a
    // replaceable slot, and the only three among 8205 item models are
    // Blizzard's own build machine ("Z:\World of Warcraft Proj Server\...")
    // and could never have resolved. It is imported models that carry them:
    // 103 of 784 from a Legion installation, naming Legion paths this
    // installation does not have. The boar's mane asked for "maehne" and drew
    // untextured when nothing answered, because CreatureDisplayInfo has no
    // second skin for it to be overwritten with.
    for (const auto& tex : model.textures) {
        VkTexture* texPtr = tex.type == 0 ? loadTexture(tex.filename, tex.flags) : nullptr;
        gpuModel.textureIds.push_back(texPtr);
    }

    vkCtx_->endUploadBatch();

    // Precompute batch render order (priorityPlane, materialLayer). The result
    // depends only on the model, so caching it here removes the per-frame
    // per-instance allocate + sort from render().
    gpuModel.sortedBatchIndices.resize(gpuModel.data.batches.size());
    std::iota(gpuModel.sortedBatchIndices.begin(), gpuModel.sortedBatchIndices.end(), 0);
    std::stable_sort(gpuModel.sortedBatchIndices.begin(), gpuModel.sortedBatchIndices.end(),
        [&batches = gpuModel.data.batches](size_t a, size_t b) {
            const auto& ba = batches[a];
            const auto& bb = batches[b];
            if (ba.priorityPlane != bb.priorityPlane)
                return ba.priorityPlane < bb.priorityPlane;
            return ba.materialLayer < bb.materialLayer;
        });

    {
        std::string lowerName = gpuModel.data.name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        gpuModel.isKoboldFlame =
            (lowerName.find("kobold") != std::string::npos) &&
            ((lowerName.find("candle") != std::string::npos) ||
             (lowerName.find("torch") != std::string::npos) ||
             (lowerName.find("mine") != std::string::npos));
    }

    models[id] = std::move(gpuModel);

    core::Logger::getInstance().debug("Loaded M2 model ", id, " (", model.vertices.size(),
                       " verts, ", model.bones.size(), " bones, ", model.sequences.size(),
                       " anims, ", model.textures.size(), " textures)");

    return true;
}

void CharacterRenderer::setupModelBuffers(M2ModelGPU& gpuModel) {
    auto& model = gpuModel.data;

    if (model.vertices.empty() || model.indices.empty()) return;

    const size_t vertCount = model.vertices.size();
    const size_t idxCount = model.indices.size();

    // Build expanded GPU vertex buffer with tangents (Lengyel's method)
    std::vector<CharVertexGPU> gpuVerts(vertCount);
    std::vector<glm::vec3> tanAccum(vertCount, glm::vec3(0.0f));
    std::vector<glm::vec3> bitanAccum(vertCount, glm::vec3(0.0f));

    // Copy base vertex data
    size_t numBones = model.bones.size();
    int outOfRangeCount = 0, nonzeroWeightOOR = 0;
    for (size_t i = 0; i < vertCount; i++) {
        const auto& src = model.vertices[i];
        auto& dst = gpuVerts[i];
        dst.position = src.position;
        std::memcpy(dst.boneWeights, src.boneWeights, 4);
        std::memcpy(dst.boneIndices, src.boneIndices, 4);
        dst.normal = src.normal;
        dst.texCoords = src.texCoords[0];
        dst.texCoords2 = src.texCoords[1];
        dst.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f); // default

        // Diagnostic: check bone indices
        for (int j = 0; j < 4; j++) {
            uint8_t bi = src.boneIndices[j];
            uint8_t bw = src.boneWeights[j];
            if (bi >= numBones) {
                outOfRangeCount++;
                if (bw > 0) nonzeroWeightOOR++;
            }
        }
    }
    // A bone index past the end of the bone list, which is a broken model or a
    // misread one.
    //
    // An index at or above 128 is not that: it is what any model with more than
    // 128 bones has, and the character models have 219. Warning on it fired on
    // every character in the world and reported outOfRange=0 every time - a
    // line per model saying nothing was wrong, in a log whose whole value is
    // that what is in it is.
    if (outOfRangeCount > 0) {
        LOG_WARNING("Model has bone indices past its bone list: bones=", numBones,
                    " verts=", vertCount, " outOfRange=", outOfRangeCount,
                    " (nonzeroWeight=", nonzeroWeightOOR, ")",
                    " - those vertices skin to nothing and collapse to the origin");
    }

    // Accumulate tangent/bitangent per triangle
    for (size_t i = 0; i + 2 < idxCount; i += 3) {
        uint16_t i0 = model.indices[i], i1 = model.indices[i+1], i2 = model.indices[i+2];
        if (i0 >= vertCount || i1 >= vertCount || i2 >= vertCount) continue;

        const glm::vec3& p0 = gpuVerts[i0].position;
        const glm::vec3& p1 = gpuVerts[i1].position;
        const glm::vec3& p2 = gpuVerts[i2].position;
        const glm::vec2& uv0 = gpuVerts[i0].texCoords;
        const glm::vec2& uv1 = gpuVerts[i1].texCoords;
        const glm::vec2& uv2 = gpuVerts[i2].texCoords;

        glm::vec3 edge1 = p1 - p0;
        glm::vec3 edge2 = p2 - p0;
        glm::vec2 duv1 = uv1 - uv0;
        glm::vec2 duv2 = uv2 - uv0;

        float det = duv1.x * duv2.y - duv2.x * duv1.y;
        if (std::abs(det) < 1e-8f) continue;
        float invDet = 1.0f / det;

        glm::vec3 t = (edge1 * duv2.y - edge2 * duv1.y) * invDet;
        glm::vec3 b = (edge2 * duv1.x - edge1 * duv2.x) * invDet;

        tanAccum[i0] += t; tanAccum[i1] += t; tanAccum[i2] += t;
        bitanAccum[i0] += b; bitanAccum[i1] += b; bitanAccum[i2] += b;
    }

    // Orthogonalize and compute handedness
    for (size_t i = 0; i < vertCount; i++) {
        const glm::vec3& n = gpuVerts[i].normal;
        const glm::vec3& t = tanAccum[i];
        if (glm::dot(t, t) < 1e-8f) {
            gpuVerts[i].tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            continue;
        }
        // Gram-Schmidt orthogonalize
        glm::vec3 tOrtho = glm::normalize(t - n * glm::dot(n, t));
        float w = (glm::dot(glm::cross(n, t), bitanAccum[i]) < 0.0f) ? -1.0f : 1.0f;
        gpuVerts[i].tangent = glm::vec4(tOrtho, w);
    }

    // Upload vertex buffer (CharVertexGPU, 56 bytes per vertex)
    auto vb = uploadBuffer(*vkCtx_,
        gpuVerts.data(),
        gpuVerts.size() * sizeof(CharVertexGPU),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    gpuModel.vertexBuffer = vb.buffer;
    gpuModel.vertexAlloc = vb.allocation;
    gpuModel.vertexCount = static_cast<uint32_t>(vertCount);

    // Upload index buffer
    auto ib = uploadBuffer(*vkCtx_,
        model.indices.data(),
        idxCount * sizeof(uint16_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    gpuModel.indexBuffer = ib.buffer;
    gpuModel.indexAlloc = ib.allocation;
    gpuModel.indexCount = static_cast<uint32_t>(idxCount);
}

void CharacterRenderer::calculateBindPose(M2ModelGPU& gpuModel) {
    auto& bones = gpuModel.data.bones;
    size_t numBones = bones.size();
    gpuModel.bindPose.resize(numBones);

    // Compute full hierarchical rest pose, then invert.
    // Each bone's rest position is T(pivot), composed with its parent chain.
    std::vector<glm::mat4> restPose(numBones);
    for (size_t i = 0; i < numBones; i++) {
        glm::mat4 local = glm::translate(glm::mat4(1.0f), bones[i].pivot);
        if (bones[i].parentBone >= 0 && static_cast<size_t>(bones[i].parentBone) < numBones) {
            restPose[i] = restPose[bones[i].parentBone] * local;
        } else {
            restPose[i] = local;
        }
        gpuModel.bindPose[i] = glm::inverse(restPose[i]);
    }
}

uint32_t CharacterRenderer::createInstance(uint32_t modelId, const glm::vec3& position,
                                           const glm::vec3& rotation, float scale) {
    auto modelIt = models.find(modelId);
    if (modelIt == models.end()) {
        core::Logger::getInstance().error("Cannot create instance: model ", modelId, " not loaded");
        return 0;
    }

    CharacterInstance instance;
    instance.id = nextInstanceId++;
    instance.modelId = modelId;
    instance.position = position;
    instance.rotation = rotation;
    instance.scale = scale;

    // Initialize bone matrices to identity
    auto& gpuRef = modelIt->second;
    instance.boneMatrices.resize(std::max(static_cast<size_t>(1), gpuRef.data.bones.size()), glm::mat4(1.0f));
    instance.cachedModel = &gpuRef;

    uint32_t id = instance.id;
    instances[id] = std::move(instance);
    return id;
}

namespace {
// The index of an animation's sequence - its variation 0, the head of its
// variations, else the first of that id - or -1 where the model has none.
int animationSequenceIndex(const pipeline::M2Model& model, uint32_t animationId) {
    int firstMatch = -1;
    for (size_t i = 0; i < model.sequences.size(); i++) {
        if (model.sequences[i].id != animationId) continue;
        if (model.sequences[i].variationIndex == 0) return static_cast<int>(i);
        if (firstMatch < 0) firstMatch = static_cast<int>(i);
    }
    return firstMatch;
}

bool modelHasAnimation(const pipeline::M2Model& model, uint32_t animationId) {
    return std::any_of(model.sequences.begin(), model.sequences.end(),
                       [animationId](const auto& seq) { return seq.id == animationId; });
}

// 0x00826350: the model's sequence for an animation, or the one along
// AnimationData's Fallbacks that stands in for it and how that one plays.
anim::ModelAnimation shownAnimation(const pipeline::M2Model& model, uint32_t animationId) {
    const auto has = [&model](uint32_t id) { return modelHasAnimation(model, id); };
    return anim::modelAnimation(anim::animationData(), animationId, has,
                                model.sequences.empty() ? 0u : model.sequences[0].id);
}

// 0x007176f0: what a unit of the instance's tier is put in for an
// animation it is asked.
uint32_t tierAnimationFor(const pipeline::M2Model& model, uint32_t animationId, uint8_t tier) {
    const auto has = [&model](uint32_t id) { return modelHasAnimation(model, id); };
    return anim::unitAnimation(anim::animationData(), animationId, tier, has);
}

// The time a sequence is sampled at: backwards from its end where it plays
// that way (0x00826b00 negates the speed and starts it at its length).
float sampledTime(const pipeline::M2Model& model, int sequence, float time, anim::PlayMode mode) {
    if (mode != anim::PlayMode::Reversed || sequence < 0 ||
        static_cast<size_t>(sequence) >= model.sequences.size()) {
        return time;
    }
    return std::max(0.0f, static_cast<float>(model.sequences[static_cast<size_t>(sequence)].duration) - time);
}

// Held on its first or last frame: its time does not run (0x00826b00's
// speed 0) and it never ends.
bool heldMode(anim::PlayMode mode) {
    return mode == anim::PlayMode::HoldStart || mode == anim::PlayMode::HoldEnd;
}

// A key bone's animation turned to blending back into the body's: from
// where it is, over 150 ms (0x00832840: end now + 0x96, rate 1/150).
template <class KeyBoneAnimation>
void returnKeyBoneState(KeyBoneAnimation& k, float clockMs, const pipeline::M2Model& model) {
    constexpr uint32_t kReturnMs = 150;
    m2_track::beginSequenceBlend(k.blend, clockMs, k.sequence, sampledTime(model, k.sequence, k.time, k.mode),
                                 !k.once, k.once, -1, kReturnMs, model.sequences);
    k.returning = true;
    k.once = false;
    k.thenSequence = -1;
}

// BehaviorID 0 (0x007176b0): Stand, FlyStand - those two by the rows
// before AnimationData is loaded.
bool isStand(uint32_t animationId) {
    const auto& data = anim::animationData();
    if (data.empty()) return animationId == anim::STAND || animationId == anim::FLY_STAND;
    return data.behavior(animationId) == 0;
}

// Where a sequence starts: its end for one held there.
float startTime(const pipeline::M2Model& model, int sequence, anim::PlayMode mode) {
    if (mode != anim::PlayMode::HoldEnd || sequence < 0 ||
        static_cast<size_t>(sequence) >= model.sequences.size()) {
        return 0.0f;
    }
    return static_cast<float>(model.sequences[static_cast<size_t>(sequence)].duration);
}
}  // namespace

void CharacterRenderer::setAnimationTier(uint32_t instanceId, uint8_t tier) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) it->second.animationTier = tier;
}

void CharacterRenderer::setRestAnimation(uint32_t instanceId, uint32_t restAnim) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) it->second.restAnimation = restAnim;
}

void CharacterRenderer::playAnimation(uint32_t instanceId, uint32_t animationId, bool loop,
                                      uint32_t oneShotReturnAnim) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) {
        core::Logger::getInstance().warning("Cannot play animation: instance ", instanceId, " not found");
        return;
    }
    // The animation of its tier the model has (0x007385c0 -> 0x007176f0).
    animationId = tierAnimationFor(models[it->second.modelId].data, animationId, it->second.animationTier);
    // A seated passenger's body is its vehicle's where it mirrors it
    // (0x007385c0 asks 0x007487e0), else its seat's (0x007385c0,
    // 0x00724500). On the way in or out nothing else is played; in the
    // seat, a stand is what the seat has its upper body stand as (the base
    // 0x00724500 asks there, 0x007485b0), and the rest is shared as any
    // unit's is.
    if (it->second.mirrorsSeatParent) return;
    if (it->second.seatAnimationHeld) {
        if (!it->second.seatRideSeated) return;
        if (isStand(animationId)) {
            standSeatedUpperBody(instanceId);
            return;
        }
        playUnitAnimation(instanceId, animationId, loop, oneShotReturnAnim, upper_body::Pass::Asked);
        return;
    }
    if (it->second.seatStandsWhole && isStand(animationId)) {
        standSeatedWhole(instanceId);
        return;
    }
    // A unit's body and upper body share it (0x007385c0).
    if (it->second.unitAnimation) {
        playUnitAnimation(instanceId, animationId, loop, oneShotReturnAnim, upper_body::Pass::Asked);
        return;
    }
    playAnimationNow(instanceId, animationId, loop, oneShotReturnAnim);
}

void CharacterRenderer::setUnitAnimationState(uint32_t instanceId, const upper_body::Unit& unit) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    // The stand state its animation is for starts as the one it has.
    if (it->second.standStateSeen < 0) it->second.standStateSeen = unit.standState;
    // A state kit that animates the unit coming or going asks its base
    // animation again (0x0073b140 type 2 and 0x0073c1d0 -> 0x0073ac30),
    // which the kit's animation then takes over (0x0071e400).
    const bool kitChanged = it->second.unitAnimation && (it->second.unit.kitAnim != unit.kitAnim ||
                                                          it->second.unit.kitAnimTiered != unit.kitAnimTiered);
    it->second.unitAnimation = true;
    it->second.unit = unit;
    if (kitChanged) playBaseAnimation(instanceId);
}

int32_t CharacterRenderer::upperBodyBone(CharacterInstance& instance) {
    if (instance.upperBodyBone == -2) {
        const auto& bones = models[instance.modelId].data.bones;
        const auto has = [&bones](int32_t key) {
            return std::any_of(bones.begin(), bones.end(), [key](const auto& b) { return b.keyBoneId == key; });
        };
        instance.upperBodyBone = vehicle_seat::upperKeyBone(has(4), has(6));
    }
    return instance.upperBodyBone;
}

void CharacterRenderer::playUnitAnimation(uint32_t instanceId, uint32_t animationId, bool loop,
                                          uint32_t oneShotReturnAnim, upper_body::Pass pass) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    auto& instance = it->second;
    const auto& model = models[instance.modelId].data;
    const int32_t bone = upperBodyBone(instance);
    // What each plays now (0x007173f0): the body - nothing once its one-shot
    // has ended - and the upper body's own.
    upper_body::Slots now;
    if (instance.currentSequenceIndex >= 0 && pass == upper_body::Pass::Asked) {
        now.body = {static_cast<int32_t>(instance.currentAnimationId), instance.animationTime, instance.animationLoop};
    }
    for (const auto& k : instance.keyBoneAnimations) {
        if (k.keyBoneId == bone && k.sequence >= 0 && !k.returning)
            now.upper = {static_cast<int32_t>(k.animationId), k.time, !k.once};
    }
    upper_body::Unit unit = instance.unit;
    // The vehicle's seats animating its upper body's bone (0x00756cd0).
    unit.vehicleHoldsUpper =
        std::any_of(instance.keyBoneAnimations.begin(), instance.keyBoneAnimations.end(),
                    [bone](const CharacterInstance::KeyBoneAnimation& k) {
                        return k.keyBoneId == bone && k.vehicle && k.sequence >= 0 && !k.returning;
                    });
    const bool seated = instance.seatAnimationHeld && instance.seatRideSeated;
    unit.seatedRide = seated;
    unit.swimIdle = tierAnimationFor(model, anim::SWIM_IDLE, instance.animationTier);
    unit.riderPoseShown = tierAnimationFor(model, unit.riderPose, instance.animationTier);
    const upper_body::Slot asked{static_cast<int32_t>(animationId), 0.0f, loop};
    upper_body::Decision d = upper_body::decide(anim::animationData(), unit, asked, now, pass);
    // A swing over a swing: the one playing runs on at twice the speed and
    // the asked one waits (+0xb88); any other ask lets it go.
    if (d.queue) {
        constexpr float kHastened = 2.0f;
        bool onUpper = false;
        for (auto& k : instance.keyBoneAnimations) {
            if (k.keyBoneId == bone && k.sequence >= 0 && !k.returning) {
                k.rate = kHastened;
                onUpper = true;
            }
        }
        if (!onUpper) instance.bodyRateSet = kHastened;
        instance.queuedAttack = static_cast<int32_t>(animationId);
        return;
    }
    instance.queuedAttack = -1;
    // Hovering on the ground, the stand and its like are the hover
    // (0x0071df30).
    upper_body::applyHover(unit, now, d, [&model](uint32_t id) { return shownAnimation(model, id).id; });
    // A state kit animating the unit has the body and an upper body of its
    // own play its animation, unblended, whatever was asked but a death
    // (0x0071e400).
    if (unit.kitAnim && !upper_body::isDeath(anim::animationData().behavior(animationId))) {
        const uint32_t kit =
            unit.kitAnimTiered ? tierAnimationFor(model, *unit.kitAnim, instance.animationTier) : *unit.kitAnim;
        if (!unit.riding) {
            upper_body::applyKitAnimation(now, d, kit);
        } else {
            // A rider: its mount plays it (the unit's sync), and its upper
            // body where it is an upper-body animation and not the mount's;
            // the body holds the rider's pose.
            upper_body::applyRiderKitAnimation(anim::animationData(), now, d, kit);
        }
    }

    // The upper body first, as 0x007385c0 plays them. Without a key bone of
    // its own, what goes there is not played (0x00737ef0).
    if (bone >= 0) {
        // In its seat, the upper body given back, or given the ride
        // animation the body plays, stands as the seat has it; a one-shot
        // there ends into the seat's loop (0x00737bd0 with Flags 0x4).
        const bool seatStand =
            seated && (d.upperAction == upper_body::Action::Release ||
                       (d.upperAction == upper_body::Action::Play && d.upperFromBody &&
                        d.upper.anim == static_cast<int32_t>(instance.currentAnimationId)));
        if (seatStand) {
            standSeatedUpperBody(instanceId);
        } else if (d.upperAction == upper_body::Action::Release) {
            returnKeyBone(instance, bone);
        } else if (d.upperAction == upper_body::Action::Play && d.upper.anim >= 0) {
            const bool once = !d.upper.loop;
            startKeyBoneAnimation(instance, bone, static_cast<uint32_t>(d.upper.anim), once,
                                  seated && once ? instance.seatUpperLoop : std::nullopt, d.upper.timeMs,
                                  d.upperBlend);
        }
    }
    // What the mount plays is its own driver's.
    if (d.bodyAction != upper_body::Action::Play || d.body.anim < 0) return;
    const auto bodyAnim = static_cast<uint32_t>(d.body.anim);
    if (d.bodyFromUpper) {
        // The swing taken back goes on from where it had got, and ends into
        // what was asked (0x0073bbd0 -> 0x0073b510 asks it then).
        playAnimationNow(instanceId, bodyAnim, d.body.loop, loop ? animationId : oneShotReturnAnim, d.bodyBlend);
        if (instance.currentSequenceIndex >= 0) {
            const float duration =
                static_cast<float>(model.sequences[static_cast<size_t>(instance.currentSequenceIndex)].duration);
            instance.animationTime = std::clamp(d.body.timeMs, 0.0f, duration);
            instance.eventsFrom = instance.animationTime;
        }
    } else if (bodyAnim == animationId) {
        playAnimationNow(instanceId, bodyAnim, loop, oneShotReturnAnim, d.bodyBlend);
    } else {
        // What the client asks of its own: SwimIdle, the rider's pose.
        playAnimationNow(instanceId, bodyAnim, true, 0, d.bodyBlend);
    }
}

void CharacterRenderer::playKitAnimation(uint32_t instanceId, const kit_animation::KitPlay& kit) {
    auto it = instances.find(instanceId);
    if (it == instances.end() || !it->second.unitAnimation) return;
    auto& instance = it->second;
    const int32_t bone = upperBodyBone(instance);
    int32_t shown = instance.currentSequenceIndex >= 0 ? static_cast<int32_t>(instance.currentAnimationId) : -1;
    for (const auto& k : instance.keyBoneAnimations) {
        if (k.keyBoneId == bone && k.sequence >= 0 && !k.returning) shown = static_cast<int32_t>(k.animationId);
    }
    const kit_animation::Action a = kit_animation::onKitPlayed(anim::animationData(), kit, shown);
    if (a.clearsStart) instance.precastStart = -1;
    switch (a.kind) {
        case kit_animation::Action::Kind::None:
            return;
        case kit_animation::Action::Kind::Wound:
            if (const auto w = woundFor(instanceId, a.critical)) playWound(instanceId, *w);
            return;
        case kit_animation::Action::Kind::Start:
            playAskedAnimation(instanceId, a.anim, false, a.askFlags);
            instances[instanceId].precastStart = static_cast<int32_t>(a.anim);
            return;
        case kit_animation::Action::Kind::Play:
            playAskedAnimation(instanceId, a.anim, !a.once, a.askFlags);
            if (a.holdsCastLoop) instances[instanceId].kitAnimationHeld = static_cast<int32_t>(a.anim);
            return;
    }
}

void CharacterRenderer::playAskedAnimation(uint32_t instanceId, uint32_t animationId, bool loop, uint32_t askFlags) {
    // Asked with kit Flags 0x4 or 0x8 (0x2 or 0x4), it is played as it is,
    // not as the unit's tier has it (0x007385c0 skips 0x007176f0).
    if ((askFlags & 0x6u) == 0) {
        playAnimation(instanceId, animationId, loop);
        return;
    }
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    if (it->second.mirrorsSeatParent || it->second.seatAnimationHeld) return;
    playUnitAnimation(instanceId, animationId, loop, 0, upper_body::Pass::Asked);
}

base_animation::Pick CharacterRenderer::pickBaseAnimation(CharacterInstance& instance,
                                                          const base_animation::Ask& ask) {
    const auto& model = models[instance.modelId].data;
    const int32_t bone = upperBodyBone(instance);
    // What plays: the body's (0x007172b0), and the upper body's own over it
    // (0x00717260).
    base_animation::Playing playing;
    if (instance.currentSequenceIndex >= 0) playing.body = static_cast<int32_t>(instance.currentAnimationId);
    playing.shown = playing.body;
    for (const auto& k : instance.keyBoneAnimations) {
        if (k.keyBoneId == bone && k.sequence >= 0 && !k.returning) playing.shown = static_cast<int32_t>(k.animationId);
    }
    upper_body::Unit unit = instance.unit;
    unit.speed = instance.locomotionSpeed;
    if (instance.standStateSeen >= 0) unit.standStateSeen = static_cast<uint8_t>(instance.standStateSeen);
    // 0x007224d0: no cast loop while a PlayImpact kit's animation plays
    // (+0xfa4); the PrecastKit's StartAnimID until it has ended (+0xa38
    // 0x10000), its AnimID after.
    if (instance.kitAnimationHeld >= 0) {
        unit.castLoopAnim.reset();
    } else if (instance.precastStart >= 0 && unit.castTimed) {
        unit.castLoopAnim = static_cast<uint32_t>(instance.precastStart);
    }
    const auto has = [&model](uint32_t id) {
        return std::any_of(model.sequences.begin(), model.sequences.end(),
                           [id](const pipeline::M2Sequence& seq) { return seq.id == id; });
    };
    const uint8_t tier = instance.animationTier;
    return base_animation::pick(anim::animationData(), unit, playing, ask, has,
                                [&has, &model, tier](uint32_t id) { return has(tierAnimationFor(model, id, tier)); });
}

void CharacterRenderer::playBaseAnimation(uint32_t instanceId) {
    auto it = instances.find(instanceId);
    if (it == instances.end() || !it->second.unitAnimation) return;
    // 0x0073ac30: what 0x00724500 answers is asked as any animation is,
    // and the stand state is the one it was given for (+0x9f8).
    const base_animation::Pick p =
        pickBaseAnimation(it->second, base_animation::Ask{.queuedAttack = it->second.queuedAttack});
    it->second.standStateSeen = it->second.unit.standState;
    if (p.anim != base_animation::kKeep) playAskedAnimation(instanceId, p.anim, p.loop, p.flags);
}

void CharacterRenderer::standStateChanged(uint32_t instanceId, uint8_t standState) {
    auto it = instances.find(instanceId);
    if (it == instances.end() || !it->second.unitAnimation) return;
    auto& instance = it->second;
    instance.unit.standState = standState;
    // 0x0073f060: nothing where it is the state it was given for.
    if (instance.standStateSeen == static_cast<int16_t>(standState)) return;
    const int16_t seen = instance.standStateSeen;
    if (standState == 0 && seen == 9) {
        // Up from under: Birth where the model has it, else Emerge.
        playAnimation(instanceId, hasAnimation(instanceId, 127) ? 127u : 224u, false);
    } else if (standState == 9) {
        playAnimation(instanceId, 201, false);  // Submerge
    } else if (standState != 7) {
        // The rest from the base animation, which knows the state it was in.
        playBaseAnimation(instanceId);
    }
    instances[instanceId].standStateSeen = standState;
}

std::optional<upper_body::Wound> CharacterRenderer::woundFor(uint32_t instanceId, bool critical) {
    auto it = instances.find(instanceId);
    if (it == instances.end() || !it->second.unitAnimation) return std::nullopt;
    auto& instance = it->second;
    const auto& model = models[instance.modelId].data;
    const int32_t bone = upperBodyBone(instance);
    const int32_t body = instance.currentSequenceIndex >= 0 ? static_cast<int32_t>(instance.currentAnimationId) : -1;
    int32_t shown = body;
    for (const auto& k : instance.keyBoneAnimations) {
        if (k.keyBoneId == bone && k.sequence >= 0 && !k.returning) shown = static_cast<int32_t>(k.animationId);
    }
    auto w = upper_body::wound(anim::animationData(), instance.unit, critical, shown, body);
    if (!w) return std::nullopt;
    w->anim = tierAnimationFor(model, w->anim, instance.animationTier);
    // Played only where the model has it (0x00825ee0), on the key bone
    // where it has one.
    if (std::none_of(model.sequences.begin(), model.sequences.end(),
                     [&w](const pipeline::M2Sequence& seq) { return seq.id == w->anim; })) {
        return std::nullopt;
    }
    if (bone < 0) w->upper = false;
    return w;
}

void CharacterRenderer::playWound(uint32_t instanceId, const upper_body::Wound& wound) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    if (wound.upper) {
        startKeyBoneAnimation(it->second, upperBodyBone(it->second), wound.anim, true, std::nullopt, 0.0f, false);
    } else {
        playAnimationNow(instanceId, wound.anim, false, 0, false);
    }
}

void CharacterRenderer::endUpperBodyOneShot(CharacterInstance& instance) {
    const auto& model = models[instance.modelId].data;
    const auto& data = anim::animationData();
    const int32_t bone = upperBodyBone(instance);
    // The base animation as the unit is put in it (0x00724500 ->
    // 0x007176f0) goes on the upper body where it is one of the upper
    // body's, may differ from what the body plays (0x00723e30), and is not
    // that; else the bone set follows the body again (0x00832840).
    // A state kit animating the unit takes the upper body (0x0071e400).
    if (instance.unit.kitAnim) {
        const uint32_t kit = instance.unit.kitAnimTiered
                                 ? tierAnimationFor(model, *instance.unit.kitAnim, instance.animationTier)
                                 : *instance.unit.kitAnim;
        startKeyBoneAnimation(instance, bone, kit, false, std::nullopt, 0.0f, true);
        return;
    }
    const base_animation::Pick p =
        pickBaseAnimation(instance, base_animation::Ask{.queuedAttack = instance.queuedAttack});
    const uint32_t shown = p.anim == base_animation::kKeep || (p.flags & 0x6) != 0
                               ? p.anim
                               : tierAnimationFor(model, p.anim, instance.animationTier);
    const int32_t body = instance.currentSequenceIndex >= 0 ? static_cast<int32_t>(instance.currentAnimationId) : -1;
    if (!upper_body::isUpperBody(data.behavior(shown)) || !upper_body::splits(data, instance.unit, shown, body) ||
        static_cast<int32_t>(shown) == body) {
        returnKeyBone(instance, bone);
        return;
    }
    startKeyBoneAnimation(instance, bone, shown, !p.loop, std::nullopt, 0.0f, true);
}

std::vector<uint32_t> CharacterRenderer::mirroringPassengers(uint32_t instanceId) const {
    std::vector<uint32_t> out;
    for (const auto& [id, inst] : instances) {
        if (inst.mirrorsSeatParent && inst.seatParent == instanceId) out.push_back(id);
    }
    return out;
}

void CharacterRenderer::playAnimationNow(uint32_t instanceId, uint32_t animationId, bool loop,
                                         uint32_t oneShotReturnAnim, bool blend) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    startAnimation(it->second, animationId, loop, oneShotReturnAnim, blend);
    // The vehicle hands what it plays to the passengers mirroring it
    // (0x00735a60, 0x00735820).
    for (uint32_t passenger : mirroringPassengers(instanceId)) {
        auto pit = instances.find(passenger);
        if (pit != instances.end()) startAnimation(pit->second, animationId, loop, oneShotReturnAnim);
    }
}

void CharacterRenderer::startAnimation(CharacterInstance& instance, uint32_t animationId, bool loop,
                                       uint32_t oneShotReturnAnim, bool blend) {
    auto& model = models[instance.modelId].data;
    instance.oneShotReturnAnim = loop ? 0 : oneShotReturnAnim;
    instance.bodyRateSet = 0.0f;

    // Track death state for preventing movement while dead
    if (animationId == 1) {
        instance.isDead = true;
    } else if (instance.isDead && animationId == 0) {
        instance.isDead = false;  // Respawned
    }

    // What is playing now, for the blend out of it below.
    const int previousSequence = instance.currentSequenceIndex;
    const float previousTime = sampledTime(model, previousSequence, instance.animationTime, instance.playMode);
    const bool previousLoops = instance.animationLoop;
    const bool previousFinished = !previousLoops && previousSequence >= 0 &&
        static_cast<size_t>(previousSequence) < model.sequences.size() &&
        instance.animationTime >= static_cast<float>(model.sequences[previousSequence].duration);

    // The animation keeps the id it was asked by (the model's +0x90,
    // 0x008267e0 reads it back); the sequence is the model's own for it, or
    // the one AnimationData's Fallbacks reach, played as they say
    // (0x00832ab0 -> 0x00826350).
    const anim::ModelAnimation shown = shownAnimation(model, animationId);
    instance.currentAnimationId = animationId;
    instance.currentSequenceIndex = animationSequenceIndex(model, shown.id);
    instance.primarySequenceIndex = -1;
    instance.playMode = shown.mode;
    instance.animationTime = 0.0f;
    instance.animationLoop = loop;
    if (instance.currentSequenceIndex >= 0 && shown.id != animationId) {
        static std::unordered_map<uint32_t, std::unordered_set<uint32_t>> loggedStandIns;
        if (loggedStandIns[instance.modelId].insert(animationId).second) {
            LOG_DEBUG("Animation ", animationId, " not in model ", instance.modelId, ": playing ", shown.id,
                      " (mode ", static_cast<int>(shown.mode), ")");
        }
    }
    if (instance.currentSequenceIndex >= 0) {
        // Roll the variation as the client does on every play (FUN_00832bxx ->
        // FUN_00826e60); update() re-rolls at the end of each looped pass.
        instance.primarySequenceIndex = instance.currentSequenceIndex;
        instance.currentSequenceIndex =
            m2_track::pickSequenceVariation(model.sequences, instance.primarySequenceIndex,
                                            static_cast<uint32_t>(std::rand()) & 0x7FFFu);
        // One held on its last frame starts there (0x00826b00).
        instance.animationTime = startTime(model, instance.currentSequenceIndex, shown.mode);
    }

    if (instance.currentSequenceIndex < 0) {
        // Fall back to first sequence
        if (!model.sequences.empty()) {
            instance.currentSequenceIndex = 0;
            instance.currentAnimationId = model.sequences[0].id;
        }

        // Only log missing animation once per model (reduce spam)
        static std::unordered_map<uint32_t, std::unordered_set<uint32_t>> loggedMissingAnims;
        uint32_t mId = instance.modelId;  // Use modelId as identifier
        if (loggedMissingAnims[mId].insert(animationId).second) {
            // First time seeing this missing animation for this model
            LOG_WARNING("Animation ", animationId, " not found in model ", mId, ", using default");
        }
    }

    // Blend from the old pose over the new sequence's blendTime, as the
    // client's FUN_00826c40 does for every animation a unit is told to play.
    if (instance.currentSequenceIndex >= 0 && blend) {
        m2_track::beginSequenceBlend(
            instance.sequenceBlend, instance.globalSequenceTime, previousSequence, previousTime,
            previousLoops, previousFinished, instance.currentSequenceIndex,
            model.sequences[instance.currentSequenceIndex].blendTime, model.sequences,
            instance.playbackRate);
    }

    // A moving unit going from one movement sequence to another keeps its
    // stride: the new one starts at the fraction of its length the old had
    // reached (FUN_007385c0, see m2_track::locomotionPhaseTime). The client
    // asks for this only when the new animation is a movement one with a
    // movingSpeed, the unit is moving, and the old sequence has a movingSpeed
    // and a length.
    if (instance.locomotionSpeed > 0.0f && instance.currentSequenceIndex >= 0 &&
        instance.playMode == anim::PlayMode::Forward &&
        previousSequence >= 0 && static_cast<size_t>(previousSequence) < model.sequences.size()) {
        const auto& from = model.sequences[static_cast<size_t>(previousSequence)];
        const auto& to = model.sequences[static_cast<size_t>(instance.currentSequenceIndex)];
        if (m2_track::isLocomotionAnimation(to.id) && to.movingSpeed != 0.0f &&
            from.movingSpeed != 0.0f) {
            instance.animationTime =
                m2_track::locomotionPhaseTime(previousTime, from.duration, to.duration);
        }
    }
    // The new sequence's events from where it starts in it.
    instance.eventsRestart = true;
    instance.eventsFrom = instance.animationTime > 0.0f ? instance.animationTime : -1.0f;
}

void CharacterRenderer::setLocomotionSpeed(uint32_t instanceId, float yardsPerSecond) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    it->second.locomotionSpeed =
        std::isfinite(yardsPerSecond) ? std::max(0.0f, yardsPerSecond) : 0.0f;
}

namespace {
// Which key bone animation each bone follows: its own key bone's, else its
// parent's. Parents come first, so one pass finds them.
template <class KeyBoneAnimations>
void assignKeyBoneAnimations(const pipeline::M2Model& model, const KeyBoneAnimations& anims,
                             std::vector<int16_t>& out) {
    out.assign(model.bones.size(), -1);
    if (anims.empty()) return;
    for (size_t i = 0; i < model.bones.size(); i++) {
        const auto& bone = model.bones[i];
        int16_t set = bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) < i ? out[bone.parentBone] : -1;
        for (size_t a = 0; a < anims.size(); a++) {
            if (anims[a].keyBoneId == bone.keyBoneId) set = static_cast<int16_t>(a);
        }
        out[i] = set;
    }
}
}  // namespace

void CharacterRenderer::update(float deltaTime, const glm::vec3& cameraPos, const glm::mat4* viewProjection) {
    // Animate everything that can be drawn. The animation radius used to be
    // 120 yards against a 130-yard draw radius, so anything between the two
    // stood frozen in its last pose; the client animates what it shows.
    const float animUpdateRadius = static_cast<float>(envSizeOrDefault("WOWEE_CHAR_RENDER_RADIUS", 130));
    const float animUpdateRadiusSq = animUpdateRadius * animUpdateRadius;
    // What it shows, and no more: a unit behind the camera keeps its clock
    // running below, but its bones are not posed until it comes into view -
    // in a city of four hundred that was most of the cost of this pass. The
    // view is widened by a few yards so a shadow cast into it from just
    // outside still moves, and everything close by is posed regardless.
    Frustum viewFrustum;
    if (viewProjection) viewFrustum.extractFromMatrix(*viewProjection);
    constexpr float kOffscreenMarginYards = 10.0f;
    constexpr float kAlwaysPoseRadiusSq = 25.0f * 25.0f;

    // Single pass: fade-in, movement, and animation bone collection
    toUpdate_.clear();

    for (auto& pair : instances) {
        auto& inst = pair.second;

        // Update fade-in opacity
        if (inst.fadeInDuration > 0.0f && inst.opacity < 1.0f) {
            inst.fadeInTime += deltaTime;
            inst.opacity = std::min(1.0f, inst.fadeInTime / inst.fadeInDuration);
            if (inst.opacity >= 1.0f) {
                inst.fadeInDuration = 0.0f;
            }
        }

        // A spell kit's alpha on its way (0x00744030).
        if (inst.kitAlphaSeconds > 0.0f) {
            inst.kitAlphaElapsed += deltaTime;
            const float t = std::min(1.0f, inst.kitAlphaElapsed / inst.kitAlphaSeconds);
            inst.kitAlpha = inst.kitAlphaFrom + (inst.kitAlphaTo - inst.kitAlphaFrom) * t;
            if (t >= 1.0f) inst.kitAlphaSeconds = 0.0f;
        }

        // Interpolate creature movement
        if (inst.isMoving) {
            inst.moveElapsed += deltaTime;
            float t = inst.moveElapsed / inst.moveDuration;
            if (t >= 1.0f) {
                inst.position = inst.moveEnd;
                inst.isMoving = false;
            } else {
                inst.position = glm::mix(inst.moveStart, inst.moveEnd, t);
            }
        }

        // Skip weapon instances for animation - their transforms are set by parent
        // bones. Enchant visuals are the exception: they are pure animated FX.
        if (inst.hasOverrideModelMatrix && !inst.isEffectModel && inst.seatParent == 0 && !inst.seatFlight) continue;

        float distSq = glm::distance2(inst.position, cameraPos);
        if (distSq > animUpdateRadiusSq && !inst.isSceneModel) continue;

        // Advance global sequence timer (accumulates independently of animation wrapping)
        inst.globalSequenceTime += deltaTime * 1000.0f;

        // An arm on its own animation runs at rate 1 (0x00735820) to its
        // event and its end, and then follows the body again.
        for (int arm = 0; arm < 2 && inst.cachedModel && !inst.animationFrozen; ++arm) {
            const int seq = inst.armSequenceIndex[arm];
            if (seq < 0 || static_cast<size_t>(seq) >= inst.cachedModel->data.sequences.size()) continue;
            // Held where it stands in, it neither runs nor ends.
            if (heldMode(inst.armMode[arm])) continue;
            const float before = inst.armTime[arm];
            inst.armTime[arm] += deltaTime * 1000.0f;
            const float eventTime = inst.armEventTime[arm];
            if (eventTime >= 0.0f && before <= eventTime && inst.armTime[arm] > eventTime) {
                inst.armEvents |= arm == 0 ? kArmEventLeft : kArmEventRight;
            }
            if (inst.armTime[arm] >= static_cast<float>(inst.cachedModel->data.sequences[seq].duration)) {
                inst.armSequenceIndex[arm] = -1;
                inst.armEvents |= arm == 0 ? kArmEndLeft : kArmEndRight;
            }
        }

        // Key bones on their own sequences run on at rate 1, looping; one
        // played once gives its bone back to the body at its end
        // (0x00757280 -> 0x00757420 -> 0x00735a60).
        if (inst.cachedModel && !inst.animationFrozen) {
            bool ended = false;
            // A unit's upper body whose one-shot ends is given the base
            // animation, or back to the body (0x0073bbd0 -> 0x00737bd0).
            const int32_t unitUpperBone =
                inst.unitAnimation && !inst.seatAnimationHeld && !inst.seatStandsWhole && !inst.mirrorsSeatParent
                    ? upperBodyBone(inst)
                    : -1;
            bool upperEnded = false;
            for (auto& k : inst.keyBoneAnimations) {
                if (k.sequence < 0 || static_cast<size_t>(k.sequence) >= inst.cachedModel->data.sequences.size())
                    continue;
                // Back to the body's once its blend there is done (0x00832840).
                if (k.returning) {
                    if (m2_track::currentBlend(k.blend, inst.globalSequenceTime, inst.cachedModel->data.sequences, -1,
                                               0.0f).weight <= 0.0f) {
                        k.sequence = -1;
                        ended = true;
                    }
                    continue;
                }
                // Held where it stands in, it neither runs nor ends.
                if (heldMode(k.mode)) continue;
                const float duration = static_cast<float>(inst.cachedModel->data.sequences[k.sequence].duration);
                k.time += deltaTime * 1000.0f * k.rate;
                if (k.once && k.time >= duration) {
                    // A one-shot ended on a bone of its own: a seated
                    // passenger's upper body has passed its start (0x0073bbd0
                    // -> 0x007484e0).
                    if (k.keyBoneId == inst.seatUpperBone) inst.seatUpperStartPlayed = true;
                    if (k.thenSequence >= 0) {
                        k.sequence = std::exchange(k.thenSequence, -1);
                        k.animationId = k.thenAnimationId;
                        k.mode = k.thenMode;
                        k.time = startTime(inst.cachedModel->data, k.sequence, k.mode);
                        k.once = false;
                        k.rate = 1.0f;
                        continue;
                    }
                    k.time = duration;
                    // 0x0073bbd0: the precast's start, or a PlayImpact
                    // kit's animation, is over.
                    if (static_cast<int32_t>(k.animationId) == inst.precastStart) inst.precastStart = -1;
                    if (static_cast<int32_t>(k.animationId) == inst.kitAnimationHeld) inst.kitAnimationHeld = -1;
                    if (unitUpperBone >= 0 && k.keyBoneId == unitUpperBone && !k.vehicle) {
                        upperEnded = true;
                        continue;
                    }
                    // Back to following the body, blended (0x00832840).
                    returnKeyBoneState(k, inst.globalSequenceTime, inst.cachedModel->data);
                } else if (duration > 0.0f) {
                    k.time = std::fmod(k.time, duration);
                }
            }
            if (upperEnded) endUpperBodyOneShot(inst);
            if (ended) {
                auto& anims = inst.keyBoneAnimations;
                anims.erase(std::remove_if(anims.begin(), anims.end(), [](const auto& k) { return k.sequence < 0; }),
                            anims.end());
                assignKeyBoneAnimations(inst.cachedModel->data, anims, inst.boneKeyBoneAnimation);
            }
        }

        // Always advance animation time (cheap)
        if (inst.cachedModel && !inst.cachedModel->data.sequences.empty() && !inst.animationFrozen) {
            if (inst.currentSequenceIndex < 0) {
                inst.currentSequenceIndex = 0;
                inst.currentAnimationId = inst.cachedModel->data.sequences[0].id;
            }
            const auto& seq = inst.cachedModel->data.sequences[inst.currentSequenceIndex];
            // The events the animation passes on its way (0x00830fb0): once
            // each, from where the last update left off - or from where a
            // sequence just started - to where this one gets, and on into
            // the next pass where it goes round.
            const float eventsFrom = inst.eventsRestart ? inst.eventsFrom : inst.animationTime;
            inst.eventsRestart = false;
            const auto stepEvents = [&](int sequence, float from, float to) {
                stepInstanceEvents(pair.first, inst, sequence, from, to);
            };
            const int eventsSequence = inst.currentSequenceIndex;
            // A movement sequence plays at the unit's speed over its own
            // movingSpeed, so the feet keep to the ground (FUN_007385c0 sets
            // the rate, FUN_0082f0f0 runs time at it). Recomputed each frame
            // from the current speed, which keeps the sequence's time as
            // FUN_00827000 does when the client changes a playing rate.
            inst.playbackRate = inst.bodyRateSet > 0.0f
                                    ? inst.bodyRateSet
                                    : m2_track::locomotionPlaybackRate(seq.id, seq.movingSpeed, inst.scale,
                                                                       inst.locomotionSpeed);
            // One held where it stands in neither runs nor ends (0x00826b00's
            // speed 0).
            const bool held = heldMode(inst.playMode);
            if (!held) inst.animationTime += deltaTime * 1000.0f * inst.playbackRate;
            const bool ended = !held && seq.duration > 0 && inst.animationTime >= static_cast<float>(seq.duration);
            if (!ended) {
                if (!held) stepEvents(eventsSequence, eventsFrom, inst.animationTime);
            } else {
                stepEvents(eventsSequence, eventsFrom, static_cast<float>(seq.duration));
                const auto& seqs = inst.cachedModel->data.sequences;
                const int primary = inst.primarySequenceIndex;
                const bool hasVariations = inst.animationLoop && primary >= 0 &&
                    static_cast<size_t>(primary) < seqs.size() &&
                    seqs[primary].nextAnimation >= 0;
                if (hasVariations) {
                    // FUN_00831fc0: a looping animation with variations re-rolls
                    // at the end of every pass - Stand -> a fidget -> Stand.
                    const int oldSeq = inst.currentSequenceIndex;
                    const float oldDur = static_cast<float>(seq.duration);
                    float carry = inst.animationTime - oldDur;
                    const int next = m2_track::pickSequenceVariation(
                        seqs, primary, static_cast<uint32_t>(std::rand()) & 0x7FFFu);
                    if (next != oldSeq) {
                        m2_track::beginSequenceBlend(
                            inst.sequenceBlend, inst.globalSequenceTime, oldSeq, oldDur,
                            /*currentLoops=*/false, /*currentFinished=*/false, next,
                            seqs[next].blendTime, seqs, inst.playbackRate);
                        inst.currentSequenceIndex = next;
                    }
                    const float newDur = static_cast<float>(seqs[inst.currentSequenceIndex].duration);
                    inst.animationTime = newDur > 0.0f ? std::fmod(std::max(0.0f, carry), newDur) : 0.0f;
                    stepEvents(inst.currentSequenceIndex, -1.0f, inst.animationTime);
                } else if (inst.animationLoop) {
                    // Subtract duration instead of fmod to preserve float precision
                    // fmod() loses precision with large animationTime values
                    inst.animationTime -= static_cast<float>(seq.duration);
                    // Clamp to [0, duration) to handle multiple loops in one frame
                    while (inst.animationTime >= static_cast<float>(seq.duration)) {
                        inst.animationTime -= static_cast<float>(seq.duration);
                    }
                    stepEvents(eventsSequence, -1.0f, inst.animationTime);
                } else {
                    // One-shot animation finished: return to the caller-specified
                    // resume anim (NPC state emote), else the loop the unit
                    // rests in (sitting, sleeping...), else Stand, unless dead
                    if (inst.currentAnimationId != anim::DEATH && !inst.holdsLastFrame) {
                        // Seated with its seat's upper ride animation as its
                        // whole stand, it is back in its loop: an ended
                        // one-shot has passed its start (0x007484e0).
                        if (inst.seatStandsWhole) inst.seatUpperStartPlayed = true;
                        const bool unitDriven = inst.unitAnimation && !inst.seatAnimationHeld &&
                                                !inst.seatStandsWhole && !inst.mirrorsSeatParent;
                        // 0x0073bbd0: the precast's start, or a PlayImpact
                        // kit's animation, is over.
                        if (static_cast<int32_t>(inst.currentAnimationId) == inst.precastStart) inst.precastStart = -1;
                        if (static_cast<int32_t>(inst.currentAnimationId) == inst.kitAnimationHeld)
                            inst.kitAnimationHeld = -1;
                        if (unitDriven && inst.oneShotReturnAnim == 0) {
                            // 0x0073b510 with nothing of its own to follow
                            // it: the base animation (0x0073ac30(0x10) ->
                            // 0x00724500), asked of the body alone. Where
                            // that leaves what plays, it stays as it ended.
                            // What the ended one asks of its own first: a
                            // stand state's loop or getting up, a hold.
                            const auto own = base_animation::afterOneShot(anim::animationData(), inst.unit,
                                                                          inst.currentAnimationId);
                            const base_animation::Pick p =
                                own ? *own
                                    : pickBaseAnimation(inst, base_animation::Ask{.free = base_animation::kBodyFree,
                                                                                  .flags = 0x10,
                                                                                  .queuedAttack = inst.queuedAttack});
                            if (!own) inst.standStateSeen = inst.unit.standState;
                            if (p.anim == base_animation::kKeep) {
                                inst.animationTime = static_cast<float>(seq.duration);
                            } else {
                                // Kit Flags 0x4 and 0x8 leave the tier be.
                                const uint32_t shown =
                                    (p.flags & 0x6) != 0
                                        ? p.anim
                                        : tierAnimationFor(inst.cachedModel->data, p.anim, inst.animationTier);
                                playUnitAnimation(pair.first, shown, p.loop, 0, upper_body::Pass::BodyEnded);
                            }
                        } else {
                            const uint32_t resumeAnim =
                                inst.oneShotReturnAnim != 0 ? inst.oneShotReturnAnim
                                : inst.seatStandsWhole && inst.seatUpperLoop ? *inst.seatUpperLoop
                                : inst.restAnimation != 0  ? inst.restAnimation
                                                           : anim::STAND;
                            // Asked again as any other is (0x0073b510 ->
                            // 0x007385c0 -> 0x007176f0).
                            const uint32_t resume =
                                tierAnimationFor(inst.cachedModel->data, resumeAnim, inst.animationTier);
                            if (unitDriven) {
                                playUnitAnimation(pair.first, resume, true, 0, upper_body::Pass::BodyEnded);
                            } else {
                                playAnimationNow(pair.first, resume, true, 0);
                            }
                        }
                    } else {
                        // Stay on last frame of death, or a mount
                        // transition's arrived rider's (0x0073bbd0)
                        inst.animationTime = static_cast<float>(seq.duration);
                    }
                }
            }
        }

        // Bones every frame, at any distance: the client steps a visible
        // model each frame. Every second or fourth frame past 45 and 90 yards
        // made creature locomotion visibly step.
        if (viewProjection && !inst.isSceneModel && distSq > kAlwaysPoseRadiusSq) {
            const float radius = (inst.cachedModel ? std::max(inst.cachedModel->data.boundRadius, 1.0f) : 2.0f) *
                                 std::max(inst.scale, 0.01f);
            if (!viewFrustum.intersectsSphere(inst.position, radius + kOffscreenMarginYards)) continue;
        }
        toUpdate_.push_back(std::ref(inst));
    }

    const size_t updatedCount = toUpdate_.size();

    // Thread bone matrix computation in chunks
    if (updatedCount >= 8 && numAnimThreads_ > 1) {
        static const size_t minAnimWorkPerThread = std::max<size_t>(
            8, envSizeOrDefault("WOWEE_CHAR_ANIM_WORK_PER_THREAD", 16));
        const size_t maxUsefulThreads = std::max<size_t>(
            1, (updatedCount + minAnimWorkPerThread - 1) / minAnimWorkPerThread);
        const size_t numThreads = std::min(static_cast<size_t>(numAnimThreads_), maxUsefulThreads);

        if (numThreads <= 1) {
            for (auto& instRef : toUpdate_) {
                calculateBoneMatrices(instRef.get());
            }
        } else {
            const size_t chunkSize = updatedCount / numThreads;
            const size_t remainder = updatedCount % numThreads;

            auto processRange = [this](size_t begin, size_t end) {
                for (size_t i = begin; i < end; i++) {
                    calculateBoneMatrices(toUpdate_[i].get());
                }
            };

            animFutures_.clear();
            if (animFutures_.capacity() < numThreads) {
                animFutures_.reserve(numThreads);
            }

            // Dispatch all but the last chunk to the shared pool; process the
            // last chunk on this thread so the pool is never a hard dependency
            // for finishing this frame's bone work.
            size_t start = 0;
            for (size_t t = 0; t + 1 < numThreads; t++) {
                size_t end = start + chunkSize + (t < remainder ? 1 : 0);
                animFutures_.push_back(core::ThreadPool::frameWorkers().submit(
                    [processRange, start, end]() { processRange(start, end); }));
                start = end;
            }
            processRange(start, updatedCount);

            for (auto& f : animFutures_) {
                f.get();
            }
        }
    } else {
        for (auto& instRef : toUpdate_) {
            calculateBoneMatrices(instRef.get());
        }
    }

    // Passengers on their vehicles' seats, then what they hold.
    placeSeatedInstances();

    // This update's events, where they are now the bones are posed: on the
    // event's bone, then in the world (0x004c21b0 with the bone's matrix and
    // the model's). Held for whoever plays them, and dropped if nobody does.
    if (firedEvents_.size() > 4096) firedEvents_.clear();
    for (const SteppedEvent& stepped : steppedEvents_) {
        const auto it = instances.find(stepped.instanceId);
        if (it == instances.end() || !it->second.cachedModel) continue;
        const CharacterInstance& inst = it->second;
        const auto& events = inst.cachedModel->data.events;
        if (stepped.eventIndex >= events.size()) continue;
        const auto& event = events[stepped.eventIndex];
        glm::mat4 bone(1.0f);
        if (event.bone < inst.boneMatrices.size()) bone = inst.boneMatrices[event.bone];
        const glm::mat4 modelMat = inst.hasOverrideModelMatrix ? inst.overrideModelMatrix : getModelMatrix(inst);
        firedEvents_.push_back({.instanceId = stepped.instanceId,
                                .id = event.id,
                                .data = event.data,
                                .position = glm::vec3(modelMat * bone * glm::vec4(event.position, 1.0f))});
    }
    steppedEvents_.clear();

    // Update weapon attachment transforms (after all bone matrices are computed)
    for (auto& pair : instances) {
        auto& instance = pair.second;
        if (instance.weaponAttachments.empty()) continue;
        if (glm::distance2(instance.position, cameraPos) > animUpdateRadiusSq) continue;

        glm::mat4 charModelMat = instance.hasOverrideModelMatrix
            ? instance.overrideModelMatrix
            : getModelMatrix(instance);

        for (auto& wa : instance.weaponAttachments) {
            auto weapIt = instances.find(wa.weaponInstanceId);
            if (weapIt == instances.end()) continue;

            // Get the bone matrix for the attachment bone
            glm::mat4 boneMat(1.0f);
            if (wa.boneIndex < instance.boneMatrices.size()) {
                boneMat = instance.boneMatrices[wa.boneIndex];
            }

            // Weapon model matrix = character model * bone transform * attachment
            // offset * item/sheath orientation.
            glm::mat4 weaponMat =
                charModelMat * boneMat * glm::translate(glm::mat4(1.0f), wa.offset) *
                wa.localTransform;

            // Back-sheathed weapons: the swinging left arm passes through the
            // canted blade while running. Sample the whole arm - shoulder,
            // elbow, hand attachment points plus segment midpoints - against
            // the weapon model's AABB and ease the blade outward, away from
            // the spine, so the arm pushes it instead of clipping. A single
            // sphere at the elbow joint missed forearm/upper-arm contact.
            constexpr uint32_t kAttachmentBack = 12;
            if (wa.attachmentId == kAttachmentBack && weapIt->second.cachedModel) {
                constexpr uint32_t kAttachmentHandLeft = 2;
                constexpr uint32_t kAttachmentElbowLeft = 4;
                constexpr uint32_t kAttachmentShoulderLeft = 6;
                constexpr float kArmRadius = 0.16f;      // arm mesh thickness around the joint chain
                constexpr float kPushClearance = 0.03f;  // keep the blade slightly off the sleeve
                constexpr float kMaxPush = 0.35f;

                const glm::mat4 weaponInv = glm::inverse(weaponMat);
                const auto& wm = weapIt->second.cachedModel->data;

                glm::vec3 joints[3];
                int jointCount = 0;
                for (uint32_t attId : {kAttachmentShoulderLeft, kAttachmentElbowLeft, kAttachmentHandLeft}) {
                    glm::mat4 m;
                    if (getAttachmentTransform(pair.first, attId, m)) {
                        joints[jointCount++] = glm::vec3(m[3]);
                    }
                }

                float targetPush = 0.0f;
                auto testPoint = [&](const glm::vec3& worldPt) {
                    const glm::vec3 local = glm::vec3(weaponInv * glm::vec4(worldPt, 1.0f));
                    const glm::vec3 closest = glm::clamp(local, wm.boundMin, wm.boundMax);
                    const float pen = kArmRadius - glm::distance(local, closest);
                    if (pen > 0.0f) targetPush = std::max(targetPush, pen + kPushClearance);
                };
                for (int j = 0; j < jointCount; ++j) {
                    testPoint(joints[j]);
                    if (j + 1 < jointCount) testPoint((joints[j] + joints[j + 1]) * 0.5f);
                }
                targetPush = std::min(targetPush, kMaxPush);
                if (targetPush > 0.0f && wa.sheathPush < 0.01f) {
                    core::Logger::getInstance().debug("Sheath push engaged: pen=", targetPush,
                                                      " joints=", jointCount);
                }

                wa.sheathPush += (targetPush - wa.sheathPush) * std::min(1.0f, deltaTime * 14.0f);
                if (wa.sheathPush > 0.001f) {
                    // Horizontal direction from the spine out through the weapon.
                    glm::vec3 outward = glm::vec3(weaponMat[3]) - glm::vec3(charModelMat[3]);
                    outward.z = 0.0f;
                    const float len = glm::length(outward);
                    if (len > 0.001f) {
                        weaponMat = glm::translate(glm::mat4(1.0f), (outward / len) * wa.sheathPush) * weaponMat;
                    }
                }
            }
            weapIt->second.overrideModelMatrix = weaponMat;
            weapIt->second.hasOverrideModelMatrix = true;

            // Enchant visuals ride the weapon, offset to their attachment point on it.
            for (const auto& fx : wa.effects) {
                auto fxIt = instances.find(fx.effectInstanceId);
                if (fxIt == instances.end()) continue;
                fxIt->second.overrideModelMatrix =
                    weaponMat * glm::translate(glm::mat4(1.0f), fx.offset);
                fxIt->second.hasOverrideModelMatrix = true;
                // Keep the effect near the character so animation distance culling
                // (which works off instance position) treats it like its wielder.
                fxIt->second.position = instance.position;
            }
        }
    }
}
// --- Bone transform calculation ---

constexpr int32_t kKeyBoneShoulderL = 2;
constexpr int32_t kKeyBoneShoulderR = 3;
constexpr int32_t kKeyBoneSpineLow = 4;
constexpr int32_t kKeyBoneHead = 6;

bool CharacterRenderer::playArmAnimation(uint32_t instanceId, int arm, uint32_t animationId, uint32_t eventId) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    // A mirroring passenger plays its vehicle's, not its own (0x00735820).
    if (it->second.mirrorsSeatParent) return false;
    const bool played = playArmAnimationNow(instanceId, arm, animationId, eventId);
    for (uint32_t passenger : mirroringPassengers(instanceId)) {
        playArmAnimationNow(passenger, arm, animationId, eventId);
    }
    return played;
}

bool CharacterRenderer::playArmAnimationNow(uint32_t instanceId, int arm, uint32_t animationId, uint32_t eventId) {
    auto it = instances.find(instanceId);
    if (it == instances.end() || arm < 0 || arm > 1) return false;
    auto& instance = it->second;
    const auto& model = models[instance.modelId].data;
    if (!hasKeyBone(instanceId, arm == 0 ? kKeyBoneShoulderL : kKeyBoneShoulderR)) return false;
    // The arm's sequence for it, or the stand-in along its Fallbacks
    // (0x00735820 -> 0x00832ab0 -> 0x00826350, for a bone as for the body).
    const anim::ModelAnimation shown = shownAnimation(model, animationId);
    const int sequence = animationSequenceIndex(model, shown.id);
    if (sequence < 0) return false;

    // Which arm each bone is in - a shoulder and everything below it. Parents
    // come first, so one pass finds both.
    if (instance.boneArm.size() != model.bones.size()) {
        instance.boneArm.assign(model.bones.size(), -1);
        for (size_t i = 0; i < model.bones.size(); i++) {
            const auto& bone = model.bones[i];
            instance.boneArm[i] = bone.keyBoneId == kKeyBoneShoulderL ? 0
                                : bone.keyBoneId == kKeyBoneShoulderR ? 1
                                : bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) < i
                                    ? instance.boneArm[bone.parentBone] : -1;
        }
    }
    instance.armSequenceIndex[arm] = sequence;
    instance.armAnimationId[arm] = animationId;
    instance.armMode[arm] = shown.mode;
    instance.armTime[arm] = startTime(model, sequence, shown.mode);
    instance.armEventTime[arm] = -1.0f;
    if (const auto* event = model.findEvent(eventId);
        event && static_cast<size_t>(sequence) < event->times.size() && !event->times[sequence].empty()) {
        // Where its time reaches the event: from the end, reversed.
        instance.armEventTime[arm] = sampledTime(model, sequence, static_cast<float>(event->times[sequence].front()),
                                                 shown.mode);
    }
    instance.armEvents &= static_cast<uint8_t>(~((arm == 0 ? kArmEventLeft : kArmEventRight) |
                                                  (arm == 0 ? kArmEndLeft : kArmEndRight)));
    return true;
}

void CharacterRenderer::stopArmAnimations(uint32_t instanceId) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    // 0x00736d30 and 0x00735a60: the vehicle's shoulders go back to the body
    // and its mirroring passengers' with them; a passenger's own do not.
    if (it->second.mirrorsSeatParent) return;
    auto stop = [](CharacterInstance& instance) {
        instance.armSequenceIndex[0] = instance.armSequenceIndex[1] = -1;
        instance.armEvents = 0;
    };
    stop(it->second);
    for (uint32_t passenger : mirroringPassengers(instanceId)) {
        auto pit = instances.find(passenger);
        if (pit != instances.end()) stop(pit->second);
    }
}

void CharacterRenderer::setInstanceSeat(uint32_t instanceId, uint32_t parentInstanceId, int32_t attachmentId,
                                        const vehicle_seat::PassengerPose& pose) {
    auto it = instances.find(instanceId);
    if (it == instances.end() || parentInstanceId == instanceId) return;
    auto& instance = it->second;
    if (instance.seatParent != parentInstanceId) instance.mirrorsSeatParent = false;
    instance.seatParent = parentInstanceId;
    instance.seatAttachment = attachmentId;
    instance.seatPose = pose;
}

void CharacterRenderer::clearInstanceSeat(uint32_t instanceId) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    auto& instance = it->second;
    if (instance.seatParent == 0) return;
    instance.seatParent = 0;
    instance.seatAttachment = -1;
    instance.seatAnimationHeld = false;
    instance.mirrorsSeatParent = false;
    instance.hasOverrideModelMatrix = instance.seatFlight.has_value();
    releaseSeatedUpperBody(instanceId);
}

void CharacterRenderer::releaseSeatedUpperBody(uint32_t instanceId) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    auto& instance = it->second;
    const int32_t bone = std::exchange(instance.seatUpperBone, vehicle_seat::kNoUpperBody);
    instance.seatUpperStart.reset();
    instance.seatUpperLoop.reset();
    instance.seatUpperStartPlayed = false;
    instance.seatStandsWhole = false;
    instance.seatRideSeated = false;
    if (bone != vehicle_seat::kNoUpperBody) stopKeyBoneAnimation(instanceId, bone);
}

void CharacterRenderer::standSeatedWhole(uint32_t instanceId) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    auto& instance = it->second;
    const bool start = instance.seatUpperStart && !instance.seatUpperStartPlayed;
    const uint32_t current = instance.currentAnimationId;
    if ((instance.seatUpperStart && current == *instance.seatUpperStart) ||
        (instance.seatUpperLoop && current == *instance.seatUpperLoop))
        return;
    if (start) {
        playAnimationNow(instanceId, *instance.seatUpperStart, false, instance.seatUpperLoop.value_or(0));
    } else if (instance.seatUpperLoop) {
        playAnimationNow(instanceId, *instance.seatUpperLoop, true, 0);
    }
}

void CharacterRenderer::standSeatedUpperBody(uint32_t instanceId) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    auto& instance = it->second;
    const int32_t bone = instance.seatUpperBone;
    if (bone == vehicle_seat::kNoUpperBody) return;
    // 0x007485b0: RideUpperAnimStart until a one-shot of its own has ended
    // there, then RideUpperAnimLoop - kept where it plays already.
    const auto& anims = instance.keyBoneAnimations;
    const auto playing =
        std::find_if(anims.begin(), anims.end(), [&](const auto& k) { return k.keyBoneId == bone; });
    const bool start = instance.seatUpperStart && !instance.seatUpperStartPlayed;
    if (start || instance.seatUpperLoop) {
        if (playing != anims.end() &&
            ((instance.seatUpperStart && playing->animationId == *instance.seatUpperStart) ||
             (instance.seatUpperLoop && playing->animationId == *instance.seatUpperLoop)))
            return;
        if (start && playKeyBoneAnimation(instanceId, bone, *instance.seatUpperStart, true, instance.seatUpperLoop))
            return;
        if (instance.seatUpperLoop && playKeyBoneAnimation(instanceId, bone, *instance.seatUpperLoop)) return;
    }
    // None: the upper body follows the body's ride animation.
    stopKeyBoneAnimation(instanceId, bone);
}

void CharacterRenderer::setSeatAnimation(uint32_t instanceId,
                                         const std::optional<vehicle_seat::RideAnimation>& ride) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    auto& instance = it->second;
    releaseSeatedUpperBody(instanceId);
    instance.seatAnimationHeld = false;
    if (!ride) return;
    // What the seat has it stand as is asked of it as anything is
    // (0x00724500 -> 0x007485b0, then 0x007385c0 -> 0x007176f0): the
    // animation of its tier the model has. One that comes out a stand
    // (BehaviorID 0) is none - 0x007385c0 gives a standing upper body the
    // body's.
    const auto& model = models[instance.modelId].data;
    const auto asked = [&](std::optional<uint32_t> a) -> std::optional<uint32_t> {
        if (!a) return std::nullopt;
        const uint32_t id = tierAnimationFor(model, *a, instance.animationTier);
        const auto has = [&model](uint32_t x) { return modelHasAnimation(model, x); };
        if (anim::animationData().behavior(id) == 0 || !anim::modelPlays(anim::animationData(), id, has))
            return std::nullopt;
        return id;
    };
    if (!ride->start && !ride->loop) {
        // No ride animation: seated, the whole unit stands as the seat's
        // upper ride animation (0x00724500 -> 0x007485b0), and plays what
        // else it is asked as itself.
        if (!ride->seated) return;
        instance.seatUpperStart = asked(ride->upperStart);
        instance.seatUpperLoop = asked(ride->upperLoop);
        instance.seatStandsWhole = true;
        standSeatedWhole(instanceId);
        return;
    }
    // 0x00747b20: the start until it has played once (the passenger's +0x10
    // 0x2, set as it ends, 0x007484e0), then the loop.
    if (ride->start) {
        startAnimation(instance, *ride->start, false, ride->loop.value_or(0));
    } else {
        startAnimation(instance, *ride->loop, true, 0);
    }
    instance.seatAnimationHeld = ride->loop.has_value();
    if (!instance.seatAnimationHeld || !ride->seated) return;
    instance.seatRideSeated = true;
    // The upper body's own (+0xb84, 0x0073e840) and what the seat has it
    // stand as there.
    instance.seatUpperBone = vehicle_seat::upperKeyBone(hasKeyBone(instanceId, 4), hasKeyBone(instanceId, 6));
    instance.seatUpperStart = asked(ride->upperStart);
    instance.seatUpperLoop = asked(ride->upperLoop);
    standSeatedUpperBody(instanceId);
}

void CharacterRenderer::setMirrorsSeatParent(uint32_t instanceId, bool mirrors) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    auto& instance = it->second;
    if (instance.mirrorsSeatParent == mirrors) return;
    instance.mirrorsSeatParent = false;
    if (!mirrors) return;
    auto pit = instances.find(instance.seatParent);
    if (pit == instances.end()) return;
    const auto& vehicle = pit->second;
    // 0x00748620: the vehicle's body and each bone set it animates, at the
    // time it has reached; a set the vehicle leaves to the body, the
    // passenger does too.
    startAnimation(instance, vehicle.currentAnimationId, vehicle.animationLoop, vehicle.oneShotReturnAnim);
    instance.animationTime = vehicle.animationTime;
    instance.eventsFrom = instance.animationTime;
    for (int arm = 0; arm < 2; ++arm) {
        instance.armSequenceIndex[arm] = -1;
        if (vehicle.armSequenceIndex[arm] < 0) continue;
        if (playArmAnimationNow(instanceId, arm, vehicle.armAnimationId[arm], 0)) {
            instance.armTime[arm] = vehicle.armTime[arm];
        }
    }
    instance.mirrorsSeatParent = true;
}

std::optional<glm::vec3> CharacterRenderer::modelAttachmentPosition(uint32_t instanceId,
                                                                    uint32_t attachmentId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return std::nullopt;
    uint16_t bone = 0;
    glm::vec3 offset(0.0f);
    if (!findAttachmentBone(it->second.modelId, attachmentId, bone, offset)) return std::nullopt;
    return offset;
}

std::optional<glm::mat4> CharacterRenderer::seatWorld(const CharacterInstance& inst) const {
    if (inst.seatParent == 0) return std::nullopt;
    auto pit = instances.find(inst.seatParent);
    if (pit == instances.end()) return std::nullopt;
    // The vehicle model's attachment for the seat (0x00831410), else
    // its own matrix (0x007490f0 without the attachment).
    glm::mat4 frame(1.0f);
    if (inst.seatAttachment < 0 ||
        !getAttachmentTransform(inst.seatParent, static_cast<uint32_t>(inst.seatAttachment), frame)) {
        frame = pit->second.hasOverrideModelMatrix ? pit->second.overrideModelMatrix : getModelMatrix(pit->second);
    }
    return vehicle_seat::passengerWorld(frame, inst.seatPose);
}

void CharacterRenderer::placeSeatedInstances() {
    for (auto& [id, inst] : instances) {
        if (inst.seatParent == 0 && !inst.seatFlight) continue;
        if (inst.seatParent != 0 && instances.find(inst.seatParent) == instances.end()) {
            inst.seatParent = 0;
            inst.seatAnimationHeld = false;
            inst.mirrorsSeatParent = false;
            inst.hasOverrideModelMatrix = false;
            releaseSeatedUpperBody(id);
            if (!inst.seatFlight) continue;
        }
        const std::optional<glm::mat4> seat = seatWorld(inst);
        if (inst.seatFlight) {
            // On its way (0x0074a7f0 states 1, 2, 4, 5): toward its end, or
            // its seat as it is now (0x007493b0).
            const glm::vec3 target = inst.seatFlight->to ? *inst.seatFlight->to
                                     : seat             ? glm::vec3((*seat)[3])
                                                        : inst.seatFlight->from;
            inst.overrideModelMatrix = vehicle_seat::flightWorld(*inst.seatFlight, target, inst.scale);
        } else if (seat) {
            inst.overrideModelMatrix = *seat;
        } else {
            continue;
        }
        inst.hasOverrideModelMatrix = true;
        inst.position = glm::vec3(inst.overrideModelMatrix[3]);
    }
}

void CharacterRenderer::setInstanceSeatFlight(uint32_t instanceId, const std::optional<vehicle_seat::Flight>& flight) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    auto& inst = it->second;
    const bool had = inst.seatFlight.has_value();
    inst.seatFlight = flight;
    if (had && !flight && inst.seatParent == 0) inst.hasOverrideModelMatrix = false;
}

std::optional<glm::mat4> CharacterRenderer::seatTargetMatrix(uint32_t instanceId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return std::nullopt;
    return seatWorld(it->second);
}


bool CharacterRenderer::playKeyBoneAnimation(uint32_t instanceId, int32_t keyBoneId, uint32_t animationId,
                                             bool once, std::optional<uint32_t> then) {
    auto it = instances.find(instanceId);
    if (it == instances.end() || keyBoneId < 0 || !hasKeyBone(instanceId, keyBoneId)) return false;
    return startKeyBoneAnimation(it->second, keyBoneId, animationId, once, then, 0.0f, false);
}

bool CharacterRenderer::startKeyBoneAnimation(CharacterInstance& instance, int32_t keyBoneId, uint32_t animationId,
                                              bool once, std::optional<uint32_t> then, float startMs, bool blend) {
    const auto& model = models[instance.modelId].data;
    // The bone's sequence for it, or the stand-in along its Fallbacks
    // (0x00832ab0 -> 0x00826350, for a bone as for the body).
    const auto sequenceOf = [&model](uint32_t id) {
        const anim::ModelAnimation shown = shownAnimation(model, id);
        return std::pair{animationSequenceIndex(model, shown.id), shown.mode};
    };
    const auto [sequence, mode] = sequenceOf(animationId);
    if (sequence < 0) return false;
    auto& anims = instance.keyBoneAnimations;
    auto a = std::find_if(anims.begin(), anims.end(), [&](const auto& k) { return k.keyBoneId == keyBoneId; });
    // What the bone set shows now - its own, else the body's - which the
    // new one blends in from over its blendTime (0x00826c40).
    m2_track::SequenceBlend from;
    if (blend) {
        const bool own = a != anims.end() && a->sequence >= 0 && !a->returning;
        if (a != anims.end()) from = a->blend;
        if (own) {
            m2_track::beginSequenceBlend(from, instance.globalSequenceTime, a->sequence,
                                         sampledTime(model, a->sequence, a->time, a->mode), !a->once, false, sequence,
                                         model.sequences[static_cast<size_t>(sequence)].blendTime, model.sequences);
        } else {
            m2_track::beginSequenceBlend(
                from, instance.globalSequenceTime, instance.currentSequenceIndex,
                sampledTime(model, instance.currentSequenceIndex, instance.animationTime, instance.playMode),
                instance.animationLoop, false, sequence, model.sequences[static_cast<size_t>(sequence)].blendTime,
                model.sequences, instance.playbackRate);
        }
    }
    if (a == anims.end()) a = anims.insert(anims.end(), CharacterInstance::KeyBoneAnimation{});
    const float duration = static_cast<float>(model.sequences[static_cast<size_t>(sequence)].duration);
    const float time = startMs > 0.0f ? std::min(startMs, duration) : startTime(model, sequence, mode);
    *a = CharacterInstance::KeyBoneAnimation{keyBoneId, animationId, sequence, time, once};
    a->mode = mode;
    a->blend = from;
    if (once && then) {
        std::tie(a->thenSequence, a->thenMode) = sequenceOf(*then);
        a->thenAnimationId = *then;
    }
    assignKeyBoneAnimations(model, anims, instance.boneKeyBoneAnimation);
    return true;
}

void CharacterRenderer::returnKeyBone(CharacterInstance& instance, int32_t keyBoneId) {
    auto& anims = instance.keyBoneAnimations;
    auto a = std::find_if(anims.begin(), anims.end(), [&](const auto& k) { return k.keyBoneId == keyBoneId; });
    if (a == anims.end() || a->sequence < 0 || a->returning) return;
    returnKeyBoneState(*a, instance.globalSequenceTime, models[instance.modelId].data);
}

bool CharacterRenderer::playVehicleKeyBoneAnimation(uint32_t instanceId, int32_t keyBoneId, uint32_t animationId,
                                                    bool once) {
    if (!playKeyBoneAnimation(instanceId, keyBoneId, animationId, once)) return false;
    for (auto& k : instances[instanceId].keyBoneAnimations) {
        if (k.keyBoneId == keyBoneId) k.vehicle = true;
    }
    return true;
}

bool CharacterRenderer::stopKeyBoneAnimation(uint32_t instanceId, int32_t keyBoneId) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto& instance = it->second;
    auto& anims = instance.keyBoneAnimations;
    auto a = std::find_if(anims.begin(), anims.end(), [&](const auto& k) { return k.keyBoneId == keyBoneId; });
    if (a == anims.end()) return false;
    anims.erase(a);
    assignKeyBoneAnimations(models[instance.modelId].data, anims, instance.boneKeyBoneAnimation);
    return true;
}

std::optional<float> CharacterRenderer::keyBoneAnimationTime(uint32_t instanceId, int32_t keyBoneId,
                                                           uint32_t animationId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return std::nullopt;
    for (const auto& k : it->second.keyBoneAnimations) {
        if (k.keyBoneId == keyBoneId && k.animationId == animationId) return k.time;
    }
    return std::nullopt;
}

std::optional<uint32_t> CharacterRenderer::armAnimation(uint32_t instanceId, int arm) const {
    auto it = instances.find(instanceId);
    if (it == instances.end() || arm < 0 || arm > 1 || it->second.armSequenceIndex[arm] < 0) return std::nullopt;
    return it->second.armAnimationId[arm];
}

bool CharacterRenderer::hasKeyBone(uint32_t instanceId, int32_t keyBoneId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto modelIt = models.find(it->second.modelId);
    if (modelIt == models.end()) return false;
    for (const auto& bone : modelIt->second.data.bones) {
        if (bone.keyBoneId == keyBoneId) return true;
    }
    return false;
}

std::optional<CharacterRenderer::HeldModelEvent> CharacterRenderer::heldModelEvent(
    uint32_t instanceId, uint32_t attachmentId, uint32_t eventId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return std::nullopt;
    for (const auto& held : it->second.weaponAttachments) {
        if (held.attachmentId != attachmentId) continue;
        auto weaponIt = instances.find(held.weaponInstanceId);
        if (weaponIt == instances.end()) return std::nullopt;
        const auto& weapon = weaponIt->second;
        auto modelIt = models.find(weapon.modelId);
        if (modelIt == models.end()) return std::nullopt;
        const auto* event = modelIt->second.data.findEvent(eventId);
        if (!event) return std::nullopt;
        // 0x008317e0: the event's position through its bone, then the
        // model's own placement.
        glm::vec4 p(event->position, 1.0f);
        if (event->bone < weapon.boneMatrices.size()) p = weapon.boneMatrices[event->bone] * p;
        const glm::mat4 placement = weapon.hasOverrideModelMatrix ? weapon.overrideModelMatrix
                                                                  : glm::translate(glm::mat4(1.0f), weapon.position);
        return HeldModelEvent{.position = glm::vec3(placement * p), .ambient = glm::vec3(weapon.drawAmbient)};
    }
    return std::nullopt;
}

uint8_t CharacterRenderer::takeArmAnimationEvents(uint32_t instanceId) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return 0;
    const uint8_t events = it->second.armEvents;
    it->second.armEvents = 0;
    return events;
}

void CharacterRenderer::calculateBoneMatrices(CharacterInstance& instance) {
    if (!instance.cachedModel) return;
    auto& model = instance.cachedModel->data;

    if (model.bones.empty()) {
        instance.posedOnce = true;  // nothing to pose: as it is, it is right
        return;
    }

    size_t numBones = model.bones.size();
    instance.boneMatrices.resize(numBones);

    const auto& gsd = model.globalSequenceDurations;

    // One-time diagnostic: check bone ordering (parents must precede children)
    static bool checkedBoneOrder = false;
    if (!checkedBoneOrder) {
        checkedBoneOrder = true;
        for (size_t i = 0; i < numBones; i++) {
            const auto& bone = model.bones[i];
            if (bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) >= i) {
                LOG_WARNING("Bone ", i, " references parent ", bone.parentBone,
                            " which comes AFTER it - will use stale matrix!");
            }
        }
    }

    // How many more calls the extreme-translation probe below runs for.
    // Declared here rather than inside the loop so it can be advanced once per
    // call, after it, instead of on a bone the loop happens to reach.
    static int diagFrames = 0;

    const bool armsOwnSequences = (instance.armSequenceIndex[0] >= 0 || instance.armSequenceIndex[1] >= 0) &&
                                  instance.boneArm.size() == numBones;

    // The pose the body is blending out of, the same for every bone: the
    // client sets the state on the root and the rest inherit it (FUN_0082f0f0).
    const float bodyTime =
        sampledTime(model, instance.currentSequenceIndex, instance.animationTime, instance.playMode);
    const m2_track::BlendSample bodyBlend = m2_track::currentBlend(
        instance.sequenceBlend, instance.globalSequenceTime, model.sequences,
        instance.currentSequenceIndex, bodyTime);

    for (size_t i = 0; i < numBones; i++) {
        const auto& bone = model.bones[i];

        int sequence = instance.currentSequenceIndex;
        float time = bodyTime;
        m2_track::BlendSample blend = bodyBlend;
        const int arm = armsOwnSequences ? instance.boneArm[i] : -1;
        const int keyAnim = instance.boneKeyBoneAnimation.size() == numBones ? instance.boneKeyBoneAnimation[i] : -1;
        if (arm >= 0 && instance.armSequenceIndex[arm] >= 0) {
            // An arm on a sequence of its own does not take the body's old pose.
            blend = {};
            sequence = instance.armSequenceIndex[arm];
            time = sampledTime(model, sequence,
                               std::min(instance.armTime[arm], static_cast<float>(model.sequences[sequence].duration)),
                               instance.armMode[arm]);
        } else if (keyAnim >= 0 && static_cast<size_t>(keyAnim) < instance.keyBoneAnimations.size() &&
                   instance.keyBoneAnimations[keyAnim].sequence >= 0) {
            blend = {};
            const auto& k = instance.keyBoneAnimations[keyAnim];
            if (k.returning) {
                // The body's, from the bone set's last pose.
                blend = m2_track::currentBlend(k.blend, instance.globalSequenceTime, model.sequences, -1, 0.0f);
            } else {
                sequence = k.sequence;
                time = sampledTime(model, k.sequence, k.time, k.mode);
                blend = m2_track::currentBlend(k.blend, instance.globalSequenceTime, model.sequences, k.sequence, time);
            }
        }

        // Local transform includes pivot bracket: T(pivot)*T*R*S*T(-pivot)
        // At rest this is identity, so no separate bind pose is needed
        glm::mat4 localTransform = getBoneTransform(bone, time, instance.globalSequenceTime,
                                                    sequence, gsd, blend);

        // The body's lag behind the facing, on SpineLow and on Head
        // (FUN_0073dab0's overrides of key bones 4 and 6).
        const float extraYawRad = bone.keyBoneId == kKeyBoneSpineLow ? instance.torsoYawOverrideRad
                                : bone.keyBoneId == kKeyBoneHead     ? instance.headYawOverrideRad
                                                                     : 0.0f;
        if (extraYawRad != 0.0f) {
            glm::mat4 extraYaw = glm::translate(glm::mat4(1.0f), bone.pivot)
                                * glm::rotate(glm::mat4(1.0f), extraYawRad, glm::vec3(0.0f, 0.0f, 1.0f))
                                * glm::translate(glm::mat4(1.0f), -bone.pivot);
            localTransform = extraYaw * localTransform;
        }

        // Compose with parent
        if (bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) < numBones) {
            instance.boneMatrices[i] = instance.boneMatrices[bone.parentBone] * localTransform;
        } else {
            instance.boneMatrices[i] = localTransform;
        }

        // Diagnostic: detect bones with extreme translation. Gated so the abs()
        // probes and the post-loop counter bump only run for the first few frames.
        if (diagFrames < 3) {
            float tx = std::abs(instance.boneMatrices[i][3][0]);
            float ty = std::abs(instance.boneMatrices[i][3][1]);
            float tz = std::abs(instance.boneMatrices[i][3][2]);
            if (tx > 50.0f || ty > 50.0f || tz > 50.0f) {
                LOG_DEBUG("BONE DIAG: bone[", i, "] keyBone=", bone.keyBoneId,
                            " flags=0x", std::hex, bone.flags, std::dec,
                            " parent=", bone.parentBone,
                            " pivot=(", bone.pivot.x, ",", bone.pivot.y, ",", bone.pivot.z, ")",
                            " mat_t=(", instance.boneMatrices[i][3][0], ",",
                            instance.boneMatrices[i][3][1], ",", instance.boneMatrices[i][3][2], ")",
                            " local_t=(", localTransform[3][0], ",", localTransform[3][1], ",",
                            localTransform[3][2], ")",
                            " animTime=", instance.animationTime,
                            " gsTime=", instance.globalSequenceTime,
                            " seqIdx=", instance.currentSequenceIndex);
            }
        }
    }
    // Once per call, not once per last bone. The bump used to sit inside the
    // loop under `i == numBones - 1`, which this loop does reach - but a
    // counter that only advances on a condition inside the block it guards is
    // the shape that had a light-volume diagnostic logging every frame of every
    // session (a5080cec), and one `continue` added above would make this the
    // same fault. tools/bounded_log_check.py reports it.
    if (diagFrames < 3) diagFrames++;
    instance.posedOnce = true;
}

glm::mat4 CharacterRenderer::getBoneTransform(const pipeline::M2Bone& bone, float animTime, float globalSeqTime,
                                               int sequenceIndex, const std::vector<uint32_t>& globalSeqDurations,
                                               const m2_track::BlendSample& blend) {
    // Resolve global sequences: bones with globalSequence >= 0 use sequence 0
    // with time wrapped at the global sequence duration, independent of the
    // character's current animation. Those never blend; the rest move toward
    // the pose of the sequence being left by the blend's weight.
    const m2_track::BoneTRS trs = m2_track::sampleBone(bone, sequenceIndex, animTime, globalSeqTime,
                                                       globalSeqDurations, blend);
    const glm::vec3& translation = trs.translation;
    const glm::quat& rotation = trs.rotation;
    const glm::vec3& scale = trs.scale;

    // M2 bone transform: T(pivot) * T(trans) * R(rot) * S(scale) * T(-pivot).
    // Build directly instead of chaining glm::translate/rotate/scale (each of
    // those is a full mat4 multiply). The composed matrix has:
    //   linear part      = R * diag(scale)     (3 column scales of R)
    //   translation part = pivot + trans - RS * pivot
    glm::mat3 R = glm::mat3_cast(rotation);
    glm::vec3 c0 = R[0] * scale.x;
    glm::vec3 c1 = R[1] * scale.y;
    glm::vec3 c2 = R[2] * scale.z;
    glm::vec3 t  = (bone.pivot + translation)
                   - (c0 * bone.pivot.x + c1 * bone.pivot.y + c2 * bone.pivot.z);
    glm::mat4 transform;
    transform[0] = glm::vec4(c0, 0.0f);
    transform[1] = glm::vec4(c1, 0.0f);
    transform[2] = glm::vec4(c2, 0.0f);
    transform[3] = glm::vec4(t,  1.0f);
    return transform;
}

// --- Rendering ---

void CharacterRenderer::prepareRender(uint32_t frameIndex) {
    // A new frame for the bone ring, whether or not anything draws.
    ++boneFrame_;
    if (instances.empty() || !opaquePipeline_) return;
    if (frameIndex >= 2 || boneDescPool_ == VK_NULL_HANDLE || boneSetLayout_ == VK_NULL_HANDLE) return;

    WOWEE_PROFILE_SCOPE("prepare: character bone ring", Cpu);
    // Pre-allocate bone SSBOs + descriptor sets on main thread (pool ops not
    // thread-safe): the whole ring at once.
    constexpr int kRing = CharacterInstance::kBoneRing;
    const VkDeviceSize bufferSize = MAX_BONES * sizeof(glm::mat4);
    for (auto& [id, instance] : instances) {
        int numBones = std::min(static_cast<int>(instance.boneMatrices.size()), MAX_BONES);
        if (numBones <= 0) continue;
        if (std::all_of(std::begin(instance.boneSet), std::end(instance.boneSet),
                        [](VkDescriptorSet set) { return set != VK_NULL_HANDLE; })) {
            continue;
        }

        bool ok = true;
        for (int k = 0; k < kRing && ok; ++k) {
            if (instance.boneBuffer[k]) continue;
            VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = bufferSize;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo allocInfo{};
            if (vmaCreateBuffer(vkCtx_->getAllocator(), &bci, &aci, &instance.boneBuffer[k],
                                &instance.boneAlloc[k], &allocInfo) != VK_SUCCESS) {
                instance.boneBuffer[k] = VK_NULL_HANDLE;
                instance.boneAlloc[k] = VK_NULL_HANDLE;
                instance.boneMapped[k] = nullptr;
                ok = false;
                break;
            }
            instance.boneMapped[k] = allocInfo.pMappedData;
            instance.boneWritten[k] = 0;
            // Initialize all bone slots to identity so out-of-range indices
            // produce correct (neutral) transforms instead of GPU garbage
            if (instance.boneMapped[k]) {
                auto* dst = static_cast<glm::mat4*>(instance.boneMapped[k]);
                for (int j = 0; j < MAX_BONES; j++) dst[j] = glm::mat4(1.0f);
            }
        }
        for (int k = 0; k < kRing && ok; ++k) {
            if (instance.boneSet[k]) continue;
            VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = boneDescPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &boneSetLayout_;
            VkResult dsRes = vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &instance.boneSet[k]);
            if (dsRes != VK_SUCCESS) {
                LOG_ERROR("CharacterRenderer::prepareRender: bone descriptor alloc failed (instance=",
                          id, ", ring=", k, ", vk=", static_cast<int>(dsRes), ")");
                instance.boneSet[k] = VK_NULL_HANDLE;
                ok = false;
                break;
            }
            const VkDescriptorBufferInfo bufInfo{instance.boneBuffer[k], 0, bufferSize};
            VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = instance.boneSet[k];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &bufInfo;
            vkUpdateDescriptorSets(vkCtx_->getDevice(), 1, &write, 0, nullptr);
        }
        // Drawn only with the whole ring; what failed is tried again next frame.
        if (!ok) destroyInstanceBones(instance, true);
    }
}

void CharacterRenderer::writeFrameBones(CharacterInstance& inst) {
    constexpr int kRing = CharacterInstance::kBoneRing;
    const int k = static_cast<int>(boneFrame_ % kRing);
    const int numBones = std::min(static_cast<int>(inst.boneMatrices.size()), MAX_BONES);
    if (inst.boneMapped[k] && inst.boneWritten[k] != boneFrame_) {
        auto* dst = static_cast<glm::mat4*>(inst.boneMapped[k]);
        if (numBones > 0) memcpy(dst, inst.boneMatrices.data(), numBones * sizeof(glm::mat4));
        inst.boneWritten[k] = boneFrame_;
    }
}

namespace {

// The depth, in yards, within which a character does not receive the sun's
// shadow from what is in front of it (CharPushConstants::lightFlags.z): its
// own diameter, held to these. The floor covers a person and anything
// attached to one (a weapon's own bounds are small, but it sits on a body);
// the ceiling keeps a dragon from ignoring a tree's shadow.
constexpr float kSelfShadowMinExtent = 2.5f;
constexpr float kSelfShadowMaxExtent = 15.0f;

// Default frustum-cull radius when model bounds aren't available.
// 4.0 covers Tauren, mounted characters, and most creature models.
constexpr float kDefaultCharacterCullRadius = 4.0f;

float characterRenderRadiusSq() {
    const float renderRadius = static_cast<float>(envSizeOrDefault("WOWEE_CHAR_RENDER_RADIUS", 130));
    return renderRadius * renderRadius;
}

// Drawn see-through as a whole - the player's body as the camera closes in, a
// unit fading in or out, a ghost: all of it goes with the blended batches, the
// world's last phase, so the depth it writes (for its own parts to sort) hides
// nothing opaque drawn after the characters - a tree trunk behind the body.
bool instanceDrawnFaded(float opacity, float kitAlpha, float cameraAlpha) {
    return opacity * kitAlpha * cameraAlpha < 0.999f;
}

/// Whether any batch of the model is blended (blend mode 2 and up).
template <class Model>
bool modelHasBlendedBatch(const Model& gpuModel) {
    const auto& mats = gpuModel.data.materials;
    return std::any_of(gpuModel.data.batches.begin(), gpuModel.data.batches.end(),
                       [&mats](const pipeline::M2Batch& b) {
                           return b.materialIndex < mats.size() && mats[b.materialIndex].blendMode >= 2;
                       });
}

} // namespace

bool CharacterRenderer::isDrawCandidate(const CharacterInstance& instance, const glm::vec3& camPos,
                                        const Frustum& frustum, float renderRadiusSq) const {
    // Skip invisible instances (e.g., player in first-person mode)
    if (!instance.visible || instance.heldBack) return false;
    // Not before it has been posed once (see posedOnce). An attachment is
    // placed by its parent's bones, not its own animation.
    if (!instance.posedOnce && !instance.hasOverrideModelMatrix) return false;

    // Character instance culling: test both distance and frustum visibility
    if (!instance.hasOverrideModelMatrix && !instance.isSceneModel) {
        const glm::vec3 toInst = instance.position - camPos;
        // Distance cull: skip if beyond render radius
        if (glm::dot(toInst, toInst) > renderRadiusSq) return false;

        // Compute per-instance bounding radius from model data when available.
        float cullRadius = kDefaultCharacterCullRadius;
        auto mIt = models.find(instance.modelId);
        if (mIt != models.end()) {
            const float modelR = mIt->second.data.boundRadius;
            if (modelR > 0.01f)
                cullRadius = std::max(kDefaultCharacterCullRadius, modelR * std::max(0.001f, instance.scale));
        }

        // Frustum cull: skip if outside view frustum
        if (!frustum.intersectsSphere(instance.position, cullRadius)) return false;
    }

    if (!instance.cachedModel) return false;
    // Skip models without GPU buffers
    if (!instance.cachedModel->vertexBuffer) return false;
    // Skip fully transparent instances
    return instance.opacity * instance.kitAlpha * instance.cameraAlpha > 0.0f;
}

std::vector<CharacterRenderer::BlendedDraw> CharacterRenderer::planBlended(const Camera& camera) const {
    std::vector<BlendedDraw> plan;
    if (instances.empty() || !opaquePipeline_) return plan;
    const glm::vec3 camPos = camera.getPosition();
    Frustum frustum;
    frustum.extractFromMatrix(camera.getViewProjectionMatrix());
    const float renderRadiusSq = characterRenderRadiusSq();
    for (const auto& [id, instance] : instances) {
        if (!isDrawCandidate(instance, camPos, frustum, renderRadiusSq)) continue;
        if (!modelHasBlendedBatch(*instance.cachedModel) &&
            !instanceDrawnFaded(instance.opacity, instance.kitAlpha, instance.cameraAlpha)) {
            continue;
        }
        const glm::vec3 at = instance.hasOverrideModelMatrix
            ? glm::vec3(instance.overrideModelMatrix[3]) : instance.position;
        const glm::vec3 d = at - camPos;
        plan.push_back({glm::dot(d, d), id});
    }
    std::sort(plan.begin(), plan.end(),
              [](const BlendedDraw& a, const BlendedDraw& b) { return a.distSq > b.distSq; });
    return plan;
}

void CharacterRenderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera,
                               Phase phase, uint32_t onlyInstance, bool gbuffer) {
    if (instances.empty() || !opaquePipeline_ || (gbuffer && gbufferVariants_.empty())) {
        return;
    }
    std::lock_guard<std::mutex> renderLock(renderMutex_);
    const float renderRadiusSq = characterRenderRadiusSq();
    const glm::vec3 camPos = camera.getPosition();

    // Extract frustum planes for per-instance visibility testing
    Frustum frustum;
    frustum.extractFromMatrix(camera.getViewProjectionMatrix());

    uint32_t frameIndex = vkCtx_->getCurrentFrame();
    uint32_t frameSlot = frameIndex % 2u;

    // Reset material ring buffer and descriptor pool once per frame slot.
    if (lastMaterialPoolResetFrame_ != frameIndex) {
        materialRingOffset_[frameSlot] = 0;
        if (materialDescPools_[frameSlot]) {
            vkResetDescriptorPool(vkCtx_->getDevice(), materialDescPools_[frameSlot], 0);
        }
        materialDescriptorCache_[frameSlot].clear();
        lastMaterialPoolResetFrame_ = frameIndex;
    }

    // Pre-compute aligned UBO stride for ring buffer sub-allocation
    const uint32_t uboStride = (sizeof(CharMaterialUBO) + materialUboAlignment_ - 1) & ~(materialUboAlignment_ - 1);
    const uint32_t ringCapacityBytes = uboStride * MATERIAL_RING_CAPACITY;
    auto getMaterialDescriptorSet = [&](VkTexture* diffuse, VkTexture* normal,
                                        VkTexture* second) -> VkDescriptorSet {
        // Valid, not merely non-null. descriptorInfo() hands back whatever the
        // texture holds - VK_NULL_HANDLE for a view and a sampler that were
        // never created - and declares SHADER_READ_ONLY_OPTIMAL either way. A
        // draw that samples that is undefined behaviour, and on NVIDIA it
        // surfaces as a graphics engine exception and a lost device rather
        // than anything this client can catch.
        //
        // Both call sites check the diffuse and neither checks the normal, and
        // the normal is the one that can arrive from an asynchronous generation
        // pass or from a flat fallback whose own upload can fail under the same
        // memory pressure that makes the texture cache start rejecting.
        if (!diffuse || !diffuse->isValid()) diffuse = whiteTexture_.get();
        if (!normal || !normal->isValid()) normal = flatNormalTexture_.get();
        if (!second || !second->isValid()) second = whiteTexture_.get();
        if (!diffuse || !diffuse->isValid() || !normal || !normal->isValid() ||
            !second || !second->isValid()) {
            // Even the fallbacks are gone. Skipping the draw loses a model;
            // binding a null view loses the device.
            return VK_NULL_HANDLE;
        }
        const VkDescriptorImageInfo diffuseInfo = diffuse->descriptorInfo();
        const VkDescriptorImageInfo normalInfo = normal->descriptorInfo();
        const VkDescriptorImageInfo secondInfo = second->descriptorInfo();
        const MaterialDescriptorKey key{.diffuse = diffuseInfo.imageView, .normal = normalInfo.imageView,
                                        .diffuseSampler = diffuseInfo.sampler, .normalSampler = normalInfo.sampler,
                                        .second = secondInfo.imageView, .secondSampler = secondInfo.sampler};
        auto& cache = materialDescriptorCache_[frameSlot];
        if (auto it = cache.find(key); it != cache.end()) return it->second;

        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = materialDescPools_[frameSlot];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &materialSetLayout_;
        if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set) != VK_SUCCESS)
            return VK_NULL_HANDLE;

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = materialRingBuffer_[frameSlot];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(CharMaterialUBO);
        VkWriteDescriptorSet writes[4] = {};
        writes[0] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = nullptr, .dstSet = set, .dstBinding = 0, .dstArrayElement = 0, .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &diffuseInfo, .pBufferInfo = nullptr, .pTexelBufferView = nullptr};
        writes[1] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = nullptr, .dstSet = set, .dstBinding = 1, .dstArrayElement = 0, .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .pImageInfo = nullptr, .pBufferInfo = &bufferInfo, .pTexelBufferView = nullptr};
        writes[2] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = nullptr, .dstSet = set, .dstBinding = 2, .dstArrayElement = 0, .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &normalInfo, .pBufferInfo = nullptr, .pTexelBufferView = nullptr};
        writes[3] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .pNext = nullptr, .dstSet = set, .dstBinding = 3, .dstArrayElement = 0, .descriptorCount = 1,
                     .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &secondInfo, .pBufferInfo = nullptr, .pTexelBufferView = nullptr};
        vkUpdateDescriptorSets(vkCtx_->getDevice(), 4, writes, 0, nullptr);
        cache.emplace(key, set);
        return set;
    };

    // Bind per-frame descriptor set (set 0) -- shared across all draws
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    // Start with opaque pipeline
    VkPipeline currentPipeline = gbuffer ? gbufferVariant(opaquePipeline_, 0x04, false) : opaquePipeline_;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, currentPipeline);

    // One instance alone, or all of them. A caller drawing several blended
    // instances between a doodad renderer's own orders them far to near.
    drawOrder_.clear();
    if (onlyInstance != 0) {
        auto it = instances.find(onlyInstance);
        if (it != instances.end()) drawOrder_.emplace_back(it->first, &it->second);
    } else {
        for (auto& pair : instances) drawOrder_.emplace_back(pair.first, &pair.second);
    }

    for (const auto& [instanceId, instancePtr] : drawOrder_) {
        auto& instance = *instancePtr;

        if (!isDrawCandidate(instance, camPos, frustum, renderRadiusSq)) continue;
        const auto& gpuModel = *instance.cachedModel;

        // Nothing to draw in the blended phase for a model with no blended batch.
        const bool faded = instanceDrawnFaded(instance.opacity, instance.kitAlpha, instance.cameraAlpha);
        if (phase == Phase::Blended && !faded && !modelHasBlendedBatch(gpuModel)) continue;

        // Set model matrix (use override for weapon instances)
        glm::mat4 modelMat = instance.hasOverrideModelMatrix
            ? instance.overrideModelMatrix
            : getModelMatrix(instance);

        // A mirrored instance turns its winding around, so it culls the other face.
        const bool mirroredInstance = glm::determinant(glm::mat3(modelMat)) < 0.0f;

        // The model matrix, and the interior floor's light when it has one
        // (w = 1); the zone's light otherwise.
        CharPushConstants charPush{};
        charPush.model = modelMat;
        if (instance.lightKnown) {
            charPush.interiorAmbient = instance.drawAmbient;
            charPush.interiorDirect = instance.drawDirect;
            charPush.lightFlags = instance.drawFlags;
        }
        // z: how deep the instance can be along any ray, in yards - its
        // bounding sphere's diameter, and no less than a person's. The
        // shader takes at least that off a fragment's depth before testing
        // the sun's shadow map, so nothing within the model's own extent can
        // shadow it. The client has no self-shadowing on a model at all, and
        // the fine near cascade otherwise darkens limbs, the back below the
        // hair and the legs wherever the body happens to be about as thick
        // as the bias. Shadows from anything farther up the ray - buildings,
        // trees, terrain - land as before.
        {
            float extent = 2.0f * gpuModel.data.boundRadius * std::max(instance.scale, 0.001f);
            charPush.lightFlags.z = glm::clamp(extent, kSelfShadowMinExtent, kSelfShadowMaxExtent);
        }
        charPush.diffuseColour = glm::vec4(instance.diffuseColour, 0.0f);

        // This frame's pose into the bone ring (set 2).
        int numBones = std::min(static_cast<int>(instance.boneMatrices.size()), MAX_BONES);
        if (numBones > 0) {
            // GPU allocation is performed by prepareRender() before command
            // recording. Never allocate buffers/descriptors from the draw loop.
            const int ring = static_cast<int>(boneFrame_ % CharacterInstance::kBoneRing);
            if (!instance.boneBuffer[ring] || !instance.boneSet[ring]) continue;
            writeFrameBones(instance);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout_, 2, 1, &instance.boneSet[ring], 0, nullptr);
        }
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(charPush), &charPush);

        // Bind vertex and index buffers
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &gpuModel.vertexBuffer, &offset);
        vkCmdBindIndexBuffer(cmd, gpuModel.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

        if (!gpuModel.data.batches.empty()) {
            bool applyGeosetFilter = !instance.activeGeosets.empty();
            if (applyGeosetFilter) {
                bool hasRenderableGeoset = false;
                for (const auto& batch : gpuModel.data.batches) {
                    if (instance.activeGeosets.find(batch.submeshId) != instance.activeGeosets.end()) {
                        hasRenderableGeoset = true;
                        break;
                    }
                }
                if (!hasRenderableGeoset) {
                    static std::unordered_set<uint32_t> loggedGeosetFallback;
                    if (loggedGeosetFallback.insert(instance.id).second) {
                        LOG_WARNING("Geoset filter matched no batches for instance ",
                                    instance.id, " (model ", instance.modelId,
                                    "); rendering all batches as fallback");
                    }
                    applyGeosetFilter = false;
                }
            }


            const bool previewMainModel = renderPassOverride_ != VK_NULL_HANDLE &&
                                          !instance.hasOverrideModelMatrix;

            // Draw batches in two passes: opaque (blendMode 0) first, then
            // alpha-key/blend after.  This ensures capes and body parts write
            // depth before hair overlay, preventing hair→cape z-fight.
            auto getBatchBlendMode = [&](const pipeline::M2Batch& b) -> uint16_t {
                if (b.materialIndex < gpuModel.data.materials.size())
                    return gpuModel.data.materials[b.materialIndex].blendMode;
                return 0;
            };

            // Use precomputed batch render order (cached on gpuModel at load time;
            // depends only on static batch metadata, so per-frame re-sorting was waste).
            const auto& sortedBatchIndices = gpuModel.sortedBatchIndices;

            for (int pass = 0; pass < 2; pass++) {
            for (size_t bi : sortedBatchIndices) {
                const auto& batch = gpuModel.data.batches[bi];
                uint16_t bm = getBatchBlendMode(batch);
                if (pass == 0 && bm != 0) continue;  // pass 0: opaque only
                if (pass == 1 && bm == 0) continue;   // pass 1: non-opaque only
                // Blended batches (2 and up) are the world's last phase.
                // A faded instance draws every batch there (instanceDrawnFaded).
                if (phase == Phase::Opaque && (bm >= 2 || faded)) continue;
                if (phase == Phase::Blended && bm < 2 && !faded) continue;
                if (applyGeosetFilter) {
                    if (instance.activeGeosets.find(batch.submeshId) == instance.activeGeosets.end()) {
                        continue;
                    }
                } else {
                    // Even without a geoset filter, skip eye glow (group 17)
                    // and group 18 unless explicitly opted in. These geosets are
                    // only for DK/NE eye glow and should be off by default.
                    //
                    // Group 15 joins them, for the same reason and a visible
                    // one: it is the cloak, and its variants above 1501 are
                    // cloaks that exist. A model drawn with no filter drew one
                    // whether or not the character wore anything - and with no
                    // cloak texture to bind, a white sheet. An NPC that owns a
                    // cape says so by naming the geoset; one that says nothing
                    // has none.
                    uint16_t grp = batch.submeshId / 100;
                    if (grp == 17 || grp == 18 || grp == 15) continue;
                }
                // One line per head batch, for the first few instances: which
                // texture slot it resolved to and what type that slot is. The
                // player and an NPC use the same model, the same art and the
                // same pairing, and one of them draws a face - so the answer is
                // in what each batch actually got, which nothing has yet shown.
                // Only instances that are actually a character in the world: one
                // with per-instance texture overrides is an NPC, and the player
                // is the one that draws its head detail. The first run of this
                // spent all its lines on the character-select models before a
                // single NPC had spawned.
                const bool worldCharacter =
                    !instance.textureSlotOverrides.empty() || instance.drawSkinExtra;
                if (batch.submeshId == 0 && worldCharacter && headBatchCanaryCount_ < 24) {
                    ++headBatchCanaryCount_;
                    uint32_t chosenType = 0;
                    if (batch.textureIndex != 0xFFFF && !gpuModel.data.textureLookup.empty()) {
                        uint16_t sl = gpuModel.data.textureLookup[batch.textureIndex];
                        if (sl < gpuModel.data.textures.size())
                            chosenType = gpuModel.data.textures[sl].type;
                    }
                    VkTexture* rt = resolveBatchTexture(instance, gpuModel, batch);
                    // At debug: this canary was for heads resolving to the
                    // white texture, and they resolve to a texture. Twenty-four
                    // of them a session is noise in a log read for faults.
                    core::Logger::getInstance().debug(
                        "Head batch: instance=", instanceId, " model=", instance.modelId,
                        " geoset=", batch.submeshId, " firstSlotType=", chosenType,
                        " resolved=", (rt == whiteTexture_.get() ? "WHITE"
                                       : (rt == nullptr ? "null" : "texture")),
                        " ptr=", static_cast<const void*>(rt),
                        " overrides=", instance.textureSlotOverrides.size(),
                        " drawSkinExtra=", (instance.drawSkinExtra ? 1 : 0),
                        " verts=", gpuModel.data.vertices.size());
                }
                // Note on the Skin Extra batch, since it has been misread twice:
                // it is NOT a layer over the head. On an HD character model it
                // is its own section - 703 vertices and 634 triangles of the
                // human female's head, separate from the 403 and 877 of the
                // sections beside it - carrying the eyes, the mouth, the ears
                // and the eyelashes. Skipping it does not remove a detail pass,
                // it removes a face's features. Whatever is wrong with an NPC
                // face, it is not this batch existing.
                // M2 color-alpha animation gates prop submeshes per animation -
                // e.g. the peasant lumberjack carry model has two wood-bundle
                // submeshes and only one is alpha-1 in any given animation.
                // Opaque batches can't express alpha in the shader, so cull
                // batches whose track evaluates to ~0 for the current sequence.
                const float batchColorAlpha = glm::clamp(
                    evalBatchColorAlpha(gpuModel.data, batch,
                                        instance.currentSequenceIndex,
                                        sampledTime(gpuModel.data, instance.currentSequenceIndex,
                                                    instance.animationTime, instance.playMode),
                                        instance.globalSequenceTime) *
                    evalBatchTextureWeight(gpuModel.data, batch,
                                           instance.currentSequenceIndex,
                                           sampledTime(gpuModel.data, instance.currentSequenceIndex,
                                                       instance.animationTime, instance.playMode),
                                           instance.globalSequenceTime),
                    0.0f, 1.0f);
                if (batchColorAlpha <= 0.01f) {
                    continue;
                }
                // Resolve texture for this batch (prefer hair textures for hair geosets).
                VkTexture* texPtr = resolveBatchTexture(instance, gpuModel, batch);
                const uint16_t batchGroup = static_cast<uint16_t>(batch.submeshId / 100);
                auto groupTexIt = instance.groupTextureOverrides.find(batchGroup);
                if (groupTexIt != instance.groupTextureOverrides.end() && groupTexIt->second != nullptr) {
                    texPtr = groupTexIt->second;
                }

                // Respect M2 material blend mode for creature/character submeshes.
                uint16_t blendMode = 0;
                uint16_t materialFlags = 0;
                if (batch.materialIndex < gpuModel.data.materials.size()) {
                    blendMode = gpuModel.data.materials[batch.materialIndex].blendMode;
                    materialFlags = gpuModel.data.materials[batch.materialIndex].flags;
                }

                const bool hairMaterial = batchIsHairCard(gpuModel.data, batch, blendMode,
                                                          instance.isSceneModel);

                // The batch's shader (0x00836980, 0x00836c90): how many
                // textures it combines, by what, and where each one's
                // coordinates come from - an attached weapon's additive and
                // env-mapped passes included, which the client draws.
                const uint16_t shaderId = m2BatchShaderId(
                    batch.shader, blendMode, batch.textureCount, batch.textureUnit,
                    gpuModel.data.globalFlags, gpuModel.data.textureCoordCombos,
                    gpuModel.data.textureCombinerCombos);
                const uint16_t firstCoord = batch.textureUnit < gpuModel.data.textureCoordCombos.size()
                    ? gpuModel.data.textureCoordCombos[batch.textureUnit] : 0;
                const M2BatchCombiner combiner = m2ResolveCombiner(shaderId, batch.textureCount, firstCoord);
                // Shader id 0x8000 gets no shader, and a batch with none is
                // not drawn (0x00821e97).
                if (!combiner.drawn) continue;
                // Each stage binds its own entry of the lookup; the pick of one
                // texture out of several is for a batch that draws only one.
                if (combiner.stages > 1 && groupTexIt == instance.groupTextureOverrides.end()) {
                    texPtr = resolveStageTexture(instance, gpuModel, batch, 0);
                }

                // For body/equipment parts with white/fallback texture, use skin (type 1) texture.
                if (texPtr == whiteTexture_.get() && !instance.isSceneModel) {
                    uint16_t group = batchGroup;
                    bool isSkinGroup = (group == 0 || group == 3 || group == 4 || group == 5 ||
                                        group == 8 || group == 9 || group == 13);
                    if (isSkinGroup) {
                        uint32_t texType = 0;
                        if (batch.textureIndex < gpuModel.data.textureLookup.size()) {
                            uint16_t lk = gpuModel.data.textureLookup[batch.textureIndex];
                            if (lk < gpuModel.data.textures.size()) {
                                texType = gpuModel.data.textures[lk].type;
                            }
                        }
                        // Do NOT apply skin composite to hair (type 6) batches
                        if (texType != 6) {
                            for (size_t ti = 0; ti < gpuModel.textureIds.size(); ti++) {
                                VkTexture* candidate = gpuModel.textureIds[ti];
                                auto itO = instance.textureSlotOverrides.find(static_cast<uint16_t>(ti));
                                if (itO != instance.textureSlotOverrides.end() && itO->second != nullptr) {
                                    candidate = itO->second;
                                }
                                if (candidate != whiteTexture_.get() && candidate != nullptr) {
                                    if (ti < gpuModel.data.textures.size() &&
                                        (gpuModel.data.textures[ti].type == 1 || gpuModel.data.textures[ti].type == 11)) {
                                        texPtr = candidate;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }

                // Determine material properties
                bool alphaCutout = false;
                if (texPtr != nullptr && texPtr != whiteTexture_.get()) {
                    auto pit = texturePropsByPtr_.find(texPtr);
                    if (pit != texturePropsByPtr_.end()) {
                        alphaCutout = pit->second.hasAlpha;
                    }
                }
                // A scene means what its materials say. Stormwind's walls are DXT5 with
                // an unused alpha channel - every texel below the 0.5 cutoff - so
                // inferring a cutout from "the texture has alpha" discards the whole
                // building and leaves the sky showing through it. Only an alpha-key
                // material (blendMode 1) cuts out here.
                //
                // The client cuts out alpha-key batches and nothing else
                // (FUN_0081fe90 sets the alpha reference by blend mode alone):
                // an opaque batch is opaque whatever its texture's alpha, and a
                // blended one is blended. Hair keeps this renderer's cutout.
                const bool blendNeedsCutout = instance.isSceneModel
                    ? (blendMode == 1)
                    : ((blendMode == 1) || hairMaterial);
                (void)alphaCutout;
                // Lit unless the material says unlit (0x1), as the client has it
                // (FUN_0081fb10, table at 0x00a45374): the additive modes are lit
                // too, and Mod and Mod2x are drawn unlit in the shader. An
                // enchant's model is no exception: it is drawn by its materials.
                const bool unlit = (materialFlags & 0x01) != 0;

                // Hair textures are authored as alpha-cut cards. If they use the
                // translucent pipeline they form a soft shell around the head.
                //
                // M2 blend modes as the 3.3.5a client draws them (its M2 -> Gx
                // table at 0x00a453b0, Gx factors at 0x00a2f964/0x00a2f994):
                // 0 Opaque, 1 AlphaKey, 2 Alpha, 3 NoAlphaAdd (ONE, ONE),
                // 4 Add (SRC_ALPHA, ONE), 5 Mod (DST_COLOR, ZERO), 6 Mod2x
                // (DST_COLOR, SRC_COLOR), 7 Alpha. 6 and 7 were drawn additive and
                // 5 alpha-blended; a Mod2x tint added instead of multiplied is the
                // mana wyrm's wings turned to solid white.
                const bool additiveBlend = (blendMode == 3 || blendMode == 4);
                const bool colourOnlyBlend = (blendMode == 3 || blendMode == 5 || blendMode == 6);

                // An enchant's model (ItemVisualEffects) included: the client
                // draws it as any M2, by its own materials (FUN_0081fe90).
                VkPipeline desiredPipeline;
                if (blendMode == 5 || blendMode == 6) {
                    // A multiply fades toward its neutral value in the shader,
                    // so a partial alpha does not divert it either.
                    desiredPipeline = blendMode == 5 ? modPipeline_ : mod2xPipeline_;
                } else if (additiveBlend) {
                    // Decided before the fade branch below, not after it. An
                    // additive card fades by adding less light - matData.opacity
                    // already scales what it contributes - so a partial alpha is
                    // not a reason to divert it to the translucent pipeline, and
                    // diverting it composites the card's black backing as black.
                    //
                    // The Darnassus wisps are the case: their glow alpha is an
                    // animated pulse, so the diversion tripped on almost every
                    // frame the pulse was not at full, and the cards showed as
                    // black roughly half the time.
                    desiredPipeline = blendMode == 3 ? noAlphaAddPipeline_ : additivePipeline_;
                } else if (instance.opacity * instance.kitAlpha * instance.cameraAlpha * batchColorAlpha < 0.999f) {
                    // Whole-instance fade (ghost form, spawn fade-in): the opaque and
                    // alpha-test pipelines have blending disabled, so the shader's
                    // texColor.a * opacity output is discarded and only hair (via
                    // alpha-to-coverage) ever looked translucent. Route every batch
                    // through the blend pipeline; the per-batch alphaTest UBO flag
                    // still handles cutout materials in the shader.
                    //
                    // Except that a batch which must not write depth still must
                    // not, fading or otherwise. translucentPipeline_ writes
                    // depth so a fading character's own solid parts keep sorting
                    // against each other; alphaPipeline_ is the same blend with
                    // the depth write off. Sending a translucent card down the
                    // writing one puts an invisible occluder in front of
                    // whatever it covers, which is how the glue screens' cloud
                    // and haze cards were punching a rectangle out of the
                    // character standing behind them - the card's own materials
                    // ask for no depth write (0x10) and the scenes animate their
                    // alpha, so almost every frame took this branch.
                    //
                    // Whether it writes depth is the material's 0x10 flag, as
                    // for every batch; see pipelineVariant below.
                    desiredPipeline = alphaPipeline_;
                } else if (hairMaterial) {
                    desiredPipeline = alphaTestPipeline_;
                } else {
                    // additiveBlend (3/4/6/7) is handled above; only the
                    // non-additive modes reach here. 4/Add is the one that used
                    // to fall through to the alpha pipeline, which composited a
                    // glow card's black backing as an actual black quad - every
                    // material on Wisp.m2 declares it.
                    switch (blendMode) {
                        case 0: desiredPipeline = opaquePipeline_; break;
                        case 1: desiredPipeline = alphaTestPipeline_; break;
                        case 2: desiredPipeline = alphaPipeline_; break;
                        default: desiredPipeline = alphaPipeline_; break;
                    }
                }
                // Cull, depth test and depth write from the material's flags, as
                // the client sets them for every blend mode (FUN_0081fe90).
                desiredPipeline = gbuffer ? gbufferVariant(desiredPipeline, materialFlags, mirroredInstance)
                                          : pipelineVariant(desiredPipeline, materialFlags, mirroredInstance);
                if (!desiredPipeline) continue;
                if (desiredPipeline != currentPipeline) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
                    currentPipeline = desiredPipeline;
                }

                // Resolve the normal map for this texture
                VkTexture* normalMap = flatNormalTexture_.get();
                if (texPtr && texPtr != whiteTexture_.get()) {
                    auto nmIt = normalMapByTexPtr_.find(texPtr);
                    if (nmIt != normalMapByTexPtr_.end()) {
                        normalMap = nmIt->second.normalMap;
                    }
                }

                const bool useAdvancedMaterials = !previewMainModel;
                const bool usePreviewSimpleShader = previewMainModel;

                // Create per-batch material UBO
                CharMaterialUBO matData{};
                matData.instanceFade = instance.opacity * instance.kitAlpha * instance.cameraAlpha;
                matData.opacity = matData.instanceFade * batchColorAlpha;
                // 1: the client's alpha-key test. 2: a blended mode, which the
                // client tests at 1/255 - only what is fully transparent goes.
                matData.alphaTest = blendNeedsCutout ? 1 : (blendMode >= 2 ? 2 : 0);
                matData.unlit = unlit ? 1 : 0;
                matData.emissiveBoost = 1.0f;
                matData.emissiveTintR = 1.0f;
                matData.emissiveTintG = 1.0f;
                matData.emissiveTintB = 1.0f;
                matData.specularIntensity = 0.5f;
                matData.enableNormalMap = (useAdvancedMaterials && normalMappingEnabled_) ? 1 : 0;
                matData.normalMapStrength = normalMapStrength_;
                matData.hairMaterial = hairMaterial ? 1 : 0;
                // Texture animation, which this renderer did not do at all.
                if (const auto* tt = m2_track::batchTextureTransform(gpuModel.data,
                                                                     batch.textureAnimIndex)) {
                    const glm::mat4 uvm = m2_track::textureTransformMatrix(
                        *tt, instance.currentSequenceIndex,
                        sampledTime(gpuModel.data, instance.currentSequenceIndex, instance.animationTime,
                                    instance.playMode),
                        instance.globalSequenceTime, gpuModel.data.globalSequenceDurations);
                    matData.uvM00 = uvm[0][0]; matData.uvM01 = uvm[1][0];
                    matData.uvM10 = uvm[0][1]; matData.uvM11 = uvm[1][1];
                    matData.uvTx = uvm[3][0];  matData.uvTy = uvm[3][1];
                }
                matData.combiners = m2PackCombinerModes(combiner);
                matData.coordSources = m2PackCoordSources(combiner);
                const glm::vec3 batchRGB = evalBatchColorRGB(gpuModel.data, batch,
                                                             instance.currentSequenceIndex,
                                                             sampledTime(gpuModel.data,
                                                                         instance.currentSequenceIndex,
                                                                         instance.animationTime, instance.playMode),
                                                             instance.globalSequenceTime);
                matData.tintR = batchRGB.r;
                matData.tintG = batchRGB.g;
                matData.tintB = batchRGB.b;
                VkTexture* secondTex = nullptr;
                if (combiner.stages > 1) {
                    // The second texture and its matrix, through the same
                    // lookups one entry on (0x0081f450).
                    secondTex = resolveStageTexture(instance, gpuModel, batch, 1);
                    if (batch.textureAnimIndex != 0xFFFF) {
                        if (const auto* tt2 = m2_track::batchTextureTransform(
                                gpuModel.data, static_cast<uint16_t>(batch.textureAnimIndex + 1))) {
                            const glm::mat4 uvm = m2_track::textureTransformMatrix(
                                *tt2, instance.currentSequenceIndex,
                                sampledTime(gpuModel.data, instance.currentSequenceIndex, instance.animationTime,
                                            instance.playMode),
                                instance.globalSequenceTime, gpuModel.data.globalSequenceDurations);
                            matData.uv2M00 = uvm[0][0]; matData.uv2M01 = uvm[1][0];
                            matData.uv2M10 = uvm[0][1]; matData.uv2M11 = uvm[1][1];
                            matData.uv2Tx = uvm[3][0];  matData.uv2Ty = uvm[3][1];
                        }
                    }
                }
                matData.colourBlend = !colourOnlyBlend ? 0
                                    : (blendMode == 5 ? 1 : (blendMode == 6 ? 2 : 3));
                // The client's fog per blend mode (table at 0x00a45390): the
                // world's for 0-2, black for the adds, white for Mod, grey for
                // Mod2x; none for an unfogged material (0x2) or mode 7, which
                // reads past the table.
                static constexpr int32_t kFogByBlend[7] = {1, 1, 1, 2, 2, 3, 4};
                matData.fogMode = ((materialFlags & 0x02) != 0) ? 0
                                : (blendMode < 7 ? kFogByBlend[blendMode] : 0);

                // The base humanoid mesh samples a mirrored character atlas,
                // with the face sitting directly beside a UV seam, which a
                // normal map derived from it does not survive. Group 0
                // body/head surfaces keep the vertex normal - and so does the
                // no-cloak panel, which is a patch of the same torso.
                if (batchGroup == 0 || batch.submeshId == core::kGeosetNoCape) {
                    normalMap = flatNormalTexture_.get();
                    matData.specularIntensity = 0.20f;
                    matData.enableNormalMap = 0;
                }

                // Tabards are a thin cloth layer over the torso. Reusing the
                // body composite's generated height/normal response makes the
                // armor underneath appear embossed and reflective through the
                // cloth as the camera moves. Keep the diffuse tabard artwork,
                // but give group 12 a flat, low-specular cloth material.
                if (batchGroup == 12) {
                    normalMap = flatNormalTexture_.get();
                    matData.specularIntensity = 0.08f;
                    matData.enableNormalMap = 0;
                }
                if (usePreviewSimpleShader) {
                    matData.enableNormalMap = 0;
                    matData.previewSimpleTexture = 1;
                }

                // WOWEE_SCENE_DIAG=1 dumps what each backdrop batch is actually told to
                // draw - texture, blend mode, shader path - once per scene model.
                static int sceneDiagLines = 0;
                if (instance.isSceneModel && sceneDiagEnabled() && sceneDiagLines++ < 40) {
                    std::string texName = "<white>";
                    if (batch.textureIndex < gpuModel.data.textureLookup.size()) {
                        uint16_t lk = gpuModel.data.textureLookup[batch.textureIndex];
                        if (lk < gpuModel.data.textures.size())
                            texName = gpuModel.data.textures[lk].filename;
                    }
                    // Warning level: the diagnostic is opt-in already, and the file log
                    // filters info out by default.
                    core::Logger::getInstance().warning(
                        "SCENE DIAG batch submesh=", batch.submeshId,
                        " blend=", blendMode, " matFlags=0x", std::hex, materialFlags, std::dec,
                        " alphaTest=", matData.alphaTest,
                        " unlit=", matData.unlit,
                        " simplePath=", matData.previewSimpleTexture,
                        " whiteFallback=", (texPtr == whiteTexture_.get() ? 1 : 0),
                        " tex=", texName);
                }

                // Sub-allocate material UBO from ring buffer
                uint32_t matOffset = materialRingOffset_[frameSlot];
                if (matOffset + uboStride > ringCapacityBytes) continue; // ring exhausted
                memcpy(static_cast<char*>(materialRingMapped_[frameSlot]) + matOffset, &matData, sizeof(CharMaterialUBO));
                materialRingOffset_[frameSlot] = matOffset + uboStride;

                VkTexture* bindTex = (texPtr && texPtr->isValid()) ? texPtr : whiteTexture_.get();
                VkDescriptorSet materialSet = getMaterialDescriptorSet(bindTex, normalMap, secondTex);
                if (!materialSet) continue;

                // Bind material descriptor set (set 1)
                const uint32_t dynamicOffset = matOffset;
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipelineLayout_, 1, 1, &materialSet, 1, &dynamicOffset);

                // Per-batch depth bias from materialLayer to separate coplanar
                // armor pieces (chest/legs/gloves) that share identical depth.
                vkCmdSetDepthBias(cmd, depth::biasFromStandard(static_cast<float>(batch.materialLayer) * 0.5f), 0.0f, 0.0f);

                vkCmdDrawIndexed(cmd, batch.indexCount, 1, batch.indexStart, 0, 0);
            }
            } // end pass loop
        } else if ((phase == Phase::Blended) == faded) {
            // Draw entire model with first texture: in the opaque phase, or
            // faded in the blended one (instanceDrawnFaded)
            VkTexture* texPtr = !gpuModel.textureIds.empty() ? gpuModel.textureIds[0] : whiteTexture_.get();
            if (!texPtr || !texPtr->isValid()) texPtr = whiteTexture_.get();

            // Whole-model fallback inherits whatever pipeline was bound last;
            // pick it explicitly so instance fades blend here too.
            VkPipeline fallbackPipeline = (instance.opacity * instance.kitAlpha * instance.cameraAlpha < 0.999f)
                ? translucentPipeline_ : opaquePipeline_;
            if (gbuffer) fallbackPipeline = gbufferVariant(opaquePipeline_, 0x04, false);
            if (fallbackPipeline != currentPipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fallbackPipeline);
                currentPipeline = fallbackPipeline;
            }

            CharMaterialUBO matData{};
            matData.opacity = instance.opacity * instance.kitAlpha * instance.cameraAlpha;
            matData.instanceFade = matData.opacity;
            matData.alphaTest = 0;
            matData.unlit = 0;
            matData.emissiveBoost = 1.0f;
            matData.emissiveTintR = 1.0f;
            matData.emissiveTintG = 1.0f;
            matData.emissiveTintB = 1.0f;
            matData.specularIntensity = 0.5f;
            const bool previewMainModel = renderPassOverride_ != VK_NULL_HANDLE &&
                                          !instance.hasOverrideModelMatrix;
            const bool useAdvancedMaterials = !previewMainModel;
            const bool usePreviewSimpleShader = previewMainModel;
            matData.enableNormalMap = (useAdvancedMaterials && normalMappingEnabled_) ? 1 : 0;
            matData.normalMapStrength = normalMapStrength_;
            matData.hairMaterial = 0;
            matData.fogMode = 1;   // the world's fog, as this path always had
            if (usePreviewSimpleShader) {
                matData.enableNormalMap = 0;
                matData.previewSimpleTexture = 1;
            }

            // Sub-allocate material UBO from ring buffer
            uint32_t matOffset2 = materialRingOffset_[frameSlot];
            if (matOffset2 + uboStride > ringCapacityBytes) continue; // ring exhausted
            memcpy(static_cast<char*>(materialRingMapped_[frameSlot]) + matOffset2, &matData, sizeof(CharMaterialUBO));
            materialRingOffset_[frameSlot] = matOffset2 + uboStride;

            VkDescriptorSet materialSet = getMaterialDescriptorSet(texPtr, flatNormalTexture_.get(), nullptr);
            if (!materialSet) continue;

            const uint32_t dynamicOffset = matOffset2;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout_, 1, 1, &materialSet, 1, &dynamicOffset);

            vkCmdDrawIndexed(cmd, gpuModel.indexCount, 1, 0, 0, 0);
        }
    }
}

bool CharacterRenderer::initializeShadow(VkRenderPass shadowRenderPass) {
    if (!vkCtx_ || shadowRenderPass == VK_NULL_HANDLE) return false;
    VkDevice device = vkCtx_->getDevice();

    // ShadowCharParams UBO (matches character_shadow.frag.glsl set=1 binding=1)
    struct ShadowCharParams {
        int32_t alphaTest = 0;
        int32_t unused0 = 0;  // was the black colour key; the client never keys by colour
    };

    // The same set the other three shadow passes bind - a sampler and a small
    // uniform buffer - with this pass's own params behind binding 1.
    if (!createShadowParamsSet(device, vkCtx_->getAllocator(), sizeof(ShadowCharParams),
                               whiteTexture_->getImageView(), whiteTexture_->getSampler(),
                               "CharacterRenderer", shadowParams_)) {
        return false;
    }

    // Alpha testing on, once, because it never varies here.
    //
    // The buffer is zero-filled at creation and was then never written, so the
    // flag read 0 and every caster stamped its bounding rectangle into the
    // shadow map - hair, capes and cloaks cast solid slabs instead of their own
    // outline. With it on, an opaque batch is bound to the white fallback whose
    // alpha is 1 and passes the cutoff untouched, while an alpha-keyed batch is
    // bound to its own texture below and cuts properly.
    {
        VmaAllocationInfo paramsInfo{};
        vmaGetAllocationInfo(vkCtx_->getAllocator(), shadowParams_.alloc, &paramsInfo);
        if (paramsInfo.pMappedData) {
            ShadowCharParams p{};
            p.alphaTest = 1;
            std::memcpy(paramsInfo.pMappedData, &p, sizeof(p));
        }
    }

    // One texture-set pool per frame in flight, reset at the top of each
    // frame's shadow pass.
    {
        VkDescriptorPoolSize texPoolSizes[2]{};
        texPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texPoolSizes[0].descriptorCount = 256;
        texPoolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        texPoolSizes[1].descriptorCount = 256;
        VkDescriptorPoolCreateInfo texPoolCI{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        texPoolCI.maxSets = 256;
        texPoolCI.poolSizeCount = 2;
        texPoolCI.pPoolSizes = texPoolSizes;
        for (uint32_t f = 0; f < kShadowTexPoolFrames; ++f) {
            if (vkCreateDescriptorPool(device, &texPoolCI, nullptr, &shadowTexPool_[f]) != VK_SUCCESS) {
                LOG_ERROR("CharacterRenderer: failed to create shadow texture pool ", f);
                return false;
            }
        }
    }

    // Pipeline layout: set 0 = shadowParams_.layout, set 1 = boneSetLayout_
    //
    // There used to be a third, perFrameLayout_, sitting at set 0 as a dummy
    // that nothing ever bound, which pushed the params to set 1 and the bones to
    // set 2. That made this the odd one out of the four passes sharing the
    // shadow render pass: terrain, WMO and M2 all bind their params at set 0
    // under a layout of their own. Binding set 1 while set 0 still carried
    // another pass's incompatible layout left the params disturbed rather than
    // bound, and the fragment shader read its alpha-test flags from nothing -
    // thousands of times a session, which GPU-assisted validation reports as
    // indexing a descriptor array of length zero. A set that is never bound has
    // no business being in the layout.
    // Push constant: 128 bytes (lightSpaceMatrix + model), VERTEX stage
    VkDescriptorSetLayout setLayouts[] = {shadowParams_.layout, boneSetLayout_};
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc.offset = 0;
    pc.size = sizeof(ShadowPush);  // one combined matrix, plus the sway slot
    VkPipelineLayoutCreateInfo plCI{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plCI.setLayoutCount = 2;
    plCI.pSetLayouts = setLayouts;
    plCI.pushConstantRangeCount = 1;
    plCI.pPushConstantRanges = &pc;
    if (vkCreatePipelineLayout(device, &plCI, nullptr, &shadowPipelineLayout_) != VK_SUCCESS) {
        LOG_ERROR("CharacterRenderer: failed to create shadow pipeline layout");
        return false;
    }

    // Load character shadow shaders
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/character_shadow.vert.spv")) {
        LOG_ERROR("CharacterRenderer: failed to load character_shadow.vert.spv");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/character_shadow.frag.spv")) {
        LOG_ERROR("CharacterRenderer: failed to load character_shadow.frag.spv");
        vertShader.destroy();
        return false;
    }

    // Character vertex format (CharVertexGPU): stride = 56 bytes
    // loc 0: vec3 aPos          (R32G32B32_SFLOAT, offset 0)
    // loc 1: vec4 aBoneWeights  (R8G8B8A8_UNORM,   offset 12)
    // loc 2: uvec4 aBoneIndices (R8G8B8A8_UINT,    offset 16)
    // loc 3: vec2 aTexCoord     (R32G32_SFLOAT,    offset 32)
    VkVertexInputBindingDescription vertBind{};
    vertBind.binding = 0;
    vertBind.stride = static_cast<uint32_t>(sizeof(CharVertexGPU));
    vertBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> vertAttrs = {
        {.location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, position))},
        {.location = 1, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UNORM,   .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, boneWeights))},
        {.location = 2, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UINT,    .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, boneIndices))},
        {.location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT,    .offset = static_cast<uint32_t>(offsetof(CharVertexGPU, texCoords))},
    };

    shadowPipeline_ = buildShadowPipeline(
        device, vkCtx_->getPipelineCache(),
        vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
        fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT),
        vertBind, vertAttrs, shadowPipelineLayout_, shadowRenderPass,
        vkCtx_->useDynamicRendering());

    vertShader.destroy();
    fragShader.destroy();

    if (!shadowPipeline_) {
        LOG_ERROR("CharacterRenderer: failed to create shadow pipeline");
        return false;
    }
    LOG_INFO("CharacterRenderer shadow pipeline initialized");
    return true;
}

void CharacterRenderer::beginShadowFrame(uint32_t frameIndex) {
    ++shadowFrameSerial_;
    // This frame slot's fence was waited on in beginFrame, so last time's sets
    // are finished with and the pool can be handed back whole. Once a frame
    // and not per cascade: a set one cascade binds is read when the GPU runs
    // its draws, after the later cascades have been recorded.
    if (frameIndex < kShadowTexPoolFrames && shadowTexPool_[frameIndex]) {
        vkResetDescriptorPool(vkCtx_->getDevice(), shadowTexPool_[frameIndex], 0);
    }
    shadowTexSetCache_.clear();
}

void CharacterRenderer::renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix,
                                     const glm::vec3& shadowCenter, float shadowRadius) {
    if (!shadowPipeline_ || !shadowParams_.set) return;
    if (instances.empty() || models.empty()) return;
    if (boneDescPool_ == VK_NULL_HANDLE || boneSetLayout_ == VK_NULL_HANDLE) return;

    uint32_t frameIndex = vkCtx_->getCurrentFrame();
    if (frameIndex >= 2) return;
    // The shadow worker records this while the doodad worker draws the
    // blended characters through render(), and both copy each instance's pose
    // into the same bone buffer (writeFrameBones).
    std::lock_guard<std::mutex> renderLock(renderMutex_);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);

    const float shadowRadiusSq = shadowRadius * shadowRadius;
    // Which set is bound at 0 right now, so a run of opaque batches does not
    // rebind the same fallback for each one.
    VkDescriptorSet currentTexSet = VK_NULL_HANDLE;
    for (auto& pair : instances) {
        auto& inst = pair.second;
        if (!inst.visible || inst.heldBack) continue;
        if (!inst.posedOnce && !inst.hasOverrideModelMatrix) continue;  // as the main pass

        // Distance cull against shadow frustum
        glm::vec3 diff = inst.position - shadowCenter;
        if (glm::dot(diff, diff) > shadowRadiusSq) continue;

        if (!inst.cachedModel) continue;
        const M2ModelGPU& gpuModel = *inst.cachedModel;
        if (!gpuModel.vertexBuffer) continue;

        glm::mat4 modelMat = inst.hasOverrideModelMatrix
            ? inst.overrideModelMatrix
            : getModelMatrix(inst);

        // This frame's pose into the bone ring, as the main pass has it (or
        // will: whichever draws first writes it). The ring is allocated by
        // prepareRender alone; an instance it has not reached casts next frame.
        const int ring = static_cast<int>(boneFrame_ % CharacterInstance::kBoneRing);
        if (inst.boneMatrices.empty() || !inst.boneSet[ring]) continue;
        writeFrameBones(inst);

        // Params at set 1 and bones at set 2, together, per instance.
        //
        // Binding set 1 once before the loop was not enough. This pass runs
        // after the M2 shadow pass, which binds its own set at set 0 under a
        // different pipeline layout, and a descriptor set bound while a lower
        // set carries an incompatible layout is disturbed rather than kept. The
        // fragment shader then read set 1 binding 1 - its alpha-test flags -
        // from a set the GPU no longer considered bound, which GPU-assisted
        // validation reports as indexing a descriptor array of length zero,
        // thousands of times a session.
        VkDescriptorSet sets[2] = {shadowParams_.set, inst.boneSet[ring]};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
            0, 2, sets, 0, nullptr);
        currentTexSet = shadowParams_.set;

        ShadowPush push{.lightSpaceModel = lightSpaceMatrix * modelMat};
        vkCmdPushConstants(cmd, shadowPipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(ShadowPush), &push);

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &gpuModel.vertexBuffer, &offset);
        vkCmdBindIndexBuffer(cmd, gpuModel.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

        // The batches cast exactly what the main pass draws, by the same
        // choices: the same geoset filter (and its fallback when it matches
        // nothing), the same groups skipped without one, nothing whose colour
        // or transparency track has it gone this frame, nothing the client has
        // no shader for, and hair cut by the texture it is drawn with. A batch
        // that casts but is not drawn, or casts solid where it is drawn cut
        // out, puts a shadow on the character that nothing on screen throws.
        bool applyGeosetFilter = !inst.activeGeosets.empty();
        if (applyGeosetFilter) {
            applyGeosetFilter = std::any_of(
                gpuModel.data.batches.begin(), gpuModel.data.batches.end(),
                [&](const pipeline::M2Batch& b) { return inst.activeGeosets.count(b.submeshId) != 0; });
        }
        for (const auto& batch : gpuModel.data.batches) {
            uint16_t blendMode = 0;
            if (batch.materialIndex < gpuModel.data.materials.size()) {
                blendMode = gpuModel.data.materials[batch.materialIndex].blendMode;
            }
            if (blendMode >= 2) continue; // skip transparent
            if (applyGeosetFilter &&
                inst.activeGeosets.find(batch.submeshId) == inst.activeGeosets.end()) continue;
            if (!applyGeosetFilter) {
                uint16_t grp = batch.submeshId / 100;
                if (grp == 17 || grp == 18 || grp == 15) continue;
            }
            const float batchAlpha =
                evalBatchColorAlpha(gpuModel.data, batch, inst.currentSequenceIndex,
                                    sampledTime(gpuModel.data, inst.currentSequenceIndex, inst.animationTime,
                                                inst.playMode),
                                    inst.globalSequenceTime) *
                evalBatchTextureWeight(gpuModel.data, batch, inst.currentSequenceIndex,
                                       sampledTime(gpuModel.data, inst.currentSequenceIndex, inst.animationTime,
                                                   inst.playMode),
                                       inst.globalSequenceTime);
            if (glm::clamp(batchAlpha, 0.0f, 1.0f) <= 0.01f) continue;
            if (inst.opacity <= 0.01f) continue;

            const uint16_t shaderId = m2BatchShaderId(
                batch.shader, blendMode, batch.textureCount, batch.textureUnit,
                gpuModel.data.globalFlags, gpuModel.data.textureCoordCombos,
                gpuModel.data.textureCombinerCombos);
            const uint16_t firstCoord = batch.textureUnit < gpuModel.data.textureCoordCombos.size()
                ? gpuModel.data.textureCoordCombos[batch.textureUnit] : 0;
            const M2BatchCombiner combiner = m2ResolveCombiner(shaderId, batch.textureCount, firstCoord);
            if (!combiner.drawn) continue;

            // An alpha-keyed batch, or hair, casts the shape of its texture;
            // everything else keeps the white fallback and casts solid. Only
            // the set at binding 0 changes, so the bones stay bound from above.
            VkDescriptorSet texSet = shadowParams_.set;
            if (blendMode == 1 || batchIsHairCard(gpuModel.data, batch, blendMode, inst.isSceneModel)) {
                // The texture the main pass binds: the group's override, else
                // the first stage's for a multi-stage batch, else the pick.
                VkTexture* tex = nullptr;
                auto groupTexIt = inst.groupTextureOverrides.find(
                    static_cast<uint16_t>(batch.submeshId / 100));
                if (groupTexIt != inst.groupTextureOverrides.end() && groupTexIt->second != nullptr) {
                    tex = groupTexIt->second;
                } else if (combiner.stages > 1) {
                    tex = resolveStageTexture(inst, gpuModel, batch, 0);
                } else {
                    tex = resolveBatchTexture(inst, gpuModel, batch);
                }
                if (tex && tex != whiteTexture_.get()) {
                    texSet = shadowTexDescSet(tex, frameIndex);
                }
            }
            if (texSet != currentTexSet) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
                                        0, 1, &texSet, 0, nullptr);
                currentTexSet = texSet;
            }

            vkCmdDrawIndexed(cmd, batch.indexCount, 1, batch.indexStart, 0, 0);
        }
    }
}

VkDescriptorSet CharacterRenderer::shadowTexDescSet(VkTexture* tex, uint32_t frameIndex) {
    // Valid, not merely non-null, for the reason in getMaterialDescriptorSet:
    // a texture whose upload or view creation failed writes a null view and
    // sampler into a live descriptor. The alpha-tested shadow pass samples it,
    // which is undefined behaviour and reaches NVIDIA as a lost device.
    //
    // Also worth the check for the cache below, which is keyed on the view: a
    // null one would key every failed texture to the same set.
    if (!tex || !tex->isValid()) return shadowParams_.set;
    if (frameIndex >= kShadowTexPoolFrames) return shadowParams_.set;
    VkDescriptorPool pool = shadowTexPool_[frameIndex];
    if (!pool) return shadowParams_.set;

    VkImageView view = tex->getImageView();
    if (auto it = shadowTexSetCache_.find(view); it != shadowTexSetCache_.end()) return it->second;

    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &shadowParams_.layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set) != VK_SUCCESS) {
        // Out of sets for this frame: the white fallback casts a full shadow,
        // which is the old behaviour rather than a hole.
        return shadowParams_.set;
    }

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView = view;
    imgInfo.sampler = tex->getSampler();
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = shadowParams_.ubo;
    bufInfo.offset = 0;
    bufInfo.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &imgInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(vkCtx_->getDevice(), 2, writes, 0, nullptr);

    shadowTexSetCache_[view] = set;
    return set;
}

VkTexture* CharacterRenderer::resolveBatchTexture(const CharacterInstance& inst,
                                                 const M2ModelGPU& gm,
                                                 const pipeline::M2Batch& b) const {
            // A skin batch can reference multiple textures (b.textureCount) starting at b.textureIndex.
            // We currently bind only a single texture, so pick the most appropriate one.
            if (b.textureIndex == 0xFFFF) return whiteTexture_.get();
            if (gm.data.textureLookup.empty() || gm.textureIds.empty()) return whiteTexture_.get();

            uint32_t comboCount = b.textureCount ? static_cast<uint32_t>(b.textureCount) : 1u;
            comboCount = std::min<uint32_t>(comboCount, 8u);

            struct Candidate { VkTexture* tex; uint32_t type; };
            Candidate first{.tex = whiteTexture_.get(), .type = 0};
            bool hasFirst = false;
            Candidate firstNonWhite{.tex = whiteTexture_.get(), .type = 0};
            bool hasFirstNonWhite = false;

            for (uint32_t i = 0; i < comboCount; i++) {
                uint32_t lookupPos = static_cast<uint32_t>(b.textureIndex) + i;
                if (lookupPos >= gm.data.textureLookup.size()) break;
                uint16_t texSlot = gm.data.textureLookup[lookupPos];
                if (texSlot >= gm.textureIds.size()) continue;

                VkTexture* texPtr = gm.textureIds[texSlot];
                uint32_t texType = (texSlot < gm.data.textures.size()) ? gm.data.textures[texSlot].type : 0;
                // Apply texture slot overrides.
                // For type-1 (skin) overrides, only apply to skin-group batches
                // to prevent the skin composite from bleeding onto cloak/hair.
                {
                    auto itO = inst.textureSlotOverrides.find(texSlot);
                    if (itO != inst.textureSlotOverrides.end() && itO->second != nullptr) {
                        if (texType == 1) {
                            // Only apply skin override to skin groups
                            uint16_t grp = b.submeshId / 100;
                            bool isSkinGroup = (grp == 0 || grp == 3 || grp == 4 || grp == 5 ||
                                                grp == 8 || grp == 9 || grp == 13 || grp == 15 ||
                                                grp == 20);
                            if (isSkinGroup) texPtr = itO->second;
                        } else {
                            texPtr = itO->second;
                        }
                    }
                }

                if (!hasFirst) {
                    first = {.tex = texPtr, .type = texType};
                    hasFirst = true;
                }

                if (texPtr == nullptr || texPtr == whiteTexture_.get()) continue;

                // Prefer the hair texture slot (type 6) whenever present in the combo.
                if (texType == 6) {
                    return texPtr;
                }

                if (!hasFirstNonWhite) {
                    firstNonWhite = {.tex = texPtr, .type = texType};
                    hasFirstNonWhite = true;
                }
            }

            if (hasFirstNonWhite) return firstNonWhite.tex;
            if (hasFirst && first.tex != nullptr) return first.tex;
            return whiteTexture_.get();
}

VkTexture* CharacterRenderer::resolveStageTexture(const CharacterInstance& inst,
                                                 const M2ModelGPU& gm,
                                                 const pipeline::M2Batch& b, uint32_t stage) const {
    // The stage's own entry of the batch's lookup, as the client binds it,
    // with the instance's slot overrides.
    if (b.textureIndex == 0xFFFF) return whiteTexture_.get();
    const uint32_t lookupPos = static_cast<uint32_t>(b.textureIndex) + stage;
    if (lookupPos >= gm.data.textureLookup.size()) return whiteTexture_.get();
    const uint16_t texSlot = gm.data.textureLookup[lookupPos];
    if (texSlot >= gm.textureIds.size()) return whiteTexture_.get();
    VkTexture* tex = gm.textureIds[texSlot];
    auto itO = inst.textureSlotOverrides.find(texSlot);
    if (itO != inst.textureSlotOverrides.end() && itO->second != nullptr) tex = itO->second;
    return tex ? tex : whiteTexture_.get();
}

bool CharacterRenderer::getInstancePose(uint32_t instanceId, glm::mat4& model,
                                        const std::vector<glm::mat4>*& bones, int& sequenceIndex,
                                        float& animationTimeMs, float& globalTimeMs) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    const auto& inst = it->second;
    model = inst.hasOverrideModelMatrix ? inst.overrideModelMatrix : getModelMatrix(inst);
    bones = &inst.boneMatrices;
    sequenceIndex = inst.currentSequenceIndex;
    // The time its tracks are read at: from the end for a sequence that
    // stands in reversed (0x00826350).
    animationTimeMs = inst.cachedModel ? sampledTime(inst.cachedModel->data, inst.currentSequenceIndex,
                                                     inst.animationTime, inst.playMode)
                                       : inst.animationTime;
    globalTimeMs = inst.globalSequenceTime;
    return true;
}

void CharacterRenderer::collectAttachedInstances(std::vector<AttachedInstance>& out) const {
    out.clear();
    for (const auto& [id, inst] : instances) {
        if (inst.weaponAttachments.empty()) continue;
        const float wearerAlpha = inst.visible ? inst.opacity * inst.kitAlpha * inst.cameraAlpha : 0.0f;
        auto add = [&](uint32_t attachedId) {
            auto it = instances.find(attachedId);
            if (it == instances.end()) return;
            out.push_back({.instanceId = attachedId,
                           .modelId = it->second.modelId,
                           .alpha = it->second.visible ? wearerAlpha : 0.0f});
        };
        for (const auto& wa : inst.weaponAttachments) {
            add(wa.weaponInstanceId);
            for (const auto& fx : wa.effects) add(fx.effectInstanceId);
        }
    }
}

glm::mat4 CharacterRenderer::getModelMatrix(const CharacterInstance& instance) const {
    glm::mat4 model = glm::mat4(1.0f);

    // Apply transformations: T * R * S
    model = glm::translate(model, instance.position + instance.renderOffset);

    // Apply rotation (euler angles, Z-up)
    // Convention: yaw around Z, pitch around X, roll around Y.
    model = glm::rotate(model, instance.rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));  // Yaw
    model = glm::rotate(model, instance.rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));  // Pitch
    model = glm::rotate(model, instance.rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));  // Roll
    if (instance.tiltBlendWeight > 0.0f) {
        // 0x0082dd80: the matrix and the fully tilted one, weighted and
        // summed element by element (0x004c2120, 0x004c1e60).
        glm::mat4 full = glm::translate(glm::mat4(1.0f), instance.position + instance.renderOffset);
        full = glm::rotate(full, instance.tiltBlendRotation.z, glm::vec3(0.0f, 0.0f, 1.0f));
        full = glm::rotate(full, instance.tiltBlendRotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
        full = glm::rotate(full, instance.tiltBlendRotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
        const float w = std::min(instance.tiltBlendWeight, 1.0f);
        model = model * (1.0f - w) + full * w;
    }

    model = glm::scale(model, glm::vec3(instance.scale));

    return model;
}

void CharacterRenderer::setInstancePosition(uint32_t instanceId, const glm::vec3& position) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        it->second.position = position;
        it->second.moveStart = position;
        it->second.moveEnd = position;
        it->second.moveElapsed = 0.0f;
        it->second.moveDuration = 0.0f;
        it->second.isMoving = false;
    }
}

std::optional<glm::vec3> CharacterRenderer::getInstanceRotation(uint32_t instanceId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return std::nullopt;
    return it->second.rotation;
}

void CharacterRenderer::setInstanceHoldsLastFrame(uint32_t instanceId, bool holds) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) it->second.holdsLastFrame = holds;
}

void CharacterRenderer::setInstanceTiltBlend(uint32_t instanceId, const glm::vec3& fullRotation, float weight) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    it->second.tiltBlendRotation = fullRotation;
    it->second.tiltBlendWeight = weight > 0.0f ? weight : 0.0f;
}

float CharacterRenderer::getInstancePlaybackRate(uint32_t instanceId) const {
    auto it = instances.find(instanceId);
    return it != instances.end() ? it->second.playbackRate : 1.0f;
}

void CharacterRenderer::setInstanceRotation(uint32_t instanceId, const glm::vec3& rotation) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        it->second.rotation = rotation;
    }
}

void CharacterRenderer::setInstanceTorsoYaw(uint32_t instanceId, float spineYawRad, float headYawRad) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        it->second.torsoYawOverrideRad = spineYawRad;
        it->second.headYawOverrideRad = headYawRad;
    }
}

void CharacterRenderer::moveInstanceTo(uint32_t instanceId, const glm::vec3& destination, float durationSeconds) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;

    auto& inst = it->second;

    // Don't move dead instances (corpses shouldn't slide around)
    if (inst.isDead) return;

    // Movement only. Which animation a moving unit plays - Walk, Run, Swim,
    // Fly, a rider's Mount - is the per-frame sync's choice, from the unit's
    // movement flags. Choosing Walk or Run here as well, from this segment's
    // apparent speed, replaced a remote player's Swim and Fly with Run on
    // every frame it moved, and its rider's Mount pose with Run, which the
    // sync then put back - restarting the pose each frame.
    float pdx = destination.x - inst.position.x;
    float pdy = destination.y - inst.position.y;
    float planarDistSq = pdx * pdx + pdy * pdy;
    if (durationSeconds <= 0.0f) {
        if (planarDistSq < 1e-4f) {
            // Stop at current location.
            inst.position = destination;
            inst.isMoving = false;
            return;
        }
        // Some cores send movement-only deltas without spline duration.
        // Synthesize a tiny duration so movement still updates.
        durationSeconds = std::clamp(std::sqrt(planarDistSq) / 7.0f, 0.05f, 0.20f);
    }

    inst.moveStart = inst.position;
    inst.moveEnd = destination;
    inst.moveDuration = durationSeconds;
    inst.moveElapsed = 0.0f;
    inst.isMoving = true;

    // Face toward destination (yaw around Z axis since Z is up)
    glm::vec3 dir = destination - inst.position;
    if (dir.x * dir.x + dir.y * dir.y > 1e-6f) {
        float angle = std::atan2(dir.y, dir.x);
        inst.rotation.z = angle;
    }
}

const pipeline::M2Model* CharacterRenderer::getModelData(uint32_t modelId) const {
    auto it = models.find(modelId);
    if (it == models.end()) return nullptr;
    return &it->second.data;
}

const pipeline::M2Model* CharacterRenderer::getInstanceModelData(uint32_t instanceId) const {
    auto instIt = instances.find(instanceId);
    if (instIt == instances.end()) return nullptr;
    return getModelData(instIt->second.modelId);
}

void CharacterRenderer::startFadeIn(uint32_t instanceId, float durationSeconds) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    it->second.opacity = 0.0f;
    it->second.fadeInTime = 0.0f;
    it->second.fadeInDuration = durationSeconds;
}

void CharacterRenderer::setInstanceScale(uint32_t instanceId, float scale) {
    // Attachments take the parent's model matrix each frame, so they follow.
    auto it = instances.find(instanceId);
    if (it != instances.end() && scale > 0.0f) it->second.scale = scale;
}

void CharacterRenderer::setInstanceDiffuseColour(uint32_t instanceId, const glm::vec3& colour) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) it->second.diffuseColour = colour;
}

bool CharacterRenderer::setInstanceAnimationFrozen(uint32_t instanceId, bool frozen, std::optional<float> atMs) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    CharacterInstance& inst = it->second;
    const bool was = inst.animationFrozen;
    inst.animationFrozen = frozen;
    if (frozen && atMs && inst.cachedModel && inst.currentSequenceIndex >= 0 &&
        static_cast<size_t>(inst.currentSequenceIndex) < inst.cachedModel->data.sequences.size()) {
        const auto duration =
            static_cast<float>(inst.cachedModel->data.sequences[inst.currentSequenceIndex].duration);
        inst.animationTime = std::min(*atMs, duration);
        inst.eventsRestart = true;
        inst.eventsFrom = inst.animationTime;
    }
    return was;
}

void CharacterRenderer::setInstanceKitAlpha(uint32_t instanceId, float alpha, float seconds) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    auto fadeTo = [&](CharacterInstance& inst) {
        // 0x00744030: as bytes; already there, or no time, is at once.
        const long now = std::lround(inst.kitAlpha * 255.0f);
        if (std::lround(alpha * 255.0f) == now || !(seconds > 0.0f)) {
            inst.kitAlpha = inst.kitAlphaTo = alpha;
            inst.kitAlphaSeconds = 0.0f;
            return;
        }
        inst.kitAlphaFrom = inst.kitAlpha;
        inst.kitAlphaTo = alpha;
        inst.kitAlphaElapsed = 0.0f;
        inst.kitAlphaSeconds = seconds;
    };
    fadeTo(it->second);
    for (const auto& attachment : it->second.weaponAttachments) {
        if (auto weaponIt = instances.find(attachment.weaponInstanceId); weaponIt != instances.end())
            fadeTo(weaponIt->second);
        for (const auto& fx : attachment.effects) {
            if (auto fxIt = instances.find(fx.effectInstanceId); fxIt != instances.end()) fadeTo(fxIt->second);
        }
    }
}

void CharacterRenderer::setInstanceCameraAlpha(uint32_t instanceId, float alpha) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    const float a = std::clamp(alpha, 0.0f, 1.0f);
    it->second.cameraAlpha = a;
    for (const auto& attachment : it->second.weaponAttachments) {
        if (auto weaponIt = instances.find(attachment.weaponInstanceId); weaponIt != instances.end())
            weaponIt->second.cameraAlpha = a;
        for (const auto& fx : attachment.effects) {
            if (auto fxIt = instances.find(fx.effectInstanceId); fxIt != instances.end()) fxIt->second.cameraAlpha = a;
        }
    }
}

void CharacterRenderer::setInstanceOpacity(uint32_t instanceId, float opacity) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        const float clampedOpacity = std::clamp(opacity, 0.0f, 1.0f);
        it->second.opacity = clampedOpacity;
        // Cancel any fade-in in progress to avoid overwriting the new opacity
        it->second.fadeInDuration = 0.0f;

        // Equipment is rendered as independent character instances. Keep the
        // whole visual together instead of leaving opaque weapons floating on
        // a translucent stealthed creature.
        for (const auto& attachment : it->second.weaponAttachments) {
            auto weaponIt = instances.find(attachment.weaponInstanceId);
            if (weaponIt != instances.end()) {
                weaponIt->second.opacity = clampedOpacity;
                weaponIt->second.fadeInDuration = 0.0f;
            }
            for (const auto& effect : attachment.effects) {
                auto effectIt = instances.find(effect.effectInstanceId);
                if (effectIt != instances.end()) {
                    effectIt->second.opacity = clampedOpacity;
                    effectIt->second.fadeInDuration = 0.0f;
                }
            }
        }
    }
}

void CharacterRenderer::setActiveGeosets(uint32_t instanceId, const std::unordered_set<uint16_t>& geosets) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        it->second.activeGeosets = geosets;
        ++it->second.rtAppearanceSerial;
    }
}

void CharacterRenderer::setDrawSkinExtra(uint32_t instanceId, bool enabled) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        it->second.drawSkinExtra = enabled;
        ++it->second.rtAppearanceSerial;
    }
}

void CharacterRenderer::setGroupTextureOverride(uint32_t instanceId, uint16_t geosetGroup, VkTexture* texture) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        it->second.groupTextureOverrides[geosetGroup] = texture;
        ++it->second.rtAppearanceSerial;
    }
}

void CharacterRenderer::setTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot, VkTexture* texture) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        it->second.textureSlotOverrides[textureSlot] = texture;
        ++it->second.rtAppearanceSerial;
    }
}

void CharacterRenderer::clearTextureSlotOverride(uint32_t instanceId, uint16_t textureSlot) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        it->second.textureSlotOverrides.erase(textureSlot);
        ++it->second.rtAppearanceSerial;
    }
}

void CharacterRenderer::refreshInteriorLights(const FloorQuery& floorAt, const InteriorPassQuery& inInteriorPass,
                                              const glm::vec3& zoneAmbient, const glm::vec3& zoneDirect,
                                              float seconds, uint64_t floorGeneration) {
    namespace wl = pipeline::wmo_doodad_light;
    const auto toBytes = [](const glm::vec3& c) {
        return glm::ivec3(glm::round(glm::clamp(c, 0.0f, 1.0f) * 255.0f));
    };
    // What a unit holds and wears - weapons, helm, shoulders, their effects -
    // is lit as the unit is, and takes its light after the loop. Asked for
    // themselves they each queried the floor every frame, their hand-carried
    // positions never holding still: four or five WMO raycasts a frame for
    // every armed NPC in a crowd.
    ++lightRefreshFrame_;
    attachedScratch_.clear();
    for (const auto& [id, instance] : instances) {
        for (const auto& wa : instance.weaponAttachments) {
            attachedScratch_.insert(wa.weaponInstanceId);
            for (const auto& fx : wa.effects) attachedScratch_.insert(fx.effectInstanceId);
        }
    }
    for (auto& [id, instance] : instances) {
        if (attachedScratch_.count(id)) continue;
        const glm::vec3 feet = instance.hasOverrideModelMatrix
            ? glm::vec3(instance.overrideModelMatrix[3])
            : instance.position;
        const glm::vec3 d = feet - instance.interiorQueryPos;
        // A unit on the move asks for its floor every fourth frame, a
        // quarter of them each frame: the light it takes from it eases in
        // over many frames anyway, and in a crowd every walker asking every
        // frame was a few hundred rays through the city.
        const bool itsTurn = ((id + lightRefreshFrame_) & 3u) == 0;
        // Asked again, standing still or not, once the buildings have
        // changed: an NPC that spawned before the building it stands in was
        // loaded found no floor, kept the zone's light for good - at night a
        // dark blue figure in a lamplit room - and never moved to ask again.
        const bool buildingsChanged = instance.floorGeneration != floorGeneration;
        if (!instance.interiorQueried || ((glm::dot(d, d) >= 0.25f * 0.25f || buildingsChanged) && itsTurn)) {
            instance.interiorQueried = true;
            instance.interiorQueryPos = feet;
            instance.floorGeneration = floorGeneration;
            instance.floor = floorAt(feet);
        }
        const auto& floorLight = instance.floor.light;

        // The ambient it should have: the floor's, carried toward the zone's
        // on a transition face, or the zone's off any interior floor. The
        // one drawn moves toward it a step a frame (0x007a1e90); a new
        // instance starts at it.
        wl::ObjectLight light{zoneAmbient, zoneDirect, 0.0f};
        if (floorLight) light = wl::floorLightInZone(*floorLight, zoneAmbient, zoneDirect);
        const glm::ivec3 target = toBytes(light.ambient);
        instance.easedAmbient = instance.lightKnown ? wl::easeAmbient(instance.easedAmbient, target, seconds)
                                                    : target;
        // The direct light's scale, toward half in the terrain's baked
        // shadow and whole elsewhere (0x007a1bc0, 0x007a1e90).
        const float scaleTarget = wl::directScaleTarget(floorLight.has_value(),
                                                        floorLight ? floorLight->towardOutside : 0,
                                                        instance.floor.inBakedShadow && !instance.floor.onWmo);
        instance.directScale = wl::easeDirectScale(instance.directScale, scaleTarget, seconds);
        instance.lightKnown = true;
        instance.drawAmbient = glm::vec4(glm::vec3(instance.easedAmbient) / 255.0f,
                                         floorLight ? 2.0f : 1.0f);
        instance.drawDirect = glm::vec4(light.direct, light.towardSun);
        instance.drawFlags = glm::vec4(inInteriorPass && inInteriorPass(feet) ? 1.0f : 0.0f,
                                       instance.directScale, 0.0f, 0.0f);
    }
    for (const auto& [id, instance] : instances) {
        if (instance.weaponAttachments.empty() || attachedScratch_.count(id)) continue;
        auto lightLike = [&](uint32_t childId) {
            auto childIt = instances.find(childId);
            if (childIt == instances.end()) return;
            auto& child = childIt->second;
            child.floor = instance.floor;
            child.easedAmbient = instance.easedAmbient;
            child.directScale = instance.directScale;
            child.lightKnown = instance.lightKnown;
            child.drawAmbient = instance.drawAmbient;
            child.drawDirect = instance.drawDirect;
            child.drawFlags = instance.drawFlags;
        };
        for (const auto& wa : instance.weaponAttachments) {
            lightLike(wa.weaponInstanceId);
            for (const auto& fx : wa.effects) lightLike(fx.effectInstanceId);
        }
    }
}

void CharacterRenderer::setInstanceVisible(uint32_t instanceId, bool visible) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) {
        if (it->second.visible != visible) {
            LOG_INFO("CharacterRenderer::setInstanceVisible id=", instanceId, " visible=", visible);
        }
        it->second.visible = visible;

        // Also hide/show attached weapons (for first-person mode)
        for (const auto& wa : it->second.weaponAttachments) {
            auto weapIt = instances.find(wa.weaponInstanceId);
            if (weapIt != instances.end()) {
                weapIt->second.visible = visible;
            }
        }
    }
}

void CharacterRenderer::setInstanceHeldBack(uint32_t instanceId, bool heldBack) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;
    it->second.heldBack = heldBack;
    // What it holds goes with it, as setInstanceVisible.
    for (const auto& wa : it->second.weaponAttachments) {
        auto weapIt = instances.find(wa.weaponInstanceId);
        if (weapIt != instances.end()) weapIt->second.heldBack = heldBack;
    }
}

bool CharacterRenderer::isInstancePosed(uint32_t instanceId) const {
    auto it = instances.find(instanceId);
    return it != instances.end() && it->second.posedOnce;
}

void CharacterRenderer::removeInstance(uint32_t instanceId) {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return;

    LOG_INFO("CharacterRenderer::removeInstance id=", instanceId,
             " pos=(", it->second.position.x, ",", it->second.position.y, ",", it->second.position.z, ")",
             " remaining=", instances.size() - 1,
             " override=", (void*)renderPassOverride_);

    // Remove child attachments first (helmets/weapons/enchant effects),
    // otherwise they leak as orphan render instances when the parent creature
    // despawns. Their models get a fresh id per attach, so unload those too
    // once the instances are gone.
    auto attachments = it->second.weaponAttachments;
    for (const auto& wa : attachments) {
        for (const auto& fx : wa.effects) removeInstance(fx.effectInstanceId);
        removeInstance(wa.weaponInstanceId);
    }
    for (const auto& wa : attachments) {
        unloadModelIfUnused(wa.weaponModelId);
        for (const auto& fx : wa.effects) unloadModelIfUnused(fx.effectModelId);
    }

    // Defer bone buffer destruction - in-flight command buffers may still
    // reference these descriptor sets.
    destroyInstanceBones(it->second, /*defer=*/true);

    instances.erase(it);
}

bool CharacterRenderer::getAnimationState(uint32_t instanceId, uint32_t& animationId,
                                          float& animationTimeMs, float& animationDurationMs) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) {
        return false;
    }

    const CharacterInstance& instance = it->second;
    auto modelIt = models.find(instance.modelId);
    if (modelIt == models.end()) {
        return false;
    }

    const auto& sequences = modelIt->second.data.sequences;
    if (instance.currentSequenceIndex < 0 || instance.currentSequenceIndex >= static_cast<int>(sequences.size())) {
        return false;
    }

    animationId = instance.currentAnimationId;
    animationTimeMs = instance.animationTime;
    animationDurationMs = static_cast<float>(sequences[instance.currentSequenceIndex].duration);
    return true;
}

void CharacterRenderer::stepInstanceEvents(uint32_t instanceId, const CharacterInstance& inst, int sequence,
                                           float from, float to) {
    // A unit's model: not a login scene's, nor an effect hung on a unit.
    if (!inst.cachedModel || inst.isSceneModel || inst.isEffectModel) return;
    const auto& model = inst.cachedModel->data;
    if (model.events.empty() || sequence < 0 || static_cast<size_t>(sequence) >= model.sequences.size()) return;
    for (uint32_t e = 0; e < model.events.size(); ++e) {
        const auto& event = model.events[e];
        // Only what a unit's callback plays. An event on a global sequence
        // reads the first sequence's times, on the playing sequence's clock
        // as any other (0x00830fb0: its track index is 0 where the track has
        // a global sequence).
        if (unit_event::classify(event.id).kind == unit_event::Kind::None) continue;
        const size_t timeline = event.globalSequence >= 0 ? 0u : static_cast<size_t>(sequence);
        if (timeline >= event.times.size()) continue;
        const auto fire = [&](uint32_t) { steppedEvents_.push_back({instanceId, e}); };
        // A sequence standing in reversed passes them from its end
        // (0x00830fb0 at the negative rate 0x00826350 gives it).
        if (inst.playMode == anim::PlayMode::Reversed && sequence == inst.currentSequenceIndex) {
            m2_event::forEachCrossedReversed(event.times[timeline], model.sequences[sequence].duration, from, to, 0.0f,
                                             fire);
        } else {
            m2_event::forEachCrossed(event.times[timeline], from, to, 0.0f, fire);
        }
    }
}

std::vector<CharacterRenderer::FiredEvent> CharacterRenderer::takeFiredEvents() {
    std::vector<FiredEvent> out;
    out.swap(firedEvents_);
    return out;
}

const std::vector<uint32_t>* CharacterRenderer::getFootstepEventTimes(uint32_t instanceId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) {
        return nullptr;
    }

    const CharacterInstance& instance = it->second;
    auto modelIt = models.find(instance.modelId);
    if (modelIt == models.end()) {
        return nullptr;
    }

    const auto& eventTimes = modelIt->second.data.footstepEventTimes;
    if (instance.currentSequenceIndex < 0 ||
        instance.currentSequenceIndex >= static_cast<int>(eventTimes.size()) ||
        eventTimes[instance.currentSequenceIndex].empty()) {
        return nullptr;
    }
    return &eventTimes[instance.currentSequenceIndex];
}

bool CharacterRenderer::hasAnimation(uint32_t instanceId, uint32_t animationId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) {
        return false;
    }

    auto modelIt = models.find(it->second.modelId);
    if (modelIt == models.end()) {
        return false;
    }

    const auto& sequences = modelIt->second.data.sequences;
    for (const auto& seq : sequences) {
        if (seq.id == animationId) {
            return true;
        }
    }
    return false;
}

bool CharacterRenderer::getAnimationSequences(uint32_t instanceId, std::vector<pipeline::M2Sequence>& out) const {
    out.clear();
    auto it = instances.find(instanceId);
    if (it == instances.end()) {
        return false;
    }

    auto modelIt = models.find(it->second.modelId);
    if (modelIt == models.end()) {
        return false;
    }

    out = modelIt->second.data.sequences;
    return !out.empty();
}

bool CharacterRenderer::getInstanceModelName(uint32_t instanceId, std::string& modelName) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) {
        return false;
    }
    auto modelIt = models.find(it->second.modelId);
    if (modelIt == models.end()) {
        return false;
    }
    modelName = modelIt->second.data.name;
    return !modelName.empty();
}

bool CharacterRenderer::findAttachmentBone(uint32_t modelId, uint32_t attachmentId,
                                          uint16_t& outBoneIndex, glm::vec3& outOffset) const {
    auto modelIt = models.find(modelId);
    if (modelIt == models.end()) return false;
    const auto& model = modelIt->second.data;

    outBoneIndex = 0;
    outOffset = glm::vec3(0.0f);
    bool found = false;

    // Try attachment lookup first
    if (attachmentId < model.attachmentLookup.size()) {
        uint16_t attIdx = model.attachmentLookup[attachmentId];
        if (attIdx < model.attachments.size()) {
            outBoneIndex = model.attachments[attIdx].bone;
            outOffset = model.attachments[attIdx].position;
            found = true;
        }
    }

    // Fallback: scan attachments by id
    if (!found) {
        for (const auto& att : model.attachments) {
            if (att.id == attachmentId) {
                outBoneIndex = att.bone;
                outOffset = att.position;
                found = true;
                break;
            }
        }
    }

    // Fallback: key-bone lookup for weapon hand attachment IDs (ID 1 = right hand, ID 2 = left hand)
    if (!found && (attachmentId == 1 || attachmentId == 2)) {
        int32_t targetKeyBone = (attachmentId == 1) ? 26 : 27;
        for (size_t i = 0; i < model.bones.size(); i++) {
            if (model.bones[i].keyBoneId == targetKeyBone) {
                outBoneIndex = static_cast<uint16_t>(i);
                outOffset = glm::vec3(0.0f);
                found = true;
                break;
            }
        }
    }

    // Fallback for head attachment (ID 11): use bone 0 if attachment not defined
    if (!found && attachmentId == 11 && !model.bones.empty()) {
        outBoneIndex = 0;
        found = true;
    }

    // Validate bone index
    if (found && outBoneIndex >= model.bones.size()) {
        found = false;
    }

    return found;
}

bool CharacterRenderer::attachWeapon(uint32_t charInstanceId, uint32_t attachmentId,
                                      const pipeline::M2Model& weaponModel, uint32_t weaponModelId,
                                      const std::string& texturePath,
                                      const glm::mat4& localTransform) {
    auto charIt = instances.find(charInstanceId);
    if (charIt == instances.end()) {
        core::Logger::getInstance().warning("attachWeapon: character instance ", charInstanceId, " not found");
        return false;
    }
    auto& charInstance = charIt->second;
    auto charModelIt = models.find(charInstance.modelId);
    if (charModelIt == models.end()) return false;

    // Find bone index for this attachment point
    uint16_t boneIndex = 0;
    glm::vec3 offset(0.0f);
    if (!findAttachmentBone(charInstance.modelId, attachmentId, boneIndex, offset)) {
        core::Logger::getInstance().warning("attachWeapon: no bone found for attachment ", attachmentId);
        return false;
    }

    // Remove existing weapon at this attachment point
    detachWeapon(charInstanceId, attachmentId);

    // Load weapon model into renderer
    if (models.find(weaponModelId) == models.end()) {
        if (!loadModel(weaponModel, weaponModelId)) {
            core::Logger::getInstance().warning("attachWeapon: failed to load weapon model ", weaponModelId);
            return false;
        }
    }

    // Apply weapon texture if provided
    if (!texturePath.empty()) {
        VkTexture* texPtr = loadTexture(texturePath);
        if (texPtr != whiteTexture_.get()) {
            // Item models can keep an authored hilt texture in slot 0 and expose
            // the DBC-selected skin through one or more replaceable slots:
            // 2 = object skin, 3 = weapon blade, 4 = weapon handle.  Applying the
            // skin to slot 0 alone leaves multi-material weapons with an untextured
            // blade (notably Melris Malagan's sword).
            bool appliedReplaceableSlot = false;
            auto modelIt = models.find(weaponModelId);
            if (modelIt != models.end()) {
                const auto& textures = modelIt->second.data.textures;
                for (uint32_t slot = 0; slot < textures.size(); ++slot) {
                    const uint32_t type = textures[slot].type;
                    if (type == 2 || type == 3 || type == 4) {
                        setModelTexture(weaponModelId, slot, texPtr);
                        appliedReplaceableSlot = true;
                    }
                }
            }
            // Older/simple item models commonly expose only one unnamed slot.
            if (!appliedReplaceableSlot) {
                setModelTexture(weaponModelId, 0, texPtr);
            }
        }
    }

    // Create weapon instance
    uint32_t weaponInstanceId = createInstance(weaponModelId, glm::vec3(0.0f));
    if (weaponInstanceId == 0) return false;

    // Mark weapon instance as override-positioned
    auto weapIt = instances.find(weaponInstanceId);
    if (weapIt != instances.end()) {
        weapIt->second.hasOverrideModelMatrix = true;
        weapIt->second.opacity = charInstance.opacity;
        weapIt->second.kitAlpha = weapIt->second.kitAlphaTo = charInstance.kitAlpha;
        weapIt->second.cameraAlpha = charInstance.cameraAlpha;
    }

    // Store attachment on parent character instance
    WeaponAttachment wa;
    wa.weaponModelId = weaponModelId;
    wa.weaponInstanceId = weaponInstanceId;
    wa.attachmentId = attachmentId;
    wa.boneIndex = boneIndex;
    wa.offset = offset;
    wa.localTransform = localTransform;
    charInstance.weaponAttachments.push_back(wa);

    core::Logger::getInstance().debug("Attached weapon model ", weaponModelId,
        " to instance ", charInstanceId, " at attachment ", attachmentId,
        " (bone ", boneIndex, ", offset ", offset.x, ",", offset.y, ",", offset.z, ")");
    return true;
}

bool CharacterRenderer::getInstanceBounds(uint32_t instanceId, glm::vec3& outCenter, float& outRadius) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto mIt = models.find(it->second.modelId);
    if (mIt == models.end()) return false;

    const auto& inst = it->second;
    const auto& model = mIt->second.data;

    glm::vec3 boundsMin = mIt->second.visualBoundMin;
    glm::vec3 boundsMax = mIt->second.visualBoundMax;
    float radius = mIt->second.visualBoundRadius;
    if (radius <= 0.001f) {
        boundsMin = model.boundMin;
        boundsMax = model.boundMax;
        radius = model.boundRadius;
        if (radius <= 0.001f) {
            radius = glm::length(boundsMax - boundsMin) * 0.5f;
        }
    }

    float scale = std::max(0.001f, inst.scale);
    const glm::vec3 localCenter = (boundsMin + boundsMax) * 0.5f;
    glm::mat4 rotation(1.0f);
    rotation = glm::rotate(rotation, inst.rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));
    rotation = glm::rotate(rotation, inst.rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
    rotation = glm::rotate(rotation, inst.rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
    outCenter = inst.position + glm::vec3(rotation * glm::vec4(localCenter * scale, 0.0f));
    outRadius = std::max(0.5f, radius * scale);
    return true;
}

bool CharacterRenderer::getInstanceKeyBonePivotZ(uint32_t instanceId, int32_t keyBoneId,
                                                 float& outZ) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto mIt = models.find(it->second.modelId);
    if (mIt == models.end()) return false;

    for (const auto& bone : mIt->second.data.bones) {
        if (bone.keyBoneId != keyBoneId) continue;
        outZ = bone.pivot.z * std::max(0.001f, it->second.scale);
        return true;
    }
    return false;
}

bool CharacterRenderer::getInstanceHeight(uint32_t instanceId, float& outHeight) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    auto mIt = models.find(it->second.modelId);
    if (mIt == models.end()) return false;

    // The tight bind-pose bounds, which is where the top of the head is. The
    // M2 header's own box describes collision on a good many models and can
    // come out shorter than the thing it encloses, so it is the fallback.
    float top = mIt->second.visualBoundMax.z;
    if (top <= 0.001f) top = mIt->second.data.boundMax.z;
    if (top <= 0.001f) return false;

    outHeight = top * std::max(0.001f, it->second.scale);
    return true;
}

bool CharacterRenderer::getInstancePosition(uint32_t instanceId, glm::vec3& outPos) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    outPos = it->second.position;
    return true;
}

void CharacterRenderer::setInstanceBlobShadow(uint32_t instanceId,
                                              const std::optional<blob_shadow::Box>& box) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) it->second.blobShadow = box;
}

std::optional<blob_shadow::Box> CharacterRenderer::instanceSequenceBounds(uint32_t instanceId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return std::nullopt;
    auto m = models.find(it->second.modelId);
    if (m == models.end()) return std::nullopt;
    const auto& seqs = m->second.data.sequences;
    const int si = it->second.currentSequenceIndex;
    // No sequence: 0x0082ced0 gives an empty box.
    if (si < 0 || static_cast<size_t>(si) >= seqs.size()) return blob_shadow::Box{};
    return blob_shadow::Box{.min = seqs[static_cast<size_t>(si)].boundMin,
                            .max = seqs[static_cast<size_t>(si)].boundMax};
}

void CharacterRenderer::collectBlobShadows(std::vector<blob_shadow::Caster>& out) const {
    for (const auto& [id, inst] : instances) {
        (void)id;
        if (!inst.blobShadow || !inst.visible || inst.heldBack) continue;
        if (!inst.posedOnce && !inst.hasOverrideModelMatrix) continue;
        blob_shadow::Caster c;
        c.world = inst.hasOverrideModelMatrix ? inst.overrideModelMatrix : getModelMatrix(inst);
        c.box = *inst.blobShadow;
        c.alpha = inst.opacity * inst.kitAlpha * inst.cameraAlpha;
        out.push_back(c);
    }
}

bool CharacterRenderer::getInstanceFrame(uint32_t instanceId, glm::mat4& outFrame) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    outFrame = it->second.hasOverrideModelMatrix ? it->second.overrideModelMatrix
                                                 : getModelMatrix(it->second);
    return true;
}

std::optional<uint32_t> CharacterRenderer::getAnimationEventTime(uint32_t instanceId, uint32_t animationId,
                                                                uint32_t eventId) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return std::nullopt;
    auto modelIt = models.find(it->second.modelId);
    if (modelIt == models.end()) return std::nullopt;
    const pipeline::M2Model& model = modelIt->second.data;
    int sequence = -1;
    for (size_t i = 0; i < model.sequences.size(); ++i) {
        if (model.sequences[i].id != animationId) continue;
        if (sequence < 0 || model.sequences[i].variationIndex == 0) sequence = static_cast<int>(i);
        if (model.sequences[i].variationIndex == 0) break;
    }
    const auto* event = model.findEvent(eventId);
    if (sequence < 0 || !event || static_cast<size_t>(sequence) >= event->times.size() ||
        event->times[static_cast<size_t>(sequence)].empty())
        return 0u;
    return event->times[static_cast<size_t>(sequence)].front();
}

void CharacterRenderer::setInstanceRenderOffset(uint32_t instanceId, const glm::vec3& offset) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) it->second.renderOffset = offset;
}

bool CharacterRenderer::getEventPosition(uint32_t instanceId, uint32_t eventId, glm::vec3& out) const {
    auto it = instances.find(instanceId);
    if (it == instances.end()) return false;
    const auto& instance = it->second;
    auto modelIt = models.find(instance.modelId);
    if (modelIt == models.end()) return false;
    const auto* event = modelIt->second.data.findEvent(eventId);
    if (!event) return false;
    glm::vec4 p(event->position, 1.0f);
    if (event->bone < instance.boneMatrices.size()) p = instance.boneMatrices[event->bone] * p;
    const glm::mat4 placement =
        instance.hasOverrideModelMatrix ? instance.overrideModelMatrix : getModelMatrix(instance);
    out = glm::vec3(placement * p);
    return true;
}

void CharacterRenderer::detachWeapon(uint32_t charInstanceId, uint32_t attachmentId) {
    auto charIt = instances.find(charInstanceId);
    if (charIt == instances.end()) return;
    auto& attachments = charIt->second.weaponAttachments;

    for (auto it = attachments.begin(); it != attachments.end(); ++it) {
        if (it->attachmentId == attachmentId) {
            // Collect model ids before erasing the attachment, then unload the
            // ones no instance still uses (each attach allocates a fresh id).
            std::vector<uint32_t> modelIds;
            modelIds.push_back(it->weaponModelId);
            for (const auto& fx : it->effects) {
                removeInstance(fx.effectInstanceId);
                modelIds.push_back(fx.effectModelId);
            }
            removeInstance(it->weaponInstanceId);
            attachments.erase(it);
            for (uint32_t mid : modelIds) unloadModelIfUnused(mid);
            core::Logger::getInstance().info("Detached weapon from instance ", charInstanceId,
                " attachment ", attachmentId);
            return;
        }
    }
}

bool CharacterRenderer::hasTexture(const std::string& path) const {
    return textureCache.find(normalizeTexturePathKey(path)) != textureCache.end();
}

uint32_t CharacterRenderer::sharedItemModelId(const std::string& key) const {
    const auto it = sharedItemModels_.find(key);
    if (it == sharedItemModels_.end() || models.find(it->second) == models.end()) return 0;
    return it->second;
}

void CharacterRenderer::rememberSharedItemModel(const std::string& key, uint32_t modelId) {
    sharedItemModels_[key] = modelId;
    sharedItemModelIds_.insert(modelId);
}

void CharacterRenderer::unloadModelIfUnused(uint32_t modelId) {
    auto modelIt = models.find(modelId);
    if (modelIt == models.end()) return;
    // A shared item model stays for the next wearer. Re-dressing an NPC takes
    // its helm off before putting it back on, and unloading in between made
    // every re-dress load the model again. Bounded by the item models seen.
    if (sharedItemModelIds_.count(modelId)) return;
    for (const auto& p : instances) {
        if (p.second.modelId == modelId) return;
    }
    destroyModelGPU(modelIt->second, /*defer=*/true);
    models.erase(modelIt);
}

bool CharacterRenderer::attachWeaponEffect(uint32_t charInstanceId, uint32_t attachmentId,
                                           uint32_t visualSlot,
                                           const pipeline::M2Model& effectModel,
                                           uint32_t effectModelId) {
    auto charIt = instances.find(charInstanceId);
    if (charIt == instances.end()) return false;

    auto& attachments = charIt->second.weaponAttachments;
    auto waIt = std::find_if(attachments.begin(), attachments.end(),
                             [attachmentId](const WeaponAttachment& wa) {
                                 return wa.attachmentId == attachmentId;
                             });
    if (waIt == attachments.end()) return false;  // no weapon to hang the effect on

    if (models.find(effectModelId) == models.end()) {
        if (!loadModel(effectModel, effectModelId)) {
            core::Logger::getInstance().warning("attachWeaponEffect: failed to load effect model ",
                                                effectModelId);
            return false;
        }
    }

    // The ItemVisuals slot names the attachment point on the weapon model itself.
    // Weapons carry few attachments, so fall back to the weapon's origin.
    uint16_t boneIndex = 0;
    glm::vec3 offset(0.0f);
    if (!findAttachmentBone(waIt->weaponModelId, visualSlot, boneIndex, offset)) {
        offset = glm::vec3(0.0f);
    }

    uint32_t effectInstanceId = createInstance(effectModelId, glm::vec3(0.0f));
    if (effectInstanceId == 0) return false;

    auto fxIt = instances.find(effectInstanceId);
    if (fxIt == instances.end()) return false;
    fxIt->second.hasOverrideModelMatrix = true;
    fxIt->second.isEffectModel = true;
    fxIt->second.animationLoop = true;
    fxIt->second.opacity = charIt->second.opacity;
    fxIt->second.kitAlpha = fxIt->second.kitAlphaTo = charIt->second.kitAlpha;
    fxIt->second.cameraAlpha = charIt->second.cameraAlpha;

    WeaponEffectAttachment fx;
    fx.effectModelId = effectModelId;
    fx.effectInstanceId = effectInstanceId;
    fx.offset = offset;
    waIt->effects.push_back(fx);

    core::Logger::getInstance().debug("Attached enchant visual model ", effectModelId,
        " to weapon at attachment ", attachmentId, " (visual slot ", visualSlot, ")");
    return true;
}

void CharacterRenderer::setInstanceSceneModel(uint32_t instanceId, bool isScene) {
    auto it = instances.find(instanceId);
    if (it != instances.end()) it->second.isSceneModel = isScene;
}

void CharacterRenderer::detachWeaponEffects(uint32_t charInstanceId, uint32_t attachmentId) {
    auto charIt = instances.find(charInstanceId);
    if (charIt == instances.end()) return;

    for (auto& wa : charIt->second.weaponAttachments) {
        if (wa.attachmentId != attachmentId) continue;
        for (const auto& fx : wa.effects) removeInstance(fx.effectInstanceId);
        wa.effects.clear();
        return;
    }
}

bool CharacterRenderer::getAttachmentTransform(uint32_t instanceId, uint32_t attachmentId, glm::mat4& outTransform) const {
    auto instIt = instances.find(instanceId);
    if (instIt == instances.end()) return false;
    const auto& instance = instIt->second;

    // Find attachment point using shared lookup logic
    uint16_t boneIndex = 0;
    glm::vec3 offset(0.0f);
    if (!findAttachmentBone(instance.modelId, attachmentId, boneIndex, offset)) {
        return false;
    }

    // Get bone matrix
    glm::mat4 boneMat(1.0f);
    if (boneIndex < instance.boneMatrices.size()) {
        boneMat = instance.boneMatrices[boneIndex];
    }

    // Compute world transform: modelMatrix * boneMatrix * offsetTranslation
    glm::mat4 modelMat = instance.hasOverrideModelMatrix
        ? instance.overrideModelMatrix
        : getModelMatrix(instance);

    outTransform = modelMat * boneMat * glm::translate(glm::mat4(1.0f), offset);
    return true;
}

void CharacterRenderer::dumpAnimations(uint32_t instanceId) const {
    auto instIt = instances.find(instanceId);
    if (instIt == instances.end()) {
        core::Logger::getInstance().info("dumpAnimations: instance ", instanceId, " not found");
        return;
    }
    const auto& instance = instIt->second;

    auto modelIt = models.find(instance.modelId);
    if (modelIt == models.end()) {
        core::Logger::getInstance().info("dumpAnimations: model not found for instance ", instanceId);
        return;
    }
    const auto& model = modelIt->second.data;

    core::Logger::getInstance().info("=== Animation dump for ", model.name, " ===");
    core::Logger::getInstance().info("Total animations: ", model.sequences.size());

    for (size_t i = 0; i < model.sequences.size(); i++) {
        const auto& seq = model.sequences[i];
        core::Logger::getInstance().info("  [", i, "] animId=", seq.id,
            " variation=", seq.variationIndex,
            " duration=", seq.duration, "ms",
            " speed=", seq.movingSpeed,
            " flags=0x", std::hex, seq.flags, std::dec);
    }
    core::Logger::getInstance().info("=== End animation dump ===");
}

void CharacterRenderer::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    // Destroy old main-pass pipelines (NOT shadow, NOT pipeline layout)
    destroy(device, opaquePipeline_);
    destroy(device, alphaTestPipeline_);
    destroy(device, alphaPipeline_);
    destroy(device, additivePipeline_);
    destroy(device, noAlphaAddPipeline_);
    destroy(device, modPipeline_);
    destroy(device, mod2xPipeline_);
    destroyPipelineVariants();
    destroy(device, translucentPipeline_);

    // --- Load shaders ---
    rendering::VkShaderModule charVert, charFrag;
    if (!charVert.loadFromFile(device, "assets/shaders/character.vert.spv") ||
        !charFrag.loadFromFile(device, "assets/shaders/character.frag.spv")) {
        LOG_ERROR("CharacterRenderer::recreatePipelines: missing required shaders");
        return;
    }

    VkRenderPass mainPass = renderPassOverride_ ? renderPassOverride_ : vkCtx_->getImGuiRenderPass();
    VkSampleCountFlagBits samples = renderPassOverride_ ? msaaSamplesOverride_ : vkCtx_->getMsaaSamples();

    LOG_INFO("CharacterRenderer::recreatePipelines: renderPass=", (void*)mainPass,
             " samples=", static_cast<int>(samples),
             " pipelineLayout=", (void*)pipelineLayout_);

    buildMainPassPipelines(device, mainPass, samples, charVert, charFrag);

    charVert.destroy();
    charFrag.destroy();

    if (!opaquePipeline_ || !alphaTestPipeline_ || !alphaPipeline_ || !additivePipeline_ ||
        !translucentPipeline_) {
        LOG_ERROR("CharacterRenderer::recreatePipelines FAILED: opaque=", (void*)opaquePipeline_,
                  " alphaTest=", (void*)alphaTestPipeline_,
                  " alpha=", (void*)alphaPipeline_,
                  " additive=", (void*)additivePipeline_,
                  " renderPass=", (void*)mainPass, " samples=", static_cast<int>(samples));
    } else {
        LOG_INFO("CharacterRenderer: pipelines recreated successfully (samples=",
                 static_cast<int>(samples), ")");
    }
}

} // namespace rendering
} // namespace wowee
