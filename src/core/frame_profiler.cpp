#include "core/frame_profiler.hpp"

#include "core/env_flag.hpp"
#include "core/logger.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <sstream>

namespace wowee::core {
namespace {

int64_t nowNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// The scope a new scope on this thread is nested in.
thread_local int t_parent = -1;

constexpr int64_t kWindowNs = 1'000'000'000;

/// A GPU pass under this reads as nothing. More than one of them in a frame
/// that took real time is the signature of a driver resolving timestamps per
/// render pass rather than per draw - see Application::reportStageTimes.
constexpr double kCollapsedGpuMs = 0.02;

const char* kindName(ProfileKind k) {
    switch (k) {
        case ProfileKind::Cpu: return "cpu";
        case ProfileKind::Worker: return "worker";
        case ProfileKind::Background: return "background";
        case ProfileKind::Wait: return "wait";
        case ProfileKind::Idle: return "idle";
        case ProfileKind::Gpu: return "gpu";
    }
    return "?";
}

}  // namespace

std::thread::id FrameProfiler::mainThread_{};

bool FrameProfiler::isMainThread() noexcept {
    return std::this_thread::get_id() == mainThread_;
}

FrameProfiler& FrameProfiler::get() {
    static FrameProfiler instance;
    return instance;
}

void FrameProfiler::setEnabled(bool on) {
    if (on == enabled()) return;
    if (on) {
        // A window that straddles the switch would average frames that were
        // never measured into the first one shown.
        for (int i = 0; i < slotCount_.load(std::memory_order_acquire); ++i) {
            slots_[i].frameNs.store(0, std::memory_order_relaxed);
            slots_[i].frameCalls.store(0, std::memory_order_relaxed);
        }
        resetWindow(nowNs());
        lastFrameEndNs_ = 0;
        gpuFrameSeen_ = false;
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        published_ = Snapshot{};
    }
    g_frameProfilerOn.store(on, std::memory_order_relaxed);
    LOG_WARNING("Frame profiler ", on ? "on" : "off",
                on ? " - the overlay fills in after a second; /profile dump writes it to the log" : "");
}

void FrameProfiler::configureFromEnvironment() {
    mainThread_ = std::this_thread::get_id();
    const bool on = envFlagEnabled("WOWEE_PROFILE", false);
    // Every ten seconds by default: long enough that the log stays readable,
    // short enough that a run of a minute or two leaves several to compare.
    logIntervalSeconds_ = on ? envIntClamped("WOWEE_PROFILE_LOG_SECONDS", 10, 0, 3600) : 0.0;
    if (const char* csv = std::getenv("WOWEE_PROFILE_CSV"); csv && *csv && envValueEnables(csv, false)) {
        // A bare flag rather than a path writes beside the executable's
        // working directory, where the log already is.
        const std::string value = csv;
        const bool justAFlag = value == "1" || value == "on" || value == "yes" || value == "true";
        std::string error;
        if (!startCsv(justAFlag ? "wowee_profile.csv" : value, error)) {
            LOG_WARNING("WOWEE_PROFILE_CSV: ", error);
        }
    }
    if (on || csvOpen()) setEnabled(true);
}

int FrameProfiler::slot(const char* label, ProfileKind kind, int parent) {
    if (!label) return -1;
    std::lock_guard<std::mutex> lock(registerMutex_);
    const int n = slotCount_.load(std::memory_order_relaxed);
    // By address first: every call site passes a literal, and almost every
    // lookup is one that has been made before from the same place.
    for (int i = 0; i < n; ++i) {
        if (slots_[i].label == label && slots_[i].kind == kind) return i;
    }
    for (int i = 0; i < n; ++i) {
        if (slots_[i].kind == kind && std::strcmp(slots_[i].label, label) == 0) return i;
    }
    if (n >= kMaxSlots) return -1;
    Slot& s = slots_[n];
    s.label = label;
    s.kind = kind;
    // A child of a slot of another kind is still shown under it - a fence
    // wait inside beginFrame belongs under beginFrame - but never under a
    // GPU pass, which has no CPU scope to be nested in.
    s.parent = (kind == ProfileKind::Gpu) ? -1 : parent;
    slotCount_.store(n + 1, std::memory_order_release);
    return n;
}

void FrameProfiler::add(int s, int64_t ns) noexcept {
    if (s < 0 || s >= kMaxSlots) return;
    slots_[s].frameNs.fetch_add(ns, std::memory_order_relaxed);
    slots_[s].frameCalls.fetch_add(1, std::memory_order_relaxed);
}

void FrameProfiler::addGpuFrame(const std::vector<std::pair<const char*, double>>& passes,
                                double frameMs) {
    if (!enabled() || passes.empty()) return;
    for (const auto& [label, ms] : passes) {
        add(slot(label, ProfileKind::Gpu), static_cast<int64_t>(ms * 1.0e6));
    }
    gpuFrameMs_ += frameMs;
    gpuFrameSeen_ = true;
}

void FrameProfiler::resetWindow(int64_t now) {
    windowStartNs_ = now;
    windowFrames_ = 0;
    windowFrameSumMs_ = 0.0;
    windowFrameMaxMs_ = 0.0;
    windowGpuFrames_ = 0;
    windowGpuSumMs_ = 0.0;
    windowGpuMaxMs_ = 0.0;
    const int n = slotCount_.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        slots_[i].windowSumMs = 0.0;
        slots_[i].windowMaxMs = 0.0;
        slots_[i].windowCalls = 0;
    }
}

