#include "rendering/minimap.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_render_target.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/camera.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include <imgui.h>
#include <backends/imgui_impl_vulkan.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace wowee {
namespace rendering {

// Push constant for tile composite vertex shader
struct MinimapTilePush {
    glm::vec2 gridOffset;  // which cell of the grid, from its corner
    float gridSize;        // cells to a side
};

// Push constant for an indoor picture: its corners and their texture
// coordinates, corner 0 at the picture's low x and y, then round.
struct MinimapIndoorTilePush {
    glm::vec4 corners01;
    glm::vec4 corners23;
    glm::vec4 uv01;
    glm::vec4 uv23;
};  // 64 bytes

// Push constant for display vertex + fragment shaders
struct MinimapDisplayPush {
    glm::vec4 rect;         // x, y, w, h in 0..1 screen space
    glm::vec2 playerUV;
    float rotation;
    float zoomRadius;
    int32_t squareShape;
    float opacity;
    int32_t hasMask;  // Textures\MinimapMask is resident
};  // 44 bytes

Minimap::Minimap() = default;

Minimap::~Minimap() {
    shutdown();
}

/// The pipeline that draws the minimap's assembled texture to the screen.
///
/// initialize() builds it beside the tile pipeline that composes that texture;
/// recreatePipelines() rebuilds only this one, because the tile pipeline draws
/// into an offscreen pass a settings change does not touch. Both described it
/// identically, vertex layout included.
void Minimap::buildDisplayPipeline(VkDevice device,
                                   const VkPipelineShaderStageCreateInfo& vertStage,
                                   const VkPipelineShaderStageCreateInfo& fragStage) {
    // Two vec2s per vertex: position then texture coordinate.
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 4 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attrs(2);
    attrs[0] = { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 };
    attrs[1] = { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 2 * sizeof(float) };

    displayPipeline = PipelineBuilder()
        .setShaders(vertStage, fragStage)
        .setVertexInput({ binding }, attrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setNoDepthTest()
        .setColorBlendAttachment(PipelineBuilder::blendAlpha())
        .setMultisample(targetPass_ != VK_NULL_HANDLE ? targetSamples_
                                                      : vkCtx->getMsaaSamples())
        .setLayout(displayPipelineLayout)
        .setRenderPass(targetPass_ != VK_NULL_HANDLE ? targetPass_
                                                     : vkCtx->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx->getPipelineCache());
}

bool Minimap::initialize(VkContext* ctx, VkDescriptorSetLayout /*perFrameLayout*/, int size) {
    vkCtx = ctx;
    mapSize = size;
    VkDevice device = vkCtx->getDevice();

    // --- Composite render target (768x768) ---
    compositeTarget = std::make_unique<VkRenderTarget>();
    if (!compositeTarget->create(*vkCtx, COMPOSITE_PX, COMPOSITE_PX)) {
        LOG_ERROR("Minimap: failed to create composite render target");
        return false;
    }

    // --- No-data fallback texture (dark blue-gray, 1x1) ---
    noDataTexture = std::make_unique<VkTexture>();
    uint8_t darkPixel[4] = { 12, 20, 30, 255 };
    noDataTexture->upload(*vkCtx, darkPixel, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false);
    noDataTexture->createSampler(device, VK_FILTER_NEAREST, VK_FILTER_NEAREST,
                                 VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);

    // --- Shared quad vertex buffer (unit quad: pos2 + uv2) ---
    float quadVerts[] = {
        // pos (x,y), uv (u,v)
        0.0f, 0.0f,  0.0f, 0.0f,
        1.0f, 0.0f,  1.0f, 0.0f,
        1.0f, 1.0f,  1.0f, 1.0f,
        0.0f, 0.0f,  0.0f, 0.0f,
        1.0f, 1.0f,  1.0f, 1.0f,
        0.0f, 1.0f,  0.0f, 1.0f,
    };
    auto quadBuf = uploadBuffer(*vkCtx, quadVerts, sizeof(quadVerts),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    quadVB = quadBuf.buffer;
    quadVBAlloc = quadBuf.allocation;

    // --- Descriptor set layout: 1 combined image sampler at binding 0 (fragment) ---
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    samplerSetLayout = createDescriptorSetLayout(device, { samplerBinding });

    // --- Descriptor pool ---
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = MAX_DESC_SETS;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = MAX_DESC_SETS;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);

    // --- Allocate all descriptor sets ---
    // Two frames of GRID x GRID tile sets, and one display set
    constexpr uint32_t kTileSets = GRID * GRID;
    constexpr uint32_t kSetCount = 2 * kTileSets + 2;
    std::vector<VkDescriptorSetLayout> layouts(kSetCount, samplerSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descPool;
    allocInfo.descriptorSetCount = kSetCount;
    allocInfo.pSetLayouts = layouts.data();

    // Checked, not assumed. On failure allSets holds nothing this code put
    // there, and every one of the nineteen is then written and bound - which
    // is an invalid handle in a live descriptor rather than a minimap that
    // does not draw.
    VkDescriptorSet allSets[kSetCount];
    if (vkAllocateDescriptorSets(device, &allocInfo, allSets) != VK_SUCCESS) {
        LOG_ERROR("Minimap: failed to allocate descriptor sets");
        return false;
    }

    for (int f = 0; f < 2; f++)
        for (uint32_t t = 0; t < kTileSets; t++)
            tileDescSets[f][t] = allSets[f * kTileSets + t];
    displayDescSet = allSets[2 * kTileSets];
    maskDescSet = allSets[2 * kTileSets + 1];

    // --- Write display descriptor set → composite render target ---
    VkDescriptorImageInfo compositeImgInfo = compositeTarget->descriptorInfo();
    VkWriteDescriptorSet displayWrite{};
    displayWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    displayWrite.dstSet = displayDescSet;
    displayWrite.dstBinding = 0;
    displayWrite.descriptorCount = 1;
    displayWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    displayWrite.pImageInfo = &compositeImgInfo;
    vkUpdateDescriptorSets(device, 1, &displayWrite, 0, nullptr);
    // The mask's set holds something valid until the mask is read
    // (compositePass); hasMask says which.
    VkDescriptorImageInfo placeholderInfo = noDataTexture->descriptorInfo();
    displayWrite.dstSet = maskDescSet;
    displayWrite.pImageInfo = &placeholderInfo;
    vkUpdateDescriptorSets(device, 1, &displayWrite, 0, nullptr);

    // --- Tile pipeline layout: samplerSetLayout + push constant (vertex) ---
    VkPushConstantRange tilePush{};
    tilePush.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    tilePush.offset = 0;
    tilePush.size = sizeof(MinimapTilePush);
    tilePipelineLayout = createPipelineLayout(device, { samplerSetLayout }, { tilePush });

    // --- Display pipeline layout: the composite, the mask, and the push constant (vert+frag) ---
    VkPushConstantRange displayPush{};
    displayPush.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    displayPush.offset = 0;
    displayPush.size = sizeof(MinimapDisplayPush);
    displayPipelineLayout = createPipelineLayout(device, { samplerSetLayout, samplerSetLayout }, { displayPush });

    // --- Vertex input: pos2 (loc 0) + uv2 (loc 1), stride 16 ---
    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = 4 * sizeof(float);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> attrs(2);
    attrs[0] = { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 };                    // aPos
    attrs[1] = { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 2 * sizeof(float) };    // aUV

    // --- Load tile shaders ---
    {
        VkShaderModule vs, fs;
        if (!vs.loadFromFile(device, "assets/shaders/minimap_tile.vert.spv") ||
            !fs.loadFromFile(device, "assets/shaders/minimap_tile.frag.spv")) {
            LOG_ERROR("Minimap: failed to load tile shaders");
            return false;
        }

        tilePipeline = PipelineBuilder()
            .setShaders(vs.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        fs.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({ binding }, attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setNoDepthTest()
            .setColorBlendAttachment(PipelineBuilder::blendDisabled())
            .setLayout(tilePipelineLayout)
            .setRenderPass(compositeTarget->getRenderPass())
            .setDynamicStates(viewportAndScissorDynamic())
            .build(device, vkCtx->getPipelineCache());

        vs.destroy();
        fs.destroy();
    }

    // --- Load display shaders ---
    {
        VkShaderModule vs, fs;
        if (!vs.loadFromFile(device, "assets/shaders/minimap_display.vert.spv") ||
            !fs.loadFromFile(device, "assets/shaders/minimap_display.frag.spv")) {
            LOG_ERROR("Minimap: failed to load display shaders");
            return false;
        }

        buildDisplayPipeline(device, vs.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                             fs.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT));

        vs.destroy();
        fs.destroy();
    }

    // --- Indoor pictures: alpha-blended over each other, lowest first ---
    {
        VkPushConstantRange indoorPush{};
        indoorPush.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        indoorPush.offset = 0;
        indoorPush.size = sizeof(MinimapIndoorTilePush);
        indoorPipelineLayout = createPipelineLayout(device, { samplerSetLayout }, { indoorPush });

        VkShaderModule vs, fs;
        if (!vs.loadFromFile(device, "assets/shaders/minimap_wmo_tile.vert.spv") ||
            !fs.loadFromFile(device, "assets/shaders/minimap_wmo_tile.frag.spv")) {
            LOG_ERROR("Minimap: failed to load indoor tile shaders");
            return false;
        }
        std::vector<VkVertexInputAttributeDescription> posOnly{ attrs[0] };
        indoorPipeline = PipelineBuilder()
            .setShaders(vs.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        fs.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({ binding }, posOnly)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setNoDepthTest()
            .setColorBlendAttachment(PipelineBuilder::blendAlpha())
            .setLayout(indoorPipelineLayout)
            .setRenderPass(compositeTarget->getRenderPass())
            .setDynamicStates(viewportAndScissorDynamic())
            .build(device, vkCtx->getPipelineCache());
        vs.destroy();
        fs.destroy();

        constexpr uint32_t kIndoorSets = 2 * MAX_INDOOR_TILES;
        VkDescriptorPoolSize indoorPoolSize{};
        indoorPoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        indoorPoolSize.descriptorCount = kIndoorSets;
        VkDescriptorPoolCreateInfo indoorPoolInfo{};
        indoorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        indoorPoolInfo.maxSets = kIndoorSets;
        indoorPoolInfo.poolSizeCount = 1;
        indoorPoolInfo.pPoolSizes = &indoorPoolSize;
        if (vkCreateDescriptorPool(device, &indoorPoolInfo, nullptr, &indoorDescPool) != VK_SUCCESS) {
            LOG_ERROR("Minimap: failed to create the indoor descriptor pool");
            return false;
        }
        std::vector<VkDescriptorSetLayout> indoorLayouts(kIndoorSets, samplerSetLayout);
        VkDescriptorSetAllocateInfo indoorAlloc{};
        indoorAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        indoorAlloc.descriptorPool = indoorDescPool;
        indoorAlloc.descriptorSetCount = kIndoorSets;
        indoorAlloc.pSetLayouts = indoorLayouts.data();
        if (vkAllocateDescriptorSets(device, &indoorAlloc, &indoorDescSets[0][0]) != VK_SUCCESS) {
            LOG_ERROR("Minimap: failed to allocate the indoor descriptor sets");
            return false;
        }
    }

    if (!tilePipeline || !displayPipeline || !indoorPipeline) {
        LOG_ERROR("Minimap: failed to create pipelines");
        return false;
    }

    LOG_INFO("Minimap initialized (", mapSize, "x", mapSize, " screen, ",
             COMPOSITE_PX, "x", COMPOSITE_PX, " composite)");
    return true;
}

void Minimap::shutdown() {
    if (!vkCtx) return;
    VkDevice device = vkCtx->getDevice();
    VmaAllocator alloc = vkCtx->getAllocator();

    vkDeviceWaitIdle(device);

    destroy(device, tilePipeline);
    destroy(device, displayPipeline);
    destroy(device, indoorPipeline);
    destroy(device, tilePipelineLayout);
    destroy(device, displayPipelineLayout);
    destroy(device, indoorPipelineLayout);
    destroy(device, descPool);
    destroy(device, indoorDescPool);
    destroy(device, samplerSetLayout);

    destroy(alloc, quadVB, quadVBAlloc);

    for (auto& [hash, tex] : tileTextureCache) {
        if (tex) tex->destroy(device, alloc);
    }
    tileTextureCache.clear();
    tileInsertionOrder.clear();

    if (noDataTexture) { noDataTexture->destroy(device, alloc); noDataTexture.reset(); }
    if (maskTexture_) { maskTexture_->destroy(device, alloc); maskTexture_.reset(); }
    maskTried_ = false;
    maskLoaded_ = false;
    if (compositeTarget) { compositeTarget->destroy(device, alloc); compositeTarget.reset(); }

    vkCtx = nullptr;
}

void Minimap::recreatePipelines() {
    if (!vkCtx || !displayPipelineLayout) return;
    VkDevice device = vkCtx->getDevice();

    destroy(device, displayPipeline);

    VkShaderModule vs, fs;
    if (!vs.loadFromFile(device, "assets/shaders/minimap_display.vert.spv") ||
        !fs.loadFromFile(device, "assets/shaders/minimap_display.frag.spv")) {
        LOG_ERROR("Minimap: failed to reload display shaders for pipeline recreation");
        return;
    }

    buildDisplayPipeline(device, vs.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                         fs.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT));

    vs.destroy();
    fs.destroy();

    LOG_INFO("Minimap: display pipeline recreated with MSAA ", static_cast<int>(vkCtx->getMsaaSamples()), "x");
}

void Minimap::setMapName(const std::string& name) {
    if (mapName != name) {
        mapName = name;
        hasCachedFrame = false;
        lastCenterTileX = -1;
        lastCenterTileY = -1;
    }
}

namespace {
// The client's table is keyed as SStrHashHT hashes: case and the way the
// slashes lean do not matter.
std::string lowerKey(std::string key) {
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) {
        return c == '/' ? '\\' : static_cast<char>(std::tolower(c));
    });
    return key;
}
}  // namespace

void Minimap::setIndoorScene(std::optional<minimap_indoor::Scene> scene) {
    // A different building, room or area, or going in or out, draws the
    // picture again; the same one does not.
    const auto sameScene = [](const minimap_indoor::Scene& a, const minimap_indoor::Scene& b) {
        return a.instanceId == b.instanceId && a.playerGroup == b.playerGroup &&
               a.area.min == b.area.min && a.area.max == b.area.max &&
               a.groups.size() == b.groups.size();
    };
    if (scene.has_value() != indoorScene_.has_value() ||
        (scene && !sameScene(*scene, *indoorScene_))) {
        indoorDirty_ = true;
    }
    indoorScene_ = std::move(scene);
}

// --------------------------------------------------------
// TRS parsing
// --------------------------------------------------------

void Minimap::parseTRS() {
    if (trsParsed || !assetManager) return;
    trsParsed = true;

    auto data = assetManager->readFile("Textures\\Minimap\\md5translate.trs");
    if (data.empty()) {
        LOG_WARNING("Failed to load md5translate.trs");
        return;
    }

    std::string content(reinterpret_cast<const char*>(data.data()), data.size());
    std::istringstream stream(content);
    std::string line;
    int count = 0;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.substr(0, 4) == "dir:") continue;

        auto tabPos = line.find('\t');
        if (tabPos == std::string::npos) continue;

        std::string key = line.substr(0, tabPos);
        std::string hashFile = line.substr(tabPos + 1);

        if (key.size() > 4 && key.substr(key.size() - 4) == ".blp")
            key = key.substr(0, key.size() - 4);
        if (hashFile.size() > 4 && hashFile.substr(hashFile.size() - 4) == ".blp")
            hashFile = hashFile.substr(0, hashFile.size() - 4);

        // Keyed case-insensitively, as 0x0055f4d0 looks it up.
        trsLookup[lowerKey(key)] = hashFile;
        count++;
    }

