#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee::game {

/// What is read in the item text window: the pages of a book, a plaque or a
/// letter's paper, one after the other (0x0058a1a0). A page is asked for as it
/// is turned to, and kept (the client's page text cache).
class ItemBook {
public:
    /// Opened at its first page; 0 closes it.
    void open(uint32_t first) {
        shown_.clear();
        at_ = 0;
        if (first != 0) shown_.push_back(first);
    }
    void close() { open(0); }
    [[nodiscard]] bool isOpen() const { return !shown_.empty(); }

    /// A page's text. True where it is the one shown: the window draws it
    /// (ITEM_TEXT_READY).
    bool arrived(uint32_t id, std::string text, uint32_t next) {
        const bool waited = wanted() == id;
        pages_[id] = {std::move(text), next};
        return waited && id != 0;
    }
    /// The page shown whose text is not there yet, 0 for none.
    [[nodiscard]] uint32_t wanted() const { return isOpen() && !here() ? shown_[at_] : 0; }

    bool turnOn() {
        const Page* page = here();
        if (!page || page->next == 0) return false;
        shown_.resize(++at_);
        shown_.push_back(page->next);
        return true;
    }
    bool turnBack() {
        if (at_ == 0) return false;
        --at_;
        return true;
    }

    /// From zero.
    [[nodiscard]] size_t page() const { return at_; }
    [[nodiscard]] bool hasNext() const { return here() && here()->next != 0; }
    [[nodiscard]] const std::string& text() const {
        static const std::string kNone;
        return here() ? here()->text : kNone;
    }

private:
    struct Page {
        std::string text;
        uint32_t next = 0;
    };
    [[nodiscard]] const Page* here() const {
        if (shown_.empty()) return nullptr;
        const auto it = pages_.find(shown_[at_]);
        return it == pages_.end() ? nullptr : &it->second;
    }
    std::unordered_map<uint32_t, Page> pages_;
    std::vector<uint32_t> shown_;
    size_t at_ = 0;
};

}  // namespace wowee::game
