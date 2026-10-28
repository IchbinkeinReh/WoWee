#include "audio/ui_sound_manager.hpp"
#include "audio/sample_load.hpp"
#include "audio/audio_engine.hpp"
#include "audio/sound_entry.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include <random>
#include <unordered_set>

namespace wowee {
namespace audio {

namespace {

// One draw per play, the way the client draws its swing (0x004c5d40).
float swingDraw() {
    static std::mt19937 rng{std::random_device{}()};
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
}

// The last element of a sound path, lower-cased. SoundEntries' File columns
// give a leaf ("LootCoinSmall.wav") and no folder, so this is what a play
// that knows only a path can look a row up by.
std::string leafLower(const std::string& path) {
    const auto pos = path.find_last_of("\\/");
    std::string leaf = (pos == std::string::npos) ? path : path.substr(pos + 1);
    for (char& ch : leaf) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return leaf;
}

} // namespace

bool UiSoundManager::initialize(pipeline::AssetManager* assets) {
    if (!assets) {
        LOG_ERROR("UISoundManager: AssetManager is null");
        return false;
    }

    assets_ = assets;
    LOG_INFO("UISoundManager: Initializing...");

    // Load window sounds
    //
    // The bag is opened and closed by iEquipmentContainerOpenA / CloseA. That
    // is what both rows for the action name - INTERFACESOUND_BACKPACKOPEN (685)
    // and _CLOSE (686), and igBackPackOpen (862) / igBackPackClose (863) - and
    // the iBackPackOpen / iBackPackClose names this asked for first are files no
    // row of the table mentions and no archive of this install contains, so
    // opening the bag was silent.
    bagOpenSounds_.resize(1);
    bool bagOpenLoaded = loadSound("Sound\\Interface\\iEquipmentContainerOpenA.wav", bagOpenSounds_[0], assets);

    bagCloseSounds_.resize(1);
    bool bagCloseLoaded = loadSound("Sound\\Interface\\iEquipmentContainerCloseA.wav", bagCloseSounds_[0], assets);

    questLogOpenSounds_.resize(1);
    bool questLogOpenLoaded = loadSound("Sound\\Interface\\iQuestLogOpenA.wav", questLogOpenSounds_[0], assets);

    questLogCloseSounds_.resize(1);
    bool questLogCloseLoaded = loadSound("Sound\\Interface\\iQuestLogCloseA.wav", questLogCloseSounds_[0], assets);

    characterSheetOpenSounds_.resize(1);
    bool charSheetOpenLoaded = loadSound("Sound\\Interface\\iAbilitiesOpenA.wav", characterSheetOpenSounds_[0], assets);

    characterSheetCloseSounds_.resize(1);
    bool charSheetCloseLoaded = loadSound("Sound\\Interface\\iAbilitiesCloseA.wav", characterSheetCloseSounds_[0], assets);

    auctionOpenSounds_.resize(1);
    loadSound("Sound\\Interface\\AuctionWindowOpen.wav", auctionOpenSounds_[0], assets);

    auctionCloseSounds_.resize(1);
    loadSound("Sound\\Interface\\AuctionWindowClose.wav", auctionCloseSounds_[0], assets);

    guildBankOpenSounds_.resize(1);
    loadSound("Sound\\Interface\\GuildVaultOpen.wav", guildBankOpenSounds_[0], assets);

    guildBankCloseSounds_.resize(1);
    loadSound("Sound\\Interface\\GuildVaultClose.wav", guildBankCloseSounds_[0], assets);

    // Load button sounds
    buttonClickSounds_.resize(1);
    bool buttonClickLoaded = loadSound("Sound\\Interface\\iUiInterfaceButtonA.wav", buttonClickSounds_[0], assets);

    menuButtonSounds_.resize(1);
    bool menuButtonLoaded = loadSound("Sound\\Interface\\iUIMainMenuButtonA.wav", menuButtonSounds_[0], assets);

    // Load quest sounds
    questActivateSounds_.resize(1);
    bool questActivateLoaded = loadSound("Sound\\Interface\\iQuestActivate.wav", questActivateSounds_[0], assets);

    questCompleteSounds_.resize(1);
    bool questCompleteLoaded = loadSound("Sound\\Interface\\iQuestComplete.wav", questCompleteSounds_[0], assets);

    questFailedSounds_.resize(1);
    bool questFailedLoaded = loadSound("Sound\\Interface\\igQuestFailed.wav", questFailedSounds_[0], assets);

    questUpdateSounds_.resize(1);
    loadSound("Sound\\Interface\\iQuestUpdate.wav", questUpdateSounds_[0], assets);

    // Load loot sounds
    lootCoinSmallSounds_.resize(1);
    bool lootCoinSmallLoaded = loadSound("Sound\\Interface\\LootCoinSmall.wav", lootCoinSmallSounds_[0], assets);

    lootCoinLargeSounds_.resize(1);
    bool lootCoinLargeLoaded = loadSound("Sound\\Interface\\LootCoinLarge.wav", lootCoinLargeSounds_[0], assets);

    lootItemSounds_.resize(1);
    bool lootItemLoaded = loadSound("Sound\\Interface\\igLootCreature.wav", lootItemSounds_[0], assets);

    // Load item pickup sounds
    dropSounds_.resize(1);
    bool dropLoaded = loadSound("Sound\\Interface\\DropOnGround.wav", dropSounds_[0], assets);

    pickupBagSounds_.resize(1);
    bool pickupBagLoaded = loadSound("Sound\\Interface\\PickUp\\PickUpBag.wav", pickupBagSounds_[0], assets);

    pickupBookSounds_.resize(1);
    bool pickupBookLoaded = loadSound("Sound\\Interface\\PickUp\\PickUpBook.wav", pickupBookSounds_[0], assets);




    // Load eating/drinking sounds
    eatingSounds_.resize(1);
    bool eatingLoaded = loadSound("Sound\\Interface\\iEating1.wav", eatingSounds_[0], assets);

    drinkingSounds_.resize(1);
    bool drinkingLoaded = loadSound("Sound\\Interface\\iDrinking1.wav", drinkingSounds_[0], assets);

    // Load level up sound
    levelUpSounds_.resize(1);
    bool levelUpLoaded = loadSound("Sound\\Interface\\LevelUp.wav", levelUpSounds_[0], assets);

    // Load achievement sound. The row the interface names AchievementSound
    // (12891) keeps its file in Sound\Spells and misspells it - Achievment -
    // and asking for the correctly spelled name asks for a file that is
    // nowhere: every achievement alert was the level-up sound.
    achievementSounds_.resize(1);
    if (!loadSound("Sound\\Spells\\AchievmentSound1.wav", achievementSounds_[0], assets)) {
        // Fallback to level-up sound if achievement sound is missing
        achievementSounds_ = levelUpSounds_;
    }

    // Load error/feedback sounds
    errorSounds_.resize(1);
    loadSound("Sound\\Interface\\Error.wav", errorSounds_[0], assets);

    selectTargetSounds_.resize(1);
    loadSound("Sound\\Interface\\iSelectTarget.wav", selectTargetSounds_[0], assets);

    deselectTargetSounds_.resize(1);
    loadSound("Sound\\Interface\\iDeselectTarget.wav", deselectTargetSounds_[0], assets);

    // Whisper notification. The table has no Whisper_Tell* row and no such file
    // in this install; the cue it does have for a whisper arriving is row 3081
    // TellMessage, whose file is Sound\Interface\iTellMessage.wav - the same
    // cue the mail notification below plays. Falls back to iSelectTarget only
    // if that file is missing.
    whisperSounds_.resize(1);
    if (!loadSound("Sound\\Interface\\iTellMessage.wav", whisperSounds_[0], assets)) {
        whisperSounds_ = selectTargetSounds_;
    }

    // New-mail notification (dedicated cue, falling back to the whisper sound)
    mailSounds_.resize(1);
    if (!loadSound("Sound\\Interface\\iTellMessage.wav", mailSounds_[0], assets)) {
        mailSounds_ = whisperSounds_;
    }

    // Minimap ping sound
    minimapPingSounds_.resize(1);
    if (!loadSound("Sound\\Interface\\MapPing.wav", minimapPingSounds_[0], assets)) {
        minimapPingSounds_ = selectTargetSounds_;  // fallback to target select sound
    }

    LOG_INFO("UISoundManager: Window sounds - Bag: ", (bagOpenLoaded && bagCloseLoaded) ? "YES" : "NO",
             ", QuestLog: ", (questLogOpenLoaded && questLogCloseLoaded) ? "YES" : "NO",
             ", CharSheet: ", (charSheetOpenLoaded && charSheetCloseLoaded) ? "YES" : "NO");
    LOG_INFO("UISoundManager: Button sounds - Click: ", buttonClickLoaded ? "YES" : "NO",
             ", Menu: ", menuButtonLoaded ? "YES" : "NO");
    LOG_INFO("UISoundManager: Quest sounds - Activate: ", questActivateLoaded ? "YES" : "NO",
             ", Complete: ", questCompleteLoaded ? "YES" : "NO",
             ", Failed: ", questFailedLoaded ? "YES" : "NO");
    LOG_INFO("UISoundManager: Loot sounds - Coins: ", (lootCoinSmallLoaded && lootCoinLargeLoaded) ? "YES" : "NO",
             ", Items: ", lootItemLoaded ? "YES" : "NO");
    LOG_INFO("UISoundManager: Item sounds - Pickup: ", (pickupBagLoaded && pickupBookLoaded) ? "YES" : "NO",
             ", Drop: ", dropLoaded ? "YES" : "NO");
    LOG_INFO("UISoundManager: Misc sounds - Eating: ", eatingLoaded ? "YES" : "NO",
             ", Drinking: ", drinkingLoaded ? "YES" : "NO",
             ", LevelUp: ", levelUpLoaded ? "YES" : "NO");

    initialized_ = true;
    LOG_INFO("UISoundManager: Initialization complete");
    return true;
}

void UiSoundManager::shutdown() {
    initialized_ = false;
}

bool UiSoundManager::loadSound(const std::string& path, UISample& sample, pipeline::AssetManager* assets) {
    return loadSampleFile(path, sample, assets, "UISoundManager");
}

void UiSoundManager::ensureSoundEntriesLoaded() {
    if (soundEntriesBuilt_) return;
    soundEntriesBuilt_ = true;          // once, whether or not it works
    if (!assets_) return;

    auto dbc = assets_->loadDBC("SoundEntries.dbc");
    if (!dbc || !dbc->isLoaded()) {
        LOG_WARNING("UISoundManager: SoundEntries.dbc not available; "
                    "PlaySound falls back to the names mapped by hand");
        return;
    }
    // 3.3.5a layout: 0 ID, 1 SoundType, 2 Name, 3..12 File[0..9],
    // 13..22 Freq[0..9], 23 DirectoryBase. The same reading zone_manager and
    // npc_voice_manager already make of this table.
    if (dbc->getFieldCount() < 24) {
        LOG_WARNING("UISoundManager: SoundEntries.dbc has ", dbc->getFieldCount(),
                    " fields, expected at least 24");
        return;
    }
    // What level a clip wants, indexed by leaf name, for the library plays
    // below: they name a file, and this table's File columns give a leaf with
    // no folder. The level and nothing else, because one clip is named by
    // several rows whose Flags differ for reasons a leaf cannot answer -
    // iEquipmentContainerOpenA is INTERFACESOUND_BACKPACKOPEN (685) plain and
    // igBackPackOpen (862) with the 0x20 one-at-a-time bit, Food (3374) plain
    // and Food - Looping (7614) looping - and a flag read off such a leaf would
    // be a guess. A leaf whose rows disagree even about the level is dropped
    // rather than guessed: LootCoinSmall.wav is LOOTWINDOWCOINSOUND (120/895)
    // at 0.7 and PickUpCoin (15034) at 0.8, and the play made of it when coins
    // are looted is the first of those - which is why playLootCoinSmall names
    // its row instead.
    std::unordered_set<std::string> disagreeing;
    const bool asksRead = dbc->getFieldCount() > 25;
    for (uint32_t row = 0; row < dbc->getRecordCount(); ++row) {
        if (asksRead) {
            const float volume = dbc->getFloat(row, 24);
            for (uint32_t f = 3; f <= 12; ++f) {
                const std::string file = dbc->getString(row, f);
                if (file.empty()) continue;
                const auto [seen, inserted] = volumeByLeaf_.try_emplace(leafLower(file), volume);
                if (!inserted && seen->second != volume) disagreeing.insert(seen->first);
            }
        }
        std::string name = dbc->getString(row, 2);
        if (name.empty()) continue;
        for (char& ch : name) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        const std::string dir = dbc->getString(row, 23);
        std::vector<std::string> paths;
        for (uint32_t f = 3; f <= 12; ++f) {
            const std::string file = dbc->getString(row, f);
            if (file.empty()) continue;
            paths.push_back(dir.empty() ? file : dir + "\\" + file);
        }
        // The first row wins. Names repeat in this table and the later rows
        // are variants; a UI click wants one sound, not a different one each
        // time.
        if (paths.empty()) continue;
        const uint32_t id = dbc->getUInt32(row, 0);
        soundIdByName_.emplace(name, id);
        soundPathsById_.emplace(id, std::move(paths));
        // What the row asks of a play of it: the level it names (the ig* rows
        // want 0.75, igMainMenuOpen/Close 0.5, the loot coin 0.7) and its
        // Flags. An older client's SoundEntries stops before that pair.
        if (asksRead)
            asksById_.try_emplace(id, UiRowAsk{dbc->getFloat(row, 24), dbc->getUInt32(row, 25)});
    }
    for (const std::string& leaf : disagreeing) volumeByLeaf_.erase(leaf);
    LOG_INFO("UISoundManager: ", soundIdByName_.size(), " sound names and the levels of ",
             volumeByLeaf_.size(), " clips from SoundEntries.dbc");
}

UiSoundManager::UiPlayResult UiSoundManager::playByName(const std::string& soundName) {
    if (!initialized_ || soundName.empty()) return UiPlayResult::Unavailable;
    ensureSoundEntriesLoaded();

    std::string key = soundName;
    for (char& ch : key) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    // 0x004ccaa0: the name is the row's, and the row's id is what plays.
    auto it = soundIdByName_.find(key);
    if (it == soundIdByName_.end()) return UiPlayResult::Unavailable;
    return playById(it->second);
}

UiSoundManager::UiPlayResult UiSoundManager::playById(uint32_t soundId) {
    if (!initialized_ || soundId == 0) return UiPlayResult::Unavailable;
    ensureSoundEntriesLoaded();

    auto cached = idSamples_.find(soundId);
    if (cached == idSamples_.end()) {
        auto it = soundPathsById_.find(soundId);
        if (it == soundPathsById_.end()) return UiPlayResult::Unavailable;
        UISample sample;
        sample.loaded = false;
        for (const std::string& path : it->second) {
            if (loadSound(path, sample, assets_)) break;
        }
        // Remembered even when nothing loaded, so a row whose file this
        // install does not have is looked for once rather than on every click.
        cached = idSamples_.emplace(soundId, std::move(sample)).first;
    }
    if (!cached->second.loaded) return UiPlayResult::Unavailable;

    UiRowAsk ask;
    if (const auto a = asksById_.find(soundId); a != asksById_.end()) ask = a->second;

    // A row flagged 0x20 is asked for one instance at a time: the client keeps
    // the ids it has started, turns a flagged ask away while the id is in that
    // list (004c0000.c:5882-5888), and frees the slot when the sound ends
    // (:5351-5361) or is stopped (:5401-5413). The engine's answer about the
    // handle asks the same question; it is put here rather than at the end of
    // the play, so a finished click is freed on the next attempt rather than at
    // the moment it stops.
    if ((ask.flags & sound_entry::kFlagOneAtATime) != 0) {
        for (auto it = playingById_.begin(); it != playingById_.end();) {
            if (AudioEngine::instance().isSoundPlaying(it->second)) ++it;
            else it = playingById_.erase(it);
        }
        if (playingById_.find(soundId) != playingById_.end()) return UiPlayResult::Refused;
    }

    // What happened, not what was attempted. playSound2D decodes WAV and
    // answers false for anything else, and SoundEntries lists a couple of
    // thousand mp3s among its wavs - claiming success for one of those would
    // skip the fallback below this and play nothing at all.
    //
    // The level is the row's own where it names one; the 0.7 this manager used
    // by guess stays for a row that names none (the ig* rows want 0.75,
    // igMainMenuOpen/Close 0.5), and the pitch moves only if the row asks it to.
    const float base = (ask.volume > 0.0f ? ask.volume : 0.7f) * volumeScale_;
    const float draw = swingDraw();
    uint32_t handle = 0;
    const bool played = AudioEngine::instance().playSound2D(
        cached->second.data, sound_entry::volumeForFlags(ask.flags, base, draw),
        sound_entry::pitchForFlags(ask.flags, draw), Channel::Own, PlayLimit::None,
        voice_priority::kDefault, &handle);
    if (played && (ask.flags & sound_entry::kFlagOneAtATime) != 0 && handle != 0)
        playingById_.emplace(soundId, handle);
    return played ? UiPlayResult::Played : UiPlayResult::Unavailable;
}

bool UiSoundManager::playFile(const std::string& path) {
    if (!initialized_ || path.empty() || !assets_) return false;

    auto cached = namedSamples_.find(path);
    if (cached == namedSamples_.end()) {
        UISample sample;
        sample.loaded = false;
        loadSound(path, sample, assets_);
        // Remembered either way, so a path this install does not have is read
        // for once rather than on every call.
        cached = namedSamples_.emplace(path, std::move(sample)).first;
    }
    if (!cached->second.loaded) return false;
    // 0x004c9110(path, 4): the SCRIPTSOUND channel at full volume. Nothing
    // sets that channel's volume and Sound_EnableSFX does not silence it -
    // only the master volume and Sound_EnableAllSound reach it.
    return AudioEngine::instance().playSound2D(cached->second.data, 1.0f, 1.0f);
}

void UiSoundManager::playSound(const std::vector<UISample>& library) {
    if (!initialized_ || library.empty() || !library[0].loaded) return;
    ensureSoundEntriesLoaded();

    // The level 0.7 this manager guessed, unless the rows naming this clip all
    // ask the same other thing: the pickup rustle 0.5, the guild vault opening
    // 0.4, the auction window 0.69, eating and drinking 0.8, the achievement
    // alert 0.8, the quest-update clip 0.75 (its only row is igBonusBarOpen,
    // 3201) and the mail and whisper tell 1.0, while the level-up, the map ping
    // and the vault closing confirm the 0.7 already played. The bag and quest
    // log openings, the button clicks and the target cues keep the guess: they
    // are named by rows that want different levels, and a leaf says which file
    // is playing, not which of those rows. Nothing here swings: no clip of
    // these libraries is named by a row carrying 0x400 or 0x800
    // (audio/sound_entry.hpp).
    float volume = 0.7f;
    if (const auto asked = volumeByLeaf_.find(leafLower(library[0].path));
        asked != volumeByLeaf_.end())
        volume = asked->second;
    AudioEngine::instance().playSound2D(library[0].data, volume * volumeScale_, 1.0f);
}

void UiSoundManager::setVolumeScale(float scale) {
    volumeScale_ = std::max(0.0f, std::min(1.0f, scale));
}

// Window sounds
void UiSoundManager::playBagOpen() { playSound(bagOpenSounds_); }
void UiSoundManager::playBagClose() { playSound(bagCloseSounds_); }
void UiSoundManager::playQuestLogOpen() { playSound(questLogOpenSounds_); }
void UiSoundManager::playQuestLogClose() { playSound(questLogCloseSounds_); }
void UiSoundManager::playCharacterSheetOpen() { playSound(characterSheetOpenSounds_); }
void UiSoundManager::playCharacterSheetClose() { playSound(characterSheetCloseSounds_); }
void UiSoundManager::playGuildBankOpen() { playSound(guildBankOpenSounds_); }
void UiSoundManager::playGuildBankClose() { playSound(guildBankCloseSounds_); }
void UiSoundManager::playAuctionHouseOpen() { playSound(auctionOpenSounds_); }
void UiSoundManager::playAuctionHouseClose() { playSound(auctionCloseSounds_); }
// Button sounds
void UiSoundManager::playButtonClick() { playSound(buttonClickSounds_); }
void UiSoundManager::playMenuButtonClick() { playSound(menuButtonSounds_); }

// Quest sounds
void UiSoundManager::playQuestActivate() { playSound(questActivateSounds_); }
void UiSoundManager::playQuestComplete() { playSound(questCompleteSounds_); }
void UiSoundManager::playQuestFailed() { playSound(questFailedSounds_); }
void UiSoundManager::playQuestUpdate() { playSound(questUpdateSounds_); }

// Loot sounds
//
// The coin a loot window takes is a row, not a file: LOOTWINDOWCOINSOUND
/// (120, repeated at 895) names LootCoinSmall.wav, asks 0.7 and carries the 0x20
/// one-at-a-time bit, which is what keeps the coin of a fast multi-loot from
/// stacking on itself. Asked by name rather than through the clip index above
/// because PickUpCoin (15034) names the same file at 0.8 without that bit, so a
/// leaf cannot say which of the two is playing; this site is the first.
void UiSoundManager::playLootCoinSmall() {
    if (playByName("LOOTWINDOWCOINSOUND") != UiPlayResult::Unavailable) return;
    playSound(lootCoinSmallSounds_);   // no table: the clip preloaded at startup
}
/// LootCoinLarge.wav is in this install and named by no row of the table at
/// all, so nothing is known of the level it wants and the small/large split by
/// amount stays this manager's own.
void UiSoundManager::playLootCoinLarge() { playSound(lootCoinLargeSounds_); }
void UiSoundManager::playLootItem() { playSound(lootItemSounds_); }

// Item sounds
void UiSoundManager::playDropOnGround() { playSound(dropSounds_); }
void UiSoundManager::playPickupBag() { playSound(pickupBagSounds_); }
void UiSoundManager::playPickupBook() { playSound(pickupBookSounds_); }
// Eating/drinking
void UiSoundManager::playEating() { playSound(eatingSounds_); }
void UiSoundManager::playDrinking() { playSound(drinkingSounds_); }

// Level up
void UiSoundManager::playLevelUp() { playSound(levelUpSounds_); }

// Achievement
void UiSoundManager::playAchievementAlert() { playSound(achievementSounds_); }

// Error/feedback
void UiSoundManager::playError() { playSound(errorSounds_); }
void UiSoundManager::playTargetSelect() { playSound(selectTargetSounds_); }
void UiSoundManager::playTargetDeselect() { playSound(deselectTargetSounds_); }

// Chat notifications
void UiSoundManager::playWhisperReceived() { playSound(whisperSounds_); }
void UiSoundManager::playMailReceived() { playSound(mailSounds_); }

// Minimap ping
void UiSoundManager::playMinimapPing() { playSound(minimapPingSounds_); }

} // namespace audio
} // namespace wowee
