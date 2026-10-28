#pragma once

#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include <cstdint>

namespace wowee {
namespace pipeline {
class AssetManager;
}

namespace audio {

class UiSoundManager {
public:
    UiSoundManager() = default;
    ~UiSoundManager() = default;

    // Initialization
    bool initialize(pipeline::AssetManager* assets);
    void shutdown();

    /// Refused is its own answer: the row asked for one instance at a time and
    /// another of the same id is still playing, which is where the client goes
    /// silent (0x004c6a40 returns 0xf) rather than starting a second one. A
    /// caller's hand-mapped fallback belongs on Unavailable only.
    enum class UiPlayResult { Played, Refused, Unavailable };

    /// Play a sound by the name FrameXML asks for - PlaySound("igQuestFailed")
    /// and its sixty-seven siblings.
    ///
    /// Resolved through SoundEntries.dbc, which is where those names come
    /// from: the interface is naming a row, and the row names the files. That
    /// is the real sound rather than the nearest one this client had already
    /// loaded, and it covers every name rather than the forty-four somebody
    /// mapped by hand.
    ///
    /// Unavailable when the name is not in the table or its file will not
    /// load, so the caller can fall back rather than going silent.
    UiPlayResult playByName(const std::string& soundName);
    /// Play a SoundEntries row by its id - PlaySound(856) (0x009858b0 ->
    /// 0x004c8fc0). As playByName, and Refused when the row asks for one at a
    /// time and this id is already playing.
    UiPlayResult playById(uint32_t soundId);

    /// Play a sound by path, which is what PlaySoundFile takes (0x009859b0 ->
    /// 0x004c9110 on the SCRIPTSOUND channel, once, at full volume). Cached
    /// by path.
    bool playFile(const std::string& path);

    // Volume control
    void setVolumeScale(float scale);
    [[nodiscard]] float getVolumeScale() const { return volumeScale_; }

    // Window sounds
    void playBagOpen();
    void playBagClose();
    void playQuestLogOpen();
    void playQuestLogClose();
    void playCharacterSheetOpen();
    void playCharacterSheetClose();
    void playGuildBankOpen();
    void playGuildBankClose();
    void playAuctionHouseOpen();
    void playAuctionHouseClose();

    // Button sounds
    void playButtonClick();
    void playMenuButtonClick();

    // Quest sounds
    void playQuestActivate();
    void playQuestComplete();
    void playQuestFailed();
    void playQuestUpdate();

    // Loot sounds
    void playLootCoinSmall();
    void playLootCoinLarge();
    void playLootItem();

    // Item sounds
    void playDropOnGround();
    void playPickupBag();
    /// Picking an item up sounds like the thing it is - cloth, food or a gem.
    /// Chosen by item class; anything else keeps the bag rustle.
    void playPickupBook();

    // Eating/drinking
    void playEating();
    void playDrinking();

    // Level up
    void playLevelUp();

    // Achievement
    void playAchievementAlert();

    // Error/feedback
    void playError();
    void playTargetSelect();
    void playTargetDeselect();

    // Chat notifications
    void playWhisperReceived();
    void playMailReceived();

    // Minimap ping
    void playMinimapPing();

private:
    /// What the row a PlaySound id names asks of its play: the level the row
    /// names (VolumeFloat) and its Flags. See audio/sound_entry.hpp.
    struct UiRowAsk {
        float volume = 1.0f;
        uint32_t flags = 0;
    };
    std::unordered_map<uint32_t, UiRowAsk> asksById_;
    /// The level asked of a clip, reached by leaf name, for the libraries
    /// below: those plays name a file rather than a row, and SoundEntries'
    /// File columns hold a leaf. Only the level is read this way, and only
    /// where the rows naming a leaf agree on it - see ensureSoundEntriesLoaded.
    std::unordered_map<std::string, float> volumeByLeaf_;
    /// The ids with a play still going, for a row whose 0x20 flag asks for one
    /// at a time: the id against the handle its play went into.
    std::unordered_map<uint32_t, uint32_t> playingById_;

    struct UISample {
        std::string path;
        std::vector<uint8_t> data;
        bool loaded;
    };

    // Sound libraries
    std::vector<UISample> bagOpenSounds_;
    std::vector<UISample> bagCloseSounds_;
    std::vector<UISample> questLogOpenSounds_;
    std::vector<UISample> questLogCloseSounds_;
    std::vector<UISample> characterSheetOpenSounds_;
    std::vector<UISample> characterSheetCloseSounds_;
    std::vector<UISample> auctionOpenSounds_;
    std::vector<UISample> auctionCloseSounds_;
    std::vector<UISample> guildBankOpenSounds_;
    std::vector<UISample> guildBankCloseSounds_;

    std::vector<UISample> buttonClickSounds_;
    std::vector<UISample> menuButtonSounds_;

    std::vector<UISample> questActivateSounds_;
    std::vector<UISample> questCompleteSounds_;
    std::vector<UISample> questFailedSounds_;
    std::vector<UISample> questUpdateSounds_;

    std::vector<UISample> lootCoinSmallSounds_;
    std::vector<UISample> lootCoinLargeSounds_;
    std::vector<UISample> lootItemSounds_;

    std::vector<UISample> dropSounds_;
    std::vector<UISample> pickupBagSounds_;
    std::vector<UISample> pickupBookSounds_;

    std::vector<UISample> eatingSounds_;
    std::vector<UISample> drinkingSounds_;

    std::vector<UISample> levelUpSounds_;
    std::vector<UISample> achievementSounds_;

    std::vector<UISample> errorSounds_;
    std::vector<UISample> selectTargetSounds_;
    std::vector<UISample> deselectTargetSounds_;
    std::vector<UISample> whisperSounds_;
    std::vector<UISample> mailSounds_;
    std::vector<UISample> minimapPingSounds_;

    // State tracking
    float volumeScale_ = 1.0f;
    bool initialized_ = false;

    // Helper methods
    bool loadSound(const std::string& path, UISample& sample, pipeline::AssetManager* assets);
    /// SoundEntries rows' files by id, and the ids by upper-cased name (the
    /// first row of a name), built on the first play. Empty after a build
    /// that found no table, which is also how the build is stopped from being
    /// attempted on every click.
    void ensureSoundEntriesLoaded();
    bool soundEntriesBuilt_ = false;
    std::unordered_map<uint32_t, std::vector<std::string>> soundPathsById_;
    std::unordered_map<std::string, uint32_t> soundIdByName_;
    /// Samples loaded on demand, by row and by PlaySoundFile's path. Kept because a UI sound is played
    /// again and again, and reading it from the archive each time is the one
    /// cost this has that the preloaded libraries do not.
    std::unordered_map<uint32_t, UISample> idSamples_;
    std::unordered_map<std::string, UISample> namedSamples_;
    pipeline::AssetManager* assets_ = nullptr;
    void playSound(const std::vector<UISample>& library);
};

} // namespace audio
} // namespace wowee
