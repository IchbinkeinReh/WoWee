#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace wowee::rendering {

/// Pictures by name, of which the longest unused are let go of where there are too many -
/// never one that was asked for in the round under way, however many those are: what is
/// made of them holds them by pointer until it has been drawn.
template <typename Picture>
class PictureCache {
public:
    explicit PictureCache(size_t most) : most_(most) {}

    /// A new picture of the whole is begun.
    void beginRound() { ++round_; }

    /// Whether the name is known: the picture is then what `find` answers, null for one
    /// there is none of.
    [[nodiscard]] bool known(const std::string& name) const { return entries_.count(name) != 0; }
    [[nodiscard]] Picture* find(const std::string& name) {
        const auto it = entries_.find(name);
        if (it == entries_.end()) return nullptr;
        it->second.used = round_;
        return it->second.picture.get();
    }
    Picture* add(const std::string& name, std::unique_ptr<Picture> picture) {
        Entry& e = entries_[name];
        e.picture = std::move(picture);
        e.used = round_;
        return e.picture.get();
    }

    /// The round is over: what is too many, the longest unused first, to `letGo`, which
    /// keeps it for as long as frames under way may use it.
    template <typename LetGo>
    void endRound(LetGo&& letGo) {
        while (size() > most_) {
            auto oldest = entries_.end();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                if (!it->second.picture || it->second.used == round_) continue;
                if (oldest == entries_.end() || it->second.used < oldest->second.used) oldest = it;
            }
            if (oldest == entries_.end()) return;
            letGo(std::move(oldest->second.picture));
            entries_.erase(oldest);
        }
    }

    /// All of it, taken out.
    [[nodiscard]] std::vector<std::unique_ptr<Picture>> takeAll() {
        std::vector<std::unique_ptr<Picture>> out;
        for (auto& [name, e] : entries_)
            if (e.picture) out.push_back(std::move(e.picture));
        entries_.clear();
        return out;
    }
    /// How many there are; those there are none of do not count.
    [[nodiscard]] size_t size() const {
        size_t n = 0;
        for (const auto& [name, e] : entries_) n += e.picture ? 1 : 0;
        return n;
    }

private:
    struct Entry {
        std::unique_ptr<Picture> picture;
        uint64_t used = 0;
    };
    std::unordered_map<std::string, Entry> entries_;
    size_t most_;
    uint64_t round_ = 0;
};

}  // namespace wowee::rendering
