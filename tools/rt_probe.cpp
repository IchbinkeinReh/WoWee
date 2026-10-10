// rt_probe: the ray traced lighting pass, run once against a scene whose
// answer is known, on whatever Vulkan device this machine has.
//
// The pass only runs in the world, and the world needs a realm. This stands
// up the real context and the real RtScene / RtLighting on a hidden window,
// puts a box between a wall and the sun, fakes the wall's depth buffer, traces
// one frame and reads the result back:
//
//   - a wall point whose ray to the sun crosses the box must be in shadow,
//   - wall points whose rays miss it must be lit,
//   - every value must be finite,
//   - in the probe volume (RtGiProbes), a probe in the open must hold the sky
//     above it and the open ground's light below, and one beside the box's
//     sunlit face must see it lit,
//   - then, with orange ground and a roof slab added and the volume left to
//     converge, the surfaces' ambient factor (rt_gi_check.comp, the shaders'
//     own code) must be about one on open ground at any orientation, with
//     little of the ground's colour in it, and well under one beneath the roof,
//   - and the sun the surfaces read must be the same in Full mode as in
//     ShadowsAO.
//
//     cmake -S . -B build -DWOWEE_BUILD_RT_PROBE=ON
//     cmake --build build --target rt_probe
//     cd build/bin && ./rt_probe            (exit status 0 = pass)
//
// WOWEE_RT_FORCE_SOFTWARE=1 selects the compute tracer on hardware that has
// ray queries, so both backends can be checked on one machine.

#include "core/logger.hpp"
#include "rendering/rt_bvh.hpp"
#include "rendering/rt_gi_volume.hpp"
#include "rendering/rt_lighting.hpp"
#include "rendering/rt_scene.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace wowee::rendering;