    LOG_INFO("Parsed md5translate.trs: ", count, " entries");
}

// --------------------------------------------------------
// Tile texture loading
// --------------------------------------------------------

VkTexture* Minimap::getOrLoadTileTexture(int tileX, int tileY) {
    // "%s\\map%d_%02d.blp" (0x007f5240): the second number is two digits,
    // so map30_09 and not map30_9.
    char name[32];
    std::snprintf(name, sizeof(name), "\\map%d_%02d", tileX, tileY);
    VkTexture* tex = loadTrsTexture(mapName + name);
    return tex ? tex : noDataTexture.get();
}

VkTexture* Minimap::loadTrsTexture(const std::string& key) {
    if (!trsParsed) parseTRS();
    auto trsIt = trsLookup.find(lowerKey(key));
    if (trsIt == trsLookup.end())
        return nullptr;

    const std::string& hash = trsIt->second;

    auto cacheIt = tileTextureCache.find(hash);
    if (cacheIt != tileTextureCache.end())
        return cacheIt->second.get();

    // Load from MPQ
    std::string blpPath = "Textures\\Minimap\\" + hash + ".blp";
    auto blpImage = assetManager->loadTexture(blpPath);
    if (!blpImage.isValid()) {
        tileTextureCache[hash] = nullptr;  // Mark as failed
        return nullptr;
    }

    auto tex = std::make_unique<VkTexture>();
    tex->upload(*vkCtx, blpImage.data.data(), blpImage.width, blpImage.height,
                VK_FORMAT_R8G8B8A8_UNORM, false);
    tex->createSampler(vkCtx->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                       VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f);

    VkTexture* ptr = tex.get();
    tileTextureCache[hash] = std::move(tex);
    tileInsertionOrder.push_back(hash);

    // Evict oldest tiles when cache grows too large to bound GPU memory usage.
    while (tileInsertionOrder.size() > MAX_TILE_CACHE) {
        const std::string& oldest = tileInsertionOrder.front();
        tileTextureCache.erase(oldest);
        tileInsertionOrder.pop_front();
    }

    return ptr;
}

