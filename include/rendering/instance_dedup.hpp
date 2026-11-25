#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>

#include <glm/vec3.hpp>

namespace wowee::rendering {

/// Which instance of a model stands at a place, to a tenth of a yard: asked
/// for a second one there, the renderer gives out the one that does.
///
/// An entry is its instance's, and goes where it goes and when it goes. An
/// entry left behind names an instance that has moved on - and shares it with
/// something standing elsewhere - or one that has been removed, and whatever
/// is made there is given an id nothing is drawn for.
class InstanceDedup {
public:
    /// The instance of the model filed here, or 0.
    [[nodiscard]] uint32_t find(uint32_t modelId, const glm::vec3& at) const {
        const auto it = map_.find(keyOf(modelId, at));
        return it == map_.end() ? 0u : it->second;
    }

    void file(uint32_t modelId, const glm::vec3& at, uint32_t instanceId) {
        map_[keyOf(modelId, at)] = instanceId;
    }

    /// The instance went from one place to another. One that was not filed
    /// where it stood is not shared, and is not filed by moving.
    void move(uint32_t modelId, const glm::vec3& from, const glm::vec3& to, uint32_t instanceId) {
        const Key oldKey = keyOf(modelId, from);
        const Key newKey = keyOf(modelId, to);
        if (oldKey == newKey) return;
        const auto it = map_.find(oldKey);
        if (it == map_.end() || it->second != instanceId) return;
        map_.erase(it);
        map_.try_emplace(newKey, instanceId);
    }

    /// The instance is gone. Only its own entry goes with it.
    void forget(uint32_t modelId, const glm::vec3& at, uint32_t instanceId) {
        const auto it = map_.find(keyOf(modelId, at));
        if (it != map_.end() && it->second == instanceId) map_.erase(it);
    }

    void clear() { map_.clear(); }
    [[nodiscard]] std::size_t size() const { return map_.size(); }

private:
    struct Key {
        uint32_t modelId;
        int32_t qx, qy, qz;  // position quantized to 0.1 units
        bool operator==(const Key& o) const {
            return modelId == o.modelId && qx == o.qx && qy == o.qy && qz == o.qz;
        }
    };
    struct Hash {
        std::size_t operator()(const Key& k) const {
            std::size_t h = std::hash<uint32_t>()(k.modelId);
            h ^= std::hash<int32_t>()(k.qx) * 2654435761u;
            h ^= std::hash<int32_t>()(k.qy) * 40503u;
            h ^= std::hash<int32_t>()(k.qz) * 12289u;
            return h;
        }
    };
    static Key keyOf(uint32_t modelId, const glm::vec3& p) {
        return {.modelId = modelId,
                .qx = static_cast<int32_t>(std::round(p.x * 10.0f)),
                .qy = static_cast<int32_t>(std::round(p.y * 10.0f)),
                .qz = static_cast<int32_t>(std::round(p.z * 10.0f))};
    }

    std::unordered_map<Key, uint32_t, Hash> map_;
};

} // namespace wowee::rendering
