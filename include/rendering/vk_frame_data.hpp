#pragma once

#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <atomic>
#include <chrono>

namespace wowee {
namespace rendering {

// Must match the PerFrame UBO layout in all shaders (std140 alignment)
struct GPUPerFrameData {
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 lightSpaceMatrix;
    glm::vec4 lightDir;       // xyz = direction, w = unused
    glm::vec4 lightColor;     // xyz = color, w = unused
    glm::vec4 ambientColor;   // xyz = color, w = the baked terrain shadows' opacity (ch8.r)
    glm::vec4 viewPos;        // xyz = camera pos, w = unused
    glm::vec4 fogColor;       // xyz = color, w = fog exponent (1 = linear)
    glm::vec4 fogParams;      // x = fogStart, y = fogEnd, z = time, w = water ripple strength
    glm::vec4 shadowParams;   // x = enabled(0/1), y = strength, z = one texel of a cascade's tile, w = unused
    // The player, for effects that react to where they are standing: water
    // ripples and the procedural grass the player brushes past. playerWake
    // trails the player by a fixed time constant, so grass the player has
    // already walked through springs back over that interval instead of
    // snapping upright.
    glm::vec4 playerPos;      // xyz = player world position, w = horizontal speed (yd/s)
    glm::vec4 playerWake;     // xyz = trailing player position, w = unused
    // The fog volume at set 0 binding 2, and how to find a depth in it:
    // x = on (0/1), y = near edge of the first slice in yards,
    // z = 1 / ln(far / near), w = slice count. Off in the reflection pass and
    // the character preview, which bind a neutral volume there.
    glm::vec4 volumetricParams;
    // Last frame's ray traced lighting at set 0 bindings 3 and 4, and how to
    // find a surface in it (rt_lighting.glsli): the view-projection and camera
    // position it was traced with, and params.x = mode, 0 when there is no
    // result to read. Zero in the reflection pass and the character preview.
    glm::mat4 rtViewProj;
    glm::vec4 rtCameraPos;
    glm::vec4 rtParams;
    // The camera's own fog colour, its WMO interior's fog blended in by how
    // far in it is (0x007f16f0's 0xd38ba0). fogColor above is the zone's
    // (0xd38b8c), which the terrain, the sky, exterior groups and whatever
    // stands outside are fogged with. The distances are the blended ones for
    // both. Only an interior group, and what stands in one, takes this
    // (0x007a9380 by way of 0x007a8440, 0x007c1730).
    glm::vec4 cameraFogColor;
    // Light mode 2 (0x007a8b10): the direct and ambient light a WMO batch
    // whose material has flag 0x20 is lit by, the zone's two averaged
    // (0x007ee750's 0xd38cb0/cb4). Only the WMO shader reads these.
    glm::vec4 averagedDirectColor;
    glm::vec4 averagedAmbientColor;
    // x: the light's window level (0x007f3230 samples the curve at 0xaf4c80
    // into 0xd38cdc), which lights a WMO material's windows at night
    // (wmo_sidn.hpp). Only the WMO shader reads it.
    glm::vec4 windowLight{0.0f};
    // The world light's specular colour (FUN_008355d0's fourth output), w 1
    // with the client's 'specular' option on: the terrain's highlight
    // (Terrain vertex program's c27, exponent 20). Off by default.
    glm::vec4 specularColor{0.0f};
    // The sun's shadow cascades (shadow_csm.glsli), last so that the shaders
    // declaring only the start of this block keep their offsets. Each is a
    // square tile of one depth atlas, centred on the player; see
    // Renderer::computeLightSpaceMatrix.
    //
    // cascadeMatrix: world to the tile's atlas UV (xy) and depth (z), the
    //   tile's scale and offset baked in.
    // cascadeRect: the tile in atlas UV, min xy and max zw.
    // cascadeTexel: x one texel in yards, y and z one atlas texel in u and v.
    // cascadeInfo: x how many cascades there are (0: none, everything lit),
    //   y the fraction of a tile over which it blends into the next one,
    //   z where the last cascade starts fading out, as a fraction of its
    //   half-width.
    static constexpr int kMaxShadowCascades = 4;
    glm::mat4 cascadeMatrix[kMaxShadowCascades]{};
    glm::vec4 cascadeRect[kMaxShadowCascades]{};
    glm::vec4 cascadeTexel[kMaxShadowCascades]{};
    glm::vec4 cascadeInfo{0.0f};
};
// std140 adds no padding between mat4 and vec4 members, so this is the block's
// size on the GPU as well. A change here is a change to every PerFrame block
// that declares the members it touches.
static_assert(sizeof(GPUPerFrameData) == 928, "GPUPerFrameData no longer matches the shaders' PerFrame");

// Push constants for the model matrix (most common case)
struct GPUPushConstants {
    glm::mat4 model;
};

// Push constants for shadow rendering passes.
//
// The light-space and model matrices are multiplied on the CPU, since nothing
// in the shadow shaders wants them apart: the fragment shader's world
// position was never read. The instanced M2 caster puts the light's matrix
// alone here and reads the model per instance.
struct ShadowPush {
    glm::mat4 lightSpaceModel;
    /// Unused and zero. This carried a procedural wind sway that the client
    /// does not have - it moves a model only by its own animation - and is
    /// kept so the layout the shadow shaders declare does not move.
    glm::vec4 sway{0.0f};
    /// x: sample the texture. y: alpha-test it. z: unused, zero. w: the first
    /// instance, for the instanced M2 caster.
    ///
    /// These were in the uniform buffer, and the M2 pass writes that buffer
    /// twice a frame - once for its solid casters, once for its foliage. A
    /// uniform buffer is read when the draw executes rather than when it is
    /// recorded, so the second write decided what the first pass's draws saw,
    /// and one persistently-mapped copy shared between frames in flight let a
    /// CPU write land in the middle of the previous frame's reads. Losing the
    /// alpha test that way turns a leaf cutout into the solid outline of the
    /// canopy. Push constants are recorded into the command buffer with the
    /// draw, so there is nothing left to race.
    glm::ivec4 flags{0};
    /// Unused and zero, as sway.
    glm::vec4 wind{0.0f};
};

// Uniform buffer for shadow rendering parameters (matches shader std140 layout)
struct ShadowParamsUBO {
    int32_t useBones;
    int32_t useTexture;
    int32_t alphaTest;
    int32_t foliageSway;
    float windTime;
    float foliageMotionDamp;
};

// Timer utility for performance profiling queries.
// Uses atomics because floor-height queries are dispatched on async threads
// from CameraController while the main thread may read the counters.
struct QueryTimer {
    std::atomic<double>* totalMs = nullptr;
    std::atomic<uint32_t>* callCount = nullptr;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    QueryTimer(std::atomic<double>* total, std::atomic<uint32_t>* calls)
        : totalMs(total), callCount(calls) {}
    ~QueryTimer() {
        if (callCount) {
            callCount->fetch_add(1, std::memory_order_relaxed);
        }
        if (totalMs) {
            auto end = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(end - start).count();
            // Relaxed is fine for diagnostics - exact ordering doesn't matter.
            double old = totalMs->load(std::memory_order_relaxed);
            while (!totalMs->compare_exchange_weak(old, old + ms, std::memory_order_relaxed)) {}
        }
    }
};

} // namespace rendering
} // namespace wowee