// --------------------------------------------------------
// Update tile descriptor sets for composite pass
// --------------------------------------------------------

void Minimap::updateTileDescriptors(uint32_t frameIdx, int centerTileX, int centerTileY) {
    constexpr int kTileCount = GRID * GRID;
    constexpr int kHalf = GRID / 2;
    VkDevice device = vkCtx->getDevice();
    std::array<VkDescriptorImageInfo, kTileCount> imgInfos{};
    std::array<VkWriteDescriptorSet, kTileCount> writes{};
    int slot = 0;

    for (int dr = -kHalf; dr <= kHalf; dr++) {
        for (int dc = -kHalf; dc <= kHalf; dc++) {
            int tx = centerTileX + dr;
            int ty = centerTileY + dc;

            VkTexture* tileTex = getOrLoadTileTexture(tx, ty);
            if (!tileTex || !tileTex->isValid())
                tileTex = noDataTexture.get();

            imgInfos[slot] = tileTex->descriptorInfo();

            writes[slot] = {};
            writes[slot].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[slot].dstSet = tileDescSets[frameIdx][slot];
            writes[slot].dstBinding = 0;
            writes[slot].descriptorCount = 1;
            writes[slot].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[slot].pImageInfo = &imgInfos[slot];
            slot++;
        }
    }

    vkUpdateDescriptorSets(device, kTileCount, writes.data(), 0, nullptr);
}