void FrameProfiler::endFrame() {
    if (!enabled()) return;
    const int64_t now = nowNs();
    if (lastFrameEndNs_ == 0) {
        // The first frame after switching on began before anything was
        // listening; its stages are half-recorded. Drop it.
        lastFrameEndNs_ = now;
        const int n = slotCount_.load(std::memory_order_acquire);
        for (int i = 0; i < n; ++i) {
            slots_[i].frameNs.store(0, std::memory_order_relaxed);
            slots_[i].frameCalls.store(0, std::memory_order_relaxed);
        }
        gpuFrameMs_ = 0.0;
        gpuFrameSeen_ = false;
        resetWindow(now);
        return;
    }
    const double frameMs = static_cast<double>(now - lastFrameEndNs_) / 1.0e6;
    lastFrameEndNs_ = now;

    ++windowFrames_;
    windowFrameSumMs_ += frameMs;
    windowFrameMaxMs_ = std::max(windowFrameMaxMs_, frameMs);
    if (gpuFrameSeen_) {
        ++windowGpuFrames_;
        windowGpuSumMs_ += gpuFrameMs_;
        windowGpuMaxMs_ = std::max(windowGpuMaxMs_, gpuFrameMs_);
    }
    gpuFrameMs_ = 0.0;
    gpuFrameSeen_ = false;

    const int n = slotCount_.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        Slot& s = slots_[i];
        // Exchanged rather than read and cleared, so a background thread
        // adding between the two is counted in the next frame, not lost.
        const int64_t ns = s.frameNs.exchange(0, std::memory_order_relaxed);
        const int32_t calls = s.frameCalls.exchange(0, std::memory_order_relaxed);
        const double ms = static_cast<double>(ns) / 1.0e6;
        s.windowSumMs += ms;
        s.windowMaxMs = std::max(s.windowMaxMs, ms);
        s.windowCalls += calls;
    }

    if (now - windowStartNs_ >= kWindowNs) closeWindow(now);
}

