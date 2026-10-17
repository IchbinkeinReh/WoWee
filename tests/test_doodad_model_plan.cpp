// A tile's WMO doodad models parsed in parallel: each model once, the first
// doodad to use it carrying it, and the jobs run by several threads with the
// results in index order.
#include <catch_amalgamated.hpp>

#include "core/work_batch.hpp"
#include "rendering/doodad_model_plan.hpp"

#include <atomic>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace plan = wowee::rendering::doodad_model_plan;
using plan::Carry;

TEST_CASE("each model is parsed once, in the order the doodads first use it", "[doodad-plan]") {
    const std::vector<uint32_t> ids = {7, 3, 7, 9, 3, 3, 5, 9};
    const auto none = [](uint32_t) { return false; };
    CHECK(plan::modelsToParse(ids, none) == std::vector<uint32_t>{7, 3, 9, 5});

    // A model an earlier tile uploaded is not read again.
    const auto has3 = [](uint32_t id) { return id == 3; };
    CHECK(plan::modelsToParse(ids, has3) == std::vector<uint32_t>{7, 9, 5});

    CHECK(plan::modelsToParse({}, none).empty());
}

TEST_CASE("the first doodad of a parsed model carries it, the rest share it", "[doodad-plan]") {
    const std::vector<uint32_t> ids = {7, 3, 7, 9, 3, 5, 9};
    // 3 was resident; 9 failed to parse.
    const std::unordered_map<uint32_t, bool> parsed = {{7, true}, {9, false}, {5, true}};
    const auto carry = plan::assign(ids, parsed);
    const std::vector<Carry> expected = {
        Carry::Model,   // 7 first
        Carry::Shared,  // 3 resident
        Carry::Shared,  // 7 again
        Carry::Drop,    // 9 failed
        Carry::Shared,  // 3 resident
        Carry::Model,   // 5 first
        Carry::Drop,    // 9 failed, every time
    };
    CHECK(carry == expected);
}

TEST_CASE("a work batch runs every job exactly once across threads", "[doodad-plan][work-batch]") {
    constexpr size_t kJobs = 5000;
    std::vector<std::atomic<int>> runs(kJobs);
    std::vector<size_t> results(kJobs, 0);
    wowee::core::WorkBatch batch(kJobs, [&](size_t i) {
        runs[i].fetch_add(1);
        results[i] = i * 3 + 1;
    });

    std::vector<std::thread> helpers;
    std::atomic<size_t> helped{0};
    for (int t = 0; t < 6; ++t) {
        helpers.emplace_back([&]() { helped += batch.runAvailable(); });
    }
    const size_t own = batch.runAvailable();
    batch.wait();
    for (auto& h : helpers) h.join();

    CHECK(batch.exhausted());
    CHECK(own + helped.load() == kJobs);
    for (size_t i = 0; i < kJobs; ++i) {
        CHECK(runs[i].load() == 1);
        CHECK(results[i] == i * 3 + 1);
    }
}

TEST_CASE("a work batch finishes on its own thread when nobody helps", "[doodad-plan][work-batch]") {
    int sum = 0;
    wowee::core::WorkBatch batch(10, [&](size_t i) { sum += static_cast<int>(i); });
    CHECK(batch.runAvailable() == 10);
    batch.wait();
    CHECK(sum == 45);

    wowee::core::WorkBatch empty(0, [](size_t) {});
    CHECK(empty.runAvailable() == 0);
    empty.wait();
    CHECK(empty.exhausted());
}

TEST_CASE("a job that throws still lets the batch finish, and wait rethrows", "[doodad-plan][work-batch]") {
    std::atomic<int> ran{0};
    wowee::core::WorkBatch batch(4, [&](size_t i) {
        ++ran;
        if (i == 2) throw std::runtime_error("bad model");
    });
    batch.runAvailable();
    CHECK_THROWS_AS(batch.wait(), std::runtime_error);
    CHECK(ran.load() == 4);
}
