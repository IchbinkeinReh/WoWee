#include "rendering/m2_event_sounds.hpp"

#include "rendering/m2_renderer.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "addons/lua_api_registrations.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_distance.hpp"
#include "audio/sound_entry.hpp"

#include <algorithm>
#include <cctype>
#include <numeric>
#include <random>
#include <string>

namespace wowee::rendering {

// A SoundEntries row as 0x004c6a40 plays it (its kit, built from the row by
// 0x004cd4a0): 3..12 File, 13..22 Freq, 23 DirectoryBase, 24 VolumeFloat
// (+0x34), 25 Flags (+0x3c), 26 MinDistance (+0x2c), 27 DistanceCutoff
// (+0x30) - the last two the channel's 3D min and max (0x00879390). Null when it has no file this
// install can read. The Flags are kept whole, because the same routine that
// reads the reach reads the pitch and volume swings out of them
// (audio/sound_entry.hpp).
namespace {
std::mt19937& rng() {
    static std::mt19937 r{std::random_device{}()};
    return r;
}
/// A draw across [0,1] for a swing, as the client draws one per flag it swings
/// (0x004c5d40, between the pair of bounds the flag sets in front of it).
float unitDraw() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng()); }
}  // namespace

const M2EventSounds::Kit* M2EventSounds::kit(uint32_t soundId, pipeline::AssetManager* assets) {
    if (soundId == 0 || !assets) return nullptr;
    auto it = kits_.find(soundId);
    if (it == kits_.end()) {
        Kit loaded;
        auto dbc = assets->loadDBC("SoundEntries.dbc");
        const int32_t idx = dbc && dbc->isLoaded() ? dbc->findRecordById(soundId) : -1;
        if (idx >= 0 && dbc->getFieldCount() > 27) {
            const uint32_t row = static_cast<uint32_t>(idx);
            const std::string dir = dbc->getString(row, 23);
            for (uint32_t f = 0; f < 10; ++f) {
                const std::string name = dbc->getString(row, 3 + f);
                if (name.empty()) continue;
                auto data = assets->readFile(dir.empty() ? name : dir + "\\" + name);
                if (data.empty()) continue;
                loaded.files.push_back(std::move(data));
                loaded.frequency.push_back(dbc->getUInt32(row, 13 + f));
            }
            const float volume = dbc->getFloat(row, 24);
            if (volume > 0.0f) loaded.volume = volume;
            loaded.flags = dbc->getUInt32(row, 25);
            loaded.loops = (loaded.flags & audio::sound_entry::kFlagLoops) != 0;
            loaded.minDistance = dbc->getFloat(row, 26);
            loaded.cutoff = dbc->getFloat(row, 27);
        }
        // Remembered either way, so a row without a file is read once.
        it = kits_.emplace(soundId, std::move(loaded)).first;
    }
    return it->second.files.empty() ? nullptr : &it->second;
}

// One of the kit's files, by their Freq weights (0x004cbac0).
const std::vector<uint8_t>* M2EventSounds::pickFile(const Kit& k) {
    const uint64_t total = std::accumulate(k.frequency.begin(), k.frequency.end(), uint64_t{0});
    if (total == 0) return &k.files[std::uniform_int_distribution<size_t>(0, k.files.size() - 1)(rng())];
    uint64_t roll = std::uniform_int_distribution<uint64_t>(0, total - 1)(rng());
    for (size_t i = 0; i < k.files.size(); ++i) {
        if (roll < k.frequency[i]) return &k.files[i];
        roll -= k.frequency[i];
    }
    return &k.files.back();
}

// GameObjectDisplayInfo.dbc (3.3.5a): 0 ID, 2..11 Sound[0..9] (0x0070bf70
// reads the row's +8 onward).
uint32_t M2EventSounds::displaySound(uint32_t displayId, uint32_t slot, pipeline::AssetManager* assets) {
    if (!displaySoundsLoaded_ && assets) {
        displaySoundsLoaded_ = true;
        auto dbc = assets->loadDBC("GameObjectDisplayInfo.dbc");
        if (dbc && dbc->isLoaded() && dbc->getFieldCount() >= 12) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                std::array<uint32_t, 10> sounds{};
                bool any = false;
                for (uint32_t s = 0; s < sounds.size(); ++s) {
                    sounds[s] = dbc->getUInt32(i, 2 + s);
                    any = any || sounds[s] != 0;
                }
                if (any) displaySounds_[dbc->getUInt32(i, 0)] = sounds;
            }
        }
    }
    auto it = displaySounds_.find(displayId);
    return it != displaySounds_.end() && slot < it->second.size() ? it->second[slot] : 0u;
}