void FrameProfiler::closeWindow(int64_t now) {
    Snapshot s;
    s.valid = windowFrames_ > 0;
    s.windowSeconds = static_cast<double>(now - windowStartNs_) / 1.0e9;
    s.frames = windowFrames_;
    if (s.valid) {
        s.frameAvgMs = windowFrameSumMs_ / windowFrames_;
        s.frameMaxMs = windowFrameMaxMs_;
        s.fps = s.windowSeconds > 0.0 ? windowFrames_ / s.windowSeconds : 0.0;
        s.gpuFrames = windowGpuFrames_;
        if (windowGpuFrames_ > 0) {
            s.gpuAvgMs = windowGpuSumMs_ / windowGpuFrames_;
            s.gpuMaxMs = windowGpuMaxMs_;
        }
    }

    const int n = slotCount_.load(std::memory_order_acquire);
    s.rows.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const Slot& sl = slots_[i];
        Row r;
        r.label = sl.label;
        r.kind = sl.kind;
        r.parent = sl.parent;
        // A GPU pass is averaged over the frames that were read back, which
        // run a frame or two behind and can skip one; everything else over
        // the frames the loop ran.
        const int frames = (sl.kind == ProfileKind::Gpu) ? windowGpuFrames_ : windowFrames_;
        if (frames > 0) {
            r.avgMs = sl.windowSumMs / frames;
            r.callsPerFrame = static_cast<double>(sl.windowCalls) / frames;
        }
        r.maxMs = sl.windowMaxMs;
        s.rows.push_back(r);
    }
    for (auto& r : s.rows) {
        int d = 0;
        for (int p = r.parent; p >= 0 && d < 16; p = s.rows[static_cast<size_t>(p)].parent) ++d;
        r.depth = d;
        if (r.kind == ProfileKind::Wait && r.callsPerFrame > 0.0) s.waitAvgMs += r.avgMs;
        if (r.kind == ProfileKind::Idle && r.callsPerFrame > 0.0) s.idleAvgMs += r.avgMs;
        if (r.kind == ProfileKind::Gpu && r.callsPerFrame > 0.0 && r.avgMs < kCollapsedGpuMs) {
            ++s.collapsedGpuPasses;
        }
    }
    s.cpuBusyAvgMs = std::max(0.0, s.frameAvgMs - s.waitAvgMs - s.idleAvgMs);

    // The verdict. The GPU first: a GPU that is busy for nearly the whole
    // frame is the limit whatever else is true, and the main thread's waiting
    // is then only the symptom. Otherwise time that nobody is busy for -
    // waiting on the display, or the frame cap's sleep - means the frame is
    // being held back on purpose. What is left is the CPU.
    if (!s.valid) {
        s.verdict = "no frames";
        s.reason = "";
    } else if (s.gpuFrames > 0 && s.gpuAvgMs >= 0.85 * s.frameAvgMs) {
        s.verdict = "GPU-bound";
        s.reason = "the GPU is busy for nearly the whole frame; look at the GPU passes";
    } else if (s.waitAvgMs + s.idleAvgMs >= 0.25 * s.frameAvgMs) {
        if (s.gpuFrames == 0 && s.idleAvgMs < s.waitAvgMs) {
            s.verdict = "probably GPU-bound";
            s.reason = "the main thread waits on the GPU, and there are no GPU timestamps to confirm it";
        } else {
            s.verdict = "capped";
            s.reason = "neither side fills the frame: it is waiting for vsync, the display or the frame cap";
        }
    } else {
        s.verdict = "CPU-bound";
        s.reason = "the main thread is busy for most of the frame; look at the CPU stages";
    }

    bool log = dumpRequested_.exchange(false, std::memory_order_relaxed);
    if (!log && logIntervalSeconds_ > 0.0 &&
        static_cast<double>(now - lastLogNs_) / 1.0e9 >= logIntervalSeconds_) {
        log = true;
    }
    if (log && s.valid) {
        lastLogNs_ = now;
        // Warning, because the log a bug report arrives with carries nothing
        // quieter - the same reason WOWEE_FRAME_PROFILE raises its own report.
        LOG_WARNING(format(s));
    }
    writeCsv(s);

    {
        std::lock_guard<std::mutex> lock(snapshotMutex_);
        published_ = std::move(s);
    }
    resetWindow(now);
}

FrameProfiler::Snapshot FrameProfiler::snapshot() const {
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    return published_;
}

