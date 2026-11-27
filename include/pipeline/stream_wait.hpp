#pragma once

// Streaming data mode (pipeline/data_stream): whether a read on this thread
// may wait for the network.
//
// The streamed archives are filled chunk by chunk as reads need them
// (ChunkCache::ensure). A read on a worker - a terrain tile, a creature's
// model - waits there for what it needs, and nothing else waits with it. On
// the main thread a wait is a frozen frame: in play, walking or flying over
// new ground, the world must go on and fill in as the data arrives, as it
// does on a local install. So a read made under a NoWait does not wait: what
// is here is read, and a range that is not is handed to a background fetch
// and the read fails at once, marked deferred - the caller shows what it has
// (nothing, a placeholder) and asks again later. Deferred is not missing:
// it is never to be remembered as a file that is not there.
//
// Outside streaming every range is on disk and a NoWait changes nothing.

#include <atomic>
#include <cstdint>
#include <thread>

namespace wowee::pipeline::stream_wait {

namespace detail {
inline thread_local int t_noWaitDepth = 0;
inline thread_local bool t_deferred = false;
inline std::atomic<int> g_loadingScreens{0};
inline std::atomic<uint64_t> g_deferredReads{0};
inline std::atomic<std::thread::id> g_mainThread{};
inline std::atomic<uint64_t> g_mainWaits{0};
inline std::atomic<uint64_t> g_mainWaitMs{0};
inline thread_local uint64_t t_fetches = 0;
}  // namespace detail

/// Fetches from the server this thread made (ChunkCache counts them): a
/// reader compares it before and after a read to learn whether it waited.
inline void noteFetch() { ++detail::t_fetches; }
[[nodiscard]] inline uint64_t threadFetches() { return detail::t_fetches; }

/// The thread that draws the frames (Application, at start).
inline void markMainThread() { detail::g_mainThread.store(std::this_thread::get_id()); }
[[nodiscard]] inline bool onMainThread() { return detail::g_mainThread.load() == std::this_thread::get_id(); }

/// A read on the main thread in play (no loading screen up) that waited for
/// the network: counted, for the log and the MCP state - each is a read
/// still to be made NoWait or moved to a worker.
inline void noteMainThreadWait(uint64_t ms) {
    detail::g_mainWaits.fetch_add(1, std::memory_order_relaxed);
    detail::g_mainWaitMs.fetch_add(ms, std::memory_order_relaxed);
}
[[nodiscard]] inline uint64_t mainThreadWaits() { return detail::g_mainWaits.load(std::memory_order_relaxed); }
[[nodiscard]] inline uint64_t mainThreadWaitMs() { return detail::g_mainWaitMs.load(std::memory_order_relaxed); }

/// Whether reads on this thread must not wait for the network now.
[[nodiscard]] inline bool noWait() { return detail::t_noWaitDepth > 0; }

/// A read on this thread was not waited for: its data is being fetched in
/// the background (ChunkCache's filler).
inline void noteDeferred() {
    detail::t_deferred = true;
    detail::g_deferredReads.fetch_add(1, std::memory_order_relaxed);
}

/// Whether a read under the innermost NoWait on this thread was deferred.
[[nodiscard]] inline bool deferredNow() { return noWait() && detail::t_deferred; }

/// Reads deferred so far, on every thread (diagnostics, the MCP state).
[[nodiscard]] inline uint64_t deferredReads() { return detail::g_deferredReads.load(std::memory_order_relaxed); }

/// While alive, reads on this thread do not wait for the network.
/// deferred() says whether any read made under it was left to the
/// background. Nests; each scope reports only its own reads.
class NoWait {
public:
    NoWait() : outerDeferred_(detail::t_deferred) {
        ++detail::t_noWaitDepth;
        detail::t_deferred = false;
    }
    ~NoWait() {
        --detail::t_noWaitDepth;
        // An outer scope learns of its inner one's deferred reads; outside
        // every scope nothing is deferred.
        detail::t_deferred = detail::t_noWaitDepth > 0 && (outerDeferred_ || detail::t_deferred);
    }
    NoWait(const NoWait&) = delete;
    NoWait& operator=(const NoWait&) = delete;
    [[nodiscard]] bool deferred() const { return detail::t_deferred; }

private:
    bool outerDeferred_;
};

/// Loading screens up now (rendering::LoadingScreen between its initialize
/// and shutdown). Behind one the main thread may wait for data: nothing
/// moves and the screen says what it waits for.
inline void loadingScreenShown(bool shown) {
    if (shown) detail::g_loadingScreens.fetch_add(1, std::memory_order_relaxed);
    else detail::g_loadingScreens.fetch_sub(1, std::memory_order_relaxed);
}
[[nodiscard]] inline bool loadingScreenUp() { return detail::g_loadingScreens.load(std::memory_order_relaxed) > 0; }

}  // namespace wowee::pipeline::stream_wait
