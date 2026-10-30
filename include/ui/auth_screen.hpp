#pragma once

#include "ui/settings_panel.hpp"
#include <future>

#include "ui/paper_ui.hpp"
#include "ui/text_edit.hpp"
#include "ui/ui_services.hpp"
#include "auth/auth_handler.hpp"
#include <vulkan/vulkan.h>
#include <string>
#include <vector>
#include <functional>
#include <utility>

namespace wowee { namespace rendering { class VkContext; } }

namespace wowee { namespace ui {

/**
 * Authentication screen UI
 *
 * Allows user to enter credentials and connect to auth server
 */
class AuthScreen {
public:
    AuthScreen();
    /// Releases the background image. It is the only Vulkan resource this
    /// screen owns outright -- bgSampler comes from the context's cache and
    /// bgDescriptorSet belongs to the ImGui backend.
    ~AuthScreen();

    /// Lets go of the background image, for a Vulkan device about to be
    /// replaced. It is decoded and uploaded again the next time it is drawn.
    void releaseGpu();

    /**
     * Render the UI
     * @param authHandler Reference to auth handler
     */
    void render(auth::AuthHandler& authHandler);

    /**
     * Set callback for successful authentication
     */
    void setOnSuccess(std::function<void()> callback) { onSuccess = std::move(callback); }

    /// Set services (dependency injection)
    void setServices(const UIServices& services) { services_ = services; }

    /// What the settings sheet's "Reset to default" runs: every graphics
    /// setting back to its default, applied and saved (GameScreen).
    void setOnResetGraphicsDefaults(std::function<void()> callback) {
        onResetGraphicsDefaults_ = std::move(callback);
    }

    /// The game's own settings, which the sheet shows and changes: the same
    /// SettingsPanel the in-game options pages drive, so a value, its range
    /// and its side effects are one thing on both screens. Wired by UIManager.
    struct SettingsAccess {
        /// A setting by schema key, as a string ("" for an unknown key).
        std::function<std::string(const std::string& key)> get;
        /// Writes and applies it, as the in-game pages do.
        std::function<bool(const std::string& key, const std::string& value)> set;
        /// Writes settings.cfg.
        std::function<void()> save;
    };
    void setSettingsAccess(SettingsAccess access) { settingsAccess_ = std::move(access); }


    /**
     * Check if authentication is in progress
     */
    [[nodiscard]] bool isAuthenticating() const { return authenticating; }

    void stopLoginMusic();

    /// Draws the login art, filling the window.
    ///
    /// Public, and called by UIManager for the realm and character screens
    /// too: those sit on the same sheet of paper this does, and the art is
    /// already decoded and uploaded by the time they are reached. It is owned
    /// here because this is the screen that pays to load it.
    void drawBackdrop();

    /**
     * Get status message
     */
    [[nodiscard]] const std::string& getStatusMessage() const { return statusMessage; }

    /// Say why the player is looking at this screen. Public because a
    /// disconnect is announced from outside: the world tears itself down on the
    /// way here and there is nowhere else left to say it.
    void setStatus(const std::string& message, bool isError = false,
                   bool prominent = false);

private:
    UIServices services_;  // Injected service references

    struct ServerProfile {
        std::string hostname;
        int port = 3724;
        std::string username;
        std::string passwordHash;  // SHA1 hex (UPPER(user):UPPER(pass))
        std::string expansionId;   // "wotlk", "tbc", "classic", "turtle", ...
        std::string assetProfileId; // empty=match protocol, "legacy"=root manifest
        /// What to call it in the list, for the servers this client already
        /// knows about. Empty for one somebody typed in themselves, which has
        /// no name beyond its address.
        std::string label;
    };

    // UI state
    //
    // The fields are TextEdits rather than char buffers because the screen
    // draws its own boxes now: what used to be ImGui's business - where the
    // caret is, what is selected - is this screen's, and TextEdit is where
    // that lives. See paper_ui.hpp for why the boxes are drawn at all.
    TextEdit hostname_{255};
    TextEdit username_{255};
    TextEdit password_{255};
    TextEdit pinCode_{31};
    /// The port as it is being typed. `port` is what the rest of the screen
    /// reads, and is re-derived from this whenever it changes - a half-typed
    /// port is a string with no number in it yet, and the connect path should
    /// never see one.
    TextEdit portText_{5};
    int port = 3724;
    int expansionIndex = 0;     // Index into expansion registry profiles
    std::string assetProfileId_; // Empty follows expansionIndex automatically
    bool authenticating = false;
    bool showPassword = false;
    bool pinAutoSubmitted_ = false;
    bool securityPromptFocused_ = false;

    /// The controls, and the frame-to-frame state behind them.
    PaperUI ui_;
    /// The server, port, expansion and asset rows, which most players never
    /// need to touch and which used to be the first thing the screen showed.
    bool advancedOpen_ = false;
    /// Whether the password box had the keyboard last frame, so that taking
    /// it can offer a remembered password up for replacement rather than for
    /// editing - there is nothing in the box to edit, only a stored hash.
    bool passwordFocused_ = false;
    /// Set the port and the text of it together.
    void setPort(int value);

    // Status
    std::string statusMessage;
    /// Drawn across the screen rather than only in the panel. For the things
    /// that happened to the player rather than to a form they filled in.
    bool statusProminent = false;
    bool statusIsError = false;
    std::string failureReason;    // Specific reason from auth handler
    float authTimer = 0.0f;       // Timeout tracker
    static constexpr float AUTH_TIMEOUT = 10.0f;

