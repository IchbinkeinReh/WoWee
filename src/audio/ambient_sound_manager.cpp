#include "audio/ambient_sound_manager.hpp"
#include "audio/sample_load.hpp"
#include "audio/audio_engine.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include <random>
#include <algorithm>
#include <cmath>

namespace wowee {
namespace audio {

namespace {
    // Distance thresholds (in game units)
    constexpr float MAX_WATER_DISTANCE = 35.0f;
    constexpr float MAX_AMBIENT_DISTANCE = 50.0f;

    // Volume settings
    constexpr float WATER_VOLUME = 0.5f;
    constexpr float BIRD_VOLUME = 0.6f;
    constexpr float CRICKET_VOLUME = 0.5f;

    // Timing settings (seconds)
    constexpr float BIRD_MIN_INTERVAL = 8.0f;
    constexpr float BIRD_MAX_INTERVAL = 20.0f;
    constexpr float CRICKET_MIN_INTERVAL = 6.0f;
    constexpr float CRICKET_MAX_INTERVAL = 15.0f;

    std::random_device rd;
    std::mt19937 gen(rd());

    float randomFloat(float min, float max) {
        std::uniform_real_distribution<float> dist(min, max);
        return dist(gen);
    }
}

bool AmbientSoundManager::initialize(pipeline::AssetManager* assets) {
    if (!assets) {
        LOG_ERROR("AmbientSoundManager: AssetManager is null");
        return false;
    }

    LOG_INFO("AmbientSoundManager: Initializing...");

    // A doodad's own sounds - a campfire's, a fountain's, a waterfall's - are
    // its model's events (0x007bd5a0, M2EventSounds), not guessed here.

    // Load water sounds
    waterSounds_.resize(1);
    loadSound("Sound\\Ambience\\Water\\River_LakeStillA.wav", waterSounds_[0], assets);

    riverSounds_.resize(1);
    loadSound("Sound\\Ambience\\Water\\RiverSlowA.wav", riverSounds_[0], assets);

    // Load wind/ambience sounds
    windSounds_.resize(1);
    bool windLoaded = loadSound("Sound\\Ambience\\ZoneAmbience\\ForestNormalDay.wav", windSounds_[0], assets);

    tavernSounds_.resize(1);
    bool tavernLoaded = loadSound("Sound\\Ambience\\WMOAmbience\\Tavern.wav", tavernSounds_[0], assets);

    // Load blacksmith ambience loop
    blacksmithSounds_.resize(1);
    bool blacksmithLoaded = loadSound("Sound\\Ambience\\WMOAmbience\\BlackSmith.wav", blacksmithSounds_[0], assets);

    // Load bird chirp sounds (daytime periodic) - up to 6 variants
    {
        static constexpr const char* birdPaths[] = {
            "Sound\\Ambience\\BirdAmbience\\BirdChirp01.wav",
            "Sound\\Ambience\\BirdAmbience\\BirdChirp02.wav",
            "Sound\\Ambience\\BirdAmbience\\BirdChirp03.wav",
            "Sound\\Ambience\\BirdAmbience\\BirdChirp04.wav",
            "Sound\\Ambience\\BirdAmbience\\BirdChirp05.wav",
            "Sound\\Ambience\\BirdAmbience\\BirdChirp06.wav",
        };
        for (const char* p : birdPaths) {
            birdSounds_.emplace_back();
            if (!loadSound(p, birdSounds_.back(), assets)) birdSounds_.pop_back();
        }
    }

    // Load cricket/insect sounds (nighttime periodic)
    {
        static constexpr const char* cricketPaths[] = {
            "Sound\\Ambience\\Insect\\InsectMorning.wav",
            "Sound\\Ambience\\Insect\\InsectNight.wav",
        };
        for (const char* p : cricketPaths) {
            cricketSounds_.emplace_back();
            if (!loadSound(p, cricketSounds_.back(), assets)) cricketSounds_.pop_back();
        }
    }

    // Load weather sounds
    rainLightSounds_.resize(1);
    bool rainLightLoaded = loadSound("Sound\\Ambience\\Weather\\RainLight.wav", rainLightSounds_[0], assets);

    rainMediumSounds_.resize(1);
    bool rainMediumLoaded = loadSound("Sound\\Ambience\\Weather\\RainMedium.wav", rainMediumSounds_[0], assets);

    rainHeavySounds_.resize(1);
    bool rainHeavyLoaded = loadSound("Sound\\Ambience\\Weather\\RainHeavy.wav", rainHeavySounds_[0], assets);

    snowLightSounds_.resize(1);
    bool snowLightLoaded = loadSound("Sound\\Ambience\\Weather\\SnowLight.wav", snowLightSounds_[0], assets);

    snowMediumSounds_.resize(1);
    bool snowMediumLoaded = loadSound("Sound\\Ambience\\Weather\\SnowMedium.wav", snowMediumSounds_[0], assets);

    snowHeavySounds_.resize(1);
    bool snowHeavyLoaded = loadSound("Sound\\Ambience\\Weather\\SnowHeavy.wav", snowHeavySounds_[0], assets);

    // Load water ambience sounds

    underwaterSounds_.resize(1);
    bool underwaterLoaded = loadSound("Sound\\Ambience\\Water\\UnderwaterSwim.wav", underwaterSounds_[0], assets);

    // Load zone ambience sounds (day and night)
    forestNormalDaySounds_.resize(1);
    bool forestDayLoaded = loadSound("Sound\\Ambience\\ZoneAmbience\\ForestNormalDay.wav", forestNormalDaySounds_[0], assets);

    forestNormalNightSounds_.resize(1);
    bool forestNightLoaded = loadSound("Sound\\Ambience\\ZoneAmbience\\ForestNormalNight.wav", forestNormalNightSounds_[0], assets);

    forestSnowDaySounds_.resize(1);
    loadSound("Sound\\Ambience\\ZoneAmbience\\ForestSnowDay.wav", forestSnowDaySounds_[0], assets);

    forestSnowNightSounds_.resize(1);
    loadSound("Sound\\Ambience\\ZoneAmbience\\ForestSnowNight.wav", forestSnowNightSounds_[0], assets);

    beachDaySounds_.resize(1);
    bool beachDayLoaded = loadSound("Sound\\Ambience\\ZoneAmbience\\BeachDay.wav", beachDaySounds_[0], assets);

    beachNightSounds_.resize(1);
    bool beachNightLoaded = loadSound("Sound\\Ambience\\ZoneAmbience\\BeachNight.wav", beachNightSounds_[0], assets);

    grasslandsDaySounds_.resize(1);
    loadSound("Sound\\Ambience\\ZoneAmbience\\GrasslandsDay.wav", grasslandsDaySounds_[0], assets);

    grasslandsNightSounds_.resize(1);
    loadSound("Sound\\Ambience\\ZoneAmbience\\GrassLandsNight.wav", grasslandsNightSounds_[0], assets);

    jungleDaySounds_.resize(1);
    loadSound("Sound\\Ambience\\ZoneAmbience\\JungleDay.wav", jungleDaySounds_[0], assets);

    jungleNightSounds_.resize(1);
    loadSound("Sound\\Ambience\\ZoneAmbience\\JungleNight.wav", jungleNightSounds_[0], assets);

    marshDaySounds_.resize(1);
    loadSound("Sound\\Ambience\\ZoneAmbience\\MarshDay.wav", marshDaySounds_[0], assets);

    marshNightSounds_.resize(1);
    loadSound("Sound\\Ambience\\ZoneAmbience\\MarshNight.wav", marshNightSounds_[0], assets);

    desertCanyonDaySounds_.resize(1);
    bool desertCanyonDayLoaded = loadSound("Sound\\Ambience\\ZoneAmbience\\CanyonDesertDay.wav", desertCanyonDaySounds_[0], assets);

    desertCanyonNightSounds_.resize(1);
    loadSound("Sound\\Ambience\\ZoneAmbience\\CanyonDesertNight.wav", desertCanyonNightSounds_[0], assets);

    desertPlainsDaySounds_.resize(1);
    bool desertPlainsDayLoaded = loadSound("Sound\\Ambience\\ZoneAmbience\\PlainsDesertDay.wav", desertPlainsDaySounds_[0], assets);

    desertPlainsNightSounds_.resize(1);
    loadSound("Sound\\Ambience\\ZoneAmbience\\PlainsDesertNight.wav", desertPlainsNightSounds_[0], assets);

    // Load city ambience sounds (day and night where available)
    stormwindDaySounds_.resize(1);
    bool stormwindDayLoaded = loadSound("Sound\\Ambience\\WMOAmbience\\StormwindDay.wav", stormwindDaySounds_[0], assets);

    stormwindNightSounds_.resize(1);
    bool stormwindNightLoaded = loadSound("Sound\\Ambience\\WMOAmbience\\StormwindNight.wav", stormwindNightSounds_[0], assets);

    ironforgeSounds_.resize(1);
    bool ironforgeLoaded = loadSound("Sound\\Ambience\\WMOAmbience\\Ironforge.wav", ironforgeSounds_[0], assets);

    darnassusDaySounds_.resize(1);
    loadSound("Sound\\Ambience\\WMOAmbience\\DarnassusDay.wav", darnassusDaySounds_[0], assets);

    darnassusNightSounds_.resize(1);
    loadSound("Sound\\Ambience\\WMOAmbience\\DarnassusNight.wav", darnassusNightSounds_[0], assets);

    orgrimmarDaySounds_.resize(1);
    bool orgrimmarDayLoaded = loadSound("Sound\\Ambience\\WMOAmbience\\OrgrimmarDay.wav", orgrimmarDaySounds_[0], assets);

    orgrimmarNightSounds_.resize(1);
    bool orgrimmarNightLoaded = loadSound("Sound\\Ambience\\WMOAmbience\\OrgrimmarNight.wav", orgrimmarNightSounds_[0], assets);

    undercitySounds_.resize(1);
    loadSound("Sound\\Ambience\\WMOAmbience\\Undercity.wav", undercitySounds_[0], assets);

    thunderbluffDaySounds_.resize(1);
    loadSound("Sound\\Ambience\\WMOAmbience\\ThunderBluffDay.wav", thunderbluffDaySounds_[0], assets);

    thunderbluffNightSounds_.resize(1);
    loadSound("Sound\\Ambience\\WMOAmbience\\ThunderBluffNight.wav", thunderbluffNightSounds_[0], assets);

    LOG_INFO("AmbientSoundManager: Wind loaded: ", windLoaded ? "YES" : "NO",
             ", Tavern loaded: ", tavernLoaded ? "YES" : "NO",
             ", Blacksmith loaded: ", blacksmithLoaded ? "YES" : "NO");
    LOG_INFO("AmbientSoundManager: Weather sounds - Rain: ", (rainLightLoaded && rainMediumLoaded && rainHeavyLoaded) ? "YES" : "NO",
             ", Snow: ", (snowLightLoaded && snowMediumLoaded && snowHeavyLoaded) ? "YES" : "NO");
    LOG_INFO("AmbientSoundManager: Water sounds - Underwater: ",
             underwaterLoaded ? "YES" : "NO");
    LOG_INFO("AmbientSoundManager: Zone sounds - Forest: ", (forestDayLoaded && forestNightLoaded) ? "YES" : "NO",
             ", Beach: ", (beachDayLoaded && beachNightLoaded) ? "YES" : "NO",
             ", Desert: ", (desertCanyonDayLoaded && desertPlainsDayLoaded) ? "YES" : "NO");
    LOG_INFO("AmbientSoundManager: City sounds - Stormwind: ", (stormwindDayLoaded && stormwindNightLoaded) ? "YES" : "NO",
             ", Ironforge: ", ironforgeLoaded ? "YES" : "NO",
             ", Orgrimmar: ", (orgrimmarDayLoaded && orgrimmarNightLoaded) ? "YES" : "NO");

    // Initialize timers with random offsets
    birdTimer_ = randomFloat(0.0f, 5.0f);
    cricketTimer_ = randomFloat(0.0f, 5.0f);

    initialized_ = true;
    LOG_INFO("AmbientSoundManager: Initialization complete");
    return true;
}

void AmbientSoundManager::shutdown() {
    stopAll();
    emitters_.clear();
    activeSounds_.clear();
    initialized_ = false;
}

void AmbientSoundManager::playAmbienceLoop(const std::vector<uint8_t>& data, float volume) {
    loops_.add(AudioEngine::instance().playSound2DStoppable(data, volume));
}

void AmbientSoundManager::stopAll() {
    // 0x004c85f0 -> 0x004c6450(1): the ambience's channels, at once.
    for (uint32_t handle : loops_.takeAll()) AudioEngine::instance().stopSound(handle);
    // And the place it was for, so the next world starts its own.
    emitters_.clear();
    activeSounds_.clear();
    currentWeather_ = WeatherType::NONE;
    currentZoneId_ = 0;
    currentZone_ = ZoneType::NONE;
    currentCity_ = CityType::NONE;
    ambienceOverridden_ = false;
    wasIndoor_ = wasBlacksmith_ = wasSwimming_ = false;
    birdTimer_ = cricketTimer_ = 0.0f;
    windLoopTime_ = blacksmithLoopTime_ = weatherLoopTime_ = oceanLoopTime_ = 0.0f;
    zoneLoopTime_ = cityLoopTime_ = 0.0f;
}

bool AmbientSoundManager::loadSound(const std::string& path, AmbientSample& sample, pipeline::AssetManager* assets) {
    return loadSampleFile(path, sample, assets, "AmbientSoundManager");
}

void AmbientSoundManager::update(float deltaTime, const glm::vec3& cameraPos, bool isIndoor, bool isSwimming, bool isBlacksmith) {
    if (!initialized_) return;

    // Update all emitter systems
    updatePositionalEmitters(deltaTime, cameraPos);

    // Don't play outdoor periodic sounds (birds) when indoors OR in blacksmith
    if (!isIndoor && !isBlacksmith) {
        updatePeriodicSounds(deltaTime, isIndoor, isSwimming);
    }

    // Handle state changes
    if (wasBlacksmith_ && !isBlacksmith) {
        LOG_INFO("Ambient: EXITED BLACKSMITH");
        blacksmithLoopTime_ = 0.0f;  // Reset timer when leaving
    }

    // Blacksmith takes priority over tavern
    if (isBlacksmith) {
        updateBlacksmithAmbience(deltaTime);
    } else {
        updateWindAmbience(deltaTime, isIndoor);
    }

    // Update weather, water, zone, and city ambience
    updateWeatherAmbience(deltaTime, isIndoor);
    updateWaterAmbience(deltaTime, isSwimming);
    updateZoneAmbience(deltaTime, isIndoor);
    updateCityAmbience(deltaTime);

    // Track indoor state changes
    wasIndoor_ = isIndoor;
    wasBlacksmith_ = isBlacksmith;
}

void AmbientSoundManager::updatePositionalEmitters(float deltaTime, const glm::vec3& cameraPos) {
    // First pass: mark emitters as active/inactive based on distance
    int activeWaterCount = 0;
    const int MAX_ACTIVE_WATER = 3;     // Max 3 water sounds at once

    for (auto& emitter : emitters_) {
        const glm::vec3 delta = emitter.position - cameraPos;
        const float distSq = glm::dot(delta, delta);

        // Determine max distance based on type
        float maxDist = MAX_AMBIENT_DISTANCE;
        bool isWater = false;

        if (emitter.type == AmbientType::WATER_SURFACE ||
            emitter.type == AmbientType::RIVER) {
            maxDist = MAX_WATER_DISTANCE;
            isWater = true;
        }

        // Update active state based on distance AND limits
        const float maxDistSq = maxDist * maxDist;
        const bool withinRange = (distSq < maxDistSq);

        if (isWater && withinRange && activeWaterCount < MAX_ACTIVE_WATER) {
            emitter.active = true;
            activeWaterCount++;
        } else if (!isWater && withinRange) {
            emitter.active = true;
        } else {
            emitter.active = false;
        }

        if (!emitter.active) continue;

        // Update play timer
        emitter.lastPlayTime += deltaTime;

        // We only need the true distance for volume attenuation once the emitter is active.
        const float distance = std::sqrt(distSq);

        // Handle different emitter types
        switch (emitter.type) {
            case AmbientType::WATER_SURFACE:
                if (emitter.lastPlayTime >= 5.0f && !waterSounds_.empty() && waterSounds_[0].loaded) {
                    float volume = WATER_VOLUME * volumeScale_ * (1.0f - (distance / maxDist));
                    AudioEngine::instance().playSound3D(waterSounds_[0].data, emitter.position, volume);
                    emitter.lastPlayTime = 0.0f;
                }
                break;

            case AmbientType::RIVER:
                if (emitter.lastPlayTime >= 5.0f && !riverSounds_.empty() && riverSounds_[0].loaded) {
                    float volume = WATER_VOLUME * volumeScale_ * (1.0f - (distance / maxDist));
                    AudioEngine::instance().playSound3D(riverSounds_[0].data, emitter.position, volume);
                    emitter.lastPlayTime = 0.0f;
                }
                break;

            default:
                break;
        }
    }
}

void AmbientSoundManager::updatePeriodicSounds(float deltaTime, bool isIndoor, bool isSwimming) {
    // Only play outdoor periodic sounds when outdoors and not swimming/underwater
    if (isIndoor || isSwimming) return;

    // Bird sounds during daytime
    if (isDaytime() && currentZoneId_ != 10) {
        birdTimer_ += deltaTime;
        if (birdTimer_ >= randomFloat(BIRD_MIN_INTERVAL, BIRD_MAX_INTERVAL)) {
            birdTimer_ = 0.0f;
            if (!birdSounds_.empty()) {
                std::uniform_int_distribution<size_t> pick(0, birdSounds_.size() - 1);
                const auto& snd = birdSounds_[pick(gen)];
                if (snd.loaded)
                    AudioEngine::instance().playSound2D(snd.data, BIRD_VOLUME, 1.0f);
            }
        }
    }

    // Cricket sounds during nighttime
    if (isNighttime()) {
        cricketTimer_ += deltaTime;
        if (cricketTimer_ >= randomFloat(CRICKET_MIN_INTERVAL, CRICKET_MAX_INTERVAL)) {
            cricketTimer_ = 0.0f;
            if (!cricketSounds_.empty()) {
                std::uniform_int_distribution<size_t> pick(0, cricketSounds_.size() - 1);
                const auto& snd = cricketSounds_[pick(gen)];
                if (snd.loaded)
                    AudioEngine::instance().playSound2D(snd.data, CRICKET_VOLUME, 1.0f);
            }
        }
    }
}

void AmbientSoundManager::updateBlacksmithAmbience(float deltaTime) {
    bool stateChanged = !wasBlacksmith_;

    if (stateChanged) {
        LOG_INFO("Ambient: ENTERED BLACKSMITH");
        blacksmithLoopTime_ = 1.5f;  // Play first hammer soon
    }

    // Only play if we have loaded sounds
    bool hasSound = false;
    for (const auto& sound : blacksmithSounds_) {
        if (sound.loaded) {
            hasSound = true;
            break;
        }
    }

    if (hasSound && blacksmithSounds_[0].loaded) {
        blacksmithLoopTime_ += deltaTime;
        // Play blacksmith ambience loop every 15 seconds
        if (blacksmithLoopTime_ >= 15.0f) {
            float volume = 0.6f * volumeScale_;  // Ambient loop volume
            playAmbienceLoop(blacksmithSounds_[0].data, volume);
            LOG_INFO("Playing blacksmith ambience loop");
            blacksmithLoopTime_ = 0.0f;
        }
    }
}

void AmbientSoundManager::updateWindAmbience(float deltaTime, bool isIndoor) {
    // Always track indoor state for next frame
    bool stateChanged = (wasIndoor_ != isIndoor);

    if (stateChanged) {
        LOG_INFO("Ambient: ", isIndoor ? "ENTERED BUILDING" : "EXITED TO OUTDOORS");
        // Start timer at 10 seconds so ambience plays after ~5 seconds
        if (isIndoor) {
            windLoopTime_ = 10.0f;  // Play tavern ambience soon
        } else {
            windLoopTime_ = 25.0f;  // Play outdoor ambience soon
        }
    }

    wasIndoor_ = isIndoor;

    // Indoor ambience (tavern sounds) - glass clinking, chatter
    if (isIndoor) {
        if (!tavernSounds_.empty() && tavernSounds_[0].loaded) {
            windLoopTime_ += deltaTime;
            // Play every 15 seconds for ambient atmosphere
            if (windLoopTime_ >= 15.0f) {
                float volume = 0.5f * volumeScale_;
                playAmbienceLoop(tavernSounds_[0].data, volume);
                LOG_INFO("Playing tavern ambience (glasses clinking)");
                windLoopTime_ = 0.0f;
            }
        }
    }
    // Outdoor wind ambience
    else {
        // This generic loop is ForestNormalDay.wav and contains prominent
        // birdsong. Duskwood already has its authored night forest ambience.
        if (currentZoneId_ == 10) {
            windLoopTime_ = 0.0f;
            return;
        }
        if (!windSounds_.empty() && windSounds_[0].loaded) {
            windLoopTime_ += deltaTime;
            if (windLoopTime_ >= 30.0f) {
                float volume = 0.3f * volumeScale_;
                playAmbienceLoop(windSounds_[0].data, volume);
                LOG_INFO("Playing outdoor ambience");
                windLoopTime_ = 0.0f;
            }
        }
    }
}

uint64_t AmbientSoundManager::addEmitter(const glm::vec3& position, AmbientType type) {
    AmbientEmitter emitter;
    emitter.id = nextEmitterId_++;
    emitter.type = type;
    emitter.position = position;
    emitter.active = false;
    emitter.lastPlayTime = randomFloat(0.0f, 2.0f);  // Random initial offset
    emitter.loopInterval = 5.0f;

    emitters_.push_back(emitter);
    return emitter.id;
}
void AmbientSoundManager::setGameTime(float hours) {
    gameTimeHours_ = std::fmod(hours, 24.0f);
    if (gameTimeHours_ < 0.0f) gameTimeHours_ += 24.0f;
}

void AmbientSoundManager::setVolumeScale(float scale) {
    volumeScale_ = std::max(0.0f, std::min(1.0f, scale));
}

void AmbientSoundManager::setWeather(WeatherType type) {
    if (currentWeather_ != type) {
        LOG_INFO("AmbientSoundManager: Weather changed from ", static_cast<int>(currentWeather_),
                 " to ", static_cast<int>(type));
        currentWeather_ = type;
        weatherLoopTime_ = 0.0f;  // Reset timer on weather change
    }
}

void AmbientSoundManager::setZoneType(ZoneType type) {
    if (currentZone_ != type) {
        LOG_INFO("AmbientSoundManager: Zone changed from ", static_cast<int>(currentZone_),
                 " to ", static_cast<int>(type));
        currentZone_ = type;
        zoneLoopTime_ = 15.0f;  // Play zone ambience soon after entering
    }
}

void AmbientSoundManager::setZoneId(uint32_t zoneId) {
    currentZoneId_ = zoneId;

    // Map WoW zone ID to ZoneType + CityType.
    // City zones: set CityType and clear ZoneType.
    // Outdoor zones: set ZoneType and clear CityType.
    CityType city = CityType::NONE;
    ZoneType zone = ZoneType::NONE;

    switch (zoneId) {
        // ---- Major cities ----
        case 1519: city = CityType::STORMWIND;    break;
        case 1537: city = CityType::IRONFORGE;    break;
        case 1657: city = CityType::DARNASSUS;    break;
        case 1637: city = CityType::ORGRIMMAR;    break;
        case 1497: city = CityType::UNDERCITY;    break;
        case 1638: city = CityType::THUNDERBLUFF; break;

        // ---- Forest / snowy forest ----
        case 12:   // Elwynn Forest
        case 141:  // Teldrassil
        case 148:  // Darkshore
        case 493:  // Moonglade
        case 361:  // Felwood
        case 331:  // Ashenvale
        case 394:  // Grizzly Hills
        case 357:  // Feralas
        case 15:   // Dustwallow Marsh (lush)
        case 267:  // Hillsbrad Foothills
        case 36:   // Alterac Mountains
        case 45:   // Arathi Highlands
        case 10:   // Duskwood (forced to the night library by its visual clock)
            zone = ZoneType::FOREST_NORMAL; break;

        case 1:    // Dun Morogh
        case 618:  // Winterspring
        case 3:    // Badlands (actually dry but close enough)
        case 2817: // Crystalsong Forest
        case 66:   // Zul'Drak
        case 67:   // The Storm Peaks
        case 210:  // Icecrown
        case 65:   // Dragonblight
        case 495:  // Howling Fjord
        case 3537: // Borean Tundra
            zone = ZoneType::FOREST_SNOW; break;

        // ---- Grasslands / plains ----
        case 40:   // Westfall
        case 215:  // Mulgore
        case 44:   // Redridge Mountains
        case 38:   // Loch Modan
            zone = ZoneType::GRASSLANDS; break;

        // ---- Desert ----
        case 17:   // The Barrens
        case 14:   // Durotar
        case 440:  // Tanaris
        case 400:  // Thousand Needles
            zone = ZoneType::DESERT_PLAINS; break;

        case 46:   // Burning Steppes
        case 51:   // Searing Gorge
        case 4:    // Blasted Lands
        case 28:   // Western Plaguelands
            zone = ZoneType::DESERT_CANYON; break;

        // ---- Jungle ----
        case 33:   // Stranglethorn Vale
        case 490:  // Un'Goro Crater
        case 1377: // Silithus (arid but closest)
            zone = ZoneType::JUNGLE; break;

        // ---- Marsh / swamp ----
        case 8:    // Swamp of Sorrows
        case 11:   // Wetlands
        case 139:  // Eastern Plaguelands
        case 3521: // Zangarmarsh
            zone = ZoneType::MARSH; break;

        // ---- Beach / coast ----
        //
        // Every id here is AreaTable's, checked against the file rather than
        // remembered. Eleven of them were not: 4 was labelled a Barrens coast
        // and is the Blasted Lands, which is how a blighted red waste came to
        // have seabirds and surf over it; 210 was labelled Uldaman and is
        // Icecrown; four Northrend ids each named the zone belonging to
        // another; 78 and 763 name nothing at all.
        case 3524: // Azuremyst Isle
        case 3525: // Bloodmyst Isle
            zone = ZoneType::BEACH; break;

        default: break;
    }

    setCityType(city);
    setZoneType(zone);
}

void AmbientSoundManager::setCityType(CityType type) {
    if (currentCity_ != type) {
        LOG_INFO("AmbientSoundManager: City changed from ", static_cast<int>(currentCity_),
                 " to ", static_cast<int>(type));
        currentCity_ = type;
        cityLoopTime_ = 12.0f;  // Play city ambience soon after entering
    }
}

void AmbientSoundManager::updateWeatherAmbience(float deltaTime, bool isIndoor) {
    // Don't play weather sounds when indoors
    if (isIndoor || currentWeather_ == WeatherType::NONE) return;

    weatherLoopTime_ += deltaTime;

    // Select appropriate sound library based on weather type
    const std::vector<AmbientSample>* weatherLibrary = nullptr;
    float loopInterval = 20.0f;  // Default 20 second loop for weather

    switch (currentWeather_) {
        case WeatherType::RAIN_LIGHT:
            weatherLibrary = &rainLightSounds_;
            loopInterval = 25.0f;
            break;
        case WeatherType::RAIN_MEDIUM:
            weatherLibrary = &rainMediumSounds_;
            loopInterval = 20.0f;
            break;
        case WeatherType::RAIN_HEAVY:
            weatherLibrary = &rainHeavySounds_;
            loopInterval = 18.0f;
            break;
        case WeatherType::SNOW_LIGHT:
            weatherLibrary = &snowLightSounds_;
            loopInterval = 30.0f;
            break;
        case WeatherType::SNOW_MEDIUM:
            weatherLibrary = &snowMediumSounds_;
            loopInterval = 25.0f;
            break;
        case WeatherType::SNOW_HEAVY:
            weatherLibrary = &snowHeavySounds_;
            loopInterval = 22.0f;
            break;
        default:
            return;
    }

    // Play weather sound if library is loaded and timer expired
    if (weatherLibrary && !weatherLibrary->empty() && (*weatherLibrary)[0].loaded) {
        if (weatherLoopTime_ >= loopInterval) {
            float volume = 0.4f * volumeScale_;  // Weather ambience at moderate volume
            playAmbienceLoop((*weatherLibrary)[0].data, volume);
            LOG_INFO("Playing weather ambience: type ", static_cast<int>(currentWeather_));
            weatherLoopTime_ = 0.0f;
        }
    }
}

void AmbientSoundManager::updateWaterAmbience(float deltaTime, bool isSwimming) {
    bool stateChanged = (wasSwimming_ != isSwimming);

    if (stateChanged) {
        LOG_INFO("Ambient: ", isSwimming ? "ENTERED WATER" : "EXITED WATER");
        oceanLoopTime_ = 0.0f;  // Reset timer on state change
    }

    wasSwimming_ = isSwimming;

    // Play underwater sounds when swimming
    if (isSwimming) {
        if (!underwaterSounds_.empty() && underwaterSounds_[0].loaded) {
            oceanLoopTime_ += deltaTime;
            // Play every 18 seconds for underwater ambience
            if (oceanLoopTime_ >= 18.0f) {
                float volume = 0.5f * volumeScale_;
                playAmbienceLoop(underwaterSounds_[0].data, volume);
                LOG_INFO("Playing underwater ambience");
                oceanLoopTime_ = 0.0f;
            }
        }
    }
    // Ocean ambience for standing beside water, rather than in it, is not
    // played and its sample is no longer loaded for it.
    //
    // It wants a distance to the nearest water surface, which nothing here
    // measures: isSwimming says in or out and nothing about how far away it is
    // when out. Being near the sea is already covered approximately by the
    // BEACH zone ambience above, which is why this was never missed. Whoever
    // adds the proximity query can load Sound\\Ambience\\Water\\OceanDeepDay.wav
    // here again - keeping it in memory until then bought nothing.
}

void AmbientSoundManager::updateZoneAmbience(float deltaTime, bool isIndoor) {
    // Don't play zone ambience when indoors or in cities
    if (isIndoor || currentZone_ == ZoneType::NONE || currentCity_ != CityType::NONE || ambienceOverridden_) return;

    zoneLoopTime_ += deltaTime;

    // Select appropriate sound library based on zone type and time of day
    const std::vector<AmbientSample>* zoneLibrary = nullptr;
    bool isDay = isDaytime();

    switch (currentZone_) {
        case ZoneType::FOREST_NORMAL:
            zoneLibrary = isDay ? &forestNormalDaySounds_ : &forestNormalNightSounds_;
            break;
        case ZoneType::FOREST_SNOW:
            zoneLibrary = isDay ? &forestSnowDaySounds_ : &forestSnowNightSounds_;
            break;
        case ZoneType::BEACH:
            zoneLibrary = isDay ? &beachDaySounds_ : &beachNightSounds_;
            break;
        case ZoneType::GRASSLANDS:
            zoneLibrary = isDay ? &grasslandsDaySounds_ : &grasslandsNightSounds_;
            break;
        case ZoneType::JUNGLE:
            zoneLibrary = isDay ? &jungleDaySounds_ : &jungleNightSounds_;
            break;
        case ZoneType::MARSH:
            zoneLibrary = isDay ? &marshDaySounds_ : &marshNightSounds_;
            break;
        case ZoneType::DESERT_CANYON:
            zoneLibrary = isDay ? &desertCanyonDaySounds_ : &desertCanyonNightSounds_;
            break;
        case ZoneType::DESERT_PLAINS:
            zoneLibrary = isDay ? &desertPlainsDaySounds_ : &desertPlainsNightSounds_;
            break;
        default:
            return;
    }

    // Play zone ambience sound if library is loaded and timer expired
    if (zoneLibrary && !zoneLibrary->empty() && (*zoneLibrary)[0].loaded) {
        // Play every 30 seconds for zone ambience (longer intervals for background atmosphere)
        if (zoneLoopTime_ >= 30.0f) {
            float volume = 0.35f * volumeScale_;  // Zone ambience at moderate-low volume
            playAmbienceLoop((*zoneLibrary)[0].data, volume);
            LOG_INFO("Playing zone ambience: type ", static_cast<int>(currentZone_),
                     " (", isDay ? "day" : "night", ")");
            zoneLoopTime_ = 0.0f;
        }
    }
}

void AmbientSoundManager::updateCityAmbience(float deltaTime) {
    // Only play city ambience when actually in a city
    if (currentCity_ == CityType::NONE || ambienceOverridden_) return;

    cityLoopTime_ += deltaTime;

    // Select appropriate sound library based on city type and time of day
    const std::vector<AmbientSample>* cityLibrary = nullptr;
    bool isDay = isDaytime();

    switch (currentCity_) {
        case CityType::STORMWIND:
            cityLibrary = isDay ? &stormwindDaySounds_ : &stormwindNightSounds_;
            break;
        case CityType::IRONFORGE:
            cityLibrary = &ironforgeSounds_;  // No day/night (underground)
            break;
        case CityType::DARNASSUS:
            cityLibrary = isDay ? &darnassusDaySounds_ : &darnassusNightSounds_;
            break;
        case CityType::ORGRIMMAR:
            cityLibrary = isDay ? &orgrimmarDaySounds_ : &orgrimmarNightSounds_;
            break;
        case CityType::UNDERCITY:
            cityLibrary = &undercitySounds_;  // No day/night (underground)
            break;
        case CityType::THUNDERBLUFF:
            cityLibrary = isDay ? &thunderbluffDaySounds_ : &thunderbluffNightSounds_;
            break;
        default:
            return;
    }

    // Play city ambience sound if library is loaded and timer expired
    if (cityLibrary && !cityLibrary->empty() && (*cityLibrary)[0].loaded) {
        // Play every 20 seconds for city ambience (moderate intervals for urban atmosphere)
        if (cityLoopTime_ >= 20.0f) {
            float volume = 0.4f * volumeScale_;  // City ambience at moderate volume
            playAmbienceLoop((*cityLibrary)[0].data, volume);
            LOG_INFO("Playing city ambience: type ", static_cast<int>(currentCity_),
                     " (", isDay ? "day" : "night", ")");
            cityLoopTime_ = 0.0f;
        }
    }
}

} // namespace audio
} // namespace wowee
