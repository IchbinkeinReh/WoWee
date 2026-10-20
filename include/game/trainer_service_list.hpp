#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "game/world_packets.hpp"

namespace wowee::game {

/// The trainer window's list as the client keeps it (ClassTrainerFrame.cpp):
/// the services the trainer sent, grouped under one header row per skill line,
/// and the filters that decide which rows the interface sees.
///
/// Built when SMSG_TRAINER_LIST arrives (0x00596450): every service is given
/// its group - a class trainer's from the skill line its spell belongs to
/// (0x00594ae0), a tradeskill trainer's from the kind of spell it is - and a
/// service with no group is not listed. One header row per group follows the
/// services. 0x00594ba0 then marks each row shown or hidden by the service
/// type filter (CVar serviceTypeFilter), the skill line filter and the
/// collapsed headers, and sorts the rows so the shown ones come first, under
/// their headers. Every Lua call that takes a service index counts in that
/// order, the hidden rows included past the shown ones.
class TrainerServiceList {
public:
    /// A row of the list, 0x38 bytes in the client.
    struct Row {
        int32_t spellId = -1;    ///< +0x00, -1 for a header row
        uint32_t line = 0;       ///< +0x04, the group (a SkillLine id or a tradeskill kind)
        TrainerSpell service;    ///< +0x08..+0x2c as the trainer sent it (zero for a header)
        uint8_t state = 0;       ///< +0x30, 0 available, 1 unavailable, 2 used
        bool visible = false;    ///< +0x34
        bool header() const { return spellId < 0; }
    };
    /// A group, 0x28 bytes in the client.
    struct Header {
        uint32_t line = 0;                  ///< +0x00
        std::array<uint32_t, 6> counts{};   ///< +0x04, services in each state
        bool lineShown = false;             ///< +0x1c, passes both filters
        bool collapsed = false;             ///< +0x20
        bool professionOnly = false;        ///< +0x24, every service costs a profession slot
    };
    /// What the list needs from the rest of the client.
    struct Lookups {
        /// A service's group, 0 for one that is not listed.
        std::function<uint32_t(const TrainerSpell&)> groupOf;
        /// A skill line's name, what a class trainer's headers are ordered by.
        std::function<std::string(uint32_t line)> lineName;
        /// A spell's name and rank; false when there is no Spell row.
        std::function<bool(uint32_t spellId, std::string& name, std::string& rank)> spellText;
        /// A service this client's trainer search box hides. Not the client's;
        /// left empty, nothing is hidden by it.
        std::function<bool(const Row&)> searchHides;
    };

    /// 0x00596450: the list from what the trainer sent. `states` holds each
    /// service's state, one per entry of `services`. Both filters on lines
    /// are reset to everything.
    void build(const std::vector<TrainerSpell>& services, const std::vector<uint8_t>& states,
               bool tradeskill, uint32_t typeMask, const Lookups& lookups);
    /// 0x00594ba0: which rows show, their order, and the selection.
    void refilter(uint32_t typeMask, const Lookups& lookups);
    /// Each service's state asked again; when one changed without a new list,
    /// the headers are recounted and the list filtered again. True then.
    bool restate(const std::function<uint8_t(const Row&)>& stateOf, uint32_t typeMask,
                 const Lookups& lookups);
    void clear();

    bool tradeskill() const { return tradeskill_; }
    size_t size() const { return rows_.size(); }
    /// GetNumTrainerServices (0x00593d10): the rows that show.
    uint32_t visibleCount() const { return visible_; }
    /// The row at a zero-based index, shown or not; null past the end.
    const Row* row(uint32_t index) const { return index < rows_.size() ? &rows_[index] : nullptr; }
    const std::vector<Header>& headers() const { return headers_; }
    /// 0x005942b0: the header a header row stands for, -1 for any other row.
    int headerOfRow(uint32_t index) const;
    /// 0x00594300: a header row whose header is collapsed.
    bool rowCollapsed(uint32_t index) const;

    uint32_t lineFilter() const { return lineFilter_; }      ///< 0xc0e4ac
    uint32_t expandedMask() const { return expanded_; }      ///< 0xc0e4b0
    void setLineFilter(uint32_t mask) { lineFilter_ = mask; }
    void setExpandedMask(uint32_t mask) { expanded_ = mask; }

    /// 0x00594170: the selected service's row, -1 with none.
    int selectedRow() const;
    /// 0x005943a0: select the row's service; a header or a bad index selects
    /// nothing.
    void select(uint32_t index);
    int32_t selectedSpell() const { return selection_; }

    /// The serial of the list this was built from, for the caller to notice a
    /// new one.
    uint64_t serial = 0;

private:
    int headerPos(uint32_t line) const;
    void count();

    std::vector<Row> rows_;
    std::vector<Header> headers_;
    uint32_t visible_ = 0;      ///< 0xc0e4a8
    uint32_t lineFilter_ = ~0u;
    uint32_t expanded_ = ~0u;
    int32_t selection_ = 0;     ///< 0xc0e49c, a spell id
    bool tradeskill_ = false;   ///< 0xc0e498 == 2
};

/// The service type a filter name stands for (0x00593db0): 0 "available",
/// 1 "unavailable", 2 "used", 6 for anything else.
int trainerServiceType(const char* name);
/// The word the interface uses for a row's state (0x00594260).
const char* trainerServiceTypeName(const TrainerServiceList::Row& row);

}  // namespace wowee::game
