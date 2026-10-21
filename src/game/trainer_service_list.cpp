#include "game/trainer_service_list.hpp"

#include "game/client_compare.hpp"

#include <algorithm>
#include <cctype>

namespace wowee::game {

namespace {

bool equalsCaseless(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b)))
            return false;
    }
    return *a == *b;
}

uint32_t bit(uint32_t n) { return 1u << (n & 31u); }

}  // namespace

int trainerServiceType(const char* name) {
    if (!name) return 6;
    if (equalsCaseless(name, "available")) return 0;
    if (equalsCaseless(name, "unavailable")) return 1;
    return equalsCaseless(name, "used") ? 2 : 6;
}

const char* trainerServiceTypeName(const TrainerServiceList::Row& row) {
    if (row.header()) return "header";
    if (row.state == 0) return "available";
    return row.state == 2 ? "used" : "unavailable";
}

void TrainerServiceList::clear() {
    rows_.clear();
    headers_.clear();
    visible_ = 0;
    selection_ = 0;
}

int TrainerServiceList::headerPos(uint32_t line) const {
    for (size_t i = 0; i < headers_.size(); ++i)
        if (headers_[i].line == line) return static_cast<int>(i);
    return -1;
}

void TrainerServiceList::count() {
    for (auto& h : headers_) {
        h.counts.fill(0);
        h.professionOnly = true;
    }
    for (const auto& r : rows_) {
        if (r.header()) continue;
        const int pos = headerPos(r.line);
        if (pos < 0) continue;
        Header& h = headers_[static_cast<size_t>(pos)];
        if (r.state < h.counts.size()) ++h.counts[r.state];
        // Kept while every service of the group has one of the two profession
        // costs (+0x24 cleared by the first that has neither).
        if (h.professionOnly)
            h.professionOnly = r.service.profDialog != 0 || r.service.profButton != 0;
    }
}

void TrainerServiceList::build(const std::vector<TrainerSpell>& services,
                               const std::vector<uint8_t>& states, bool tradeskill,
                               uint32_t typeMask, const Lookups& lookups) {
    clear();
    tradeskill_ = tradeskill;
    lineFilter_ = ~0u;
    expanded_ = ~0u;
    for (size_t i = 0; i < services.size(); ++i) {
        const TrainerSpell& s = services[i];
        const uint32_t line = lookups.groupOf ? lookups.groupOf(s) : 0;
        if (line == 0) continue;
        Row r;
        r.spellId = static_cast<int32_t>(s.spellId);
        r.line = line;
        r.service = s;
        r.state = i < states.size() ? states[i] : s.state;
        rows_.push_back(r);
        if (headerPos(line) < 0) {
            Header h;
            h.line = line;
            headers_.push_back(h);
        }
    }
    count();

    // The groups in order: a tradeskill trainer's by kind (0x00593ce0); a
    // class trainer's by name, a group of nothing but profession first ranks
    // after the rest (0x005941b0).
    std::stable_sort(headers_.begin(), headers_.end(), [&](const Header& a, const Header& b) {
        if (tradeskill_) return a.line < b.line;
        if (a.professionOnly != b.professionOnly) return !a.professionOnly;
        const std::string na = lookups.lineName ? lookups.lineName(a.line) : std::string();
        const std::string nb = lookups.lineName ? lookups.lineName(b.line) : std::string();
        if (na.empty() || nb.empty()) return false;
        return compareCaseless(na, nb) < 0;
    });
    for (const auto& h : headers_) {
        Row r;
        r.spellId = -1;
        r.line = h.line;
        rows_.push_back(r);
    }
    refilter(typeMask, lookups);
}

bool TrainerServiceList::restate(const std::function<uint8_t(const Row&)>& stateOf,
                                 uint32_t typeMask, const Lookups& lookups) {
    bool changed = false;
    for (auto& r : rows_) {
        if (r.header()) continue;
        const uint8_t now = stateOf(r);
        if (now != r.state) {
            r.state = now;
            changed = true;
        }
    }
    if (!changed) return false;
    count();
    refilter(typeMask, lookups);
    return true;
}

