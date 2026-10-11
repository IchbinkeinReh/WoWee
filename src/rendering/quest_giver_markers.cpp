#include "rendering/quest_giver_markers.hpp"
#include "rendering/m2_renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/logger.hpp"
#include <unordered_set>

namespace wowee {
namespace rendering {

uint32_t QuestGiverMarkers::modelId(const char* path) {
    auto it = modelIds_.find(path);
    if (it != modelIds_.end()) {
        // Loaded once, but a map change empties the renderer.
        if (it->second == 0 || m2_->hasModel(it->second)) return it->second;
    }
    const uint32_t id = it != modelIds_.end()
                            ? it->second
                            : kFirstModelId + static_cast<uint32_t>(modelIds_.size());
    modelIds_[path] = 0;
    if (!assets_) return 0;

    const auto data = assets_->readFile(path);
    pipeline::M2Model model = data.empty() ? pipeline::M2Model{} : pipeline::M2Loader::load(data);
    if (model.vertices.empty()) {
        LOG_WARNING("QuestGiverMarkers: ", path, " is missing or empty");
        return 0;
    }
    if (model.name.empty()) model.name = path;
    if (model.version >= 264) {
        const auto skin = assets_->readFile(pipeline::skinPathForM2(path));
        if (!skin.empty()) pipeline::M2Loader::loadSkin(skin, model);
    }
    if (!m2_->loadModel(model, id)) {
        LOG_WARNING("QuestGiverMarkers: could not load ", path);
        return 0;
    }
    // Its own animation runs, it blocks nothing, and it is never merged with
    // another mark at the same spot.
    m2_->markModelAsSpellEffect(id);
    modelIds_[path] = id;
    return id;
}

void QuestGiverMarkers::update(M2Renderer* m2, pipeline::AssetManager* assets,
                               const std::vector<Marker>& markers) {
    if (m2 != m2_) {
        // A different renderer holds none of the instances or models kept here.
        placed_.clear();
        modelIds_.clear();
        m2_ = m2;
    }
    assets_ = assets;
    if (!m2) return;
    if (markers.empty() && placed_.empty()) return;

    std::unordered_set<uint64_t> wanted;
    wanted.reserve(markers.size());
    for (const Marker& marker : markers) {
        if (!marker.model) continue;
        const uint32_t id = modelId(marker.model);
        if (id == 0) continue;
        wanted.insert(marker.guid);

        auto it = placed_.find(marker.guid);
        if (it != placed_.end() && (it->second.modelId != id || !m2->hasInstance(it->second.instance))) {
            // The status changed the model (0x00744400 rebuilds it), or the
            // instance went with the renderer's contents.
            m2->removeInstance(it->second.instance);
            placed_.erase(it);
            it = placed_.end();
        }
        if (it == placed_.end()) {
            const uint32_t instance =
                m2->createInstance(id, glm::vec3(marker.frame[3]), glm::vec3(0.0f), 1.0f, false);
            if (instance == 0) continue;
            // Drawn as far as the object it hangs on, not by a doodad's size.
            m2->setInstanceIsGameObject(instance, true);
            it = placed_.emplace(marker.guid, Placed{instance, id}).first;
        }
        m2->setInstanceTransform(it->second.instance, marker.frame);
    }

    for (auto it = placed_.begin(); it != placed_.end();) {
        if (wanted.count(it->first) == 0) {
            m2->removeInstance(it->second.instance);
            it = placed_.erase(it);
        } else {
            ++it;
        }
    }
}

void QuestGiverMarkers::clear() {
    if (m2_) {
        for (const auto& [guid, placed] : placed_) {
            if (m2_->hasInstance(placed.instance)) m2_->removeInstance(placed.instance);
        }
    }
    placed_.clear();
}

} // namespace rendering
} // namespace wowee
