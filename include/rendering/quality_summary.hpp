#pragma once

/// The graphics settings actually in force, as one line for /perf.
///
/// A measurement sent from a phone is only worth what is known about the run
/// behind it, and "on Low with FSR at 77%" was a guess: the preset, the render
/// resolution and the upscaler had no line in the profile, so a run where the
/// saved settings had undone the preset read the same as one where they had
/// not. This is what the renderer is doing, not what the panel was asked for.
///
/// Only the standard library, so a test can build it without Vulkan.

#include <cstdint>
#include <cstdio>
#include <string>

namespace wowee::rendering {

struct QualitySummary {
    std::string preset;          ///< as the settings panel names it
    uint32_t sceneWidth = 0;     ///< what the world is drawn at
    uint32_t sceneHeight = 0;
    uint32_t outputWidth = 0;    ///< the swapchain
    uint32_t outputHeight = 0;
    int upscaler = 0;            ///< 0 off, 1 FSR 1, 2 FSR 3
    int msaaSamples = 1;
    bool fxaa = false;
    bool shadows = false;
    int shadowCascades = 0;
    float shadowDistance = 0.0f;
    float viewDistance = 0.0f;
    bool lowCostShading = false;
};

inline std::string formatQualitySummary(const QualitySummary& s) {
    char scale[32] = "?";
    if (s.outputWidth > 0) {
        std::snprintf(scale, sizeof(scale), "%.0f%%",
                      100.0 * static_cast<double>(s.sceneWidth) / static_cast<double>(s.outputWidth));
    }
    static constexpr const char* kUpscalers[] = {"off", "FSR 1", "FSR 3"};
    const char* upscaler = (s.upscaler >= 0 && s.upscaler <= 2) ? kUpscalers[s.upscaler] : "?";
    char shadows[64];
    if (s.shadows) {
        std::snprintf(shadows, sizeof(shadows), "on, %d cascade%s to %.0f yd", s.shadowCascades,
                      s.shadowCascades == 1 ? "" : "s", static_cast<double>(s.shadowDistance));
    } else {
        std::snprintf(shadows, sizeof(shadows), "off");
    }
    char line[384];
    std::snprintf(line, sizeof(line),
                  "preset %s; world drawn at %ux%u of %ux%u (%s), upscaling %s; MSAA %dx, "
                  "FXAA %s; shadows %s; view %.0f yd; low-cost shading %s",
                  s.preset.empty() ? "?" : s.preset.c_str(), s.sceneWidth, s.sceneHeight,
                  s.outputWidth, s.outputHeight, scale, upscaler, s.msaaSamples,
                  s.fxaa ? "on" : "off", shadows, static_cast<double>(s.viewDistance),
                  s.lowCostShading ? "on" : "off");
    return line;
}

}  // namespace wowee::rendering
