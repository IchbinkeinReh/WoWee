#pragma once

/**
 * rt_capability.hpp - whether this device can run the ray traced lighting,
 * and if not, why.
 *
 * Decided once at start-up from what the physical device reports, before the
 * logical device is made (VkContext::selectPhysicalDevice), and asked again
 * when the anti-aliasing changes, because one requirement - shading per
 * sample - only applies under MSAA.
 *
 * A pure function of plain numbers so the decision can be tested without a
 * GPU: a phone that lacks dual-source blending, a laptop whose driver lists
 * the ray query extension without the feature, a desktop with everything.
 *
 * What the lighting needs (docs/deferred_rt_design.md):
 *
 *  - A tracer. The hardware one wants VK_KHR_acceleration_structure,
 *    VK_KHR_ray_query and VK_KHR_deferred_host_operations listed, their
 *    features (accelerationStructure, rayQuery) and bufferDeviceAddress, on a
 *    1.2 instance. Without them the compute tracer walks a BVH in storage
 *    buffers: it needs nothing but the compute limits below.
 *  - The compute passes: a 16 x 16 workgroup (rt_gi_exposure.comp), up to
 *    ten storage buffers in one stage (the GI trace: its two plus the
 *    software scene's eight), five storage images in one stage (rt_lighting's
 *    set), and rgba16f storage images.
 *  - The deferred scene passes: dualSrcBlend for the apply pass's
 *    dst * factor + addend, independentBlend for the G-buffer written
 *    unblended beside the blended colour, the G-buffer format renderable at
 *    the scene's sample count, and sampleRateShading to apply per sample
 *    under MSAA (VkContext::createDeferredScenePasses).
 */

#include <cstdint>
#include <string>

namespace wowee {
namespace rendering {
namespace rt_capability {

/// What the device reports, gathered by VkContext.
struct Input {
    // Hardware tracer.
    bool vulkan12 = false;                 ///< instance created at 1.2 or later
    bool accelerationStructureExt = false; ///< VK_KHR_acceleration_structure listed
    bool rayQueryExt = false;              ///< VK_KHR_ray_query listed
    bool deferredHostOpsExt = false;       ///< VK_KHR_deferred_host_operations listed
    bool bufferDeviceAddress = false;
    bool accelerationStructure = false;
    bool rayQuery = false;
    bool forceSoftware = false;            ///< WOWEE_RT_FORCE_SOFTWARE

    // Deferred scene passes.
    bool dualSrcBlend = false;
    bool independentBlend = false;
    bool sampleRateShading = false;
    /// VkSampleCountFlags the G-buffer format can be a sampled colour
    /// attachment at; 0 when it cannot be one at all.
    uint32_t gbufferSampleCounts = 0;

