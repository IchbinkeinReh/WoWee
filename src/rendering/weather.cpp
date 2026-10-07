#include "rendering/weather.hpp"

#include "core/coordinates.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wowee {
namespace rendering {

namespace cw = client_weather;

namespace {

// What a packet holds on the device: the particle as the vertex programs read
// it, in the renderer's coordinates.
struct GpuParticle {
    float position[3];
    float velocity[3];
    float times[2];
};
static_assert(sizeof(GpuParticle) == 32);

struct GpuSplash {
    float position[3];
    float times[2];
};
static_assert(sizeof(GpuSplash) == 20);

struct WeatherPush {
    glm::vec4 color;
    glm::vec4 params;
};

constexpr uint32_t kMaxTextures = 16;

// A mist sheet's corner: where, which texel, how opaque.
struct GpuMistVertex {
    float position[3];
    float uv[2];
    float alpha;
};
static_assert(sizeof(GpuMistVertex) == 24);
constexpr uint32_t kMistVertices = cw::kMistSlots * 6;

glm::vec3 toRender(const glm::vec3& c) { return core::coords::canonicalToRender(c); }
glm::vec3 toCanonical(const glm::vec3& r) { return core::coords::renderToCanonical(r); }

GpuParticle gpuParticle(const cw::Particle& p) {
    const glm::vec3 pos = toRender(p.position);
    const glm::vec3 vel = toRender(p.velocity);
    return {{pos.x, pos.y, pos.z}, {vel.x, vel.y, vel.z}, {p.start, p.end}};
}

} // namespace

Weather::Weather() : rng_(std::random_device{}()) {}

Weather::~Weather() {
    shutdown();
}

bool Weather::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout) {
    vkCtx_ = ctx;
    perFrameLayout_ = perFrameLayout;
    VkDevice device = vkCtx_->getDevice();

    VkDescriptorSetLayoutBinding sampler{};
    sampler.binding = 0;
    sampler.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler.descriptorCount = 1;
    sampler.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    textureLayout_ = createDescriptorSetLayout(device, {sampler});

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures};
    VkDescriptorPoolCreateInfo pool{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool.maxSets = kMaxTextures;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (!textureLayout_ || vkCreateDescriptorPool(device, &pool, nullptr, &descriptorPool_) != VK_SUCCESS) {
        LOG_ERROR("Weather: failed to create descriptor resources");
        return false;
    }

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(WeatherPush);
    pipelineLayout_ = createPipelineLayout(device, {perFrameLayout_, textureLayout_}, {push});
    if (!pipelineLayout_ || !buildPipelines()) {
        LOG_ERROR("Weather: failed to create pipelines");
        return false;
    }

    for (int i = 0; i < kMistFrames; ++i) {
        AllocatedBuffer buf = createBuffer(vkCtx_->getAllocator(), kMistVertices * sizeof(GpuMistVertex),
                                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        mistBuffer_[i] = buf.buffer;
        mistAllocation_[i] = buf.allocation;
        mistMapped_[i] = buf.info.pMappedData;
    }
    mists_.assign(cw::kMistSlots, cw::Mist{});

    // What sand binds: its program reads no texture, but the set must be there.
    blank_ = std::make_unique<Texture>();
    const uint8_t white[4] = {255, 255, 255, 255};
    if (blank_->texture.upload(*vkCtx_, white, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false) &&
        blank_->texture.createSampler(device)) {
        VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = descriptorPool_;
        alloc.descriptorSetCount = 1;
        alloc.pSetLayouts = &textureLayout_;
        if (vkAllocateDescriptorSets(device, &alloc, &blank_->set) == VK_SUCCESS) {
            VkDescriptorImageInfo info = blank_->texture.descriptorInfo();
            VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = blank_->set;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &info;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
    }
    return true;
}

bool Weather::buildPipelines() {
    VkDevice device = vkCtx_->getDevice();

    // Rain and its splashes: Mod2x (0x0078a640 and 0x0078a030 set blend mode
    // 5), the destination's alpha left alone; no depth write, no culling.
    VkPipelineColorBlendAttachmentState mod2x = PipelineBuilder::blendAdditive();
    mod2x.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    mod2x.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    mod2x.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    mod2x.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;

    {
        auto shaders = loadShaderPair(device, "assets/shaders/weather_streak.vert.spv",
                                      "assets/shaders/weather.frag.spv", "weather rain");
        if (!shaders) return false;
        VkVertexInputBindingDescription binding{0, sizeof(GpuParticle), VK_VERTEX_INPUT_RATE_INSTANCE};
        std::vector<VkVertexInputAttributeDescription> attrs = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
        };
        streakPipeline_ = PipelineBuilder()
            .setShaders(shaders.vertStage, shaders.fragStage)
            .setVertexInput({binding}, attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setColorBlendAttachment(mod2x)
            .setMultisample(vkCtx_->getMsaaSamples())
            .setLayout(pipelineLayout_)
            .setRenderPass(vkCtx_->getImGuiRenderPass())
            .setDynamicStates(viewportAndScissorDynamic())
            .build(device, vkCtx_->getPipelineCache());
    }
    {
        auto shaders = loadShaderPair(device, "assets/shaders/weather_splash.vert.spv",
                                      "assets/shaders/weather.frag.spv", "weather splash");
        if (!shaders) return false;
        VkVertexInputBindingDescription binding{0, sizeof(GpuSplash), VK_VERTEX_INPUT_RATE_INSTANCE};
        std::vector<VkVertexInputAttributeDescription> attrs = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
        };
        splashPipeline_ = PipelineBuilder()
            .setShaders(shaders.vertStage, shaders.fragStage)
            .setVertexInput({binding}, attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setColorBlendAttachment(mod2x)
            .setMultisample(vkCtx_->getMsaaSamples())
            .setLayout(pipelineLayout_)
            .setRenderPass(vkCtx_->getImGuiRenderPass())
            .setDynamicStates(viewportAndScissorDynamic())
            .build(device, vkCtx_->getPipelineCache());
    }
    {
        // Snow and sand: alpha blended (blend mode 2, 0x0078aee0 and
        // 0x0078bee0), no depth write, points.
        auto shaders = loadShaderPair(device, "assets/shaders/weather_point.vert.spv",
                                      "assets/shaders/weather_point.frag.spv", "weather points");
        if (!shaders) return false;
        VkVertexInputBindingDescription binding{0, sizeof(GpuParticle), VK_VERTEX_INPUT_RATE_VERTEX};
        std::vector<VkVertexInputAttributeDescription> attrs = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
            {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
        };
        pointPipeline_ = PipelineBuilder()
            .setShaders(shaders.vertStage, shaders.fragStage)
            .setVertexInput({binding}, attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setColorBlendAttachment(PipelineBuilder::blendAlpha())
            .setMultisample(vkCtx_->getMsaaSamples())
            .setLayout(pipelineLayout_)
            .setRenderPass(vkCtx_->getImGuiRenderPass())
            .setDynamicStates(viewportAndScissorDynamic())
            .build(device, vkCtx_->getPipelineCache());
    }
    {
        // The mist: alpha blended, no depth write, no fog (0x00786e10).
        auto shaders = loadShaderPair(device, "assets/shaders/weather_mist.vert.spv",
                                      "assets/shaders/weather_mist.frag.spv", "weather mist");
        if (!shaders) return false;
        VkVertexInputBindingDescription binding{0, sizeof(GpuMistVertex), VK_VERTEX_INPUT_RATE_VERTEX};
        std::vector<VkVertexInputAttributeDescription> attrs = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
            {2, 0, VK_FORMAT_R32_SFLOAT, 20},
        };
        mistPipeline_ = PipelineBuilder()
            .setShaders(shaders.vertStage, shaders.fragStage)
            .setVertexInput({binding}, attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setColorBlendAttachment(PipelineBuilder::blendAlpha())
            .setMultisample(vkCtx_->getMsaaSamples())
            .setLayout(pipelineLayout_)
            .setRenderPass(vkCtx_->getImGuiRenderPass())
            .setDynamicStates(viewportAndScissorDynamic())
            .build(device, vkCtx_->getPipelineCache());
    }
    return streakPipeline_ && splashPipeline_ && pointPipeline_ && mistPipeline_;
}

void Weather::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    destroy(device, streakPipeline_);
    destroy(device, splashPipeline_);
    destroy(device, pointPipeline_);
    destroy(device, mistPipeline_);
    if (!buildPipelines()) LOG_ERROR("Weather::recreatePipelines: failed to create pipelines");
}

void Weather::loadAssets(pipeline::AssetManager* assets) {
    if (!assets || assets_) return;
    assets_ = assets;
    auto dbc = assets->loadDBC("Weather.dbc");
    if (!dbc || !dbc->isLoaded() || dbc->getFieldCount() < 8) {
        LOG_WARNING("Weather: Weather.dbc unavailable - no weather will be drawn");
        return;
    }
    for (uint32_t r = 0; r < dbc->getRecordCount(); ++r) {
        cw::WeatherRow row;
        const uint32_t type = dbc->getUInt32(r, 2);
        row.effect = type <= 3 ? static_cast<Effect>(type) : Effect::None;
        row.light = dbc->getFloat(r, 3);
        row.color = {dbc->getFloat(r, 4), dbc->getFloat(r, 5), dbc->getFloat(r, 6)};
        row.texture = dbc->getString(r, 7);
        rows_[dbc->getUInt32(r, 0)] = std::move(row);
    }
    LOG_INFO("Weather: ", rows_.size(), " Weather.dbc rows");
    // Weather the server sent before the table was read.
    if (haveWeather_) setWeather(weatherId_, target_, true);
}

void Weather::setGroundQuery(cw::HeightCache::Query query) {
    // The cache asks in the client's coordinates; the renderer answers in its own.
    ground_.setQuery([q = std::move(query)](float x, float y, float z) {
        const glm::vec3 r = toRender({x, y, z});
        return q ? q(r.x, r.y, r.z) : z - 200.0f;
    });
}

void Weather::setDensityLevel(int level) {
    densityLevel_ = std::clamp(level, 0, 3);
}

void Weather::setWeather(uint32_t weatherId, float intensity, bool abrupt) {
    // 0x007846a0. The row decides the effect, its colour and its texture.
    haveWeather_ = true;
    weatherId_ = weatherId;
    cw::WeatherRow row;
    if (auto it = rows_.find(weatherId); it != rows_.end()) row = it->second;
    const Effect effect = row.effect;
    light_.set(effect, intensity, row.light, abrupt, active_, now_);
    smooth_ = !abrupt;
    color_ = row.color;
    std::string texture = row.texture;
    if (texture.empty()) texture = cw::defaultTexture(effect);
    if (effect != active_ || texture != texture_) {
        pending_ = effect;
        changePending_ = true;
        texture_ = texture;
    }
    from_ = target_;
    target_ = std::clamp(intensity, 0.0f, 1.0f);
    if (abrupt) {
        from_ = target_;
        current_ = target_;
    }
    easeStart_ = now_;
}

Weather::Type Weather::getWeatherType() const {
    switch (active_) {
        case Effect::Rain: return Type::RAIN;
        case Effect::Snow: return Type::SNOW;
        case Effect::Sand: return Type::SAND;
        default: return Type::NONE;
    }
}

int Weather::getParticleCount() const {
    int n = 0;
    for (const auto& p : packets_) n += static_cast<int>(p->count);
    return n;
}

Weather::Packet* Weather::takePacket() {
    std::unique_ptr<Packet> packet;
    for (auto it = freePackets_.begin(); it != freePackets_.end(); ++it) {
        if ((*it)->freeAfterFrame <= frame_) {
            packet = std::move(*it);
            freePackets_.erase(it);
            break;
        }
    }
    if (!packet) {
        packet = std::make_unique<Packet>();
        AllocatedBuffer buf = createBuffer(vkCtx_->getAllocator(), cw::kPacketSize * sizeof(GpuParticle),
                                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        AllocatedBuffer splash = createBuffer(vkCtx_->getAllocator(), cw::kPacketSize * sizeof(GpuSplash),
                                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        packet->buffer = buf.buffer;
        packet->allocation = buf.allocation;
        packet->mapped = buf.info.pMappedData;
        packet->splashBuffer = splash.buffer;
        packet->splashAllocation = splash.allocation;
        packet->splashMapped = splash.info.pMappedData;
        if (!packet->mapped || !packet->splashMapped) {
            destroy(vkCtx_->getAllocator(), packet->buffer, packet->allocation);
            destroy(vkCtx_->getAllocator(), packet->splashBuffer, packet->splashAllocation);
            return nullptr;
        }
    }
    packet->count = 0;
    packet->splashCount = 0;
    packet->gone = 0.0f;
    packet->open = true;
    packets_.push_back(std::move(packet));
    return packets_.back().get();
}

void Weather::retire(std::unique_ptr<Packet> packet) {
    // Still read by the frames in flight that drew it.
    packet->freeAfterFrame = frame_ + 3;
    freePackets_.push_back(std::move(packet));
}

void Weather::clearPackets() {
    for (auto& p : packets_) retire(std::move(p));
    packets_.clear();
}

void Weather::spawn(float dt, const cw::SpawnContext& ctx) {
    const float rate = cw::spawnRate(active_, cw::densityScale(densityLevel_), ctx.strength);
    int n = cw::spawnCount(dt, rate);
    if (n <= 0) return;

    Packet* packet = packets_.empty() || !packets_.back()->open ? nullptr : packets_.back().get();
    if (!packet) {
        packet = takePacket();
        if (!packet) return;
        packet->base = now_ - dt;
    }
    // The packet's own clock: the frame just gone runs from `from` to `from + dt`.
    const float from = static_cast<float>(now_ - dt - packet->base);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const cw::Random rnd = [&]() { return uni(rng_); };

    n = std::min<int>(n, static_cast<int>(cw::kPacketSize - packet->count));
    auto* out = static_cast<GpuParticle*>(packet->mapped);
    auto* splashes = static_cast<GpuSplash*>(packet->splashMapped);
    for (int i = 0; i < n; ++i) {
        const float start = rnd() * dt + from;
        cw::Particle p;
        float gone = 0.0f;
        switch (active_) {
            case Effect::Rain:
                p = cw::spawnRain(rnd, ctx, ground_, start);
                gone = cw::rainGone(p);
                if (cw::splashes(p, ctx.riding)) {
                    const cw::Splash s = cw::splashOf(p);
                    const glm::vec3 pos = toRender(s.position);
                    splashes[packet->splashCount++] = {{pos.x, pos.y, pos.z}, {s.start, s.end}};
                    gone = std::max(gone, s.end);
                }
                break;
            case Effect::Snow:
                p = cw::spawnSnow(rnd, ctx, ground_, start);
                gone = cw::snowGone(p);
                break;
            case Effect::Sand:
                p = cw::spawnSand(rnd, ctx, ground_, start);
                gone = cw::sandGone(p);
                break;
            default:
                return;
        }
        out[packet->count++] = gpuParticle(p);
        packet->gone = std::max(packet->gone, gone);
    }
    // Closed when full or when it has been filling for its window (0x00787ce0).
    if (packet->count >= cw::kPacketSize ||
        static_cast<float>(now_ - packet->base) > cw::packetWindow(active_)) {
        packet->open = false;
    }
}

void Weather::update(const FrameInput& frame) {
    const float dt = std::max(frame.deltaTime, 0.0f);
    now_ += dt;
    ++frame_;
    viewportWidth_ = frame.viewportWidth;

    // The player's movement, from where it has been (0x0078c500).
    const glm::vec3 player = toCanonical(frame.playerPosition);
    if (havePlayer_) velocity_.add(player - lastPlayer_, static_cast<int>(std::lround(dt * 1000.0f)));
    lastPlayer_ = player;
    havePlayer_ = true;

    // Close a packet left open past its window, then retire what has all
    // fallen (0x00787ce0).
    if (!packets_.empty() && packets_.back()->open &&
        static_cast<float>(now_ - packets_.back()->base) > cw::packetWindow(active_)) {
        packets_.back()->open = false;
    }
    for (auto it = packets_.begin(); it != packets_.end();) {
        if (!(*it)->open && static_cast<float>(now_ - (*it)->base) > (*it)->gone) {
            retire(std::move(*it));
            it = packets_.erase(it);
        } else {
            ++it;
        }
    }

    // 0x0078d170: a change of effect waits for the old one's particles to be
    // gone unless it came abruptly, the old one making no more meanwhile and
    // easing toward a quarter.
    bool fading = false;
    if (changePending_) {
        const bool mistsLive = std::any_of(mists_.begin(), mists_.end(), [&](const cw::Mist& m) {
            return m.live() && m.start < now_ && now_ <= m.end;
        });
        if ((packets_.empty() && !mistsLive) || !smooth_) {
            clearPackets();
            std::fill(mists_.begin(), mists_.end(), cw::Mist{});
            mistAccum_ = 0.0f;
            active_ = pending_;
            changePending_ = false;
            stopping_ = false;
            from_ = current_;
            easeStart_ = now_;
            ground_.clear();
            // Loaded here, on the frame's own thread, not while recording.
            if (active_ != Effect::None) textureSet(texture_);
            if (active_ == Effect::Rain) textureSet(cw::kSplashTexture);
            if (active_ != Effect::None) textureSet(cw::mistSpec(active_).texture);
        } else {
            stopping_ = true;
            fading = true;
            current_ = cw::easedIntensity(from_, std::min(target_, 0.25f),
                                          static_cast<float>(now_ - easeStart_));
            if (!packets_.empty()) packets_.back()->open = false;
        }
    }
    if (!fading) current_ = cw::easedIntensity(from_, target_, static_cast<float>(now_ - easeStart_));

    cameraRender_ = frame.cameraPosition;
    right_ = frame.cameraRight * (cw::kMistSize * 0.5f);
    up_ = frame.cameraUp * (cw::kMistSize * 0.5f);
    if (active_ == Effect::None) return;

    const glm::vec3 camera = toCanonical(frame.cameraPosition);
    ground_.recenter(glm::vec2(camera));

    cw::SpawnContext ctx;
    ctx.camera = camera;
    ctx.playerVelocity = velocity_.velocity();
    const float facing = core::coords::characterYawDegToCanonical(frame.playerYawDeg);
    ctx.wind = cw::windFor(ctx.playerVelocity, facing < 0.0f ? facing + 6.2831855f : facing);
    ctx.strength = cw::effectStrength(current_);
    ctx.riding = frame.riding;
    updateMists(dt, ctx);
    if (!stopping_) spawn(dt, ctx);
}

void Weather::updateMists(float dt, const cw::SpawnContext& ctx) {
    // 0x00786e10: the sheets made a second, never more banked than slots.
    const float before = static_cast<float>(now_ - dt);
    const float now = static_cast<float>(now_);
    mistAccum_ = std::min(mistAccum_ + dt * cw::mistRate(active_, cw::densityScale(densityLevel_),
                                                         ctx.strength),
                          static_cast<float>(cw::kMistSlots));
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    const cw::Random rnd = [&]() { return uni(rng_); };
    const cw::MistSpec spec = cw::mistSpec(active_);
    for (cw::Mist& m : mists_) {
        if (m.live() && m.end < now) m = cw::Mist{};
        if (stopping_ && m.live() && now <= m.start) m = cw::Mist{};
        if (!m.live() && mistAccum_ >= 1.0f && !stopping_) {
            mistAccum_ -= 1.0f;
            m = cw::spawnMist(rnd, spec, ctx, ground_, before + rnd() * dt);
        }
        if (m.live()) cw::stepMist(m, before, now);
    }
}

VkDescriptorSet Weather::textureSet(const std::string& path) {
    if (path.empty()) return blank_ ? blank_->set : VK_NULL_HANDLE;
    auto it = textures_.find(path);
    if (it != textures_.end()) return it->second ? it->second->set : VK_NULL_HANDLE;
    std::unique_ptr<Texture> tex;
    if (assets_ && textures_.size() + 1 < kMaxTextures) {
        pipeline::BLPImage image = assets_->loadTexture(path);
        auto t = std::make_unique<Texture>();
        VkDevice device = vkCtx_->getDevice();
        if (image.isValid() &&
            t->texture.upload(*vkCtx_, image.data.data(), image.width, image.height,
                              VK_FORMAT_R8G8B8A8_UNORM, true) &&
            t->texture.createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR,
                                     VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE) &&
            t->texture.isValid()) {
            VkDescriptorSetAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            alloc.descriptorPool = descriptorPool_;
            alloc.descriptorSetCount = 1;
            alloc.pSetLayouts = &textureLayout_;
            if (vkAllocateDescriptorSets(device, &alloc, &t->set) == VK_SUCCESS) {
                VkDescriptorImageInfo info = t->texture.descriptorInfo();
                VkWriteDescriptorSet write{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = t->set;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &info;
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
                tex = std::move(t);
            }
        }
        if (!tex) LOG_WARNING("Weather: failed to load ", path);
    }
    VkDescriptorSet set = tex ? tex->set : VK_NULL_HANDLE;
    textures_[path] = std::move(tex);
    return set;
}

VkDescriptorSet Weather::loadedSet(const std::string& path) const {
    if (path.empty()) return blank_ ? blank_->set : VK_NULL_HANDLE;
    auto it = textures_.find(path);
    return it != textures_.end() && it->second ? it->second->set : VK_NULL_HANDLE;
}

void Weather::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!vkCtx_) return;
    const Effect effect = active_;
    if (effect == Effect::None) return;

    WeatherPush push{};
    push.color = glm::vec4(color_, 1.0f);
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDeviceSize offset = 0;

    auto drawPackets = [&](VkPipeline pipeline, VkDescriptorSet set, bool splash) {
        if (!pipeline || !set) return;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        VkDescriptorSet sets[2] = {perFrameSet, set};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2, sets, 0, nullptr);
        for (const auto& p : packets_) {
            const uint32_t n = splash ? p->splashCount : p->count;
            if (n == 0) continue;
            push.params.x = static_cast<float>(now_ - p->base);
            vkCmdPushConstants(cmd, pipelineLayout_, stages, 0, sizeof(push), &push);
            vkCmdBindVertexBuffers(cmd, 0, 1, splash ? &p->splashBuffer : &p->buffer, &offset);
            if (pipeline == pointPipeline_) vkCmdDraw(cmd, n, 1, 0, 0);
            else vkCmdDraw(cmd, 3, n, 0, 0);
        }
    };

    switch (effect) {
        case Effect::Rain:
            // 0x0078ae20: the drops (0x0078a640), then the splashes (0x0078a030).
            drawPackets(streakPipeline_, loadedSet(texture_), false);
            drawPackets(splashPipeline_, loadedSet(cw::kSplashTexture), true);
            break;
        case Effect::Snow:
            push.params.y = 0.0f;
            drawPackets(pointPipeline_, loadedSet(texture_), false);
            break;
        case Effect::Sand:
            // c9.z: the viewport's width by 0.0025 (0x0078bee0). Its colour, c0,
            // is the zone's fog colour, which the program reads from the frame.
            push.params.y = 1.0f;
            push.params.z = static_cast<float>(viewportWidth_) * 0.0025f;
            drawPackets(pointPipeline_, loadedSet(""), false);
            break;
        default:
            break;
    }

    // The mist, after the particles (0x0078ae20 and its siblings end with
    // 0x00786e10): a quad a sheet facing the camera, in the fog's colour.
    const VkDescriptorSet mistSet = loadedSet(cw::mistSpec(effect).texture);
    if (!mistPipeline_ || !mistSet) return;
    mistFrame_ = (mistFrame_ + 1) % kMistFrames;
    auto* out = static_cast<GpuMistVertex*>(mistMapped_[mistFrame_]);
    if (!out) return;
    const float now = static_cast<float>(now_);
    const glm::vec3 camera = toCanonical(cameraRender_);
    uint32_t n = 0;
    for (const cw::Mist& m : mists_) {
        if (!m.live() || now < m.start) continue;
        const glm::vec3 centre = toRender(m.position);
        const glm::vec3 c[4] = {centre - right_ + up_, centre + right_ + up_,
                                centre + right_ - up_, centre - right_ - up_};
        const glm::vec2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        float a[4];
        for (int k = 0; k < 4; ++k) a[k] = cw::mistAlpha(m, now, glm::length(toCanonical(c[k]) - camera));
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int k : order) {
            out[n++] = {{c[k].x, c[k].y, c[k].z}, {uv[k].x, uv[k].y}, a[k]};
        }
    }
    if (n == 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mistPipeline_);
    VkDescriptorSet sets[2] = {perFrameSet, mistSet};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 2, sets, 0, nullptr);
    vkCmdPushConstants(cmd, pipelineLayout_, stages, 0, sizeof(push), &push);
    vkCmdBindVertexBuffers(cmd, 0, 1, &mistBuffer_[mistFrame_], &offset);
    vkCmdDraw(cmd, n, 1, 0, 0);
}

void Weather::shutdown() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator allocator = vkCtx_->getAllocator();
    vkDeviceWaitIdle(device);
    auto release = [&](std::unique_ptr<Packet>& p) {
        destroy(allocator, p->buffer, p->allocation);
        destroy(allocator, p->splashBuffer, p->splashAllocation);
    };
    for (auto& p : packets_) release(p);
    for (auto& p : freePackets_) release(p);
    packets_.clear();
    freePackets_.clear();
    for (auto& [path, t] : textures_) if (t) t->texture.destroy(device, allocator);
    textures_.clear();
    if (blank_) blank_->texture.destroy(device, allocator);
    blank_.reset();
    destroy(device, streakPipeline_);
    destroy(device, splashPipeline_);
    destroy(device, pointPipeline_);
    destroy(device, mistPipeline_);
    for (int i = 0; i < kMistFrames; ++i) destroy(allocator, mistBuffer_[i], mistAllocation_[i]);
    destroy(device, pipelineLayout_);
    destroy(device, descriptorPool_);
    destroy(device, textureLayout_);
    vkCtx_ = nullptr;
}

} // namespace rendering
} // namespace wowee