std::string FrameProfiler::format(const Snapshot& s) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(2);
    out << "frame profile over " << s.windowSeconds << "s: " << s.frames << " frames, "
        << s.fps << " fps, frame " << s.frameAvgMs << "ms avg / " << s.frameMaxMs << "ms worst\n";
    out << "  verdict: " << s.verdict << " - " << s.reason << "\n";
    out << "  main thread busy " << s.cpuBusyAvgMs << "ms, waiting " << s.waitAvgMs
        << "ms, sleeping " << s.idleAvgMs << "ms; GPU ";
    if (s.gpuFrames > 0) {
        out << s.gpuAvgMs << "ms avg / " << s.gpuMaxMs << "ms worst\n";
    } else {
        out << "not measured (no timestamps read back)\n";
    }

    const auto section = [&](const char* title, auto&& wanted) {
        // Each level sorted by cost, children under their parent, so the line
        // to read is always the first one under its heading.
        std::vector<int> order;
        std::function<void(int)> walk = [&](int parent) {
            std::vector<int> kids;
            for (size_t i = 0; i < s.rows.size(); ++i) {
                const Row& r = s.rows[i];
                if (r.parent == parent && wanted(r) && r.callsPerFrame > 0.0) {
                    kids.push_back(static_cast<int>(i));
                }
            }
            std::sort(kids.begin(), kids.end(), [&](int a, int b) {
                return s.rows[static_cast<size_t>(a)].avgMs > s.rows[static_cast<size_t>(b)].avgMs;
            });
            for (int k : kids) {
                order.push_back(k);
                walk(k);
            }
        };
        walk(-1);
        if (order.empty()) return;
        out << "  " << title << " (avg / worst ms per frame)\n";
        for (int i : order) {
            const Row& r = s.rows[static_cast<size_t>(i)];
            std::string name(static_cast<size_t>(4 + 2 * r.depth), ' ');
            name += r.label;
            if (name.size() < 40) name.resize(40, ' ');
            out << name << " " << r.avgMs << " / " << r.maxMs;
            if (r.callsPerFrame > 1.5) out << "  (x" << r.callsPerFrame << ")";
            out << "\n";
        }
    };
    section("GPU passes, each from the previous mark to its own",
            [](const Row& r) { return r.kind == ProfileKind::Gpu; });
    section("CPU, main thread", [](const Row& r) {
        return r.kind == ProfileKind::Cpu || r.kind == ProfileKind::Wait || r.kind == ProfileKind::Idle;
    });
    section("CPU, render workers (in parallel with the main thread)",
            [](const Row& r) { return r.kind == ProfileKind::Worker; });
    section("CPU, background threads (off the frame's path)",
            [](const Row& r) { return r.kind == ProfileKind::Background; });
    if (s.collapsedGpuPasses >= 2 && s.gpuAvgMs > 1.0) {
        out << "  (" << s.collapsedGpuPasses << " GPU passes read under 20us. A driver that "
               "resolves timestamps per render pass puts a whole pass on its first mark; "
               "WOWEE_PASS_ABLATION=1 measures the scene pass by switching passes off.)\n";
    }
    std::string text = out.str();
    if (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

bool FrameProfiler::startCsv(const std::string& path, std::string& error) {
    std::lock_guard<std::mutex> lock(csvMutex_);
    if (csv_) {
        std::fclose(csv_);
        csv_ = nullptr;
    }
    csv_ = std::fopen(path.c_str(), "w");
    if (!csv_) {
        error = "could not open " + path + " for writing";
        return false;
    }
    csvPath_ = path;
    csvStartNs_ = nowNs();
    // Long form, one row per label per window: the set of labels grows as
    // passes first run, and a wide table would need its header rewritten.
    std::fputs("t_s,kind,label,avg_ms,max_ms,calls_per_frame\n", csv_);
    std::fflush(csv_);
    LOG_WARNING("Frame profiler: writing a row per stage per second to ", path);
    return true;
}

void FrameProfiler::stopCsv() {
    std::lock_guard<std::mutex> lock(csvMutex_);
    if (!csv_) return;
    std::fclose(csv_);
    csv_ = nullptr;
    LOG_WARNING("Frame profiler: closed ", csvPath_);
}

bool FrameProfiler::csvOpen() const {
    std::lock_guard<std::mutex> lock(csvMutex_);
    return csv_ != nullptr;
}

std::string FrameProfiler::csvPath() const {
    std::lock_guard<std::mutex> lock(csvMutex_);
    return csvPath_;
}

void FrameProfiler::writeCsv(const Snapshot& s) {
    std::lock_guard<std::mutex> lock(csvMutex_);
    if (!csv_ || !s.valid) return;
    const double t = static_cast<double>(nowNs() - csvStartNs_) / 1.0e9;
    std::fprintf(csv_, "%.2f,frame,frame,%.3f,%.3f,%d\n", t, s.frameAvgMs, s.frameMaxMs, s.frames);
    std::fprintf(csv_, "%.2f,frame,fps,%.2f,,\n", t, s.fps);
    std::fprintf(csv_, "%.2f,frame,cpu busy,%.3f,,\n", t, s.cpuBusyAvgMs);
    std::fprintf(csv_, "%.2f,frame,waiting,%.3f,,\n", t, s.waitAvgMs);
    std::fprintf(csv_, "%.2f,frame,sleeping,%.3f,,\n", t, s.idleAvgMs);
    if (s.gpuFrames > 0) {
        std::fprintf(csv_, "%.2f,frame,gpu,%.3f,%.3f,%d\n", t, s.gpuAvgMs, s.gpuMaxMs, s.gpuFrames);
    }
    std::fprintf(csv_, "%.2f,verdict,%s,,,\n", t, s.verdict);
    for (const Row& r : s.rows) {
        if (r.callsPerFrame <= 0.0) continue;
        // Labels are the client's own literals; none has a comma or a quote,
        // but one that ever did would split its row.
        std::fprintf(csv_, "%.2f,%s,\"%s\",%.3f,%.3f,%.2f\n", t, kindName(r.kind), r.label,
                     r.avgMs, r.maxMs, r.callsPerFrame);
    }
    std::fflush(csv_);
}

int ProfileScope::currentParent() noexcept { return t_parent; }

void ProfileScope::begin(int s) noexcept {
    if (s < 0) return;
    slot_ = s;
    prevParent_ = t_parent;
    t_parent = s;
    startNs_ = nowNs();
}

void ProfileScope::end() noexcept {
    FrameProfiler::get().add(slot_, nowNs() - startNs_);
    t_parent = prevParent_;
}

}  // namespace wowee::core