uint32_t M2EventSounds::idByName(const std::string& name, pipeline::AssetManager* assets) {
    if (!idsByNameLoaded_ && assets) {
        idsByNameLoaded_ = true;
        auto dbc = assets->loadDBC("SoundEntries.dbc");
        if (dbc && dbc->isLoaded() && dbc->getFieldCount() > 2) {
            for (uint32_t i = 0; i < dbc->getRecordCount(); ++i) {
                std::string n = dbc->getString(i, 2);
                std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return std::tolower(c); });
                idsByName_.emplace(std::move(n), dbc->getUInt32(i, 0));
            }
        }
    }
    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
    const auto it = idsByName_.find(key);
    return it != idsByName_.end() ? it->second : 0;
}

void M2EventSounds::playOnce(uint32_t soundId, const glm::vec3* at, float volumeScale,
                             pipeline::AssetManager* assets, audio::PlayLimit limit, bool absolute,
                             int priority, uint32_t* outHandle) {
    if (outHandle) *outHandle = 0;
    const Kit* k = kit(soundId, assets);
    if (!k) return;
    float minDistance = 0.0f, maxDistance = 0.0f;
    audio::sound_distance::entryDistance(k->minDistance, k->cutoff, minDistance, maxDistance);
    const std::vector<uint8_t>* file = pickFile(*k);
    auto& audio = audio::AudioEngine::instance();
    // An SFX play, as every one 0x004c5990 sets up (type 0).
    constexpr auto kSfx = audio::Channel::Effects;
    const float base = absolute ? volumeScale : k->volume * volumeScale;
    // An `absolute` play brings its own volume in the (0,1] range, and the client
    // draws no volume swing on that branch (004c0000.c:6005-6018); the pitch swing
    // it draws either way (:6020).
    const float volume = absolute ? base : audio::sound_entry::volumeForFlags(k->flags, base, unitDraw());
    const float pitch = audio::sound_entry::pitchForFlags(k->flags, unitDraw());
    if (!at) audio.playSound2D(*file, volume, pitch, kSfx, limit, priority, outHandle);
    else
        audio.playSound3D(*file, *at, volume, pitch, maxDistance, minDistance, kSfx, limit, priority, outHandle);
}

