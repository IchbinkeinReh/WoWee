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
    glm::vec4 fogColor;       // xyz = color, w = unused
    glm::vec4 fogParams;      // x = fogStart, y = fogEnd, z = time, w = water ripple strength
    glm::vec4 shadowParams;   // x = enabled(0/1), y = strength, z = one shadow-map texel, w = unused
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
};

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
