#pragma once

// A list's selection, kept as the client keeps it: what the selected row is of
// - a guild member's guid (0x005caf40), a skill line (0x005cd860), a faction
// (0x005d07b0), a recipe's spell (0x005da900) - and looked for among the rows
// when its place is asked for (0x005ca950, 0x005cd360, 0x005d01b0, 0x005da670).
// Kept as the place itself, it was of another row as soon as one came, went,
// moved or was folded away before it.

#include <cstddef>

namespace wowee::addons {

template <class Key>
class RowSelection {
public:
    /// The row at `index`, from one; off either end there is none.
    template <class Rows, class KeyOf>
    void select(const Rows& rows, int index, KeyOf&& keyOf) {
        key_ = index >= 1 && static_cast<size_t>(index) <= rows.size()
                   ? static_cast<Key>(keyOf(rows[static_cast<size_t>(index) - 1]))
                   : Key{};
    }

    /// Its place among the rows there are now, from one; 0 for none of them.
    template <class Rows, class KeyOf>
    [[nodiscard]] int index(const Rows& rows, KeyOf&& keyOf) const {
        if (key_ == Key{}) return 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (static_cast<Key>(keyOf(rows[i])) == key_) return static_cast<int>(i) + 1;
        }
        return 0;
    }

    [[nodiscard]] Key key() const { return key_; }
    void clear() { key_ = Key{}; }

private:
    Key key_{};
};

}  // namespace wowee::addons
