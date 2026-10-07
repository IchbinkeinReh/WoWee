#pragma once

#include "rendering/client_weather.hpp"
#include "rendering/vk_texture.hpp"

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace wowee {
namespace pipeline { class AssetManager; }
namespace rendering {

class VkContext;

/// The client's weather (MapWeather.cpp): rain with its splashes, snow and
/// sand, made in packets around the camera and drawn by ports of the client's
/// rain, patter, snowpoint and sand vertex programs. The arithmetic is in
/// client_weather.hpp; this holds the state, the packets and the drawing.
///
/// Each effect also drifts mist sheets through a smaller box (SnowMist01,
/// WeatherMistGrainy01; 0x00786560, 0x00786e10).
class Weather {
public:
    enum class Type { NONE, RAIN, SNOW, SAND };

    /// What one frame tells the weather. Positions are the renderer's.
    struct FrameInput {
        float deltaTime = 0.0f;
        glm::vec3 cameraPosition{0.0f};
        glm::vec3 cameraRight{1.0f, 0.0f, 0.0f};  ///< for the mist's billboards
        glm::vec3 cameraUp{0.0f, 0.0f, 1.0f};
        glm::vec3 playerPosition{0.0f};
        float playerYawDeg = 0.0f;   ///< the renderer's character yaw
        bool riding = false;         ///< on a taxi
        uint32_t viewportWidth = 0;  ///< for sand's grain size
    };

    Weather();
    ~Weather();

    [[nodiscard]] bool initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout);
    /// Weather.dbc, read once the assets are there.
    void loadAssets(pipeline::AssetManager* assets);
    void recreatePipelines();
    void shutdown();

    /// SMSG_WEATHER (0x007846a0): the Weather.dbc row the server names, how
    /// hard, and whether to cut straight to it rather than ease over.
    void setWeather(uint32_t weatherId, float intensity, bool abrupt);

    /// The surface under a point, as the renderer can find it: the highest
    /// within 200 yards above or below `z`, or `z - 200` (0x007ade10).
    /// Renderer coordinates in and out.
    void setGroundQuery(client_weather::HeightCache::Query query);

    void update(const FrameInput& frame);
    void render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet);

    /// The weatherDensity setting, 0-3 (0x00784040).
    void setDensityLevel(int level);
    [[nodiscard]] int densityLevel() const { return densityLevel_; }

    [[nodiscard]] Type getWeatherType() const;
    /// The intensity as it stands, eased toward the server's.
    [[nodiscard]] float getIntensity() const { return current_; }
    [[nodiscard]] bool isEnabled() const { return active_ != client_weather::Effect::None; }
    [[nodiscard]] int getParticleCount() const;

private:
    using Effect = client_weather::Effect;

    struct Packet {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkBuffer splashBuffer = VK_NULL_HANDLE;
        VmaAllocation splashAllocation = VK_NULL_HANDLE;
        void* splashMapped = nullptr;
        uint32_t count = 0;
        uint32_t splashCount = 0;
        double base = 0.0;   ///< the weather clock it counts from
        float gone = 0.0f;   ///< from base, when its last particle is gone
        bool open = true;
        uint64_t freeAfterFrame = 0;
    };

    struct Texture {
        VkTexture texture;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    bool buildPipelines();
    Packet* takePacket();
    void retire(std::unique_ptr<Packet> packet);
    void clearPackets();
    void spawn(float dt, const client_weather::SpawnContext& ctx);
    VkDescriptorSet textureSet(const std::string& path);
    VkDescriptorSet loadedSet(const std::string& path) const;

    VkContext* vkCtx_ = nullptr;
    VkDescriptorSetLayout perFrameLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout textureLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline streakPipeline_ = VK_NULL_HANDLE;
    VkPipeline splashPipeline_ = VK_NULL_HANDLE;
    VkPipeline pointPipeline_ = VK_NULL_HANDLE;
    VkPipeline mistPipeline_ = VK_NULL_HANDLE;
    static constexpr int kMistFrames = 3;
    VkBuffer mistBuffer_[kMistFrames] = {};
    VmaAllocation mistAllocation_[kMistFrames] = {};
    void* mistMapped_[kMistFrames] = {};
    int mistFrame_ = 0;

    pipeline::AssetManager* assets_ = nullptr;
    std::unordered_map<uint32_t, client_weather::WeatherRow> rows_;
    std::unordered_map<std::string, std::unique_ptr<Texture>> textures_;
    std::unique_ptr<Texture> blank_;

    // The state 0x007846a0 and 0x0078d170 keep.
    Effect active_ = Effect::None;    ///< the effect whose packets these are
    Effect pending_ = Effect::None;   ///< to change to, when changePending_
    bool changePending_ = false;
    bool smooth_ = true;              ///< not abrupt: wait for the old to fall
    bool stopping_ = false;           ///< the old effect makes no more
    glm::vec3 color_{1.0f};
    std::string texture_;
    float target_ = 0.0f, from_ = 0.0f, current_ = 0.0f;
    double easeStart_ = 0.0;
    int densityLevel_ = client_weather::kDefaultDensityLevel;
    bool haveWeather_ = false;
    uint32_t weatherId_ = 0;

    double now_ = 0.0;
    uint64_t frame_ = 0;
    uint32_t viewportWidth_ = 0;
    std::vector<std::unique_ptr<Packet>> packets_;
    std::vector<std::unique_ptr<Packet>> freePackets_;

    client_weather::HeightCache ground_;
    std::vector<client_weather::Mist> mists_;
    float mistAccum_ = 0.0f;
    glm::vec3 cameraRender_{0.0f};
    glm::vec3 right_{0.0f}, up_{0.0f};  ///< half a sheet across and up
    void updateMists(float dt, const client_weather::SpawnContext& ctx);
    client_weather::VelocityWindow velocity_;
    glm::vec3 lastPlayer_{0.0f};
    bool havePlayer_ = false;
    std::mt19937 rng_;
};

} // namespace rendering
} // namespace wowee
