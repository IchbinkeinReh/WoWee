// A cinematic's client side (core/cinematic_player.hpp).
#include "core/cinematic_player.hpp"

#include "addons/lua_api_registrations.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/audio_engine.hpp"
#include "core/logger.hpp"
#include "game/game_handler.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/renderer.hpp"
#include "rendering/terrain_manager.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <memory>
#include <numeric>
#include <random>

namespace wowee::core {

namespace {

namespace cin = game::cinematic;
namespace m2cam = rendering::m2_camera;

/// CinematicSequences: ID, SoundID, Camera[8].
constexpr uint32_t kSequenceSound = 1;
constexpr uint32_t kSequenceCamera0 = 2;
/// CinematicCamera: ID, Model, SoundID, OriginX/Y/Z, OriginFacing.
constexpr uint32_t kCameraModel = 1;
constexpr uint32_t kCameraSound = 2;
constexpr uint32_t kCameraOrigin = 3;
constexpr uint32_t kCameraFacing = 6;

/// The world around a camera that will not come: how long the black waits
/// for it at most, and how long for streaming to have started at all.
constexpr uint32_t kAreaWaitLimitMs = 15000;
constexpr uint32_t kAreaWaitStreamMs = 1000;

/// A model path as the tables name it (.mdx, .mdl) to the file there is.
std::string m2PathOf(std::string path) {
    const size_t dot = path.find_last_of('.');
    if (dot != std::string::npos) {
        std::string ext = path.substr(dot);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
        if (ext == ".mdx" || ext == ".mdl") path = path.substr(0, dot) + ".m2";
    }
    return path;
}

}  // namespace

CinematicPlayer::CinematicPlayer(game::GameHandler& gameHandler, rendering::Renderer& renderer,
                                 pipeline::AssetManager& assets)
    : gameHandler_(gameHandler), renderer_(renderer), assets_(assets), director_(*this) {}

CinematicPlayer::~CinematicPlayer() = default;

uint32_t CinematicPlayer::nowMs() const {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

void CinematicPlayer::start(uint32_t sequenceId) {
    LOG_INFO("Cinematic: sequence ", sequenceId);
    director_.start(sequenceId, nowMs());
}

void CinematicPlayer::stop() { director_.stop(nowMs()); }

void CinematicPlayer::update() { director_.update(nowMs()); }

void CinematicPlayer::abandon() {
    stopCameraSound();
    stopSequenceSound();
    renderer_.setCinematicView(std::nullopt);
    model_.reset();
    director_.reset();
    gameHandler_.setInCinematic(false);
}

std::optional<cin::SequenceRow> CinematicPlayer::sequenceRow(uint32_t id) {
    auto dbc = assets_.loadDBCOptional("CinematicSequences.dbc");
    if (!dbc || dbc->getFieldCount() < kSequenceCamera0 + 8) return std::nullopt;
    const int32_t row = dbc->findRecordById(id);
    if (row < 0) return std::nullopt;
    const auto r = static_cast<uint32_t>(row);
    cin::SequenceRow out;
    out.soundId = dbc->getUInt32(r, kSequenceSound);
    for (uint32_t i = 0; i < 8; ++i) out.cameras[i] = dbc->getUInt32(r, kSequenceCamera0 + i);
    return out;
}

std::optional<cin::CameraRow> CinematicPlayer::cameraRow(uint32_t id) {
    auto dbc = assets_.loadDBCOptional("CinematicCamera.dbc");
    if (!dbc || dbc->getFieldCount() <= kCameraFacing) return std::nullopt;
    const int32_t row = dbc->findRecordById(id);
    if (row < 0) return std::nullopt;
    const auto r = static_cast<uint32_t>(row);
    cin::CameraRow out;
    out.id = id;
    out.model = dbc->getString(r, kCameraModel);
    out.soundId = dbc->getUInt32(r, kCameraSound);
    out.origin = glm::vec3(dbc->getFloat(r, kCameraOrigin), dbc->getFloat(r, kCameraOrigin + 1),
                           dbc->getFloat(r, kCameraOrigin + 2));
    out.facing = dbc->getFloat(r, kCameraFacing);
    return out;
}

bool CinematicPlayer::hasActivePlayer() {
    const uint64_t guid = gameHandler_.getPlayerGuid();
    return guid != 0 && gameHandler_.getEntityManager().getEntity(guid) != nullptr;
}

void CinematicPlayer::setInCinematic(bool in) { gameHandler_.setInCinematic(in); }
void CinematicPlayer::stopWorldSounds() {
    // 0x00409550: the music and the ambience, as the world's unload stops them.
    if (auto* ac = renderer_.getAudioCoordinator()) ac->leaveWorld();
}

void CinematicPlayer::fireStart() { gameHandler_.fireAddonEvent("CINEMATIC_START"); }
void CinematicPlayer::fireStop() { gameHandler_.fireAddonEvent("CINEMATIC_STOP"); }

uint32_t CinematicPlayer::playKit(uint32_t soundId) {
    if (soundId == 0) return 0;
    auto dbc = assets_.loadDBCOptional("SoundEntries.dbc");
    const int32_t idx = dbc && dbc->getFieldCount() > 24 ? dbc->findRecordById(soundId) : -1;
    if (idx < 0) return 0;
    const auto row = static_cast<uint32_t>(idx);
    // SoundEntries: File[10] at 3, Freq[10] at 13, DirectoryBase at 23,
    // VolumeFloat at 24 - one file by the weights (0x004cbac0).
    std::vector<std::string> names;
    std::vector<uint32_t> weights;
    for (uint32_t f = 0; f < 10; ++f) {
        std::string name = dbc->getString(row, 3 + f);
        if (name.empty()) continue;
        names.push_back(std::move(name));
        weights.push_back(dbc->getUInt32(row, 13 + f));
    }
    if (names.empty()) return 0;
    static std::mt19937 rng{std::random_device{}()};
    size_t pick = 0;
    const uint64_t total = std::accumulate(weights.begin(), weights.end(), uint64_t{0});
    if (total == 0) {
        pick = std::uniform_int_distribution<size_t>(0, names.size() - 1)(rng);
    } else {
        uint64_t roll = std::uniform_int_distribution<uint64_t>(0, total - 1)(rng);
        for (; pick + 1 < names.size() && roll >= weights[pick]; ++pick) roll -= weights[pick];
    }
    const std::string dir = dbc->getString(row, 23);
    auto data = assets_.readFile(dir.empty() ? names[pick] : dir + "\\" + names[pick]);
    if (data.empty()) return 0;
    const float volume = dbc->getFloat(row, 24);
    return audio::AudioEngine::instance().playStream(
        std::make_shared<const std::vector<uint8_t>>(std::move(data)), volume > 0.0f ? volume : 1.0f, false);
}

void CinematicPlayer::playSequenceSound(uint32_t soundId) {
    // A CINEMATIC sound, with Sound_EnableMusic on (0x00528af0).
    stopSequenceSound();
    if (soundId == 0 || addons::storedCVarValue("Sound_EnableMusic", "1") == "0") return;
    if (auto* ac = renderer_.getAudioCoordinator()) ac->stopMusicChannel();
    sequenceSound_ = playKit(soundId);
}

void CinematicPlayer::stopSequenceSound() {
    if (sequenceSound_ != 0) audio::AudioEngine::instance().stopStream(sequenceSound_);
    sequenceSound_ = 0;
}

void CinematicPlayer::playCameraSound(uint32_t soundId) {
    // A RACIALCINEMATIC sound, one at a time (0x009f2268 allows one).
    stopCameraSound();
    if (auto* ac = renderer_.getAudioCoordinator()) ac->stopMusicChannel();
    cameraSound_ = playKit(soundId);
}

void CinematicPlayer::stopCameraSound() {
    if (cameraSound_ != 0) audio::AudioEngine::instance().stopStream(cameraSound_);
    cameraSound_ = 0;
}

std::optional<uint32_t> CinematicPlayer::loadCamera(const cin::CameraRow& row) {
    // 0x00606570: the old camera's model goes; the row's model, whose first
    // camera the world camera takes (0x006018c0), playing its Stand (0x00832ab0
    // with animation 0).
    model_.reset();
    cache_ = {};
    const std::string path = m2PathOf(row.model);
    auto data = assets_.readFile(path);
    if (data.empty()) {
        LOG_WARNING("Cinematic: no camera model ", path);
        return std::nullopt;
    }
    pipeline::M2Model model = pipeline::M2Loader::load(data);
    if (model.cameras.empty()) {
        LOG_WARNING("Cinematic: ", path, " has no camera");
        return std::nullopt;
    }
    uint32_t sequence = 0;
    for (uint32_t i = 0; i < model.sequences.size(); ++i) {
        if (model.sequences[i].id == 0) {
            sequence = i;
            break;
        }
    }
    uint32_t duration = 0;
    if (sequence < model.sequences.size()) {
        const pipeline::M2Sequence& seq = model.sequences[sequence];
        duration = seq.duration;
        // Its keys in an .anim file of its own where the model has them not.
        if ((seq.flags & 0x20u) == 0) {
            char suffix[32];
            std::snprintf(suffix, sizeof(suffix), "%04u-%02u.anim", seq.id, seq.variationIndex);
            const std::string base = path.substr(0, path.size() - 3);
            auto anim = assets_.readFile(base + suffix);
            if (!anim.empty()) pipeline::M2Loader::loadAnimFile(data, anim, sequence, model);
        }
    }
    model_ = std::move(model);
    sequenceIndex_ = sequence;
    row_ = row;
    cameraWaitStartMs_ = 0;
    LOG_INFO("Cinematic: camera ", row.id, " ", path, ", ", duration, " ms");
    return duration;
}

void CinematicPlayer::poseCamera(uint32_t timeMs) {
    if (!model_ || model_->cameras.empty()) return;
    const m2cam::Pose local = m2cam::evaluate(model_->cameras[0], sequenceIndex_, timeMs, cache_);
    const glm::vec3 position = m2cam::placeInWorld(local.position, row_.origin, row_.facing);
    const glm::vec3 target = m2cam::placeInWorld(local.target, row_.origin, row_.facing);
    const m2cam::View view = m2cam::viewOf(position, target, local.roll);
    cameraPosition_ = position;
    renderer_.setCinematicView(rendering::Renderer::CinematicView{
        .position = position, .forward = view.forward, .up = view.up});
}

bool CinematicPlayer::areaReady(const glm::vec3& at, uint32_t& waitStartMs) {
    // The client loads it and waits for every read (0x00781500, 0x004bae10);
    // here the ground streams around the camera, and the black holds until
    // the tile under it is in - or nothing is left to come, or too long.
    rendering::TerrainManager* terrain = renderer_.getTerrainManager();
    if (!terrain) return true;
    const uint32_t now = nowMs();
    if (waitStartMs == 0) waitStartMs = now;
    const uint32_t waited = now - waitStartMs;
    if (terrain->isTileLoadedAt(at.x, at.y) || waited >= kAreaWaitLimitMs ||
        (waited >= kAreaWaitStreamMs && terrain->getRemainingTileCount() == 0)) {
        waitStartMs = 0;
        return true;
    }
    return false;
}

bool CinematicPlayer::cameraAreaReady() { return areaReady(cameraPosition_, cameraWaitStartMs_); }

bool CinematicPlayer::playerAreaReady() {
    return areaReady(renderer_.getCharacterPosition(), playerWaitStartMs_);
}

void CinematicPlayer::releaseCamera() {
    // 0x005fe360: the world camera the player's again.
    model_.reset();
    playerWaitStartMs_ = 0;
    renderer_.setCinematicView(std::nullopt);
}

void CinematicPlayer::sendNextCamera() { gameHandler_.sendNextCinematicCamera(); }
void CinematicPlayer::sendComplete() { gameHandler_.sendCompleteCinematic(); }

}  // namespace wowee::core
