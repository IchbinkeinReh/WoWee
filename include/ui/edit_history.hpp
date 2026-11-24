#pragma once

/// An edit box's history (CSimpleEditBox +0x310 how many, +0x314 where, +0x320
/// the lines): a ring with a place in it. A line sent goes in at the place, and
/// the place moves on (0x00964690) - so one sent after walking back takes the
/// place of the line it was walked back to. The arrows move the place to the
/// next line there is, round the ring (0x00966600, 0x00966670).

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace wowee::ui {

class EditHistory {
public:
    /// How many lines are kept (0x00966fd0); 0 for none.
    void keep(int count) {
        const size_t n = count > 0 ? static_cast<size_t>(count) : 0;
        lines_.resize(n);
        if (n <= at_) at_ = n > 0 ? n - 1 : 0;
    }
    [[nodiscard]] int kept() const { return static_cast<int>(lines_.size()); }

    void add(const std::string& line) {
        if (lines_.empty()) return;
        lines_[at_] = line;
        at_ = (at_ + 1) % lines_.size();
    }

    /// Forgotten, and as many kept (0x00964720).
    void clear() {
        for (auto& line : lines_) line.reset();
        at_ = 0;
    }

    /// The line before the place, and the one after it; null where there is
    /// none at all.
    const std::string* previous() { return move(lines_.empty() ? 0 : lines_.size() - 1); }
    const std::string* next() { return move(1); }

private:
    const std::string* move(size_t step) {
        const size_t n = lines_.size();
        for (size_t k = 1, to = at_; k <= n; ++k) {
            to = (to + step) % n;
            if (!lines_[to]) continue;
            at_ = to;
            return &*lines_[to];
        }
        return nullptr;
    }

    std::vector<std::optional<std::string>> lines_;
    size_t at_ = 0;
};

}  // namespace wowee::ui
