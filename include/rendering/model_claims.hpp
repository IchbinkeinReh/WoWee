#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wowee::rendering {

/// Which models the renderer has for the tiles, and which a tile on its way counts on.
///
/// A tile's worker reads no model the renderer has; the renderer throws out what nothing has
/// drawn for a while. A tile read seconds before it is put up counted on what was gone by then:
/// its doodads were left out. The client keeps a model by its references; a tile's are these,
/// from its reading to its end. From any thread.
class ModelClaims {
public:
    void uploaded(uint32_t id) {
        const std::lock_guard lock(mutex_);
        uploaded_.insert(id);
    }
    [[nodiscard]] bool isUploaded(uint32_t id) const {
        const std::lock_guard lock(mutex_);
        return uploaded_.count(id) > 0;
    }
    /// Whether a model may be thrown out, which it then is: no tile counts on it.
    bool letGo(uint32_t id) {
        const std::lock_guard lock(mutex_);
        if (claims_.count(id) > 0) return false;
        uploaded_.erase(id);
        return true;
    }
    void forget(const std::vector<uint32_t>& ids) {
        const std::lock_guard lock(mutex_);
        for (const uint32_t id : ids) uploaded_.erase(id);
    }
    void clear() {
        const std::lock_guard lock(mutex_);
        uploaded_.clear();
    }

    /// A tile's, for as long as it is.
    class Hold {
    public:
        Hold() = default;
        explicit Hold(std::shared_ptr<ModelClaims> of) : of_(std::move(of)) {}
        Hold(Hold&& other) noexcept : of_(std::move(other.of_)), ids_(std::move(other.ids_)) { other.of_.reset(); }
        Hold& operator=(Hold&& other) noexcept {
            if (this != &other) {
                release();
                of_ = std::move(other.of_);
                ids_ = std::move(other.ids_);
                other.of_.reset();
                other.ids_.clear();
            }
            return *this;
        }
        Hold(const Hold&) = delete;
        Hold& operator=(const Hold&) = delete;
        ~Hold() { release(); }

        /// Counts on a model from here on; whether the renderer has it.
        bool take(uint32_t id) {
            if (!of_) return false;
            const std::lock_guard lock(of_->mutex_);
            if (ids_.insert(id).second) ++of_->claims_[id];
            return of_->uploaded_.count(id) > 0;
        }

    private:
        void release() {
            if (!of_) return;
            const std::lock_guard lock(of_->mutex_);
            for (const uint32_t id : ids_) {
                const auto it = of_->claims_.find(id);
                if (it != of_->claims_.end() && --it->second == 0) of_->claims_.erase(it);
            }
            ids_.clear();
        }

        std::shared_ptr<ModelClaims> of_;
        std::unordered_set<uint32_t> ids_;
    };

private:
    mutable std::mutex mutex_;
    std::unordered_set<uint32_t> uploaded_;
    std::unordered_map<uint32_t, uint32_t> claims_;
};

}  // namespace wowee::rendering
