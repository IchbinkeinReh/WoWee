#include "game/macro_options.hpp"

#include <cctype>
#include <cstring>

namespace wowee::game::macro_options {

namespace {

struct Span {
    size_t begin = 0;
    size_t end = 0;
    bool empty() const { return begin >= end; }
};

Span trim(const std::string& s, Span r) {
    while (r.begin < r.end && s[r.begin] == ' ') ++r.begin;
    while (r.end > r.begin && s[r.end - 1] == ' ') --r.end;
    return r;
}

std::string text(const std::string& s, Span r) { return r.empty() ? std::string() : s.substr(r.begin, r.end - r.begin); }

/// strncmp (0x0076e760): "no" and "target=" are matched with their case.
bool startsWith(const std::string& s, size_t at, size_t end, const char* what) {
    const size_t n = std::strlen(what);
    return end >= at + n && s.compare(at, n, what) == 0;
}

/// _strnicmp (0x0076e780), for the cursor's kind.
bool equalsCaseless(const std::string& a, const char* b) {
    if (a.size() != std::strlen(b)) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

/// 0x005eeb00: the leading digits of an argument.
int number(const std::string& arg) {
    int v = 0;
    for (char c : arg) {
        if (c < '0' || c > '9') break;
        v = v * 10 + (c - '0');
    }
    return v;
}

enum Handler {
    kNone, kCombat, kExists, kHelp, kHarm, kParty, kRaid, kDead, kFlyable, kIndoors, kOutdoors,
    kSwimming, kFlying, kMounted, kStealth, kGroup, kForm, kPet, kModifier, kButton, kActionBar,
    kBonusBar, kEquipped, kChanneling, kSpec, kVehicleUi, kUnitVehicleUi, kCursor,
};

/// The names at 0x00ad14d8 and the handler each is answered by (0x00ad1560).
struct Option {
    const char* name;
    Handler handler;
};
constexpr Option kOptions[] = {
    {"", kNone},
    {"combat", kCombat}, {"exists", kExists}, {"help", kHelp}, {"harm", kHarm},
    {"party", kParty}, {"raid", kRaid}, {"dead", kDead}, {"flyable", kFlyable},
    {"indoors", kIndoors}, {"outdoors", kOutdoors}, {"swimming", kSwimming},
    {"flying", kFlying}, {"mounted", kMounted}, {"stealth", kStealth}, {"group", kGroup},
    {"stance", kForm}, {"form", kForm}, {"pet", kPet}, {"modifier", kModifier}, {"mod", kModifier},
    {"button", kButton}, {"btn", kButton}, {"actionbar", kActionBar}, {"bar", kActionBar},
    {"bonusbar", kBonusBar}, {"equipped", kEquipped}, {"worn", kEquipped},
    {"channeling", kChanneling}, {"spec", kSpec}, {"vehicleui", kVehicleUi},
    {"unithasvehicleui", kUnitVehicleUi}, {"cursor", kCursor},
};

struct Condition {
    Handler handler = kNone;
    bool negate = false;
    std::vector<std::string> args;
};

struct Group {
    std::vector<Condition> conditions;
    std::string unit;   ///< target= / @ (+0x18..+0x1c)
};

/// 0x005f0040: one condition - "no", the name, the arguments.
Condition condition(const std::string& s, Span r, State& state) {
    Condition c;
    if (r.end - r.begin >= 3 && startsWith(s, r.begin, r.end, "no")) {
        c.negate = true;
        r.begin += 2;
    }
    size_t colon = r.begin;
    while (colon < r.end && s[colon] != ':') ++colon;
    const std::string name = text(s, trim(s, Span{r.begin, colon}));
    bool known = false;
    for (const auto& o : kOptions) {
        if (name == o.name) {
            c.handler = o.handler;
            known = true;
            break;
        }
    }
    if (!known) {
        state.unknownOption(name);
        c.handler = kNone;
        return c;
    }
    if (colon + 1 < r.end) {
        size_t at = colon;
        while (at < r.end) {
            const size_t start = at + 1;
            size_t stop = start;
            while (stop < r.end && s[stop] != '/') ++stop;
            c.args.push_back(text(s, trim(s, Span{start, stop})));
            at = stop;
        }
    }
    return c;
}

/// 0x005f0bb0: the comma list of a group.
Group group(const std::string& s, Span r, State& state) {
    Group g;
    size_t at = r.begin;
    while (at < r.end) {
        size_t stop = at;
        while (stop < r.end && s[stop] != ',') ++stop;
        const Span item = trim(s, Span{at, stop});
        at = stop + 1;
        if (item.end - item.begin >= 7 && startsWith(s, item.begin, item.end, "target=")) {
            g.unit = text(s, trim(s, Span{item.begin + 7, item.end}));
        } else if (!item.empty() && s[item.begin] == '@') {
            g.unit = text(s, trim(s, Span{item.begin + 1, item.end}));
        } else {
            g.conditions.push_back(condition(s, item, state));
        }
    }
    return g;
}

/// The handlers that answer one truth, compared with a number when given one.
bool flag(bool v, const Condition& c) { return c.args.empty() ? v : (v ? 1 : 0) == number(c.args.front()); }

bool anyNumber(const Condition& c, int v) {
    for (auto it = c.args.rbegin(); it != c.args.rend(); ++it)
        if (number(*it) == v) return true;
    return false;
}

bool answer(const Condition& c, uint64_t unit, State& st) {
    switch (c.handler) {
        case kNone: return true;
        case kCombat: return flag(st.inCombat(), c);
        case kExists: return flag(st.exists(unit), c);
        case kHelp: return flag(st.hasPlayer() && unit != 0 && st.canAssist(unit), c);
        case kHarm: return flag(st.hasPlayer() && unit != 0 && st.canAttack(unit), c);
        case kParty: return flag(unit != 0 && st.inPartyAsMember(unit), c);
        case kRaid: return flag(unit != 0 && (st.inPartyAsMember(unit) || st.inRaidAsMember(unit)), c);
        case kDead: return flag(st.dead(unit), c);
        case kFlyable: return flag(st.flyable(unit), c);
        case kIndoors: return flag(st.hasPlayer() && !st.outdoors(), c);
        case kOutdoors: return flag(st.hasPlayer() && st.outdoors(), c);
        case kSwimming: return flag(st.hasPlayer() && st.swimming(), c);
        case kFlying: return flag(st.hasPlayer() && st.flying(), c);
        case kMounted: return flag(st.hasPlayer() && st.mounted(), c);
        case kStealth: return flag(st.hasPlayer() && st.stealthed(), c);
        case kGroup:
            if (c.args.empty()) return st.inParty() || st.inRaid();
            for (auto it = c.args.rbegin(); it != c.args.rend(); ++it) {
                if (*it == "party") return st.inParty();
                if (*it == "raid") return st.inRaid();
            }
            return false;
        case kForm: {
            const int form = st.shapeshiftForm();
            return c.args.empty() ? form != 0 : anyNumber(c, form);
        }
        case kPet: {
            std::string name, family;
            if (!st.hasPlayer() || !st.pet(name, family)) return false;
            if (c.args.empty()) return true;
            for (auto it = c.args.rbegin(); it != c.args.rend(); ++it)
                if (*it == name || *it == family) return true;
            return false;
        }
        case kModifier:
            if (c.args.empty()) return st.modifier(std::string());
            for (auto it = c.args.rbegin(); it != c.args.rend(); ++it)
                if (st.modifier(*it)) return true;
            return false;
        case kButton: {
            std::string button = st.clickButton();
            const std::string key = st.macroButton();
            if (button.empty() && key.empty()) button = "LeftButton";
            for (auto it = c.args.rbegin(); it != c.args.rend(); ++it) {
                const std::string name =
                    !it->empty() && (*it)[0] >= '0' && (*it)[0] <= '9' ? st.mouseButtonName(number(*it)) : *it;
                if ((!button.empty() && name == button) || (!key.empty() && name == key)) return true;
            }
            return false;
        }
        case kActionBar: return anyNumber(c, st.actionBarPage());
        case kBonusBar: return anyNumber(c, st.bonusBarOffset());
        case kEquipped:
            if (!st.hasPlayer()) return false;
            for (auto it = c.args.rbegin(); it != c.args.rend(); ++it)
                if (st.equipped(*it)) return true;
            return false;
        case kChanneling: {
            std::string spell;
            if (!st.hasPlayer() || !st.channeling(spell)) return false;
            if (c.args.empty()) return true;
            for (auto it = c.args.rbegin(); it != c.args.rend(); ++it)
                if (*it == spell) return true;
            return false;
        }
        case kSpec: return anyNumber(c, st.activeSpec());
        case kVehicleUi: return st.playerVehicleUi();
        case kUnitVehicleUi: return unit != 0 && st.vehicleUi(unit);
        case kCursor: {
            const std::string held = st.cursorType();
            if (held.empty()) return false;
            if (c.args.empty()) return true;
            for (auto it = c.args.rbegin(); it != c.args.rend(); ++it)
                if (equalsCaseless(*it, held.c_str())) return true;
            return false;
        }
    }
    return false;
}

/// 0x005ef5c0: every condition holds, asked about the group's unit.
bool passes(const Group& g, State& st) {
    if (g.conditions.empty()) return true;
    // The unit's name is cut at 31 characters before it is looked up.
    const uint64_t unit = st.unitGuid(g.unit.substr(0, 31));
    for (const Condition& c : g.conditions) {
        bool v = answer(c, unit, st);
        if (c.negate) v = !v;
        if (!v) return false;
    }
    return true;
}

}  // namespace

bool parse(const std::string& s, State& state, std::string& result, std::string& target) {
    target.clear();
    size_t clause = 0;        // where the clause's text starts (param_1)
    size_t at = 0;
    bool sawGroup = false;    // pcVar6
    bool passed = false;      // iVar2
    const size_t end = s.size();
    while (true) {
        if (at >= end || s[at] == ';') {
            if (!sawGroup || passed) {
                result = text(s, trim(s, Span{clause, at}));
                return true;
            }
            if (at >= end) return false;
            clause = at + 1;
            at = clause;
            sawGroup = false;
            continue;
        }
        if (s[at] != '[') {
            ++at;
            continue;
        }
        size_t open = at + 1;
        while (open < end && s[open] == ' ') ++open;
        size_t close = open;
        while (close < end && s[close] != ']') ++close;
        if (!passed) {
            const Group g = group(s, trim(s, Span{open, close}), state);
            passed = passes(g, state);
            if (passed) target = g.unit;
        }
        sawGroup = true;
        clause = close + 1;
        at = clause;
    }
}

}  // namespace wowee::game::macro_options