namespace {

constexpr uint32_t kSize = 256;       // scene depth, square
constexpr float kWallDistance = 20.0f;

RtScene::MeshSource box(const glm::vec3& lo, const glm::vec3& hi,
                        const glm::vec3& albedo = glm::vec3(0.6f)) {
    RtScene::MeshSource m;
    for (int i = 0; i < 8; ++i) {
        m.positions.emplace_back((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
    }
    const uint32_t faces[6][4] = {{0, 2, 6, 4}, {1, 5, 7, 3}, {0, 4, 5, 1},
                                  {2, 3, 7, 6}, {0, 1, 3, 2}, {4, 6, 7, 5}};
    for (const auto& f : faces) {
        for (uint32_t idx : {f[0], f[1], f[2], f[0], f[2], f[3]}) m.indices.push_back(idx);
    }
    m.surfaces.push_back(packRtSurface(albedo, 1.0f));
    return m;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // what was printed survives a crash
    wowee::core::Logger::getInstance().setLogLevel(wowee::core::LogLevel::INFO);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }
    SDL_Window* window = SDL_CreateWindow("rt_probe", kSize, kSize,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return 2;
    }

    int status = 1;
    {
        VkContext ctx;
        if (!ctx.initialize(window)) {
            std::fprintf(stderr, "VkContext failed\n");
            return 2;
        }
        RtScene scene;
        RtLighting lighting;
        if (!scene.initialize(&ctx) || !lighting.initialize(&ctx, &scene)) {
            std::fprintf(stderr, "RT init failed\n");
            return 2;
        }
        std::printf("backend: %s\n",
                    scene.backend() == RtScene::Backend::Hardware ? "hardware" : "compute");

        // A 2x4x4 box five yards in front of the wall. The camera looks down
        // +X with Z up, as the client's does.
        const RtScene::MeshId mesh = scene.addMesh(box({14, -2, -2}, {16, 2, 2}));
        scene.addInstance(mesh, glm::mat4(1.0f));
        // A second instance of it, moved, to exercise the top level.
        scene.addInstance(mesh, glm::translate(glm::mat4(1.0f), glm::vec3(0, 40, 0)));

        glm::mat4 proj = glm::perspective(glm::radians(60.0f), 1.0f, 0.5f, 2000.0f);
        proj[1][1] *= -1.0f;
        const glm::mat4 view = glm::lookAt(glm::vec3(0), glm::vec3(1, 0, 0), glm::vec3(0, 0, 1));
        const glm::mat4 viewProj = proj * view;
        const glm::vec4 wallClip = viewProj * glm::vec4(kWallDistance, 0, 0, 1);
        const float wallDepth = wallClip.z / wallClip.w;

        const VkExtent2D sceneExtent{kSize, kSize};
        AllocatedImage depth = createImage(ctx.getDevice(), ctx.getAllocator(), kSize, kSize,
                                           ctx.getDepthFormat(),
                                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                               VK_IMAGE_USAGE_SAMPLED_BIT |
                                               VK_IMAGE_USAGE_TRANSFER_DST_BIT);

        lighting.setMode(RtLighting::Mode::Full);
        lighting.prepare(sceneExtent);

        RtLighting::FrameInputs in{};
        in.viewProj = viewProj;
        in.cameraPos = glm::vec3(0);
        in.sunDir = glm::normalize(glm::vec3(-1.0f, 0.0f, 0.25f));  // toward the sun
        in.sunColor = glm::vec3(1.0f);
        in.skyColor = glm::vec3(0.3f, 0.35f, 0.4f);
        in.sunUp = true;

        auto depthLayout = [&](VkCommandBuffer cmd, VkImageLayout from, VkImageLayout to) {
            VkImageMemoryBarrier2 b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcStageMask = b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            b.srcAccessMask = b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = depth.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            cmdPipelineBarrier2(cmd, dep);
        };
        ctx.immediateSubmit([&](VkCommandBuffer cmd) {
            depthLayout(cmd, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearDepthStencilValue clear{wallDepth, 0};
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            vkCmdClearDepthStencilImage(cmd, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        &clear, 1, &range);
            depthLayout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            lighting.record(cmd, depth.image, sceneExtent, false, in);
        });

        const VkExtent2D te = lighting.traceExtent();
        const VkDeviceSize bytes = VkDeviceSize(te.width) * te.height * 8;
        AllocatedBuffer light = createBuffer(ctx.getAllocator(), bytes,
                                             VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                             VMA_MEMORY_USAGE_CPU_ONLY);
        namespace gi = wowee::rendering::gi;
        const VkImage atlas = lighting.probes().atlasImage();
        const VkDeviceSize atlasBytes = VkDeviceSize(gi::kAtlasWidth) * gi::kAtlasHeight * 8;
        AllocatedBuffer probes = createBuffer(ctx.getAllocator(), atlasBytes,
                                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                              VMA_MEMORY_USAGE_CPU_ONLY);
        ctx.immediateSubmit([&](VkCommandBuffer cmd) {
            VkMemoryBarrier2 mb{};
            mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
            mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo dep{};
            dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &mb;
            cmdPipelineBarrier2(cmd, dep);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {te.width, te.height, 1};
            vkCmdCopyImageToBuffer(cmd, lighting.lightImageWrittenBy(ctx.getCurrentFrame()),
                                   VK_IMAGE_LAYOUT_GENERAL, light.buffer, 1, &region);
            if (atlas != VK_NULL_HANDLE) {
                region.imageExtent = {uint32_t(gi::kAtlasWidth), uint32_t(gi::kAtlasHeight), 1};
                vkCmdCopyImageToBuffer(cmd, atlas, VK_IMAGE_LAYOUT_GENERAL, probes.buffer, 1, &region);
            }
        });
        vkDeviceWaitIdle(ctx.getDevice());

        const auto* lightTexels = static_cast<const uint16_t*>(light.info.pMappedData);
        auto texel = [&](const uint16_t* base, uint32_t x, uint32_t y, int c) {
            return glm::unpackHalf1x16(base[(size_t(y) * te.width + x) * 4 + c]);
        };
        auto sample = [&](const glm::vec3& world, float& sun, float& dist) {
            const glm::vec4 c = viewProj * glm::vec4(world, 1.0f);
            const glm::vec2 uv = glm::vec2(c) / c.w * 0.5f + 0.5f;
            const auto x = static_cast<uint32_t>(uv.x * te.width);
            const auto y = static_cast<uint32_t>(uv.y * te.height);
            sun = texel(lightTexels, x, y, 0);
            dist = texel(lightTexels, x, y, 3);
        };

        int failures = 0;
        auto expect = [&](const char* what, bool ok) {
            std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };

        // Rays toward the sun rise a quarter yard per yard: from the wall, the
        // box's front face (x = 16) is four yards back, so a wall point is
        // shadowed for z in about [-3, 1] and |y| < 2.
        float sun = 0, dist = 0;
        sample({kWallDistance, 0.0f, -1.0f}, sun, dist);
        std::printf("wall behind box: sun=%.3f dist=%.2f\n", sun, dist);
        expect("wall point behind the box is shadowed", sun < 0.25f);
        expect("stored distance matches the wall", std::abs(dist - kWallDistance) < 0.5f);
        sample({kWallDistance, 0.0f, 5.0f}, sun, dist);
        std::printf("wall above shadow: sun=%.3f\n", sun);
        expect("wall point above the shadow is lit", sun > 0.75f);
        sample({kWallDistance, 7.0f, 0.0f}, sun, dist);
        std::printf("wall beside shadow: sun=%.3f\n", sun);
        expect("wall point beside the shadow is lit", sun > 0.75f);

        bool finite = true;
        uint32_t shadowed = 0;
        for (uint32_t y = 0; y < te.height; ++y) {
            for (uint32_t x = 0; x < te.width; ++x) {
                for (int c = 0; c < 4; ++c) finite = finite && std::isfinite(texel(lightTexels, x, y, c));
                if (texel(lightTexels, x, y, 0) < 0.5f) ++shadowed;
            }
        }
        std::printf("trace %ux%u, %u shadowed texels\n", te.width, te.height, shadowed);
        expect("every value is finite", finite);
        expect("the shadow is a patch, not the frame", shadowed > 20 && shadowed < te.width * te.height / 3);

        // The probe volume. The camera is at the origin, so the near cascade's
        // probe (0, 0, 0) is there, in the open - the box is a speck from it -
        // and probe (4, 0, 0) stands two yards before the box's sunlit face.
        expect("the probe volume exists in Full mode", atlas != VK_NULL_HANDLE);
        if (atlas != VK_NULL_HANDLE) {
            const auto* atlasTexels = static_cast<const uint16_t*>(probes.info.pMappedData);
            auto atlasTexel = [&](glm::ivec2 p) {
                const size_t at = (size_t(p.y) * gi::kAtlasWidth + p.x) * 4;
                return glm::vec3(glm::unpackHalf1x16(atlasTexels[at]),
                                 glm::unpackHalf1x16(atlasTexels[at + 1]),
                                 glm::unpackHalf1x16(atlasTexels[at + 2]));
            };
            // The irradiance a probe holds for the interior texel facing
            // closest to `dir`.
            auto irradiance = [&](glm::ivec3 abs, glm::vec3 dir) {
                const glm::ivec2 origin = gi::irradianceTileOrigin(0, gi::storageOf(abs));
                float best = -2.0f;
                glm::ivec2 pick(0);
                for (int j = 0; j < gi::kIrradianceTexels; ++j) {
                    for (int i = 0; i < gi::kIrradianceTexels; ++i) {
                        const float c = glm::dot(gi::texelDirection({i, j}, gi::kIrradianceTexels), dir);
                        if (c > best) {
                            best = c;
                            pick = {i, j};
                        }
                    }
                }
                return atlasTexel(origin + 1 + pick);
            };
            bool atlasFinite = true;
            for (size_t i = 0; i < size_t(gi::kAtlasWidth) * gi::kAtlasHeight * 4; ++i) {
                atlasFinite = atlasFinite && std::isfinite(glm::unpackHalf1x16(atlasTexels[i]));
            }
            expect("every probe value is finite", atlasFinite);

            const glm::vec3 sky = in.skyColor;
            const glm::vec3 up = irradiance({0, 0, 0}, {0, 0, 1});
            const glm::vec3 down = irradiance({0, 0, 0}, {0, 0, -1});
            std::printf("open probe: up (%.3f %.3f %.3f) down (%.3f %.3f %.3f), sky (%.3f %.3f %.3f)\n",
                        up.r, up.g, up.b, down.r, down.g, down.b, sky.r, sky.g, sky.b);
            expect("an open probe holds the sky above it",
                   glm::all(glm::lessThan(glm::abs(up - sky), sky * 0.2f)));
            expect("and less below it, where the ground is", down.b < up.b * 0.8f);
            const glm::vec3 open = irradiance({0, 0, 0}, {1, 0, 0});
            const glm::vec3 lit = irradiance({4, 0, 0}, {1, 0, 0});
            std::printf("toward +x: open (%.3f %.3f %.3f), before the sunlit face (%.3f %.3f %.3f)\n",
                        open.r, open.g, open.b, lit.r, lit.g, lit.b);
            expect("a probe before a sunlit face is lit by it", lit.r > open.r * 1.2f);
        }

        // --- A field, left to converge ---------------------------------
        // Orange ground two yards below the camera, as a dirt road is - one
        // sheet, flagged as terrain, so the probes under it are found out -
        // and a roof slab over part of it. Everything else stays: the box
        // now stands on the ground.
        const glm::vec3 dirt(0.55f, 0.35f, 0.2f);
        RtScene::MeshSource ground;
        ground.positions = {{-400, -400, -2}, {400, -400, -2}, {400, 400, -2}, {-400, 400, -2}};
        ground.indices = {0, 1, 2, 0, 2, 3};
        ground.surfaces.push_back(packRtSurface(dirt, 1.0f));
        ground.flags = RtScene::kMeshTerrain;
        scene.addInstance(scene.addMesh(std::move(ground)), glm::mat4(1.0f));
        scene.addInstance(scene.addMesh(box({-26, -31, 1.0f}, {-4, -9, 1.5f})), glm::mat4(1.0f));
        // A solid block standing on the ground, as a building's foundation
        // does, with normals so its triangles have fronts: the probes inside
        // it must be switched off, or they darken its top.
        {
            RtScene::MeshSource block = box({20, -62, -2}, {62, -28, 4.5f});
            const glm::vec3 centre(41, -45, 1.25f);
            for (const auto& p : block.positions) block.normals.push_back(glm::normalize(p - centre));
            scene.addInstance(scene.addMesh(std::move(block)), glm::mat4(1.0f));
        }

        auto frame = [&]() {
            lighting.prepare(sceneExtent);
            ctx.immediateSubmit([&](VkCommandBuffer cmd) {
                lighting.record(cmd, depth.image, sceneExtent, false, in);
            });
        };
        // Long enough for a probe updated one frame in four to be blended
        // over many times, and for the bounces to settle.
        for (int i = 0; i < 160; ++i) frame();
        vkDeviceWaitIdle(ctx.getDevice());

        struct Point {
            const char* what;
            glm::vec3 p, n;
        };
        const Point points[] = {
            {"open ground, facing up", {-10, 10, -2}, {0, 0, 1}},
            {"open ground, facing up, far", {-40, 30, -2}, {0, 0, 1}},
            {"in the open, facing sideways", {-10, 10, -1}, {1, 0, 0}},
            {"in the open, facing sideways (2)", {-10, 10, -1}, {0, -1, 0}},
            {"in the open, facing down", {-10, 10, -0.5f}, {0, 0, -1}},
            {"under the roof", {-15, -20, -2}, {0, 0, 1}},
            {"before the box's sunlit face", {12.5f, 0, -2}, {0, 0, 1}},
            {"on top of the solid block", {41, -45, 4.5f}, {0, 0, 1}},
        };
        constexpr uint32_t kPoints = sizeof(points) / sizeof(points[0]);
        const RtLighting::ConsumerData cd = lighting.consumerData();
        expect("the surfaces are told the volume is on", cd.gi.params.x > 0.5f);

        VkDevice dev = ctx.getDevice();
        AllocatedBuffer pts = createBuffer(ctx.getAllocator(), kPoints * 3 * sizeof(glm::vec4),
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        AllocatedBuffer res = createBuffer(ctx.getAllocator(), kPoints * 2 * sizeof(glm::vec4),
                                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
        struct CheckBlock {
            glm::vec4 vol[2], volMod[2], sky, ground, params;
        } block{};
        for (int c = 0; c < 2; ++c) {
            block.vol[c] = cd.gi.volume[c];
            block.volMod[c] = cd.gi.volumeMod[c];
        }
        block.sky = cd.gi.sky;
        block.ground = cd.gi.ground;
        block.params = glm::vec4(cd.gi.params.z, 0, 0, 0);
        AllocatedBuffer ubo = createBuffer(ctx.getAllocator(), sizeof(block),
                                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
        std::memcpy(ubo.info.pMappedData, &block, sizeof(block));
        auto* pv = static_cast<glm::vec4*>(pts.info.pMappedData);
        for (uint32_t i = 0; i < kPoints; ++i) {
            pv[i * 3] = glm::vec4(points[i].p, 0);
            pv[i * 3 + 1] = glm::vec4(points[i].n, 0);
            pv[i * 3 + 2] = glm::vec4(glm::normalize(in.cameraPos - points[i].p), 0);
        }

        VkDescriptorSetLayoutBinding lb[4]{};
        const VkDescriptorType types[4] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                           VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        for (uint32_t b = 0; b < 4; ++b) {
            lb[b].binding = b;
            lb[b].descriptorType = types[b];
            lb[b].descriptorCount = 1;
            lb[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 4;
        li.pBindings = lb;
        VkDescriptorSetLayout checkLayout = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(dev, &li, nullptr, &checkLayout);
        VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
                                      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = 1;
        pci.poolSizeCount = 3;
        pci.pPoolSizes = ps;
        VkDescriptorPool checkPool = VK_NULL_HANDLE;
        vkCreateDescriptorPool(dev, &pci, nullptr, &checkPool);
        VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        sai.descriptorPool = checkPool;
        sai.descriptorSetCount = 1;
        sai.pSetLayouts = &checkLayout;
        VkDescriptorSet checkSet = VK_NULL_HANDLE;
        vkAllocateDescriptorSets(dev, &sai, &checkSet);
        VkDescriptorImageInfo atlasInfo{lighting.sampler(), lighting.giViewForSlot(0), VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo bi[3] = {{ubo.buffer, 0, VK_WHOLE_SIZE},
                                        {pts.buffer, 0, VK_WHOLE_SIZE},
                                        {res.buffer, 0, VK_WHOLE_SIZE}};
        VkWriteDescriptorSet w[4]{};
        for (uint32_t b = 0; b < 4; ++b) {
            w[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[b].dstSet = checkSet;
            w[b].dstBinding = b;
            w[b].descriptorCount = 1;
            w[b].descriptorType = types[b];
            if (b == 0) w[b].pImageInfo = &atlasInfo;
            else w[b].pBufferInfo = &bi[b - 1];
        }
        vkUpdateDescriptorSets(dev, 4, w, 0, nullptr);
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &checkLayout;
        VkPipelineLayout checkPipelineLayout = VK_NULL_HANDLE;
        vkCreatePipelineLayout(dev, &plci, nullptr, &checkPipelineLayout);
        VkPipeline checkPipeline = VK_NULL_HANDLE;
        {
            wowee::rendering::VkShaderModule module;
            if (module.loadFromFile(dev, "assets/shaders/rt_gi_check.comp.spv")) {
                VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
                cpci.stage = module.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
                cpci.layout = checkPipelineLayout;
                vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &checkPipeline);
                module.destroy();
            }
        }
        expect("the surfaces' probe code loads", checkPipeline != VK_NULL_HANDLE);
        if (checkPipeline != VK_NULL_HANDLE) {
            ctx.immediateSubmit([&](VkCommandBuffer cmd) {
                VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers = &mb;
                cmdPipelineBarrier2(cmd, dep);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, checkPipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, checkPipelineLayout, 0, 1,
                                        &checkSet, 0, nullptr);
                vkCmdDispatch(cmd, kPoints, 1, 1);
                mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
                mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
                mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
                cmdPipelineBarrier2(cmd, dep);
            });
            vkDeviceWaitIdle(dev);
            const auto* rv = static_cast<const glm::vec4*>(res.info.pMappedData);
            std::printf("sky (%.3f %.3f %.3f) ground ref (%.3f %.3f %.3f)\n", block.sky.r, block.sky.g,
                        block.sky.b, block.ground.r, block.ground.g, block.ground.b);
            glm::vec3 ratio[kPoints];
            for (uint32_t i = 0; i < kPoints; ++i) {
                const glm::vec4 e = rv[i * 2];
                ratio[i] = glm::vec3(rv[i * 2 + 1]);
                std::printf("  %-34s E (%.3f %.3f %.3f) cover %.2f  ratio (%.3f %.3f %.3f)\n",
                            points[i].what, e.r, e.g, e.b, e.a, ratio[i].r, ratio[i].g, ratio[i].b);
            }
            auto lum = [](glm::vec3 c) { return glm::dot(c, glm::vec3(0.299f, 0.587f, 0.114f)); };
            auto near1 = [&](glm::vec3 r, float tol) { return std::abs(lum(r) - 1.0f) < tol; };
            auto hueSpread = [](glm::vec3 r) {
                return std::max(r.r, std::max(r.g, r.b)) / std::max(1e-4f, std::min(r.r, std::min(r.g, r.b)));
            };
            expect("open ground facing up keeps the flat ambient", near1(ratio[0], 0.1f) && near1(ratio[1], 0.1f));
            expect("open ground at other orientations, near it too",
                   near1(ratio[2], 0.25f) && near1(ratio[3], 0.25f) && near1(ratio[4], 0.35f));
            bool mildHue = true;
            for (uint32_t i = 0; i < kPoints; ++i) mildHue = mildHue && hueSpread(ratio[i]) < 1.6f;
            expect("the ground's colour tints, it does not take over", mildHue);
            expect("beneath the roof is darker", lum(ratio[5]) < 0.75f);
            expect("the top of a solid is not darkened by the probes inside it", near1(ratio[7], 0.1f));
        }
        vkDestroyPipeline(dev, checkPipeline, nullptr);
        vkDestroyPipelineLayout(dev, checkPipelineLayout, nullptr);
        vkDestroyDescriptorPool(dev, checkPool, nullptr);
        vkDestroyDescriptorSetLayout(dev, checkLayout, nullptr);
        destroyBuffer(ctx.getAllocator(), pts);
        destroyBuffer(ctx.getAllocator(), res);
        destroyBuffer(ctx.getAllocator(), ubo);

        // --- The sun in Full mode and in ShadowsAO --------------------------
        // The surfaces read the same image in both; the probes must leave it
        // alone.
        auto readSun = [&](float out[3]) {
            ctx.immediateSubmit([&](VkCommandBuffer cmd) {
                VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
                mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.memoryBarrierCount = 1;
                dep.pMemoryBarriers = &mb;
                cmdPipelineBarrier2(cmd, dep);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {te.width, te.height, 1};
                vkCmdCopyImageToBuffer(cmd, lighting.lightImageWrittenBy(ctx.getCurrentFrame()),
                                       VK_IMAGE_LAYOUT_GENERAL, light.buffer, 1, &region);
            });
            vkDeviceWaitIdle(dev);
            const glm::vec3 at[3] = {{kWallDistance, 0.0f, -1.0f}, {kWallDistance, 0.0f, 5.0f},
                                     {kWallDistance, 7.0f, 0.0f}};
            for (int i = 0; i < 3; ++i) {
                float dist = 0;
                sample(at[i], out[i], dist);
            }
        };
        float sunFull[3], sunAo[3];
        frame();
        readSun(sunFull);
        lighting.setMode(RtLighting::Mode::ShadowsAO);
        for (int i = 0; i < 8; ++i) frame();
        readSun(sunAo);
        std::printf("sun, Full vs ShadowsAO: %.2f/%.2f %.2f/%.2f %.2f/%.2f\n", sunFull[0], sunAo[0],
                    sunFull[1], sunAo[1], sunFull[2], sunAo[2]);
        bool sameSun = true;
        for (int i = 0; i < 3; ++i) sameSun = sameSun && std::abs(sunFull[i] - sunAo[i]) < 0.25f;
        expect("the sun reads the same in Full mode as in ShadowsAO", sameSun);

        destroyBuffer(ctx.getAllocator(), light);
        destroyBuffer(ctx.getAllocator(), probes);
        destroyImage(ctx.getDevice(), ctx.getAllocator(), depth);
        lighting.shutdown();
        scene.shutdown();
        ctx.shutdown();
        status = failures == 0 ? 0 : 1;
        std::printf("%s\n", status == 0 ? "PASS" : "FAIL");
    }
    SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}