    // Saved password hash (SHA1(UPPER(user):UPPER(pass)) as hex)
    std::string savedPasswordHash;
    bool usingStoredHash = false;
    static constexpr const char* PASSWORD_PLACEHOLDER = "\x01\x01\x01\x01\x01\x01\x01\x01";

    // Saved server-specific profiles
    std::vector<ServerProfile> servers_;
    int selectedServerIndex_ = -1;  // -1 = custom/unlisted

    // Callbacks
    std::function<void()> onSuccess;

    /**
     * Attempt authentication (starts a fresh attempt, resetting the protocol
     * fallback chain).
     */
    void attemptAuth(auth::AuthHandler& authHandler);

    /**
     * Connect + authenticate using authProtocols_[authProtocolAttempt_].
     * Shared by the initial attempt and each protocol fallback retry.
     */
    void beginAuthAttempt(auth::AuthHandler& authHandler);

    // Auth protocol versions to try, in order. Vanilla-family servers disagree
    // on this byte - vmangos-derived 1.12 realms speak protocol 8 while stock
    // mangos/cmangos 1.12 speak 3 - and the profile can only name one of them,
    // so a mismatch is retried on the next candidate instead of hard-failing.
    // Retries only fire for protocol-shaped failures (see
    // AuthHandler::lastFailureWasProtocol) - never for a rejected password.
    std::vector<uint8_t> authProtocols_;
    size_t authProtocolAttempt_ = 0;

    /**
     * Update status message
     */

    /**
     * Persist/restore login fields
     */
    void saveLoginInfo(bool includePasswordHash);
    void loadLoginInfo();
    static std::string getConfigPath();
    bool loginInfoLoaded = false;

    static std::string makeServerKey(const std::string& host, int port);
    /// How one reads in the server list: its name if this client knows one,
    /// its address otherwise, and the account already saved against it.
    static std::string serverRowLabel(const ServerProfile& s);
    /// Put the servers this client is known to work against into the list, so
    /// that a first run offers somewhere to actually connect to rather than a
    /// blank address box and a realmlist to go and look up.
    void seedKnownServers();
    void selectServerProfile(int index);
    void upsertCurrentServerProfile(bool includePasswordHash);
    [[nodiscard]] std::string currentExpansionId() const;

    // Background image (Vulkan)
    bool bgInitAttempted = false;
    // The background is a 1408x768 PNG in a 2.4MB file; decoding it on the
    // frame it first appears cost ~190ms. Decode on a worker and upload when it
    // arrives - the screen simply renders without it until then.
    struct DecodedBackground {
        std::vector<unsigned char> pixels;
        int width = 0;
        int height = 0;
    };
    std::future<DecodedBackground> bgDecodeFuture;
    bool bgDecodeStarted = false;
    bool uploadBackgroundImage(const unsigned char* pixels);
    void destroyBackgroundImage();
    rendering::VkContext* bgVkCtx = nullptr;
    VkImage bgImage = VK_NULL_HANDLE;
    VkDeviceMemory bgMemory = VK_NULL_HANDLE;
    VkImageView bgImageView = VK_NULL_HANDLE;
    VkSampler bgSampler = VK_NULL_HANDLE;
    VkDescriptorSet bgDescriptorSet = VK_NULL_HANDLE;
    int bgWidth = 0;
    int bgHeight = 0;

    bool musicInitAttempted = false;
    bool musicPlaying = false;
    bool missingIntroTracksLogged_ = false;
    bool introTracksScanned_ = false;
    std::string loginTrackPath_;
    bool loginMusicVolumeAdjusted_ = false;
    int savedMusicVolume_ = 30;

    // ----- Login-screen graphics settings sheet -----
    bool settingsOpen_ = false;

    /// The graphics the sheet showed when it opened, every row by key, for
    /// Cancel to put back. Everything applies as it is changed, as on the
    /// in-game pages; Apply saves.
    std::vector<std::pair<std::string, std::string>> sheetOpened_;
    /// The page of the sheet shown: a graphics category of the schema.
    std::string sheetCategory_;
    /// Which part of that page, when it is too long for the sheet.
    int sheetPart_ = 0;
    bool sheetHookChecked_ = false;
    /// "Reset to default" pressed: the yes/no over the sheet is showing.
    bool confirmGraphicsReset_ = false;
    // The last session ended while ray tracing (core/rt_session_marker.hpp):
    // asked once at the first frame, then a dialog until answered.
    bool rtMarkerChecked_ = false;
    bool rtMarkerShown_ = false;
    uint64_t rtMarkerAvailableMB_ = 0;
    uint64_t rtMarkerTracingMB_ = 0;
    void renderRtCrashDialog(float screenW, float screenH);
    std::function<void()> onResetGraphicsDefaults_;

    /// Draws the card and everything on it. Answers the rect it took, so the
    /// caller can hand the pointer over the whole sheet rather than only over
    /// the controls on it.
    void renderCard(auth::AuthHandler& authHandler, float screenW, float screenH);
    void renderLoginSettingsSheet(float screenW, float screenH);
    /// The paper strip a disconnect is announced on, across the top of the
    /// screen rather than inside the card.
    void renderProminentStatus(float screenW, float screenH);
    /// The login music, which has nothing to do with the controls.
    void updateMusic();
    void openLoginSettingsSheet();
    /// Closes the sheet; without `keep`, what was changed on it goes back.
    void closeLoginSettingsSheet(bool keep);
    SettingsAccess settingsAccess_;
};

}} // namespace wowee::ui
