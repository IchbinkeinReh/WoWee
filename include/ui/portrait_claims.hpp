#pragma once

/// Whose face a portrait texture shows, and the views that draw the faces.
///
/// SetPortraitTexture (0x006113e0) gives a texture the unit's face as the
/// token names it at that moment: the picture stays that unit's whatever the
/// token names later, until it is asked for again. So a texture is claimed
/// for a guid, not for a token - and any unit has a face, not a list of them.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace wowee::ui::portrait_claims {

/// The claim of the player's own face, which has a view of its own.
inline constexpr const char* kPlayer = "player";

/// The claim of any other unit's face.
inline std::string unitKey(uint64_t guid) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "unit:%016llx", static_cast<unsigned long long>(guid));
    return buf;
}

/// Whose face a claim is, or none for a claim that is no unit's.
inline std::optional<uint64_t> unitOf(const std::string& key) {
    if (key.rfind("unit:", 0) != 0) return std::nullopt;
    return std::strtoull(key.c_str() + 5, nullptr, 16);
}

/// The views the faces are drawn in: one to each claimed unit. A unit nobody
/// claims any more keeps its view until another needs it - its picture is
/// there should it be claimed again, as the client keeps a display's
/// (0x006f6020) - and past kKept of them the longest unclaimed goes first.
class Pool {
public:
    static constexpr size_t kKept = 8;

    /// In step with the claims there are.
    void assign(const std::vector<std::string>& claimed) {
        ++now_;
        for (const auto& key : claimed)
            if (const auto view = viewOf(key)) holders_[*view].used = now_;
        for (const auto& key : claimed) {
            if (key.empty() || viewOf(key)) continue;
            const size_t view = freeView();
            holders_[view] = {key, now_};
        }
    }
    [[nodiscard]] std::optional<size_t> viewOf(const std::string& key) const {
        for (size_t i = 0; i < holders_.size(); ++i)
            if (!key.empty() && holders_[i].claim == key) return i;
        return std::nullopt;
    }
    [[nodiscard]] size_t views() const { return holders_.size(); }

private:
    struct Holder {
        std::string claim;
        uint64_t used = 0;
    };
    size_t freeView() {
        std::optional<size_t> oldest;
        for (size_t i = 0; i < holders_.size(); ++i) {
            if (holders_[i].used == now_) continue;
            if (!oldest || holders_[i].used < holders_[*oldest].used) oldest = i;
        }
        if (oldest && holders_.size() >= kKept) return *oldest;
        holders_.emplace_back();
        return holders_.size() - 1;
    }
    std::vector<Holder> holders_;
    uint64_t now_ = 0;
};

}  // namespace wowee::ui::portrait_claims
