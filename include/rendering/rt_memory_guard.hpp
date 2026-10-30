#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

// Whether there is memory for the ray tracing, before it is switched on and
// while it runs.
//
// On a phone the GPU's memory is the system's, and running out of it is not
// an error the client is told of: the system's out-of-memory killer ends the
// process, so the player sees the game vanish the moment they switch the
// tracing on. What the tracing will need is known well enough beforehand -
// measured with /mem: some 100 bytes a triangle of the scene for its copy and
// its acceleration structure, the G-buffer and trace images by the screen,
// the probe volume in Full mode - to refuse it with a reason instead.
//
// Free of the renderer and the platform, so the test states each case.
namespace wowee::rendering::rt_memory {

inline constexpr uint64_t kMiB = 1024ull * 1024ull;

/// What is in use and what may be, at a moment.
struct Snapshot {
    /// The device-local heaps' budget and this process's use of them
    /// (VK_EXT_memory_budget, or VMA's estimate without it). 0 for unknown.
    uint64_t gpuBudget = 0;
    uint64_t gpuUsage = 0;
    /// The system's RAM: what is available to take, and all of it.
    uint64_t ramAvailable = 0;
    uint64_t ramTotal = 0;
    /// The device-local memory is the system's RAM (an integrated GPU, every
    /// phone): what the tracing takes on the GPU it takes from the RAM.
    bool unified = false;
    /// The system said it is short (Android's onTrimMemory/onLowMemory,
    /// SDL_EVENT_LOW_MEMORY).
    bool systemLow = false;
};

/// What switching the tracing on will take.
struct Need {
    uint64_t gpu = 0;
    uint64_t ram = 0;
};

/// The estimate, from the scene and the screen. Per triangle: its traced
/// copy (48 bytes) and its acceleration structure (some 56 with the hardware
/// tracer, 40 of nodes with the compute one). The G-buffer two 8-byte
/// targets a sample; the trace, history and filter images some 82 bytes a
/// pixel at full resolution and a quarter of that at half; the probe volume
/// 61 MB in Full mode; the hit textures some 40 MB; and the upload's own
/// peak, a chunk staged and built (uploadAllPending), some 50 MB on the CPU.
inline Need estimate(uint64_t triangles, uint32_t width, uint32_t height, uint32_t samples, bool fullResolution,
                     bool probes, bool hardware) {
    const uint64_t pixels = uint64_t(width) * height;
    Need n;
    n.gpu = triangles * (48 + (hardware ? 56 : 40)) + pixels * std::max(samples, 1u) * 16 +
            pixels * (fullResolution ? 82 : 21) + (probes ? 61 * kMiB : 0) + 40 * kMiB;
    n.ram = 50 * kMiB;
    return n;
}

/// What must stay free of the system's RAM: an eighth of it, and never under
/// 256 MB - below that a phone's system starts ending apps.
inline uint64_t ramReserve(uint64_t ramTotal) { return std::max<uint64_t>(256 * kMiB, ramTotal / 8); }

enum class Short { None, Gpu, Ram };

struct Verdict {
    Short what = Short::None;
    uint64_t needed = 0;     ///< bytes the tracing would take of what ran short
    uint64_t available = 0;  ///< bytes there were of it
};

/// Whether switching the tracing on fits.
inline Verdict judgeEnable(const Snapshot& s, const Need& n) {
    if (s.gpuBudget > 0) {
        const uint64_t free = s.gpuBudget > s.gpuUsage ? s.gpuBudget - s.gpuUsage : 0;
        if (n.gpu > free) return {Short::Gpu, n.gpu, free};
    }
    if (s.ramTotal > 0) {
        const uint64_t take = n.ram + (s.unified ? n.gpu : 0);
        const uint64_t reserve = ramReserve(s.ramTotal);
        const uint64_t free = s.ramAvailable > reserve ? s.ramAvailable - reserve : 0;
        if (s.systemLow || take > free) return {Short::Ram, take, s.ramAvailable};
    }
    return {};
}

/// Whether the system's RAM is short enough, with the tracing on, to offer
/// to switch it off: the system says so, or under half the reserve is left.
inline bool ramCritical(const Snapshot& s) {
    if (s.systemLow) return true;
    if (s.ramTotal == 0) return false;
    return s.ramAvailable < ramReserve(s.ramTotal) / 2;
}

/// Why the tracing was refused or offered to be switched off, for the dialog.
enum class Reason {
    GpuBudget,        ///< switching on would not fit the graphics memory budget
    RamShort,         ///< switching on would not fit the system's RAM
    OutOfDeviceMemory,  ///< an allocation failed: out of graphics memory
    OutOfHostMemory,    ///< an allocation failed: out of system memory
    RamCritical,        ///< running, and the system's RAM is nearly gone
    PreviousSession,    ///< the last session ended while tracing, unexpectedly
};

struct Refusal {
    Reason reason = Reason::GpuBudget;
    uint64_t needed = 0;
    uint64_t available = 0;
    /// The tracing was switched off already (and saved so); otherwise the
    /// dialog offers to.
    bool switchedOff = true;
};

/// The dialog's text, in German for a German client and English otherwise.
inline std::string message(const Refusal& r, bool german) {
    auto mb = [](uint64_t b) { return std::to_string((b + kMiB / 2) / kMiB); };
    const std::string need = mb(r.needed), have = mb(r.available);
    if (german) {
        switch (r.reason) {
            case Reason::GpuBudget:
                return "Nicht genug Grafikspeicher f\xC3\xBCr Raytracing: ben\xC3\xB6tigt etwa " + need +
                       " MB, verf\xC3\xBCgbar " + have + " MB. Raytracing wurde ausgeschaltet.";
            case Reason::RamShort:
                return "Nicht genug Arbeitsspeicher f\xC3\xBCr Raytracing: ben\xC3\xB6tigt etwa " + need +
                       " MB, verf\xC3\xBCgbar " + have + " MB. Raytracing wurde ausgeschaltet.";
            case Reason::OutOfDeviceMemory:
                return "Der Grafikspeicher ist voll (eine Anforderung von " + need +
                       " MB schlug fehl). Raytracing wurde ausgeschaltet.";
            case Reason::OutOfHostMemory:
                return "Der Arbeitsspeicher ist voll (eine Anforderung von " + need +
                       " MB schlug fehl). Raytracing wurde ausgeschaltet.";
            case Reason::RamCritical:
                return "Der Arbeitsspeicher wird knapp (noch " + have +
                       " MB frei). Raytracing ausschalten, damit das Spiel nicht beendet wird?";
            case Reason::PreviousSession:
                return "Die letzte Sitzung wurde bei eingeschaltetem Raytracing unerwartet beendet, "
                       "vermutlich wegen Speichermangels (zuletzt " + have + " MB frei, Raytracing " + need +
                       " MB). Raytracing ausschalten?";
        }
    }
    switch (r.reason) {
        case Reason::GpuBudget:
            return "Not enough graphics memory for ray tracing: it needs about " + need + " MB and " + have +
                   " MB are available. Ray tracing has been turned off.";
        case Reason::RamShort:
            return "Not enough system memory for ray tracing: it needs about " + need + " MB and " + have +
                   " MB are available. Ray tracing has been turned off.";
        case Reason::OutOfDeviceMemory:
            return "Graphics memory ran out (a request for " + need +
                   " MB failed). Ray tracing has been turned off.";
        case Reason::OutOfHostMemory:
            return "System memory ran out (a request for " + need + " MB failed). Ray tracing has been turned off.";
        case Reason::RamCritical:
            return "System memory is running low (" + have +
                   " MB free). Turn ray tracing off so the game is not closed?";
        case Reason::PreviousSession:
            return "The last session ended unexpectedly while ray tracing was on, probably out of memory (last " +
                   have + " MB free, ray tracing " + need + " MB). Turn ray tracing off?";
    }
    return {};
}

}  // namespace wowee::rendering::rt_memory
