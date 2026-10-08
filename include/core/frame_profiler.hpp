#pragma once

/// Where a frame's milliseconds go, on the CPU and on the GPU, live.
///
/// The client already had the pieces: per-stage CPU timings that are reported
/// every ten seconds, GPU timestamps between passes, a pass ablation that
/// switches passes off one at a time. What none of them answers on its own is
/// the first question anyone asks about a slow frame - is it the CPU or the
/// GPU, and which step - because the CPU stages include the time spent waiting
/// for the GPU, and the GPU breakdown arrived as one frame in a log.
///
/// So this collects both into one table, averaged over about a second, with
/// the waits kept apart from the work: a frame whose main thread spends 9ms
/// waiting on a fence is GPU bound however long its CPU stages look, and that
/// wait is the number that says so.
///
/// Off unless asked for (WOWEE_PROFILE=1, /profile, or Ctrl+F12). Off, every
/// scope is one relaxed load and a branch, and no GPU timestamps are written
/// at all.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace wowee::core {

/// What a timing is, which decides where it is shown and what it adds to.
enum class ProfileKind : uint8_t {
    Cpu,         ///< main-thread work on the frame's critical path
    Worker,      ///< a render worker recording alongside the main thread
    Background,  ///< a streaming or loading thread, off the frame's path
    Wait,        ///< the main thread blocked on the GPU or the presentation engine
    Idle,        ///< the main thread sleeping on purpose - the frame cap, pacing
    Gpu,         ///< a GPU pass, from timestamps
};

/// Read on every scope, so it is a plain global rather than behind a function
/// static and its guard.
inline std::atomic<bool> g_frameProfilerOn{false};

class FrameProfiler {
public:
    static FrameProfiler& get();

    [[nodiscard]] static bool enabled() noexcept {
        return g_frameProfilerOn.load(std::memory_order_relaxed);
    }

    /// Collecting and the overlay go together: there is no use for one without
    /// the other except the log, which WOWEE_PROFILE turns on with both.
    void setEnabled(bool on);
    void toggle() { setEnabled(!enabled()); }

    /// WOWEE_PROFILE, WOWEE_PROFILE_LOG_SECONDS and WOWEE_PROFILE_CSV. Read
    /// once, at the top of the main loop - which also makes the calling thread
    /// the main thread.
    void configureFromEnvironment();

    /// Whether this is the thread that runs the frame. A wait on any other
    /// thread does not hold the frame up, so callers shared between threads
    /// file their waits by this.
    [[nodiscard]] static bool isMainThread() noexcept;

    /// The slot a label is accumulated in, registered the first time it is
    /// seen. Labels are held by pointer and must outlive the program - string
    /// literals. Two equal strings at different addresses share a slot.
    int slot(const char* label, ProfileKind kind, int parent = -1);

    /// Any thread. One relaxed add.
    void add(int slot, int64_t ns) noexcept;

    /// One GPU frame's passes, read back from the timestamps. Main thread.
    void addGpuFrame(const std::vector<std::pair<const char*, double>>& passes,
                     double frameMs);

    /// Once per loop iteration, at its very end, on the main thread. Closes the
    /// frame, and the window once it has run a second.
    void endFrame();

    /// Writes the next completed window to the log at warning level, whatever
    /// the log interval is.
    void requestDump() { dumpRequested_.store(true, std::memory_order_relaxed); }

    /// One row per label per window, appended for as long as it is open.
    bool startCsv(const std::string& path, std::string& error);
    void stopCsv();
    [[nodiscard]] bool csvOpen() const;
    [[nodiscard]] std::string csvPath() const;

    struct Row {
        const char* label = nullptr;
        ProfileKind kind = ProfileKind::Cpu;
        int parent = -1;     ///< index into Snapshot::rows, or -1
        int depth = 0;
        double avgMs = 0.0;  ///< per frame, over the window
        double maxMs = 0.0;  ///< the worst single frame in the window
        double callsPerFrame = 0.0;
    };

    struct Snapshot {
        bool valid = false;
        double windowSeconds = 0.0;
        int frames = 0;
        double fps = 0.0;
        double frameAvgMs = 0.0;
        double frameMaxMs = 0.0;
        double waitAvgMs = 0.0;    ///< every Wait row together
        double idleAvgMs = 0.0;    ///< every Idle row together
        double cpuBusyAvgMs = 0.0; ///< the frame less its waits and sleeps
        int gpuFrames = 0;
        double gpuAvgMs = 0.0;     ///< first timestamp of a frame to its last
        double gpuMaxMs = 0.0;
        int collapsedGpuPasses = 0;
        const char* verdict = "";
        const char* reason = "";
        std::vector<Row> rows;
    };