void M2EventSounds::update(M2Renderer& m2, pipeline::AssetManager* assets, const ShakeCamera& shakeCamera) {
    auto& audio = audio::AudioEngine::instance();
    for (const M2Renderer::FiredEvent& fired : m2.takeFiredEvents()) {
        if (m2_event::shakesCamera(fired.owner, fired.id)) {
            if (shakeCamera) shakeCamera(fired.data, fired.position);
            continue;
        }
        if (fired.owner == m2_event::Owner::FallingModel && !m2_event::fallingEventPlays(fallingEvents_)) continue;
        const m2_event::SoundPlan plan = m2_event::planSound(fired.owner, fired.id, fired.data);
        if (plan.play == m2_event::Play::None) continue;
        if (plan.play == m2_event::Play::StopLoop) {
            // 0x004c6390(obj, 0, -1): over the fade out the loop was started
            // with, at once where it had none (0x00879840).
            if (auto it = loops_.find(fired.instanceId); it != loops_.end()) {
                audio.stopSoundWithFade(it->second.handle, it->second.fadeOut);
                loops_.erase(it);
            }
            continue;
        }
        const uint32_t soundId = plan.slotted ? displaySound(fired.ownerArg, plan.sound, assets) : plan.sound;
        const Kit* k = kit(soundId, assets);
        if (!k) continue;
        float minDistance = 0.0f, maxDistance = 0.0f;
        audio::sound_distance::entryDistance(k->minDistance, k->cutoff, minDistance, maxDistance);
        // A spell effect on the unit the active mover is (0x006f7b00 asks
        // 0x004d43c0 as the event fires; the effect's owner arg is the unit's
        // instance): kept over a crowd's (0x6e), and with the listener at the
        // character (Sound_ListenerAtCharacter, default 1) at 0.65 of its
        // volume (0x009f2818) and where the listener is.
        const bool onMover = fired.owner == m2_event::Owner::Effect && fired.ownerArg != 0 &&
                             moverInstanceQuery_ && fired.ownerArg == moverInstanceQuery_();
        const int priority = onMover ? audio::voice_priority::kActiveMover : audio::voice_priority::kDefault;
        const bool atListener = onMover && addons::storedCVarValue("Sound_ListenerAtCharacter", "1") != "0";
        // And the swings the row asks of the play (audio/sound_entry.hpp). Every
        // one of these plays is on the client's kit-volume branch, the one it does
        // draw the volume swing on.
        const float base = k->volume * (atListener ? 0.65f : 1.0f);
        const float volume = audio::sound_entry::volumeForFlags(k->flags, base, unitDraw());
        const float pitch = audio::sound_entry::pitchForFlags(k->flags, unitDraw());

        bool loops = false;
        switch (plan.play) {
        case m2_event::Play::ObjectLoop: loops = true; break;
        case m2_event::Play::ObjectKit:
        case m2_event::Play::OnEffect: loops = k->loops; break;
        default: break;
        }
        // Every owner's play is an SFX one (0x004c5990 leaves the type 0,
        // "SFX"): heard at the Sound Effects volume, and not with
        // Sound_EnableSFX off (0x004c6a40).
        constexpr auto kSfx = audio::Channel::Effects;
        if (!loops) {
            const std::vector<uint8_t>* file = pickFile(*k);
            if (atListener) audio.playSound2D(*file, volume, pitch, kSfx, audio::PlayLimit::None, priority);
            else
                audio.playSound3D(*file, fired.position, volume, pitch, maxDistance, minDistance, kSfx,
                                  audio::PlayLimit::None, priority);
            continue;
        }
        // A loop on its owner: none while the owner already plays one
        // (0x004c5be0), and none for a game object or a doodad where the kit
        // already plays within sqrt(6) of the event (0x004cfe00).
        if (loops_.count(fired.instanceId)) continue;
        if (fired.owner == m2_event::Owner::GameObject || fired.owner == m2_event::Owner::Doodad) {
            bool loopNear = false;
            for (const auto& [id, loop] : loops_) {
                const glm::vec3 d = loop.position - fired.position;
                loopNear = loopNear || (loop.soundId == soundId && glm::dot(d, d) < 6.0f);
            }
            if (loopNear) continue;
        }
        const auto origin = m2.getInstancePosition(fired.instanceId);
        if (!origin) continue;
        // A loop carries its volume swing. Its pitch is not swung: a looping play
        // takes no pitch argument, and exactly one row of the 12941 asks for both
        // a pitch swing and a loop - 15582 Lightning_Fel_Precast_Low_Hand
        // (IronCouncil), Flags 0x600.
        const uint32_t handle = audio.playSound3DLooping(*pickFile(*k), fired.position, volume, maxDistance,
                                                         minDistance, plan.fadeIn, kSfx, priority);
        if (handle != 0)
            loops_[fired.instanceId] = Loop{.handle = handle, .soundId = soundId, .offset = fired.position - *origin,
                                            .position = fired.position, .fadeOut = plan.fadeOut};
    }

    // A loop goes with its owner, and ends when it does: the owner's sound
    // object let go fades a looping sound out over half a second (a doodad
    // unloaded, 0x007c2350 -> 0x004c7440 -> 0x008799e0).
    for (auto it = loops_.begin(); it != loops_.end();) {
        const auto origin = m2.getInstancePosition(it->first);
        if (!origin) {
            audio.stopSoundWithFade(it->second.handle, m2_event::kOwnerGoneFadeSeconds);
            it = loops_.erase(it);
            continue;
        }
        const glm::vec3 position = *origin + it->second.offset;
        if (position != it->second.position) {
            it->second.position = position;
            audio.setSoundPosition(it->second.handle, position);
        }
        ++it;
    }
}

void M2EventSounds::clear() {
    for (const auto& [id, loop] : loops_) audio::AudioEngine::instance().stopSound(loop.handle);
    loops_.clear();
}

}  // namespace wowee::rendering