// --------------------------------------------------------
// Off-screen composite pass (call BEFORE main render pass)
// --------------------------------------------------------

void Minimap::loadMask() {
    if (maskTried_ || !assetManager || !vkCtx || maskDescSet == VK_NULL_HANDLE) return;
    maskTried_ = true;
    // The round mask the client draws the map through, in the second texture
    // stage (0x005832f0 loads it, 0x00581740 binds it).
    auto image = assetManager->loadTexture("Textures\\MinimapMask.blp");
    if (!image.isValid()) {
        LOG_WARNING("Minimap: Textures\\MinimapMask.blp not found - cut round without it");
        return;
    }
    maskTexture_ = std::make_unique<VkTexture>();
    if (!maskTexture_->upload(*vkCtx, image.data.data(), image.width, image.height,
                              VK_FORMAT_R8G8B8A8_UNORM, false) ||
        !maskTexture_->createSampler(vkCtx->getDevice(), VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                     VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 1.0f)) {
        maskTexture_.reset();
        return;
    }
    // Last bound by a display draw frames ago; the queue is idle for the
    // upload just made.
    vkDeviceWaitIdle(vkCtx->getDevice());
    VkDescriptorImageInfo info = maskTexture_->descriptorInfo();
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = maskDescSet;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(vkCtx->getDevice(), 1, &w, 0, nullptr);
    maskLoaded_ = true;
}

