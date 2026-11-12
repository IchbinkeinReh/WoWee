#include "rendering/horizon_renderer.hpp"

#include "rendering/camera.hpp"
#include "rendering/depth_convention.hpp"
#include "rendering/frustum.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/wdl_loader.hpp"
#include "core/logger.hpp"

#include <glm/glm.hpp>
#include <algorithm>

namespace wowee::rendering {

namespace {
constexpr const char* kVert = "assets/shaders/horizon.vert.spv";
constexpr const char* kFrag = "assets/shaders/horizon.frag.spv";

/// The horizon's own depth planes (0x00791170) and where its depths go.
struct HorizonPush {
    /// x: near (far clip - 50), y: far (scale x far clip), z, w: the depths
    /// the near and the far plane land at in the scene's reversed depth.
    glm::vec4 planes;
};

/// The client puts the horizon at 0.998..0.999 of its depth range, the world
/// at 0..0.94 and the sky behind both. Reversed, and below the depth of any
/// surface of the world: a surface at the camera's far plane (30000 yd, near
/// 0.2) is still at 0.2/30000 - nothing of the world comes near these.
constexpr float kDepthAtNear = 2.0e-6f;
constexpr float kDepthAtFar = 1.0e-6f;
}  // namespace

HorizonRenderer::~HorizonRenderer() { shutdown(); }

bool HorizonRenderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    vkCtx_ = ctx;
    slot_.fill(-1);
    VkDevice device = vkCtx_->getDevice();
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    range.size = sizeof(HorizonPush);
    pipelineLayout_ = createPipelineLayout(device, {perFrameLayout}, {range});
    if (pipelineLayout_ == VK_NULL_HANDLE) return false;
    buildPipeline();
    return pipeline_ != VK_NULL_HANDLE;
}

void HorizonRenderer::buildPipeline() {
    VkDevice device = vkCtx_->getDevice();
    auto shaders = loadShaderPair(device, kVert, kFrag, "horizon");
    if (!shaders) return;
    const VkVertexInputAttributeDescription pos{
        .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0};
    // Opaque, depth tested and written, both faces (render state 17 is 0 for
    // the tile's cells; its holes go with the faces culled, which from above
    // leaves them the same).
    pipeline_ = PipelineBuilder()
        .setShaders(shaders.vertStage, shaders.fragStage)
        .setVertexInput({tightVertexBinding(sizeof(float) * 3)}, {pos})
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, true, depth::kNearerOrEqual)
        .setColorBlendAttachment(PipelineBuilder::blendDisabled())
        .setMultisample(vkCtx_->getMsaaSamples())
        .setLayout(pipelineLayout_)
        .setRenderPass(vkCtx_->getImGuiRenderPass())
        .setDynamicStates(viewportAndScissorDynamic())
        .build(device, vkCtx_->getPipelineCache());
    if (pipeline_ == VK_NULL_HANDLE) LOG_ERROR("Horizon: pipeline could not be made");
}

void HorizonRenderer::recreatePipelines() {
    if (!vkCtx_) return;
    destroy(vkCtx_->getDevice(), pipeline_);
    buildPipeline();
}

void HorizonRenderer::releaseMesh() {
    if (!vkCtx_) return;
    // Both frames in flight may still read the old ones.
    VmaAllocator allocator = vkCtx_->getAllocator();
    for (AllocatedBuffer* b : {&vertices_, &indices_}) {
        if (b->buffer == VK_NULL_HANDLE) continue;
        ::VkBuffer buffer = b->buffer;
        VmaAllocation alloc = b->allocation;
        vkCtx_->deferAfterAllFrameFences([allocator, buffer, alloc]() { vmaDestroyBuffer(allocator, buffer, alloc); });
        *b = AllocatedBuffer{};
    }
    tiles_.clear();
    slot_.fill(-1);
}

void HorizonRenderer::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    destroyPipeline(device, pipeline_, pipelineLayout_);
    VmaAllocator allocator = vkCtx_->getAllocator();
    destroy(allocator, vertices_.buffer, vertices_.allocation);
    destroy(allocator, indices_.buffer, indices_.allocation);
    tiles_.clear();
    vkCtx_ = nullptr;
}

