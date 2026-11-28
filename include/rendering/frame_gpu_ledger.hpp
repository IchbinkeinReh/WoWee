#pragma once

// What one-time GPU work each frame carried, and a cap on its uploads (U25).
//
// The user's iOS log (U25) has one frame that took the GPU about 0.8 s - the
// CPU waited 844 ms for it in beginFrame - after a 518 ms renderer->update,
// and the device was lost on the next submit. Every frame there already waits
// ~62 ms on the GPU (8x MSAA at 2622x1206), and the log cannot say what the
// heavy frame added: uploads, a tile's textures and mips, the traced
// lighting's triangles, or something else.
//
// The ledger counts, from every thread, what was handed to the GPU between
// two beginFrames: bytes staged for upload, upload batches, other submits,
// triangles added to the traced-lighting scene and its pools grown. When a
// beginFrame waits long for the GPU, the frames it waited on are described in
// the log ("Heavy GPU frame ..."), so the next such log names the work.
//
// The cap: on a phone the in-play tile finalization stops starting new steps
// once this frame has staged `capBytes` (a step already started finishes, so
// at least one step runs a frame). It spreads a big WMO's textures over more
// frames; it changes no setting. WOWEE_FRAME_UPLOAD_MB overrides it (0 = off).

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace wowee::rendering {

struct FrameGpuWork {
    uint64_t uploadBytes = 0;     ///< staged for a GPU copy (textures, buffers)
    uint32_t uploadBatches = 0;   ///< upload batches submitted (async or sync)
    uint32_t submits = 0;         ///< every queue submit, the frame's own included
    uint64_t rtTriangles = 0;     ///< triangles added to the traced-lighting scene
    uint32_t rtPoolGrows = 0;     ///< its GPU pools grown (a copy of the whole pool)

    [[nodiscard]] std::string describe() const {
        std::string s;
        s += std::to_string((uploadBytes + (1u << 19)) >> 20);
        s += " MB staged in ";
        s += std::to_string(uploadBatches);
        s += " upload batches, ";
        s += std::to_string(submits);
        s += " submits, ";
        s += std::to_string(rtTriangles);
        s += " traced-lighting triangles, ";
        s += std::to_string(rtPoolGrows);
        s += " pool grows";
        return s;
    }
};

class FrameGpuLedger {
public:
    // Any thread.
    void addUploadBytes(uint64_t n) { uploadBytes_.fetch_add(n, std::memory_order_relaxed); }
    void addUploadBatch() { uploadBatches_.fetch_add(1, std::memory_order_relaxed); }
    void addSubmit() { submits_.fetch_add(1, std::memory_order_relaxed); }
    void addRtTriangles(uint64_t n) { rtTriangles_.fetch_add(n, std::memory_order_relaxed); }
    void addRtPoolGrow() { rtPoolGrows_.fetch_add(1, std::memory_order_relaxed); }

    /// Bytes staged since the last closeFrame.
    [[nodiscard]] uint64_t uploadBytesThisFrame() const {
        return uploadBytes_.load(std::memory_order_relaxed);
    }

    /// The frame cap: 0 = none. Set once at startup (defaultCapBytes).
    void setCapBytes(uint64_t bytes) { capBytes_ = bytes; }
    [[nodiscard]] uint64_t capBytes() const { return capBytes_; }
    /// Whether this frame's uploads have reached the cap: the caller starts no
    /// more one-time upload work this frame.
    [[nodiscard]] bool overUploadCap() const {
        return capBytes_ != 0 && uploadBytesThisFrame() >= capBytes_;
    }

    /// At beginFrame (main thread): this frame's counts become the last one's,
    /// the last one's the one before. Returns what was closed.
    FrameGpuWork closeFrame() {
        FrameGpuWork w;
        w.uploadBytes = uploadBytes_.exchange(0, std::memory_order_relaxed);
        w.uploadBatches = uploadBatches_.exchange(0, std::memory_order_relaxed);
        w.submits = submits_.exchange(0, std::memory_order_relaxed);
        w.rtTriangles = rtTriangles_.exchange(0, std::memory_order_relaxed);
        w.rtPoolGrows = rtPoolGrows_.exchange(0, std::memory_order_relaxed);
        beforeLast_ = last_;
        last_ = w;
        return w;
    }
    [[nodiscard]] const FrameGpuWork& lastFrame() const { return last_; }
    [[nodiscard]] const FrameGpuWork& frameBeforeLast() const { return beforeLast_; }

    /// Whether a beginFrame that waited `waitMs` for the GPU is worth a line:
    /// over the threshold, and the first, a new worst, or 30 s after the last
    /// line. Main thread.
    bool shouldReport(double waitMs, double nowSeconds) {
        if (waitMs < thresholdMs_) return false;
        const bool first = lastReportAt_ < 0.0;
        if (!first && waitMs <= worstMs_ && nowSeconds - lastReportAt_ < 30.0) return false;
        lastReportAt_ = nowSeconds;
        if (waitMs > worstMs_) worstMs_ = waitMs;
        return true;
    }
    void setThresholdMs(double ms) { thresholdMs_ = ms; }

    /// The phone default (48 MB a frame) or none, and WOWEE_FRAME_UPLOAD_MB
    /// (megabytes, 0 = off) over either.
    static uint64_t defaultCapBytes(bool mobile, const char* env) {
        if (env && *env) {
            char* end = nullptr;
            const double mb = std::strtod(env, &end);
            if (end != env && mb >= 0.0) return static_cast<uint64_t>(mb * 1048576.0);
        }
        return mobile ? (48ull << 20) : 0;
    }

private:
    std::atomic<uint64_t> uploadBytes_{0};
    std::atomic<uint32_t> uploadBatches_{0};
    std::atomic<uint32_t> submits_{0};
    std::atomic<uint64_t> rtTriangles_{0};
    std::atomic<uint32_t> rtPoolGrows_{0};
    uint64_t capBytes_ = 0;
    FrameGpuWork last_{};
    FrameGpuWork beforeLast_{};
    double thresholdMs_ = 250.0;
    double worstMs_ = 0.0;
    double lastReportAt_ = -1.0;
};

/// The process's one ledger (vk_context.cpp).
FrameGpuLedger& frameGpuLedger();

}  // namespace wowee::rendering