void Minimap::compositePass(VkCommandBuffer cmd, const glm::vec3& centerWorldPos) {
    if (!enabled || !assetManager || !compositeTarget || !compositeTarget->isValid()) return;

    if (!trsParsed) parseTRS();
    loadMask();

    if (indoorScene_) {
        compositeIndoor(cmd);
        return;
    }

    // Check if composite needs refresh
    const auto now = std::chrono::steady_clock::now();
    bool needsRefresh = !hasCachedFrame || compositeIndoors_;
    if (!needsRefresh) {
        float mdx = centerWorldPos.x - lastUpdatePos.x;
        float mdy = centerWorldPos.y - lastUpdatePos.y;
        float movedSq = mdx * mdx + mdy * mdy;
        float elapsed = std::chrono::duration<float>(now - lastUpdateTime).count();
        needsRefresh = (movedSq >= updateDistance * updateDistance) || (elapsed >= updateIntervalSec);
    }

    // Also refresh if player crossed a tile boundary
    auto [curTileX, curTileY] = core::coords::worldToTile(centerWorldPos.x, centerWorldPos.y);
    if (curTileX != lastCenterTileX || curTileY != lastCenterTileY)
        needsRefresh = true;

    if (!needsRefresh) return;

    uint32_t frameIdx = vkCtx->getCurrentFrame();

    // Update tile descriptor sets
    updateTileDescriptors(frameIdx, curTileX, curTileY);

    // Begin off-screen render pass
    VkClearColorValue clearColor = {{ 0.05f, 0.08f, 0.12f, 1.0f }};
    compositeTarget->beginPass(cmd, clearColor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tilePipeline);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &quadVB, &offset);

    // Draw the tile grid
    constexpr int kHalf = GRID / 2;
    int slot = 0;
    for (int dr = -kHalf; dr <= kHalf; dr++) {
        for (int dc = -kHalf; dc <= kHalf; dc++) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    tilePipelineLayout, 0, 1,
                                    &tileDescSets[frameIdx][slot], 0, nullptr);

            MinimapTilePush push{};
            push.gridOffset = glm::vec2(static_cast<float>(dc + kHalf),
                                        static_cast<float>(dr + kHalf));
            push.gridSize = static_cast<float>(GRID);
            vkCmdPushConstants(cmd, tilePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                               0, sizeof(push), &push);

            vkCmdDraw(cmd, 6, 1, 0, 0);
            slot++;
        }
    }

    compositeTarget->endPass(cmd);

    // Update tracking
    lastCenterTileX = curTileX;
    lastCenterTileY = curTileY;
    lastUpdateTime = now;
    lastUpdatePos = centerWorldPos;
    hasCachedFrame = true;
    compositeIndoors_ = false;
    indoorDirty_ = false;
}