    // Compute passes.
    bool storageImageRgba16f = false;      ///< R16G16B16A16_SFLOAT storage + sampled
    uint32_t maxPerStageStorageBuffers = 0;
    uint32_t maxPerStageStorageImages = 0;
    uint32_t maxComputeWorkGroupInvocations = 0;
};

enum class Tracer { None, Hardware, Software };

/// The requirements, as numbers, for the tests and the log to name.
constexpr uint32_t kStorageBuffersSoftware = 10;  ///< GI set (2) + software scene (8)
constexpr uint32_t kStorageBuffersHardware = 8;   ///< GI set (2) + hardware scene (6)
constexpr uint32_t kStorageImages = 5;            ///< rt_lighting's set
constexpr uint32_t kWorkGroupInvocations = 256;   ///< 16 x 16 (rt_gi_exposure.comp)

/// Whether the hardware tracer can be used, and if not, the first reason.
inline bool hardwareTracerUsable(const Input& in, std::string* whyNot = nullptr) {
    auto no = [&](const char* why) {
        if (whyNot) *whyNot = why;
        return false;
    };
    if (in.forceSoftware) return no("WOWEE_RT_FORCE_SOFTWARE");
    if (!in.accelerationStructureExt) return no("no VK_KHR_acceleration_structure");
    if (!in.rayQueryExt) return no("no VK_KHR_ray_query");
    if (!in.deferredHostOpsExt) return no("no VK_KHR_deferred_host_operations");
    if (!in.vulkan12) return no("Vulkan 1.2 instance unavailable");
    if (!in.bufferDeviceAddress) return no("no bufferDeviceAddress");
    if (!in.accelerationStructure) return no("no accelerationStructure feature");
    if (!in.rayQuery) return no("no rayQuery feature");
    if (in.maxPerStageStorageBuffers < kStorageBuffersHardware) return no("too few storage buffers per stage");
    if (whyNot) whyNot->clear();
    return true;
}

/// The decision.
struct Result {
    bool available = false;      ///< the lighting can run at this sample count
    Tracer tracer = Tracer::None;
    /// Why it cannot run, naming what is missing; empty when it can.
    std::string missing;
    /// Why the hardware tracer is not used, when it is not; for the log.
    std::string hardwareWhyNot;
    /// It could run, but not at this sample count: MSAA off would allow it.
    bool blockedByMsaa = false;
};

/// What the device lacks for the ray traced lighting at `msaaSamples`
/// (1, 2, 4, 8), every missing piece named, comma separated.
inline Result evaluate(const Input& in, uint32_t msaaSamples) {
    Result r;
    std::string missing;
    auto lack = [&](const std::string& what) {
        if (!missing.empty()) missing += ", ";
        missing += what;
    };

    const bool hardware = hardwareTracerUsable(in, &r.hardwareWhyNot);

    // The compute passes, whichever tracer.
    if (!in.storageImageRgba16f) lack("rgba16f storage images");
    if (in.maxPerStageStorageImages < kStorageImages) lack("5 storage images per stage");
    if (in.maxComputeWorkGroupInvocations < kWorkGroupInvocations) lack("256-invocation compute workgroups");
    const uint32_t buffersNeeded = hardware ? kStorageBuffersHardware : kStorageBuffersSoftware;
    if (in.maxPerStageStorageBuffers < buffersNeeded) {
        lack(std::to_string(buffersNeeded) + " storage buffers per stage (software BVH)");
    }

    // The deferred scene passes.
    if (!in.dualSrcBlend) lack("dualSrcBlend");
    if (!in.independentBlend) lack("independentBlend");
    const bool baseOk = missing.empty();

    // What only the current sample count asks.
    const uint32_t samples = msaaSamples == 0 ? 1u : msaaSamples;
    bool msaaOnly = false;
    if (samples > 1 && !in.sampleRateShading) {
        lack("sampleRateShading (needed with MSAA)");
        msaaOnly = true;
    }
    if ((in.gbufferSampleCounts & samples) == 0) {
        if ((in.gbufferSampleCounts & 1u) != 0 && samples > 1) {
            lack("G-buffer format at " + std::to_string(samples) + "x MSAA");
            msaaOnly = true;
        } else {
            lack("G-buffer format (rgba16 unorm) as a colour attachment");
        }
    }

    r.missing = missing;
    r.available = missing.empty();
    r.blockedByMsaa = baseOk && !r.available && msaaOnly &&
                      (in.gbufferSampleCounts & 1u) != 0;
    r.tracer = r.available ? (hardware ? Tracer::Hardware : Tracer::Software) : Tracer::None;
    return r;
}

/// One line for the log and the settings tooltip.
inline std::string describe(const Result& r) {
    if (r.available) {
        return r.tracer == Tracer::Hardware
                   ? std::string("available (hardware ray queries)")
                   : "available (compute tracer: " + r.hardwareWhyNot + ")";
    }
    std::string s = "unavailable - missing " + r.missing;
    if (r.blockedByMsaa) s += " (available with anti-aliasing off)";
    return s;
}

/// What the settings say under a ray tracing row it greys out; empty when
/// the lighting can run.
inline std::string settingsReason(const Result& r, bool german = false) {
    if (r.available) return {};
    if (german) {
        if (r.blockedByMsaa) {
            return "Nicht verfÃ¼gbar bei eingeschalteter KantenglÃ¤ttung (MSAA): dieser Grafikkarte fehlt " +
                   r.missing + ". KantenglÃ¤ttung ausschalten, um es zu nutzen.";
        }
        return "Auf dieser Grafikkarte nicht verfÃ¼gbar: ihr fehlt " + r.missing + ".";
    }
    if (r.blockedByMsaa) {
        return "Unavailable with anti-aliasing (MSAA) on: this graphics card lacks " + r.missing +
               ". Turn anti-aliasing off to use it.";
    }
    return "Unavailable on this graphics card: it lacks " + r.missing + ".";
}

}  // namespace rt_capability
}  // namespace rendering
}  // namespace wowee
