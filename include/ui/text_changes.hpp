#pragma once

/// Which edit boxes owe an OnTextChanged (CSimpleEditBox's +0x2a4: 1 the text
/// changed, 8 by the player).
///
/// Told at the box's next update (0x00967670 -> 0x00967020), which only a box
/// that is shown has, and before it is given a key or a click (0x00967760,
/// 0x00967cd0): once, however often the text was changed, and as the last
/// change had it (0x00965960). A new box owes one (0x00966b70).

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace wowee::ui {

class TextChanges {
public:
    struct Owed {
        uint32_t box = 0;
        bool userInput = false;
    };
    enum class Box { Gone, Hidden, Shown };

    void note(uint32_t box, bool userInput) {
        for (auto& o : owed_) {
            if (o.box == box) {
                o.userInput = userInput;
                return;
            }
        }
        owed_.push_back({box, userInput});
    }

    /// One box's, told now; nothing where it owes none.
    std::optional<bool> takeOf(uint32_t box) {
        for (size_t i = 0; i < owed_.size(); ++i) {
            if (owed_[i].box != box) continue;
            const bool userInput = owed_[i].userInput;
            owed_.erase(owed_.begin() + static_cast<std::ptrdiff_t>(i));
            return userInput;
        }
        return std::nullopt;
    }

    /// Those of the boxes shown, in the order they changed; the hidden ones go
    /// on owing theirs.
    template <typename State>
    std::vector<Owed> take(State state) {
        std::vector<Owed> now, later;
        for (const auto& o : owed_) {
            const Box box = state(o.box);
            if (box == Box::Shown) now.push_back(o);
            else if (box == Box::Hidden) later.push_back(o);
        }
        owed_ = std::move(later);
        return now;
    }

    void clear() { owed_.clear(); }
    [[nodiscard]] size_t size() const { return owed_.size(); }

private:
    std::vector<Owed> owed_;
};

}  // namespace wowee::ui
