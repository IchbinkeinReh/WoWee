#pragma once

// What a map change lets go of, freed beside the loading screen rather than
// in front of it.
//
// The old map's CPU-side data - every creature model's animation tracks, the
// terrain's cached tiles with their decoded textures - is millions of heap
// blocks. Freed on the map change's own thread it held the loading screen
// (V650: 1.42 s inside CharacterRenderer::clear with 643 models after
// Dalaran, about 1 s in the terrain's soft reset), while the frees themselves
// need nothing of the thread they run on.
//
// Only plain data may go this way: whatever the value's destructor touches
// must be safe on another thread and must not be anything the caller still
// uses. GPU objects are destroyed by the caller first, as before.

#include "core/thread_pool.hpp"

#include <atomic>
#include <memory>
#include <type_traits>
#include <utility>

namespace wowee::core {

namespace detail {
inline std::atomic<int>& backgroundReleasesPending() {
    static std::atomic<int> pending{0};
    return pending;
}

// One thread, apart from the frame workers: a long free must not hold a
// frame's bone or culling job behind it. Never destroyed, as the other pools
// (ThreadPool::frameWorkers).
inline ThreadPool& releaseWorker() {
    static ThreadPool* pool = new ThreadPool(1);
    return *pool;
}
}  // namespace detail

/// Takes `value` and destroys it on the release worker.
template <typename T>
void releaseInBackground(T&& value) {
    static_assert(!std::is_lvalue_reference_v<T>, "move the value in: it is gone from the caller");
    auto held = std::make_shared<std::decay_t<T>>(std::move(value));
    detail::backgroundReleasesPending().fetch_add(1, std::memory_order_acq_rel);
    detail::releaseWorker().submit([held = std::move(held)]() mutable {
        held.reset();
        detail::backgroundReleasesPending().fetch_sub(1, std::memory_order_acq_rel);
    });
}

/// How many releases have not finished yet.
[[nodiscard]] inline int backgroundReleasesPending() {
    return detail::backgroundReleasesPending().load(std::memory_order_acquire);
}

/// Waits until every release handed over so far has finished (tests).
inline void waitForBackgroundReleases() {
    detail::releaseWorker().submit([] {}).wait();
}

}  // namespace wowee::core