    /// The last completed window. Updated once a second, so the overlay reads
    /// steady numbers rather than one frame's.
    [[nodiscard]] Snapshot snapshot() const;

    /// The window as text, the same table the overlay shows, for the log.
    [[nodiscard]] static std::string format(const Snapshot& s);

private:
    FrameProfiler() = default;

    struct Slot {
        const char* label = nullptr;
        ProfileKind kind = ProfileKind::Cpu;
        int parent = -1;
        std::atomic<int64_t> frameNs{0};
        std::atomic<int32_t> frameCalls{0};
        // Main thread only, in endFrame.
        double windowSumMs = 0.0;
        double windowMaxMs = 0.0;
        int64_t windowCalls = 0;
    };

    /// Fixed, so a slot never moves while a worker is adding to it. More labels
    /// than this are dropped rather than grown into.
    static constexpr int kMaxSlots = 192;
    Slot slots_[kMaxSlots];
    std::atomic<int> slotCount_{0};
    std::mutex registerMutex_;

    // The open window, main thread only.
    int64_t windowStartNs_ = 0;
    int64_t lastFrameEndNs_ = 0;
    int windowFrames_ = 0;
    double windowFrameSumMs_ = 0.0;
    double windowFrameMaxMs_ = 0.0;
    int windowGpuFrames_ = 0;
    double windowGpuSumMs_ = 0.0;
    double windowGpuMaxMs_ = 0.0;
    double gpuFrameMs_ = 0.0;
    bool gpuFrameSeen_ = false;

    mutable std::mutex snapshotMutex_;
    Snapshot published_;

    std::atomic<bool> dumpRequested_{false};
    static std::thread::id mainThread_;
    double logIntervalSeconds_ = 0.0;
    int64_t lastLogNs_ = 0;

    mutable std::mutex csvMutex_;
    std::FILE* csv_ = nullptr;
    std::string csvPath_;
    int64_t csvStartNs_ = 0;

    void closeWindow(int64_t nowNs);
    void writeCsv(const Snapshot& s);
    void resetWindow(int64_t nowNs);
};

/// Times the enclosing scope into a slot. Nested scopes on one thread become
/// children of the scope they sit in, so the overlay can show the frame as a
/// tree and add up only the top level.
class ProfileScope {
public:
    /// A call site that caches its slot: the macro below.
    ProfileScope(std::atomic<int>& cachedSlot, const char* label, ProfileKind kind) noexcept {
        if (!FrameProfiler::enabled()) return;
        int s = cachedSlot.load(std::memory_order_relaxed);
        if (s < 0) {
            s = FrameProfiler::get().slot(label, kind, currentParent());
            cachedSlot.store(s, std::memory_order_relaxed);
        }
        begin(s);
    }
    /// A label that arrives at run time - the main loop's stage lambdas, which
    /// are handed their names. Looked up each time, so only for a handful of
    /// calls a frame.
    ProfileScope(const char* label, ProfileKind kind) noexcept {
        if (!FrameProfiler::enabled()) return;
        begin(FrameProfiler::get().slot(label, kind, currentParent()));
    }
    ~ProfileScope() { if (slot_ >= 0) end(); }
    ProfileScope(const ProfileScope&) = delete;
    ProfileScope& operator=(const ProfileScope&) = delete;

private:
    int slot_ = -1;
    int prevParent_ = -1;
    int64_t startNs_ = 0;
    static int currentParent() noexcept;
    void begin(int s) noexcept;
    void end() noexcept;
};

}  // namespace wowee::core

#define WOWEE_PROFILE_CAT2(a, b) a##b
#define WOWEE_PROFILE_CAT(a, b) WOWEE_PROFILE_CAT2(a, b)

/// Times the rest of the enclosing scope under `label`, a string literal.
#define WOWEE_PROFILE_SCOPE(label, kind)                                              \
    static constinit std::atomic<int> WOWEE_PROFILE_CAT(wowee_prof_slot_, __LINE__){-1}; \
    ::wowee::core::ProfileScope WOWEE_PROFILE_CAT(wowee_prof_scope_, __LINE__)(       \
        WOWEE_PROFILE_CAT(wowee_prof_slot_, __LINE__), label, ::wowee::core::ProfileKind::kind)
