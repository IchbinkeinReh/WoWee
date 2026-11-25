// Pictures kept by name (rendering/picture_cache.hpp).
#include <catch_amalgamated.hpp>

#include "rendering/picture_cache.hpp"

#include <string>
#include <vector>

using wowee::rendering::PictureCache;

namespace {
struct Pictures {
    PictureCache<int> cache{3};
    std::vector<int> gone;
    int* get(int n) {
        const std::string name = std::to_string(n);
        if (cache.known(name)) return cache.find(name);
        return cache.add(name, std::make_unique<int>(n));
    }
    void end() {
        cache.endRound([this](std::unique_ptr<int> p) { gone.push_back(*p); });
    }
};
}  // namespace

TEST_CASE("no picture of the round under way is let go of", "[picture_cache]") {
    Pictures p;
    p.cache.beginRound();
    std::vector<int*> round;
    for (int n = 1; n <= 8; ++n) round.push_back(p.get(n));
    p.end();
    CHECK(p.gone.empty());
    CHECK(p.cache.size() == 8);
    for (int n = 1; n <= 8; ++n) CHECK(*round[static_cast<size_t>(n - 1)] == n);

    // The next asks for three of them: the others go, what is asked for stays.
    p.cache.beginRound();
    CHECK(p.get(2) == round[1]);
    CHECK(p.get(7) == round[6]);
    CHECK(p.get(9) != nullptr);
    p.end();
    CHECK(p.gone.size() == 6);
    CHECK(p.cache.size() == 3);
    CHECK(p.cache.find("2") == round[1]);
    CHECK(p.cache.find("7") == round[6]);
    CHECK_FALSE(p.cache.known("1"));
}

TEST_CASE("the longest unused goes first", "[picture_cache]") {
    Pictures p;
    for (int n = 1; n <= 3; ++n) {
        p.cache.beginRound();
        p.get(n);
        p.end();
    }
    CHECK(p.gone.empty());
    p.cache.beginRound();
    p.get(1);   // used again
    p.get(4);
    p.end();
    CHECK(p.gone == std::vector<int>{2});
    // One there is no picture of is known, and counts for nothing.
    p.cache.beginRound();
    CHECK(p.cache.add("none", nullptr) == nullptr);
    CHECK(p.cache.known("none"));
    CHECK(p.cache.find("none") == nullptr);
    p.end();
    CHECK(p.gone.size() == 1);
    CHECK(p.cache.takeAll().size() == 3);
    CHECK(p.cache.size() == 0);
}