void HorizonRenderer::setMapName(const std::string& name) { mapName_ = name; }

void HorizonRenderer::update(pipeline::AssetManager* assets) {
    if (!vkCtx_ || !assets || mapName_ == loadedMap_) return;
    loadedMap_ = mapName_;
    releaseMesh();
    if (mapName_.empty()) return;
    // 0x007cc310: "%s\%s.wdl" with the map's directory and name.
    const std::string path = "World\\Maps\\" + mapName_ + "\\" + mapName_ + ".wdl";
    const auto data = assets->readFileOptional(path);
    if (data.empty()) {
        LOG_INFO("Horizon: ", mapName_, " has no ", path);
        return;
    }
    const pipeline::WdlData wdl = pipeline::parseWDL(data);
    if (wdl.empty()) {
        LOG_WARNING("Horizon: ", path, " has no heights");
        return;
    }
    std::vector<float> verts;
    verts.reserve(wdl.tiles.size() * pipeline::wdl::kVertices * 3);
    for (int s = 0; s < 64 * 64; ++s) {
        const int32_t t = wdl.tileIndex[s];
        if (t < 0) continue;
        const pipeline::WdlTile& tile = wdl.tiles[t];
        const int row = s / 64, col = s % 64;
        const auto v = pipeline::wdl::tileVertices(tile, row, col);
        verts.insert(verts.end(), v.begin(), v.end());
        slot_[s] = static_cast<int32_t>(tiles_.size());
        tiles_.push_back({row, col, static_cast<float>(tile.minHeight), static_cast<float>(tile.maxHeight)});
    }
    const auto idx = pipeline::wdl::tileIndices();
    vertices_ = uploadBuffer(*vkCtx_, verts.data(), verts.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    indices_ = uploadBuffer(*vkCtx_, idx.data(), idx.size() * sizeof(uint16_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (vertices_.buffer == VK_NULL_HANDLE || indices_.buffer == VK_NULL_HANDLE) {
        LOG_WARNING("Horizon: could not upload ", path);
        releaseMesh();
        return;
    }
    LOG_INFO("Horizon: ", path, " - ", tiles_.size(), " tiles");
}

void HorizonRenderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera,
                             float farClip, float scale) {
    tilesDrawn_ = 0;
    if (pipeline_ == VK_NULL_HANDLE || tiles_.empty() || farClip <= 50.0f) return;
    const float nearZ = pipeline::wdl::horizonNear(farClip);
    const float farZ = pipeline::wdl::horizonFar(farClip, scale);

    Frustum frustum;
    frustum.extractFromMatrix(camera.getProjectionMatrix() * camera.getViewMatrix());
    const glm::vec3 eye = camera.getPosition();
    const auto range = pipeline::wdl::visibleTiles(eye.x, eye.y, farClip, scale);

    bool bound = false;
    for (int row = range.rowMin; row <= range.rowMax; ++row) {
        for (int col = range.colMin; col <= range.colMax; ++col) {
            const int32_t t = slot_[row * 64 + col];
            if (t < 0) continue;
            const Tile& tile = tiles_[t];
            // 0x007cc310's box: the tile's square, its lowest to its highest height.
            const float maxX = pipeline::wdl::kZeroPoint - static_cast<float>(row) * pipeline::wdl::kTileSize;
            const float maxY = pipeline::wdl::kZeroPoint - static_cast<float>(col) * pipeline::wdl::kTileSize;
            const glm::vec3 lo(maxX - pipeline::wdl::kTileSize, maxY - pipeline::wdl::kTileSize, tile.minZ);
            const glm::vec3 hi(maxX, maxY, tile.maxZ);
            if (!frustum.intersectsAABB(lo, hi)) continue;
            if (!bound) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &perFrameSet,
                                        0, nullptr);
                const HorizonPush push{glm::vec4(nearZ, farZ, kDepthAtNear, kDepthAtFar)};
                vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &offset);
                vkCmdBindIndexBuffer(cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT16);
                bound = true;
            }
            vkCmdDrawIndexed(cmd, pipeline::wdl::kIndices, 1, 0, t * pipeline::wdl::kVertices, 0);
            ++tilesDrawn_;
        }
    }
}

}  // namespace wowee::rendering