// --------------------------------------------------------
// Indoors: the WMO groups' own pictures (0x007f5ba0, drawn by 0x00581290)
// --------------------------------------------------------

void Minimap::compositeIndoor(VkCommandBuffer cmd) {
    if (hasCachedFrame && compositeIndoors_ && !indoorDirty_) return;
    const minimap_indoor::Scene& scene = *indoorScene_;
    namespace mi = minimap_indoor;

    struct Picture {
        VkTexture* texture = nullptr;
        float key = 0.0f;
        mi::Tile tile;
    };
    std::vector<Picture> pictures;
    for (const auto& group : scene.groups) {
        const float key = mi::drawKey((group.min.z + group.max.z) * 0.5f, scene.playerLocalZ,
                                      group.index == scene.playerGroup);
        for (const mi::Tile& tile : mi::groupTiles(group.min, group.max,
                                                   scene.localMin, scene.localMax)) {
            // A picture md5translate has no name for is drawn as nothing
            // ("No minimap texture", 0x007f5070).
            VkTexture* texture = loadTrsTexture(
                mi::tileName(scene.wmoBase, static_cast<int>(group.index), tile.x, tile.y));
            if (!texture || !texture->isValid()) continue;
            pictures.push_back({texture, key, tile});
        }
    }
    // Lowest first, the player's own group last (0x0057bd10).
    std::stable_sort(pictures.begin(), pictures.end(),
                     [](const Picture& a, const Picture& b) { return a.key < b.key; });
    if (pictures.size() > MAX_INDOOR_TILES) pictures.resize(MAX_INDOOR_TILES);

    const uint32_t frameIdx = vkCtx->getCurrentFrame();
    std::vector<VkDescriptorImageInfo> images(pictures.size());
    std::vector<VkWriteDescriptorSet> writes(pictures.size());
    for (size_t i = 0; i < pictures.size(); ++i) {
        images[i] = pictures[i].texture->descriptorInfo();
        writes[i] = {};
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = indoorDescSets[frameIdx][i];
        writes[i].dstBinding = 0;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    if (!writes.empty()) {
        vkUpdateDescriptorSets(vkCtx->getDevice(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }

    // The composite is the area: three of the zoom's radii across, centred
    // on the cell the player is in. Its u runs against render x and its v
    // against render y, as the terrain's do (render() and the blips agree).
    const glm::vec2 center = scene.area.center();
    const float span = scene.area.span();
    const auto toComposite = [&](float x, float y) {
        const glm::vec4 r = scene.modelMatrix * glm::vec4(x, y, scene.playerLocalZ, 1.0f);
        return glm::vec2(0.5f - (r.x - center.x) / span, 0.5f - (r.y - center.y) / span);
    };

    // Black where no picture is (0x00581cd0 clears to it).
    VkClearColorValue clearColor = {{ 0.0f, 0.0f, 0.0f, 1.0f }};
    compositeTarget->beginPass(cmd, clearColor);
    if (!pictures.empty()) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, indoorPipeline);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &quadVB, &offset);
        for (size_t i = 0; i < pictures.size(); ++i) {
            const mi::Tile& t = pictures[i].tile;
            // Half a texel in from each edge, and the picture's top row at
            // its high y (0x0057e7f0).
            const float eu = 0.5f / static_cast<float>(pictures[i].texture->getWidth());
            const float ev = 0.5f / static_cast<float>(pictures[i].texture->getHeight());
            MinimapIndoorTilePush push{};
            const glm::vec2 c0 = toComposite(t.min.x, t.min.y);
            const glm::vec2 c1 = toComposite(t.max.x, t.min.y);
            const glm::vec2 c2 = toComposite(t.max.x, t.max.y);
            const glm::vec2 c3 = toComposite(t.min.x, t.max.y);
            push.corners01 = glm::vec4(c0, c1);
            push.corners23 = glm::vec4(c2, c3);
            push.uv01 = glm::vec4(eu, 1.0f - ev, 1.0f - eu, 1.0f - ev);
            push.uv23 = glm::vec4(1.0f - eu, ev, eu, ev);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, indoorPipelineLayout,
                                    0, 1, &indoorDescSets[frameIdx][i], 0, nullptr);
            vkCmdPushConstants(cmd, indoorPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(push), &push);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        }
    }
    compositeTarget->endPass(cmd);

    compositeCenter_ = center;
    compositeSpan_ = span;
    compositeIndoors_ = true;
    indoorDirty_ = false;
    hasCachedFrame = true;
    // The terrain's picture is gone; coming back out draws it again.
    lastCenterTileX = -1;
    lastCenterTileY = -1;
}

