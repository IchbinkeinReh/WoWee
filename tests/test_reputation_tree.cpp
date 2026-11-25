// The reputation list's order (game/reputation_tree.hpp).
#include <catch_amalgamated.hpp>

#include "game/reputation_tree.hpp"

#include <set>

namespace tree = wowee::game::reputation_tree;

namespace {
struct World {
    std::map<uint32_t, uint32_t> parents;
    std::map<uint32_t, std::string> names;
    std::set<uint32_t> closed;
    [[nodiscard]] std::string list(const std::vector<tree::Known>& known) const {
        std::string out;
        for (const tree::Row& r : tree::rows(known, {.parent = [this](uint32_t id) { return at(parents, id); },
                                                     .name = [this](uint32_t id) { return at(names, id); },
                                                     .collapsed = [this](uint32_t id) { return closed.count(id) > 0; }})) {
            out += r.header ? "[" + at(names, r.id) + (r.id == tree::kOther ? "Other" : r.id == tree::kInactive ? "Inactive" : "") + "]"
                            : at(names, r.id);
            out += r.child ? "+ " : " ";
        }
        return out;
    }
    template <typename T>
    static T at(const std::map<uint32_t, T>& m, uint32_t id) {
        const auto it = m.find(id);
        return it == m.end() ? T{} : it->second;
    }
};
}  // namespace

// 0x005d1880, 0x005d1710, 0x005d02f0
TEST_CASE("the reputation list", "[reputation]") {
    World w;
    w.names = {{1, "Nordend"}, {2, "Classic"}, {10, "Wyrmruh"}, {11, "kirin tor"}, {12, "Expedition"}, {13, "Taunka"},
               {14, "Hand"}, {20, "Beutebucht"}, {30, "Allein"}};
    w.parents = {{10, 1}, {11, 1}, {12, 1}, {13, 12}, {14, 12}, {20, 2}};
    const std::vector<tree::Known> all{{10}, {11}, {12}, {13}, {14}, {20}, {30}};
    CHECK(w.list(all) == "[Classic] Beutebucht [Nordend] kirin tor Wyrmruh [Expedition]+ Hand+ Taunka+ [Other] Allein ");
    // Closed: its heading alone.
    w.closed = {1};
    CHECK(w.list(all) == "[Classic] Beutebucht [Nordend] [Other] Allein ");
    w.closed.clear();
    // Inactive: at the end, whatever its group; a group left with nothing goes.
    CHECK(w.list({{10}, {11, true}, {20, true}}) == "[Nordend] Wyrmruh [Inactive] Beutebucht kirin tor ");
    CHECK(w.list({}).empty());
}
