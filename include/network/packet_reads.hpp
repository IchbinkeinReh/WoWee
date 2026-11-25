#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wowee::network {

/// What the packets' handlers have read of them: bytes left over, and reads past the end. One
/// laid out otherwise than it is read shows as one of the two.
class PacketReads {
public:
    struct Row {
        uint16_t opcode = 0;   ///< on the wire
        uint32_t packets = 0;
        uint32_t leftOver = 0;      ///< of them with bytes left
        size_t mostLeftOver = 0;    ///< and the most
        uint32_t past = 0;          ///< of them read past their end
    };

    void note(uint16_t opcode, size_t left, bool past) {
        Row& row = rows_[opcode];
        row.opcode = opcode;
        ++row.packets;
        if (left > 0) {
            ++row.leftOver;
            row.mostLeftOver = std::max(row.mostLeftOver, left);
        }
        if (past) ++row.past;
    }

    /// Those with either, the ones read past their end first, then by how often.
    [[nodiscard]] std::vector<Row> odd() const {
        std::vector<Row> out;
        for (const auto& [opcode, row] : rows_)
            if (row.leftOver != 0 || row.past != 0) out.push_back(row);
        std::sort(out.begin(), out.end(), [](const Row& a, const Row& b) {
            if (a.past != b.past) return a.past > b.past;
            if (a.leftOver != b.leftOver) return a.leftOver > b.leftOver;
            return a.opcode < b.opcode;
        });
        return out;
    }

private:
    std::unordered_map<uint16_t, Row> rows_;
};

}  // namespace wowee::network