// --------------------------------------------------------
// Display quad (call INSIDE main render pass)
// --------------------------------------------------------

glm::vec4 Minimap::screenUvRect(int screenWidth, int screenHeight) const {
    const float sw = static_cast<float>(screenWidth);
    const float sh = static_cast<float>(screenHeight);
    if (haveRect_ && rectW_ > 0.0f && rectH_ > 0.0f) {
        return {rectX_ / sw, rectY_ / sh, rectW_ / sw, rectH_ / sh};
    }
    // The top-right corner at its own size, as render() places it.
    constexpr float margin = 10.0f;
    const float w = static_cast<float>(mapSize) / sw;
    const float h = static_cast<float>(mapSize) / sh;
    return {1.0f - w - margin / sw, margin / sh, w, h};
}

void Minimap::render(VkCommandBuffer cmd, const Camera& playerCamera,
                     const glm::vec3& centerWorldPos,
                     int screenWidth, int screenHeight) {
    if (!enabled || !hasCachedFrame || !displayPipeline) return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, displayPipeline);

    const VkDescriptorSet displaySets[2] = {displayDescSet, maskDescSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            displayPipelineLayout, 0, 2,
                            displaySets, 0, nullptr);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &quadVB, &offset);

    // Top-right corner, unless something asked for a particular rect - which
    // is what happens when FrameXML owns the minimap and the map has to sit
    // inside the frame it drew.
    float margin = 10.0f;
    float pixelW, pixelH, x, y;
    if (haveRect_ && rectW_ > 0.0f && rectH_ > 0.0f) {
        pixelW = rectW_ / screenWidth;
        pixelH = rectH_ / screenHeight;
        x = rectX_ / screenWidth;
        y = rectY_ / screenHeight;   // y=0 is the top edge in Vulkan
    } else {
        pixelW = static_cast<float>(mapSize) / screenWidth;
        pixelH = static_cast<float>(mapSize) / screenHeight;
        x = 1.0f - pixelW - margin / screenWidth;
        y = margin / screenHeight;
    }

    // Compute player's UV in the composite texture
    constexpr float TILE_SIZE = core::coords::TILE_SIZE;
    float playerU = 0.5f;
    float playerV = 0.5f;
    float zoomRadius = 0.0f;
    if (compositeIndoors_) {
        // The area compositeIndoor drew, in the same sense as the tiles.
        playerU = 0.5f - (centerWorldPos.x - compositeCenter_.x) / compositeSpan_;
        playerV = 0.5f - (centerWorldPos.y - compositeCenter_.y) / compositeSpan_;
        zoomRadius = getViewRadius() / compositeSpan_;
    } else {
        auto [tileX, tileY] = core::coords::worldToTile(centerWorldPos.x, centerWorldPos.y);

        float fracNS = 32.0f - static_cast<float>(tileX) - centerWorldPos.y / TILE_SIZE;
        float fracEW = 32.0f - static_cast<float>(tileY) - centerWorldPos.x / TILE_SIZE;

        constexpr float kHalfGrid = static_cast<float>(GRID / 2);
        playerU = (kHalfGrid + fracEW) / static_cast<float>(GRID);
        playerV = (kHalfGrid + fracNS) / static_cast<float>(GRID);

        zoomRadius = getViewRadius() / (TILE_SIZE * static_cast<float>(GRID));
    }

    // Rotating with the camera is off everywhere: the saved setting is read and
    // dropped in loadSettings, since "Stabilize transports and correct minimap
    // orientation". This is why, as far as it can be worked out without the
    // client on screen.
    //
    // The two modes disagree by half a turn. Take the heading where
    // atan2(-fwd.x, fwd.y) is 0. North-up draws the arrow at pi - that same
    // expression, which is pi: pointing down, so that heading renders as south.
    // Rotating mode turns the map by the expression itself, which is 0 - no
    // rotation at all - while pinning the arrow up. So the map says the player
    // faces north and the arrow agrees, and both are half a turn from where
    // north-up puts them.
    //
    // North-up is the mode that has been looked at, so its arrow is the one to
    // trust: the map wants turning by the negative of where that arrow points,
    // not by the expression the arrow is built from.
    //
    // The direction is a second question and not settled here. What the shader
    // does with this angle is a mirror as well as a turn - the matrix it builds
    // has determinant -1 - so the map may also rotate the wrong way once the
    // half turn is accounted for. That mirror is right for north-up, where the
    // angle is zero, which is what makes this hard to reason about and easy to
    // check by turning it on and walking north.
    float rotation = 0.0f;
    if (rotateWithCamera) {
        glm::vec3 fwd = playerCamera.getForward();
        rotation = std::atan2(-fwd.x, fwd.y);
    }

    MinimapDisplayPush push{};
    push.rect = glm::vec4(x, y, pixelW, pixelH);
    push.playerUV = glm::vec2(playerU, playerV);
    push.rotation = rotation;
    push.zoomRadius = zoomRadius;
    push.squareShape = squareShape ? 1 : 0;
    push.opacity = opacity_;
    push.hasMask = maskLoaded_ ? 1 : 0;

    vkCmdPushConstants(cmd, displayPipelineLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), &push);

    vkCmdDraw(cmd, 6, 1, 0, 0);
}

} // namespace rendering
} // namespace wowee
