// Which addons are loaded, and in what order (addons/addon_load.hpp).
#include <catch_amalgamated.hpp>

#include "addons/addon_load.hpp"

#include <map>
#include <set>

using namespace wowee::addons::addon_load;

namespace {
struct Addons : World {
    std::map<std::string, Addon> all;
    std::set<std::string> on;
    std::string order;
    /// What each asks for while its files run (LoadAddOn).
    std::map<std::string, std::string> asks;
    Addon& add(const std::string& name) {
        all[name].name = name;
        return all[name];
    }
    const Addon* find(const std::string& name) override {
        const auto it = all.find(name);
        return it == all.end() ? nullptr : &it->second;
    }
    bool loaded(const Addon& a) override { return on.count(a.name) != 0; }
    void begin(const Addon& a, bool is) override {
        if (is) on.insert(a.name);
        else on.erase(a.name);
    }
    void run(const Addon& a) override {
        order += (order.empty() ? "" : " ") + a.name;
        if (const auto it = asks.find(a.name); it != asks.end()) load(*this, it->second, true);
    }
};
}  // namespace

// 0x005f80b0
TEST_CASE("an addon after what it needs, before what goes with it", "[addon_load]") {
    Addons w;
    w.add("A").required = {"C"};
    w.all["A"].optional = {"D", "Nowhere"};
    w.add("B");
    w.add("C").required = {"E"};
    w.add("D").onDemand = true;
    w.add("E").onDemand = true;
    w.add("F").onDemand = true;
    w.add("G").onDemand = true;
    w.all["B"].withIt = {"C", "F"};
    loadAll(w, {"A", "B", "C", "D", "E", "F", "G"});
    // Asked with the interface, what goes with B and is on demand stays.
    CHECK(w.order == "D E C A B");
    CHECK(load(w, "G", true));
    CHECK(load(w, "G", true));
    CHECK(w.order == "D E C A B G");
    Addons asked;
    asked.add("B").withIt = {"F"};
    asked.add("F").onDemand = true;
    CHECK(load(asked, "B", true));
    CHECK(asked.order == "B F");
}

// 0x005f7e90
TEST_CASE("why an addon is not loaded", "[addon_load]") {
    Addons w;
    w.add("A").required = {"Nowhere"};
    w.add("B").enabled = false;
    w.add("C").banned = true;
    w.add("D").onDemand = true;
    w.add("E").required = {"B"};
    w.add("F").required = {"A"};
    w.add("G").required = {"D"};
    w.add("H").required = {"C"};
    CHECK(refusal(w, "Nowhere", true) == "MISSING");
    CHECK(refusal(w, "A", true) == "DEP_MISSING");
    CHECK(refusal(w, "B", true) == "DISABLED");
    CHECK(refusal(w, "C", true) == "BANNED");
    CHECK(refusal(w, "D", true).empty());
    CHECK(refusal(w, "D", false) == "DEMAND_LOADED");
    CHECK(refusal(w, "E", true) == "DEP_DISABLED");
    CHECK(refusal(w, "F", true) == "DEP_MISSING");
    CHECK(refusal(w, "G", false).empty());
    CHECK(refusal(w, "H", true) == "DEP_BANNED");
    loadAll(w, {"A", "B", "C", "D", "E", "F", "G", "H"});
    CHECK(w.order == "D G");
    CHECK(w.on == std::set<std::string>{"D", "G"});
    CHECK_FALSE(load(w, "Nowhere", true));
    // One that is loaded all the same is no reason.
    w.on.insert("B");
    CHECK(refusal(w, "E", true).empty());
}

TEST_CASE("addons that need one another are loaded once", "[addon_load]") {
    Addons w;
    w.add("A").required = {"B"};
    w.add("B").required = {"A"};
    w.add("C").optional = {"C", "A"};
    w.asks["B"] = "A";
    loadAll(w, {"A", "B", "C"});
    CHECK(w.order == "B A C");
}

// 0x005f84a0
TEST_CASE("an addon with a manager waits for it", "[addon_load]") {
    Addons w;
    w.add("A").managers = {"Nowhere", "M"};
    w.add("M");
    w.add("B").managers = {"Nowhere"};
    loadAll(w, {"A", "B", "M"});
    CHECK(w.order == "M B");
}
