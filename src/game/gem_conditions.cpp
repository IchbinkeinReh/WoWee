#include "game/gem_conditions.hpp"

namespace wowee::game::gem_conditions {

const char* colorKey(uint8_t color) {
    switch (color) {
        case 0: return "";
        case 1: return "META_GEM";
        case 2: return "RED_GEM";
        case 3: return "YELLOW_GEM";
        case 4: return "BLUE_GEM";
        default: return nullptr;
    }
}

int countOf(const Counts& counts, uint8_t color) {
    if (color < 1 || color > 4) return 0;
    return counts[static_cast<size_t>(color - 1)];
}

void addGem(Counts& counts, uint32_t colorMask) {
    for (size_t bit = 0; bit < 4; ++bit)
        if (colorMask & (1u << bit)) ++counts[bit];
}

Result evaluate(const Row& row, const Counts& counts) {
    Result out;
    for (size_t i = 0; i < 5; ++i) {
        Line line;
        const uint8_t lt = row.color[i];
        const uint8_t rt = row.compareColor[i];
        const uint8_t op = row.comparator[i];
        if (rt == 0) {
            // Against a number: the count of Color, the number in Value.
            const int have = countOf(counts, lt);
            int want = row.value[i];
            line.valueForm = true;
            line.colorA = lt;
            switch (op) {
                case 0: line.key = "ENCHANT_CONDITION_EQUAL_VALUE";     line.met = have == want; break;
                case 1: line.key = "ENCHANT_CONDITION_NOT_EQUAL_VALUE"; line.met = have != want; break;
                case 3: ++want; [[fallthrough]];
                case 5: line.key = "ENCHANT_CONDITION_MORE_VALUE";      line.met = want <= have; break;
                case 4: ++want; [[fallthrough]];
                case 2: line.key = "ENCHANT_CONDITION_LESS_VALUE";      line.met = have < want;  break;
                default: break;
            }
            line.number = want;
        } else {
            // Against another colour's count.
            const int l = countOf(counts, lt);
            const int r = countOf(counts, rt);
            line.colorA = lt;
            line.colorB = rt;
            switch (op) {
                case 0: line.key = "ENCHANT_CONDITION_EQUAL_COMPARE";     line.met = l == r; break;
                case 1: line.key = "ENCHANT_CONDITION_NOT_EQUAL_COMPARE"; line.met = l != r; break;
                case 2: line.key = "ENCHANT_CONDITION_MORE_COMPARE";
                        line.colorA = rt; line.colorB = lt;               line.met = r > l;  break;
                case 3: line.key = "ENCHANT_CONDITION_MORE_COMPARE";      line.met = l > r;  break;
                case 4: line.key = "ENCHANT_CONDITION_MORE_EQUAL_COMPARE";
                        line.colorA = rt; line.colorB = lt;               line.met = l <= r; break;
                case 5: line.key = "ENCHANT_CONDITION_MORE_EQUAL_COMPARE"; line.met = r <= l; break;
                default: break;
            }
        }
        if (!line.met) out.allMet = false;
        out.lines.push_back(line);
        if (row.logic[i] == 0) break;
    }
    return out;
}

}  // namespace wowee::game::gem_conditions
