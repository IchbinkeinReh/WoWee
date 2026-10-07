#include "audio/spell_sound_manager.hpp"
#include "audio/sample_load.hpp"
#include "audio/audio_engine.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include <algorithm>

namespace wowee {
namespace audio {

bool SpellSoundManager::initialize(pipeline::AssetManager* assets) {
    if (!assets) {
        LOG_ERROR("SpellSoundManager: AssetManager is null");
        return false;
    }

    LOG_INFO("SpellSoundManager: Initializing...");

    // Load Fire precast sounds
    precastFireLowSounds_.resize(1);
    loadSound("Sound\\Spells\\PreCastFireLow.wav", precastFireLowSounds_[0], assets);

    precastFireMediumSounds_.resize(1);
    loadSound("Sound\\Spells\\PreCastFireMedium.wav", precastFireMediumSounds_[0], assets);

    precastFireHighSounds_.resize(1);
    loadSound("Sound\\Spells\\PreCastFireHigh.wav", precastFireHighSounds_[0], assets);

    // Load Frost precast sounds
    precastFrostLowSounds_.resize(1);
    loadSound("Sound\\Spells\\PreCastFrostMagicLow.wav", precastFrostLowSounds_[0], assets);

    precastFrostMediumSounds_.resize(1);
    loadSound("Sound\\Spells\\PreCastFrostMagicMedium.wav", precastFrostMediumSounds_[0], assets);

    precastFrostHighSounds_.resize(1);
    loadSound("Sound\\Spells\\PreCastFrostMagicHigh.wav", precastFrostHighSounds_[0], assets);

    // Load Holy precast sounds
    precastHolyLowSounds_.resize(1);
    loadSound("Sound\\Spells\\PreCastHolyMagicLow.wav", precastHolyLowSounds_[0], assets);

    precastHolyMediumSounds_.resize(1);
    loadSound("Sound\\Spells\\PreCastHolyMagicMedium.wav", precastHolyMediumSounds_[0], assets);

    precastHolyHighSounds_.resize(1);
    loadSound("Sound\\Spells\\PreCastHolyMagicHigh.wav", precastHolyHighSounds_[0], assets);

    // Load Nature precast sounds
    precastNatureLowSounds_.resize(1);
    loadSound("Sound\\Spells\\PreCastNatureMagicLow.wav", precastNatureLowSounds_[0], assets);

    precastNatureMediumSounds_.resize(1);
    loadSound("Sound\\Spells\\PreCastNatureMagicMedium.wav", precastNatureMediumSounds_[0], assets);

    precastNatureHighSounds_.resize(1);
    loadSound("Sound\\Spells\\PreCastNatureMagicHigh.wav", precastNatureHighSounds_[0], assets);

    // Load Shadow precast sounds
    precastShadowLowSounds_.resize(1);
    loadSound("Sound\\Spells\\PreCastShadowMagicLow.wav", precastShadowLowSounds_[0], assets);

    precastShadowMediumSounds_.resize(1);
    loadSound("Sound\\Spells\\PreCastShadowMagicMedium.wav", precastShadowMediumSounds_[0], assets);

    precastShadowHighSounds_.resize(1);
    loadSound("Sound\\Spells\\PreCastShadowMagicHigh.wav", precastShadowHighSounds_[0], assets);

    // Load Arcane precast sounds
    precastArcaneSounds_.resize(1);
    loadSound("Sound\\Spells\\Arcane_Form_Precast.wav", precastArcaneSounds_[0], assets);

    // Load Cast sounds (when spell fires)
    castFireSounds_.resize(1);
    loadSound("Sound\\Spells\\Cast\\FireCast.wav", castFireSounds_[0], assets);

    castFrostSounds_.resize(1);
    loadSound("Sound\\Spells\\Cast\\IceCast.wav", castFrostSounds_[0], assets);

    castHolySounds_.resize(1);
    loadSound("Sound\\Spells\\Cast\\HolyCast.wav", castHolySounds_[0], assets);

    castNatureSounds_.resize(1);
    loadSound("Sound\\Spells\\Cast\\NatureCast.wav", castNatureSounds_[0], assets);

    castShadowSounds_.resize(1);
    loadSound("Sound\\Spells\\Cast\\ShadowCast.wav", castShadowSounds_[0], assets);

    LOG_INFO("SpellSoundManager: Precast sounds - Fire: ", precastFireLowSounds_[0].loaded ? "YES" : "NO",
             ", Frost: ", precastFrostLowSounds_[0].loaded ? "YES" : "NO",
             ", Holy: ", precastHolyLowSounds_[0].loaded ? "YES" : "NO");
    LOG_INFO("SpellSoundManager: Cast sounds - Fire: ", castFireSounds_[0].loaded ? "YES" : "NO",
             ", Frost: ", castFrostSounds_[0].loaded ? "YES" : "NO",
             ", Shadow: ", castShadowSounds_[0].loaded ? "YES" : "NO");

    initialized_ = true;
    LOG_INFO("SpellSoundManager: Initialization complete");
    return true;
}

void SpellSoundManager::shutdown() {
    initialized_ = false;
}

bool SpellSoundManager::loadSound(const std::string& path, SpellSample& sample, pipeline::AssetManager* assets) {
    // Quietly: not every sound in this bank ships with every install.
    return loadSampleFile(path, sample, assets, nullptr);
}

void SpellSoundManager::playSound(const std::vector<SpellSample>& library, float volumeMultiplier) {
    if (!initialized_ || library.empty() || !library[0].loaded) return;

    float volume = 0.75f * volumeScale_ * volumeMultiplier;
    AudioEngine::instance().playSound2D(library[0].data, volume, 1.0f);
}

void SpellSoundManager::setVolumeScale(float scale) {
    volumeScale_ = std::clamp(scale, .0f, 1.f);
}

void SpellSoundManager::playPrecast(MagicSchool school, SpellPower power) {
    const std::vector<SpellSample>* library = nullptr;

    switch (school) {
        case MagicSchool::FIRE:
            library = (power == SpellPower::LOW) ? &precastFireLowSounds_ :
                     (power == SpellPower::MEDIUM) ? &precastFireMediumSounds_ :
                     &precastFireHighSounds_;
            break;
        case MagicSchool::FROST:
            library = (power == SpellPower::LOW) ? &precastFrostLowSounds_ :
                     (power == SpellPower::MEDIUM) ? &precastFrostMediumSounds_ :
                     &precastFrostHighSounds_;
            break;
        case MagicSchool::HOLY:
            library = (power == SpellPower::LOW) ? &precastHolyLowSounds_ :
                     (power == SpellPower::MEDIUM) ? &precastHolyMediumSounds_ :
                     &precastHolyHighSounds_;
            break;
        case MagicSchool::NATURE:
            library = (power == SpellPower::LOW) ? &precastNatureLowSounds_ :
                     (power == SpellPower::MEDIUM) ? &precastNatureMediumSounds_ :
                     &precastNatureHighSounds_;
            break;
        case MagicSchool::SHADOW:
            library = (power == SpellPower::LOW) ? &precastShadowLowSounds_ :
                     (power == SpellPower::MEDIUM) ? &precastShadowMediumSounds_ :
                     &precastShadowHighSounds_;
            break;
        case MagicSchool::ARCANE:
            library = &precastArcaneSounds_;
            break;
        default:
            return;
    }

    if (library && !library->empty() && (*library)[0].loaded) {
        stopPrecast();  // Stop any previous precast still playing
        float volume = 0.75f * volumeScale_;
        activePrecastId_ = AudioEngine::instance().playSound2DStoppable((*library)[0].data, volume);
    }
}

void SpellSoundManager::stopPrecast() {
    if (activePrecastId_ != 0) {
        AudioEngine::instance().stopSound(activePrecastId_);
        activePrecastId_ = 0;
    }
}

void SpellSoundManager::playCast(MagicSchool school) {
    stopPrecast();  // Ensure precast doesn't overlap the cast sound
    switch (school) {
        case MagicSchool::FIRE:
            playSound(castFireSounds_);
            break;
        case MagicSchool::FROST:
            playSound(castFrostSounds_);
            break;
        case MagicSchool::HOLY:
            playSound(castHolySounds_);
            break;
        case MagicSchool::NATURE:
            playSound(castNatureSounds_);
            break;
        case MagicSchool::SHADOW:
            playSound(castShadowSounds_);
            break;
        default:
            break;
    }
}

} // namespace audio
} // namespace wowee
