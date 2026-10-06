#include "pipeline/m2_asset_loader.hpp"

#include <cstdio>

#include "pipeline/asset_manager.hpp"
#include "pipeline/m2_loader.hpp"

namespace wowee {
namespace pipeline {

namespace {

// The M2 and its .skin, keeping the M2's bytes: the .anim data is slotted into
// track headers that are only read from there.
bool readM2AndSkin(AssetManager& assets, const std::string& m2Path, M2Model& outModel,
                   std::vector<uint8_t>& outM2Data) {
    outM2Data = assets.readFile(m2Path);
    if (outM2Data.empty()) return false;

    outModel = M2Loader::load(outM2Data);
    // Everything downstream identifies a model by its name, and a model that
    // does not carry one is identified by nothing.
    if (outModel.name.empty()) outModel.name = m2Path;

    if (outModel.version >= 264) {
        auto skinData = assets.readFile(skinPathForM2(m2Path));
        if (!skinData.empty()) M2Loader::loadSkin(skinData, outModel);
    }
    return outModel.isValid();
}

}  // namespace

bool loadM2WithSkin(AssetManager& assets, const std::string& m2Path, M2Model& outModel) {
    std::vector<uint8_t> m2Data;
    return readM2AndSkin(assets, m2Path, outModel, m2Data);
}

bool loadM2WithAllAnimations(AssetManager& assets, const std::string& m2Path, M2Model& outModel) {
    std::vector<uint8_t> m2Data;
    if (!readM2AndSkin(assets, m2Path, outModel, m2Data)) return false;
    loadExternalAnimations(assets, m2Path, m2Data, outModel);
    return true;
}

std::string animPathForM2(const std::string& m2Path, uint32_t animId, uint32_t variationIndex) {
    const std::string base = skinPathForM2(m2Path);
    // skinPathForM2 answers "<stem>00.skin"; the stem is what is wanted here.
    const std::string stem = base.substr(0, base.size() - 7);
    char tail[32];
    std::snprintf(tail, sizeof(tail), "%04u-%02u.anim", animId, variationIndex);
    return stem + tail;
}

void loadExternalAnimations(AssetManager& assets, const std::string& m2Path,
                            const std::vector<uint8_t>& m2Data, M2Model& model,
                            std::initializer_list<uint32_t> wantedAnimIds) {
    for (uint32_t si : externalSequenceIndices(model, wantedAnimIds)) {
        auto animData = assets.readFileOptional(
            animPathForM2(m2Path, model.sequences[si].id, model.sequences[si].variationIndex));
        if (!animData.empty()) M2Loader::loadAnimFile(m2Data, animData, si, model);
    }
}

}  // namespace pipeline
}  // namespace wowee