void TrainerServiceList::refilter(uint32_t typeMask, const Lookups& lookups) {
    for (size_t u = 0; u < headers_.size(); ++u) {
        Header& h = headers_[u];
        // The first state the type filter lets through that the group has a
        // service in decides, by the line filter.
        h.lineShown = false;
        for (uint32_t s = 0; s < 6; ++s) {
            if ((typeMask & bit(s)) != 0 && h.counts[s] != 0) {
                h.lineShown = (lineFilter_ & bit(static_cast<uint32_t>(u))) != 0;
                break;
            }
        }
        h.collapsed = (expanded_ & bit(static_cast<uint32_t>(u))) == 0;
    }
    visible_ = static_cast<uint32_t>(rows_.size());
    for (auto& r : rows_) {
        if (r.header() || (typeMask & bit(r.state)) != 0) {
            r.visible = true;
            if (!headers_.empty()) {
                const int found = headerPos(r.line);
                const Header& h = headers_[found < 0 ? 0 : static_cast<size_t>(found)];
                if (!h.lineShown || (!r.header() && h.collapsed)) r.visible = false;
            }
            if (r.visible && !r.header() && lookups.searchHides && lookups.searchHides(r))
                r.visible = false;
        } else {
            r.visible = false;
        }
        if (!r.visible) --visible_;
    }

    // The shown rows first, each under its header (0x005947d0, 0x005949a0).
    // A row whose spell has no Spell row compares equal to the one beside it.
    const auto compare = [&](const Row& a, const Row& b) -> int {
        if (!a.visible || !b.visible) {
            if (a.visible == b.visible) return 0;
            return a.visible ? -1 : 1;
        }
        if (tradeskill_) {
            if (a.line != b.line) return static_cast<int32_t>(b.line) < static_cast<int32_t>(a.line) ? 1 : -1;
        } else if (!headers_.empty()) {
            const int pa = std::max(headerPos(a.line), 0), pb = std::max(headerPos(b.line), 0);
            if (pa != pb) return pb < pa ? 1 : -1;
        }
        if (a.header() || b.header()) return a.header() ? -1 : 1;
        std::string nameA, rankA, nameB, rankB;
        if (!lookups.spellText || !lookups.spellText(static_cast<uint32_t>(a.spellId), nameA, rankA) ||
            !lookups.spellText(static_cast<uint32_t>(b.spellId), nameB, rankB))
            return 0;
        if (!tradeskill_ && a.service.reqLevel != b.service.reqLevel)
            return b.service.reqLevel < a.service.reqLevel ? 1 : -1;
        if (a.service.reqSkillValue != b.service.reqSkillValue)
            return b.service.reqSkillValue < a.service.reqSkillValue ? 1 : -1;
        int c = compareCaseless(nameA, nameB);
        if (c == 0 && !tradeskill_) c = compareCaseless(rankA, rankB);
        return c;
    };
    std::stable_sort(rows_.begin(), rows_.end(),
                     [&](const Row& a, const Row& b) { return compare(a, b) < 0; });

    // The first shown service that can be learned is selected; failing one,
    // the second row's, when there is a second row and it is a service.
    selection_ = 0;
    bool found = false;
    for (const auto& r : rows_) {
        if (r.spellId > 0 && r.state == 0 && r.visible) {
            selection_ = r.spellId;
            found = true;
            break;
        }
    }
    if (!found && rows_.size() >= 2) selection_ = std::max(rows_[1].spellId, 0);
}

int TrainerServiceList::headerOfRow(uint32_t index) const {
    const Row* r = row(index);
    if (!r || !r->header()) return -1;
    return headerPos(r->line);
}

bool TrainerServiceList::rowCollapsed(uint32_t index) const {
    const int pos = headerOfRow(index);
    return pos >= 0 && (expanded_ & bit(static_cast<uint32_t>(pos))) == 0;
}

int TrainerServiceList::selectedRow() const {
    if (selection_ == 0) return -1;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (rows_[i].spellId == selection_) return static_cast<int>(i);
    return -1;
}

void TrainerServiceList::select(uint32_t index) {
    const Row* r = row(index);
    selection_ = r && r->spellId >= 0 ? r->spellId : 0;
}

}  // namespace wowee::game
