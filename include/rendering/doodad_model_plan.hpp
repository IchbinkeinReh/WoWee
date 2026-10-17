#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/// Which M2 models a tile's WMO doodads need read and parsed, and which doodad
/// carries each parsed model to finalization.
///
/// A tile's doodads are gathered in order first, the models are then parsed
/// in parallel, and the doodads are filled in afterwards in the same order.
/// The plan is what keeps that order: each model is parsed once however many
/// doodads use it, and the first doodad to use it carries the model, as the
/// serial loop did; the others name it by id and find it uploaded when their
/// turn comes, since finalization uploads the doodads in order.
namespace wowee::rendering::doodad_model_plan {

/// The models to parse: each id once, in the order the doodads first use it,
/// without the ids `resident` already has (uploaded by an earlier tile).
template <typename Resident>
std::vector<uint32_t> modelsToParse(const std::vector<uint32_t>& doodadModelIds,
                                    Resident&& resident) {
    std::vector<uint32_t> out;
    std::unordered_set<uint32_t> seen;
    for (uint32_t id : doodadModelIds) {
        if (!seen.insert(id).second) continue;
        if (resident(id)) continue;
        out.push_back(id);
    }
    return out;
}

/// What a doodad takes to finalization.
enum class Carry : uint8_t {
    Drop,    // its model was to be parsed here and could not be: no doodad
    Model,   // the first doodad of a model parsed here: it carries the model
    Shared,  // the model is resident, or an earlier doodad carries it
};

/// One Carry per doodad, in order. `parsed` holds each model that was to be
/// parsed, true if it parsed; a model it does not hold was resident.
inline std::vector<Carry> assign(const std::vector<uint32_t>& doodadModelIds,
                                 const std::unordered_map<uint32_t, bool>& parsed) {
    std::vector<Carry> out;
    out.reserve(doodadModelIds.size());
    std::unordered_set<uint32_t> carried;
    for (uint32_t id : doodadModelIds) {
        auto it = parsed.find(id);
        if (it == parsed.end()) {
            out.push_back(Carry::Shared);
        } else if (!it->second) {
            out.push_back(Carry::Drop);
        } else {
            out.push_back(carried.insert(id).second ? Carry::Model : Carry::Shared);
        }
    }
    return out;
}

} // namespace wowee::rendering::doodad_model_plan
