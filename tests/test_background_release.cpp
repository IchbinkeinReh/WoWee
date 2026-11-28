// What a map change lets go of is freed on the release worker
// (core/background_release.hpp, V650's waits in CharacterRenderer::clear and
// the terrain's soft reset).
#include "catch_amalgamated.hpp"

#include "core/background_release.hpp"

#include <atomic>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

struct Witness {
    std::atomic<int>* destroyed = nullptr;
    std::atomic<std::thread::id>* on = nullptr;
    Witness(std::atomic<int>* d, std::atomic<std::thread::id>* t) : destroyed(d), on(t) {}
    Witness(Witness&& o) noexcept : destroyed(o.destroyed), on(o.on) { o.destroyed = nullptr; }
    Witness(const Witness&) = delete;
    ~Witness() {
        if (!destroyed) return;
        on->store(std::this_thread::get_id());
        destroyed->fetch_add(1);
    }
};

}  // namespace

TEST_CASE("a released value is destroyed once, on the release worker", "[background_release]") {
    std::atomic<int> destroyed{0};
    std::atomic<std::thread::id> on{};
    {
        std::vector<Witness> v;
        v.emplace_back(&destroyed, &on);
        wowee::core::releaseInBackground(std::move(v));
        // Moved out: the caller's vector holds nothing to destroy.
    }
    wowee::core::waitForBackgroundReleases();
    REQUIRE(destroyed.load() == 1);
    REQUIRE(on.load() != std::this_thread::get_id());
    REQUIRE(wowee::core::backgroundReleasesPending() == 0);
}

TEST_CASE("the caller's container is left empty and usable", "[background_release]") {
    std::unordered_map<int, std::vector<int>> models;
    for (int i = 0; i < 100; ++i) models[i].assign(1000, i);
    struct Old {
        std::unordered_map<int, std::vector<int>> m;
    };
    wowee::core::releaseInBackground(Old{std::move(models)});
    models.clear();
    REQUIRE(models.empty());
    models[1].push_back(7);
    REQUIRE(models.size() == 1);
    wowee::core::waitForBackgroundReleases();
    REQUIRE(wowee::core::backgroundReleasesPending() == 0);
}
