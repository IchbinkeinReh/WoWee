#include "ui/auth_screen.hpp"
#include "rendering/rt_memory_guard.hpp"
#include "core/rt_session_marker.hpp"
#include "addons/lua_api_registrations.hpp"
#include <future>
#include <chrono>
#include "ui/settings_i18n.hpp"
#include "ui/settings_schema.hpp"
#include "game/interface_text.hpp"
#include "ui/settings_panel.hpp"
#include "ui/data_folder_picker.hpp"
#include "auth/crypto.hpp"
#include "core/application.hpp"
#include "pipeline/asset_inventory.hpp"
#include "core/config_paths.hpp"
#include "core/env_flag.hpp"
#include "core/logger.hpp"
#include "core/open_url.hpp"
#include "core/version.hpp"
#include "core/window.hpp"
#include "rendering/renderer.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/imgui_texture.hpp"
#include "pipeline/asset_manager.hpp"
#include "audio/audio_coordinator.hpp"
#include "audio/music_manager.hpp"
#include "game/expansion_profile.hpp"
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include "stb_image.h"
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <iomanip>
#include <unordered_map>

namespace wowee { namespace ui {

static std::string trimAscii(std::string s) {
    auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    size_t b = 0;
    while (b < s.size() && isSpace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && isSpace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static std::string hexEncode(const std::vector<uint8_t>& data) {
    std::ostringstream ss;
    for (uint8_t b : data)
        ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(b);
    return ss.str();
}

static std::vector<uint8_t> hexDecode(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        try {
            uint8_t b = static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
            bytes.push_back(b);
        } catch (...) {
            return {};
        }
    }
    return bytes;
}

AuthScreen::AuthScreen() {
}

AuthScreen::~AuthScreen() {
    // The background image was uploaded with plain vkCreateImage and
    // vkAllocateMemory and nothing released it, so it and its view outlived
    // the device on every run. bgSampler is the context's, cached and shared,
    // and bgDescriptorSet belongs to the ImGui backend, which frees its own
    // pool -- neither is this class's to destroy.
    destroyBackgroundImage();
}

void AuthScreen::destroyBackgroundImage() {
    if (!bgVkCtx) return;
    VkDevice device = bgVkCtx->getDevice();
    if (device == VK_NULL_HANDLE) return;
    if (bgImageView) { vkDestroyImageView(device, bgImageView, nullptr); bgImageView = VK_NULL_HANDLE; }
    if (bgImage)     { vkDestroyImage(device, bgImage, nullptr);         bgImage = VK_NULL_HANDLE; }
    if (bgMemory)    { vkFreeMemory(device, bgMemory, nullptr);          bgMemory = VK_NULL_HANDLE; }
}

void AuthScreen::releaseGpu() {
    // The set goes back to the backend that is about to go itself; the
    // sampler is the context's cache's, gone with the context.
    rendering::removeImGuiTexture(bgDescriptorSet);
    bgSampler = VK_NULL_HANDLE;
    destroyBackgroundImage();
    // Decoded and uploaded again the next time the screen is drawn. One still
    // decoding is left to land: it uploads to whatever device is there then.
    if (bgInitAttempted) {
        bgInitAttempted = false;
        bgDecodeStarted = false;
    }
}

std::string AuthScreen::makeServerKey(const std::string& host, int port) {
    std::ostringstream ss;
    ss << host << ":" << port;
    return ss.str();
}

std::string AuthScreen::serverRowLabel(const ServerProfile& s) {
    std::string row = s.label.empty() ? makeServerKey(s.hostname, s.port) : s.label;
    if (!s.username.empty()) row += "   " + s.username;
    return row;
}

std::string AuthScreen::currentExpansionId() const {
    auto* reg = core::Application::getInstance().getExpansionRegistry();
    if (reg && reg->getActive()) {
        return reg->getActive()->id;
    }
    return "wotlk";
}

void AuthScreen::seedKnownServers() {
    // The servers this client is developed and tested against, offered by
    // name. A first run used to open on an empty list with the address
    // defaulted to localhost - which is a server nobody new has - so the only
    // way to reach anything was to already know a realmlist, and to find the
    // box for it behind "more options". Nothing here is a recommendation; it
    // is the address somebody would otherwise be looking up.
    struct Known {
        const char* label;
        const char* hostname;
        int port;
        const char* expansionId;
    };
    static constexpr Known kKnown[] = {
        {"ChromieCraft", "logon.chromiecraft.com", 3724, "wotlk"},
    };

    for (const Known& k : kKnown) {
        const std::string key = makeServerKey(k.hostname, k.port);
        bool already = false;
        for (ServerProfile& s : servers_) {
            if (makeServerKey(s.hostname, s.port) != key) continue;
            // Somebody has logged into it before, so their own entry stands -
            // it carries their account. It only gains the name.
            if (s.label.empty()) s.label = k.label;
            already = true;
            break;
        }
        if (already) continue;

        ServerProfile s;
        s.hostname = k.hostname;
        s.port = k.port;
        s.expansionId = k.expansionId;
        s.label = k.label;
        servers_.push_back(std::move(s));
    }
}

void AuthScreen::selectServerProfile(int index) {
    if (index < 0 || index >= static_cast<int>(servers_.size())) {
        selectedServerIndex_ = -1;
        return;
    }

    selectedServerIndex_ = index;
    const auto& s = servers_[index];

    hostname_.setText(s.hostname);
    setPort(s.port);
    username_.setText(s.username);

    savedPasswordHash = s.passwordHash;
    usingStoredHash = !savedPasswordHash.empty();
    password_.setText(usingStoredHash ? PASSWORD_PLACEHOLDER : "");

    auto& app = core::Application::getInstance();
    if (!s.expansionId.empty()) {
        auto* expReg = app.getExpansionRegistry();
        if (expReg && expReg->setActive(s.expansionId)) {
            auto& profiles = expReg->getAllProfiles();
            for (int i = 0; i < static_cast<int>(profiles.size()); ++i) {
                if (profiles[i].id == s.expansionId) { expansionIndex = i; break; }
            }
        }
    }
    assetProfileId_ = s.assetProfileId;
    if (!app.setAssetExpansionOverride(assetProfileId_)) {
        assetProfileId_.clear();
        app.setAssetExpansionOverride({});
    }
    app.reloadExpansionData();
}

void AuthScreen::upsertCurrentServerProfile(bool includePasswordHash) {
    const std::string hostStr = hostname_.text();
    if (hostStr.empty() || port <= 0) {
        return;
    }

    const std::string key = makeServerKey(hostStr, port);
    int foundIndex = -1;
    for (int i = 0; i < static_cast<int>(servers_.size()); ++i) {
        if (makeServerKey(servers_[i].hostname, servers_[i].port) == key) {
            foundIndex = i;
            break;
        }
    }

    ServerProfile s;
    s.hostname = hostStr;
    s.port = port;
    s.username = username_.text();
    s.expansionId = currentExpansionId();
    s.assetProfileId = assetProfileId_;
    if (includePasswordHash && !savedPasswordHash.empty()) {
        s.passwordHash = savedPasswordHash;
    } else if (foundIndex >= 0) {
        // Preserve existing stored hash if we aren't updating it.
        s.passwordHash = servers_[foundIndex].passwordHash;
    }

    if (foundIndex >= 0) {
        servers_[foundIndex] = std::move(s);
        selectedServerIndex_ = foundIndex;
    } else {
        servers_.push_back(std::move(s));
        selectedServerIndex_ = static_cast<int>(servers_.size()) - 1;
    }

    // Keep deterministic ordering (and stable combo ordering) across runs.
    std::sort(servers_.begin(), servers_.end(),
              [](const ServerProfile& a, const ServerProfile& b) {
                  if (a.hostname != b.hostname) return a.hostname < b.hostname;
                  return a.port < b.port;
              });

    // Fix up index after sort.
    for (int i = 0; i < static_cast<int>(servers_.size()); ++i) {
        if (makeServerKey(servers_[i].hostname, servers_[i].port) == key) {
            selectedServerIndex_ = i;
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// The card
// ---------------------------------------------------------------------------

namespace {

// The card's layout, written in units and drawn at a scale taken from the
// window, so what follows is its proportions rather than its pixels.
constexpr float kCardWidth     = 432.0f;
constexpr float kCardPadX      = 34.0f;
constexpr float kCardPadTop    = 26.0f;
constexpr float kCardPadBottom = 20.0f;
constexpr float kTitleSize     = 44.0f;
constexpr float kLabelSize     = 14.0f;
constexpr float kBodySize      = 15.0f;
constexpr float kSmallSize     = 13.0f;
constexpr float kFieldHeight   = 38.0f;
constexpr float kButtonHeight  = 46.0f;
constexpr float kRowGap        = 14.0f;
constexpr float kLabelGap      = 3.0f;

/// A code that looks like one someone finished typing.
///
/// PIN grids run four to ten digits and an authenticator is six, which is why
/// six is named twice: it is in range either way and the check reads clearer
/// saying so than leaving it implied.
bool looksLikeSecurityCode(const std::string& code) {
    if (code.empty()) return false;
    for (char c : code)
        if (c < '0' || c > '9') return false;
    return (code.size() >= 4 && code.size() <= 10) || code.size() == 6;
}

} // namespace

void AuthScreen::setPort(int value) {
    port = std::clamp(value, 1, 65535);
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%d", port);
    portText_.setText(buf);
}

void AuthScreen::render(auth::AuthHandler& authHandler) {
    // The folder dialog's answer, if it has given one, on this thread.
    dataFolderPicker().poll();

    // Load saved login info on first render
    if (!loginInfoLoaded) {
        loadLoginInfo();
        // After the load, so it covers every way out of it - a config that
        // was read, one that was migrated, and the first run where there is
        // no file at all and the list would otherwise be empty.
        seedKnownServers();
        if (selectedServerIndex_ < 0 && !servers_.empty()) selectServerProfile(0);
        loginInfoLoaded = true;
        if (portText_.empty()) setPort(port);
        // Only when the list left nothing to point at. This used to run
        // unconditionally, so a first run opened aimed at a server the person
        // running it does not have.
        if (hostname_.empty()) hostname_.setText("localhost");
        auto* registry = core::Application::getInstance().getExpansionRegistry();
        if (registry && registry->getActive()) {
            const auto& profiles = registry->getAllProfiles();
            for (int i = 0; i < static_cast<int>(profiles.size()); ++i) {
                if (profiles[i].id == registry->getActive()->id) {
                    expansionIndex = i;
                    break;
                }
            }
        }
    }

    if (!rtMarkerChecked_) {
        rtMarkerChecked_ = true;
        const std::string markerFile = core::getConfigRoot() + "/rt_session.marker";
        if (auto last = core::rt_session::read(markerFile)) {
            rtMarkerShown_ = true;
            rtMarkerAvailableMB_ = last->availableMB;
            rtMarkerTracingMB_ = last->tracingMB;
            LOG_WARNING("The last session ended while ray tracing (", last->availableMB, " MB of RAM available, ",
                        last->tracingMB, " MB held by the tracing, ", last->privateMB, " MB private)");
        }
    }

    drawBackdrop();
    updateMusic();

    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    // A share of the window rather than a count of pixels, so the card is the
    // same size on a phone, on a laptop and on a 4K monitor. The floor keeps
    // it legible in a small window; the ceiling keeps it from swallowing a
    // large one.
    const float scale = std::clamp(std::min(screen.x / 1280.0f, screen.y / 760.0f), 0.62f, 2.6f);
    ui_.begin(ImGui::GetIO().DeltaTime, scale);
    ui_.setLayer(PaperLayer::Page);

    // Everything about authenticating that is not drawing. It runs before the
    // card so the card can simply read the state it leaves behind.
    const auto authState = authHandler.getState();
    const bool waitingForSecurityCode = (authState == auth::AuthState::PIN_REQUIRED ||
                                         authState == auth::AuthState::AUTHENTICATOR_REQUIRED);

    if (authenticating) {
        if (!waitingForSecurityCode) {
            pinAutoSubmitted_ = false;
            securityPromptFocused_ = false;
            authTimer += ImGui::GetIO().DeltaTime;
        } else if (!pinAutoSubmitted_ && looksLikeSecurityCode(pinCode_.text())) {
            // The player typed a code before pressing the button; send it now
            // rather than making them press a second one.
            authHandler.submitSecurityCode(pinCode_.text());
            pinCode_.clear();
            pinAutoSubmitted_ = true;
        }

        if (authState == auth::AuthState::AUTHENTICATED) {
            setStatus("Authentication successful!", false);
            authenticating = false;

            // Compute and save password hash if user typed a fresh password
            if (!usingStoredHash) {
                std::string upperUser = username_.text();
                std::string upperPass = password_.text();
                auto toUp = [](unsigned char c) { return static_cast<char>(std::toupper(c)); };
                std::transform(upperUser.begin(), upperUser.end(), upperUser.begin(), toUp);
                std::transform(upperPass.begin(), upperPass.end(), upperPass.begin(), toUp);
                std::string combined = upperUser + ":" + upperPass;
                auto hash = auth::Crypto::sha1(combined);
                savedPasswordHash = hexEncode(hash);
            }
            saveLoginInfo(true);

            if (onSuccess) {
                onSuccess();
            }
        } else if (authState == auth::AuthState::FAILED) {
            // Protocol fallback: a 1.12 realm that speaks the other vanilla auth
            // protocol byte rejects or drops our handshake rather than replying
            // usefully. Retry once on the next candidate before giving up - but
            // only for protocol-shaped failures, never for a rejected password
            // (retrying those can trip server-side lockouts).
            const bool haveFallback = (authProtocolAttempt_ + 1 < authProtocols_.size());
            if (haveFallback && authHandler.lastFailureWasProtocol()) {
                LOG_INFO("Auth failed on protocol ",
                         static_cast<int>(authProtocols_[authProtocolAttempt_]),
                         " (", failureReason, ") - retrying with protocol ",
                         static_cast<int>(authProtocols_[authProtocolAttempt_ + 1]));
                ++authProtocolAttempt_;
                authHandler.disconnect();
                beginAuthAttempt(authHandler);
            } else {
                setStatus(failureReason.empty() ? "Authentication failed" : failureReason, true);
                authenticating = false;
            }
        } else if (!waitingForSecurityCode && authTimer >= AUTH_TIMEOUT) {
            setStatus("Connection timed out - server did not respond", true);
            authenticating = false;
            authHandler.disconnect();
        }
    }

    renderProminentStatus(screen.x, screen.y);

    // Whether a dropdown was down when the frame began. Escape closes one
    // from inside the control, so asking afterwards would find it already
    // shut and go on to close the settings sheet behind it - two things
    // dismissed by one press.
    const bool popupWasOpen = ui_.popupOpen();

    // The card hears nothing while the settings sheet is over it, but it is
    // still drawn - a modal that blanks what it covers loses the player's
    // place.
    //
    // Asked once. The gear is on the card, so rendering it can turn the sheet
    // on halfway through, and asking again afterwards popped an inertness
    // that had never been pushed.
    // A picture of the sheet (WOWEE_SCREENSHOT): WOWEE_LOGIN_SHEET=<page>,
    // with ":<part>" for a later part of a long page or ":reset" for the
    // yes/no, opens it on that page. Only when a screenshot is asked for.
    if (!sheetHookChecked_) {
        sheetHookChecked_ = true;
        const char* page = std::getenv("WOWEE_LOGIN_SHEET");
        if (page && *page && std::getenv("WOWEE_SCREENSHOT")) {
            const std::string spec = page;
            const std::size_t colon = spec.find(':');
            openLoginSettingsSheet();
            sheetCategory_ = spec.substr(0, colon);
            const std::string extra = colon == std::string::npos ? std::string() : spec.substr(colon + 1);
            if (extra == "reset") {
                confirmGraphicsReset_ = true;
            } else if (!extra.empty()) {
                sheetPart_ = std::atoi(extra.c_str());
            }
        }
    }
    const bool sheetWasOpen = settingsOpen_;
    if (sheetWasOpen) ui_.pushInert();
    renderCard(authHandler, screen.x, screen.y);
    if (sheetWasOpen) ui_.popInert();

    if (settingsOpen_) renderLoginSettingsSheet(screen.x, screen.y);
    if (rtMarkerShown_) renderRtCrashDialog(screen.x, screen.y);

    // Escape backs out of one thing per press: the open dropdown, then the
    // settings sheet, then the keyboard.
    if (!popupWasOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (settingsOpen_) {
            closeLoginSettingsSheet(false);
        } else {
            ui_.clearFocus();
        }
    }

    // Enter with the keyboard in none of the boxes still means log in. The
    // screen opens with nothing focused - deliberately, so a phone does not
    // meet it with the on-screen keyboard already up over the card - and
    // pressing Enter there should do the obvious thing rather than nothing.
    if (!settingsOpen_ && !authenticating && !ui_.wantsTextInput() &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
         ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false))) {
        attemptAuth(authHandler);
    }

    // WOWEE_AUTO_LOGIN=1 (or 2, which also enters the world) presses Login
    // for unattended test runs that have to reach character select.
    // Only with an account this screen already remembers: it takes the same
    // path as the button, with the stored hash, and never sees a password.
    // A few tries, five seconds apart, then it waits for a person: a client
    // relaunched straight after the last one is now and then refused by the
    // server ("Account not found") and let in on the next try.
    {
        static const bool autoLogin = core::envIntClamped("WOWEE_AUTO_LOGIN", 0, 0, 2) > 0;
        static int autoLoginTries = 0;
        static double lastAutoLogin = -1e9;
        const double now = ImGui::GetTime();
        if (autoLogin && autoLoginTries < 5 && now - lastAutoLogin > 5.0 && loginInfoLoaded &&
            !authenticating && !settingsOpen_ && usingStoredHash &&
            password_.text() == PASSWORD_PLACEHOLDER) {
            ++autoLoginTries;
            lastAutoLogin = now;
            LOG_WARNING("WOWEE_AUTO_LOGIN: logging in with the remembered account (try ",
                        autoLoginTries, ")");
            attemptAuth(authHandler);
        }
    }

    // Build version, bottom-left over the login art, as the retail client
    // does. On the overlay because the art behind it is a photograph and it
    // needs its own shadow to stay readable over any part of one.
    {
        ui_.setLayer(PaperLayer::Overlay);
        const float size = ui_.px(kSmallSize);
        const ImVec2 at(ui_.px(14), screen.y - ui_.lineHeight(size) - ui_.px(8));
        ui_.text(ImVec2(at.x + 1.0f, at.y + 1.0f), core::kVersionString, size,
                 IM_COL32(0, 0, 0, 150));
        ui_.text(at, core::kVersionString, size, IM_COL32(0xEC, 0xE2, 0xCC, 0xC8));

        // And beside it, if GitHub has a newer one. Next to the version
        // rather than in the card: it is about the program, not about
        // logging in, and the card is the busiest thing on the screen
        // already. Nothing is downloaded - clicking it opens the release
        // page and the player decides from there.
        const core::UpdateCheck& updates = core::Application::getInstance().getUpdateCheck();
        if (const std::string newer = updates.newerVersion(); !newer.empty()) {
            const std::string note = "  -  " + newer + " is available";
            const ImVec2 beside(at.x + ui_.textWidth(core::kVersionString, size), at.y);
            ui_.text(ImVec2(beside.x + 1.0f, beside.y + 1.0f), note.c_str(), size,
                     IM_COL32(0, 0, 0, 150));
            if (ui_.link("update", beside, note.c_str(), size,
                         IM_COL32(0xF6, 0xD9, 0x6B, 0xE0))) {
                // openExternalUrl refuses anything that is not a plain
                // http(s) URL, which is the standard every caller is held to
                // - this one comes off the network like a chat link does.
                core::openExternalUrl(updates.releaseUrl());
            }
        }
        ui_.setLayer(PaperLayer::Page);
    }

    ui_.end();
}

void AuthScreen::renderProminentStatus(float screenW, float screenH) {
    // Across the top of the screen, on its own strip of paper.
    //
    // A disconnect is not a form being wrong - the player did not ask to be
    // here and may not have been looking - so it is said where it cannot be
    // missed rather than as another red line inside the card. It is cleared
    // like any other status: by typing, or by connecting again.
    if (!statusProminent || statusMessage.empty()) return;

    ui_.setLayer(PaperLayer::Overlay);
    const float size = ui_.px(30);
    const float pad = ui_.px(26);
    const float width = std::min(screenW - ui_.px(80),
                                 ui_.textWidth(statusMessage.c_str(), size) + pad * 2.0f);
    const float textW = width - pad * 2.0f;
    const float textH = ui_.wrappedHeight(textW, statusMessage.c_str(), size);
    const float x0 = (screenW - width) * 0.5f;
    const float y0 = screenH * 0.10f;
    const ImVec2 a(x0, y0);
    const ImVec2 b(x0 + width, y0 + textH + pad);

    ui_.sheet(a, b, /*taped=*/false);
    ui_.wrapped(ImVec2(a.x + pad, a.y + pad * 0.5f), textW, statusMessage.c_str(), size,
                ui_.theme().crayonRed);
    ui_.setLayer(PaperLayer::Page);
}

namespace {

/// One asset set that can be played with: a label to show and the id to set.
struct AssetChoice {
    std::string label;
    std::string id;     ///< empty means "whatever the protocol expansion is"
};

/// Every asset set actually installed, in the order they should be offered.
///
/// Several games can be built into one Data folder, each under its own name,
/// and which one to draw with is a choice worth making before logging in - a
/// Wrath server played with Cataclysm's art is the whole point of building
/// more than one. Only sets with a manifest are listed: an expansion the
/// registry knows about but which was never extracted is not something anyone
/// can choose.
std::vector<AssetChoice> assetChoices(const game::ExpansionRegistry* registry) {
    std::vector<AssetChoice> out;
    if (registry == nullptr) return out;

    const game::ExpansionProfile* active = registry->getActive();
    out.push_back({active ? "Match protocol  (" + active->shortName + ")"
                          : std::string("Match protocol"),
                   std::string()});

    for (const auto& candidate : registry->getAllProfiles()) {
        if (!std::filesystem::exists(candidate.dataPath + "/manifest.json")) continue;
        out.push_back({candidate.shortName + " assets", candidate.id});
    }

    const char* dataPathEnv = std::getenv("WOW_DATA_PATH");
    const std::filesystem::path baseData = dataPathEnv ? dataPathEnv : "./Data";
    if (std::filesystem::exists(baseData / "manifest.json")) {
        out.push_back({"Legacy root Data", "legacy"});
    }
    return out;
}

/// One line saying what the assets about to be used actually are.
///
/// Which set that is depends on the override: with none, it is whatever the
/// protocol expansion's is, which is the answer somebody gets by doing nothing.
std::string assetLine(const pipeline::AssetInventory& inventory,
                      const game::ExpansionRegistry* registry,
                      const std::string& overrideId) {
    std::string id = overrideId;
    if (id == "legacy") return {};
    if (id.empty() && registry != nullptr) {
        if (const game::ExpansionProfile* active = registry->getActive()) id = active->id;
    }
    const pipeline::AssetSet* set = inventory.find(id);
    if (set == nullptr || !set->usable()) return {};
    return set->summary();
}

}  // namespace

void AuthScreen::renderCard(auth::AuthHandler& authHandler, float screenW, float screenH) {
    const PaperTheme& theme = ui_.theme();
    const auto px = [this](float units) { return ui_.px(units); };

    const auto authState = authHandler.getState();
    const bool waitingForSecurityCode = (authState == auth::AuthState::PIN_REQUIRED ||
                                         authState == auth::AuthState::AUTHENTICATOR_REQUIRED);

    const float labelSize = px(kLabelSize);
    const float bodySize = px(kBodySize);
    const float smallSize = px(kSmallSize);
    const float labelRow = ui_.lineHeight(labelSize) + px(kLabelGap);
    const float fieldRow = labelRow + px(kFieldHeight);
    const float smallRow = ui_.lineHeight(smallSize);
    const float contentW = px(kCardWidth) - px(kCardPadX) * 2.0f;

    // A security prompt outranks the disclosure: when the server is waiting
    // for a code, the box for it belongs in the middle of the card and not
    // three rows down behind a link.
    const bool codeInMain = waitingForSecurityCode;
    const bool codeInAdvanced = !codeInMain;

    const float statusH = statusMessage.empty()
        ? 0.0f
        : ui_.wrappedHeight(contentW, statusMessage.c_str(), bodySize);

    // The card is sized before it is drawn, so every row that might not
    // appear has to be asked about here on the same terms the drawing asks -
    // otherwise the sheet comes out taller than what is on it.
    auto* registry = core::Application::getInstance().getExpansionRegistry();
    const bool haveExpansions = registry && !registry->getAllProfiles().empty();

    float advancedH = 0.0f;
    if (advancedOpen_) {
        advancedH = px(kRowGap) + px(2);                     // the rule above it
        advancedH += fieldRow + px(kRowGap);                 // address and port
        if (haveExpansions) {
            advancedH += fieldRow + px(kRowGap);             // expansion
        } else {
            advancedH += smallRow + px(kRowGap);             // the one line instead
        }
        if (codeInAdvanced) advancedH += fieldRow + px(kRowGap);
#ifdef WOWEE_HAVE_ASSET_PANEL
        advancedH += smallRow + px(kRowGap);             // the assets link
#endif
    }

    // The title, its rule and the gap under it, measured once and laid out from
    // the same number below. Measuring it as the em while drawing it as the
    // ink left the card short by a third of a title's height, and everything
    // that sits at the bottom of it - the graphics button, the close button -
    // was drawn over the sheet's own bottom edge.
    const float titleBlockH = ui_.inkHeight(px(kTitleSize), true) * 1.02f + px(18);

    float contentH = titleBlockH;                            // title and its underline
    contentH += fieldRow + px(kRowGap);                      // server
    contentH += fieldRow + px(kRowGap);                      // account
    contentH += fieldRow + px(kRowGap);                      // password

    // The assets row sits in the card itself rather than behind the disclosure,
    // and only when there is more than one set installed - with a single set
    // there is nothing to choose and the row is a question with one answer.
    const std::vector<AssetChoice> assets = assetChoices(registry);
    const bool chooseAssets = assets.size() > 2;
    if (chooseAssets) {
        contentH += fieldRow + px(kRowGap);
        if (!assetProfileId_.empty()) contentH += smallRow + px(4);
    }

    // What is installed, said on the screen somebody is looking at when they
    // wonder. Either a line describing the set about to be played with, or -
    // when there is nothing to play with - what to do about it, which is the
    // one thing the client never used to say.
    const pipeline::AssetInventory& inventory =
        core::Application::getInstance().getAssetInventory();
    const std::string trouble = inventory.troubleText();
    const std::string haveLine = assetLine(inventory, registry, assetProfileId_);

    float troubleH = 0.0f;
    float haveH = 0.0f;
    if (!trouble.empty()) {
        troubleH = ui_.wrappedHeight(contentW, trouble.c_str(), smallSize) + px(6);
        contentH += troubleH;
    } else if (!haveLine.empty()) {
        haveH = ui_.wrappedHeight(contentW, haveLine.c_str(), smallSize) + px(6);
        contentH += haveH;
    }

    if (codeInMain) contentH += fieldRow + px(kRowGap);
    if (statusH > 0.0f) contentH += statusH + px(10);
    contentH += px(kButtonHeight) + px(kRowGap);
    contentH += smallRow + px(kRowGap);                      // the disclosure link
    contentH += advancedH;
    contentH += px(10) + smallRow;                           // the footer rule and row

    const float cardW = px(kCardWidth);
    const float cardH = px(kCardPadTop) + contentH + px(kCardPadBottom);
    const ImVec2 cardA((screenW - cardW) * 0.5f,
                       std::max(px(12), (screenH - cardH) * 0.48f));
    const ImVec2 cardB(cardA.x + cardW, cardA.y + cardH);

    ui_.sheet(cardA, cardB);
    ui_.claimMouse(cardA, cardB);

    Column col{cardA.x + px(kCardPadX), cardB.x - px(kCardPadX), cardA.y + px(kCardPadTop)};
    const float centreX = (cardA.x + cardB.x) * 0.5f;

    // ---- title ----------------------------------------------------------
    {
        const float titleSize = px(kTitleSize);
        const float top = col.y;
        ui_.textCentered(centreX, top, "WoWee", titleSize, theme.ink, /*titleFace=*/true);
        const float titleW = ui_.textWidth("WoWee", titleSize, true);
        // Below the title's own descenders, which is further down than the
        // size names: a size is an em and the face draws taller than one.
        const float underlineY = top + ui_.inkHeight(titleSize, true) * 1.02f;
        ui_.squiggle(ImVec2(centreX - titleW * 0.62f, underlineY),
                     ImVec2(centreX + titleW * 0.62f, underlineY),
                     theme.crayonRed, px(2.0f), 0x51A1u);
        // The height the card was measured with, so the two cannot drift.
        col.y = top + titleBlockH;
    }

    // ---- credentials ----------------------------------------------------
    const auto labelledField = [&](const char* label, const char* id, TextEdit& edit,
                                   const PaperUI::FieldOpts& opts) {
        ui_.text(col.at(), label, labelSize, theme.inkSoft);
        col.gap(labelRow);
        const auto [a, b] = col.row(px(kFieldHeight));
        const PaperUI::FieldResult r = ui_.field(id, a, b, edit, opts);
        col.gap(px(kRowGap));
        return r;
    };

    bool submit = false;

    // ---- which server ----------------------------------------------------
    //
    // First, and in the card rather than behind the disclosure: which server
    // you are logging into is the question that comes before who you are, and
    // somebody who has never done this cannot answer the two below it without
    // it. It used to sit three rows down behind "more options", so the screen
    // asked for an account on a server it never named.
    {
        std::vector<std::string> rows;
        rows.reserve(servers_.size() + 1);
        for (const ServerProfile& s : servers_) rows.push_back(serverRowLabel(s));
        rows.emplace_back("Somewhere else...");

        const bool known = selectedServerIndex_ >= 0 &&
                           selectedServerIndex_ < static_cast<int>(servers_.size());
        const std::string preview =
            known ? serverRowLabel(servers_[static_cast<size_t>(selectedServerIndex_)])
                  : makeServerKey(hostname_.text(), port);

        int choice = known ? selectedServerIndex_ : static_cast<int>(servers_.size());
        ui_.text(col.at(), "Server", labelSize, theme.inkSoft);
        col.gap(labelRow);
        const auto [a, b] = col.row(px(kFieldHeight));
        if (ui_.dropdown("servers", a, b, preview, rows, &choice)) {
            if (choice >= static_cast<int>(servers_.size())) {
                // "Somewhere else" is a request for the address box, which
                // lives behind the disclosure - so open it, or the choice
                // does nothing visible and there is nowhere to type.
                selectedServerIndex_ = -1;
                advancedOpen_ = true;
            } else {
                selectServerProfile(choice);
            }
        }
        col.gap(px(kRowGap));
    }

    {
        PaperUI::FieldOpts opts;
        opts.placeholder = "account name";
        const PaperUI::FieldResult r = labelledField("Account", "account", username_, opts);
        if (r.changed) statusProminent = false;
        if (r.submitted) ui_.focus("password");
    }

    {
        // The eye sits on the label row, right-aligned, so the box itself is
        // the same shape as the one above it.
        ui_.text(col.at(), "Password", labelSize, theme.inkSoft);
        const float eyeR = labelSize * 0.72f;
        if (ui_.glyphButton("reveal", ImVec2(col.x1 - eyeR, col.y + labelSize * 0.5f), eyeR,
                            showPassword ? PaperUI::Glyph::Eye : PaperUI::Glyph::EyeClosed)) {
            showPassword = !showPassword;
        }
        col.gap(labelRow);

        const bool hadPlaceholder = usingStoredHash && password_.text() == PASSWORD_PLACEHOLDER;
        const auto [a, b] = col.row(px(kFieldHeight));
        PaperUI::FieldOpts opts;
        opts.password = !showPassword;
        opts.placeholder = "password";
        const PaperUI::FieldResult r = ui_.field("password", a, b, password_, opts);
        col.gap(px(kRowGap));

        // A remembered password is a stored hash behind eight placeholder
        // bytes; there is nothing in the box worth editing. Taking the
        // keyboard therefore offers the whole of it up for replacement, so
        // the first keystroke starts a new password rather than appending to
        // a value that is not one.
        if (hadPlaceholder && r.focused && !passwordFocused_) password_.selectAll();
        passwordFocused_ = r.focused;

        if (r.changed) statusProminent = false;
        if (r.submitted) submit = true;
    }

    if (codeInMain) {
        ui_.text(col.at(), "Security code", labelSize, theme.inkSoft);
        ui_.textRight(col.x1, col.y + (labelSize - smallSize) * 0.5f,
                      authState == auth::AuthState::AUTHENTICATOR_REQUIRED ? "authenticator"
                                                                           : "PIN",
                      smallSize, theme.pencil);
        col.gap(labelRow);
        const auto [a, b] = col.row(px(kFieldHeight));
        PaperUI::FieldOpts opts;
        opts.password = true;
        opts.digitsOnly = true;
        opts.placeholder = "code";
        const PaperUI::FieldResult r = ui_.field("pin", a, b, pinCode_, opts);
        col.gap(px(kRowGap));
        if (r.submitted) submit = true;

        if (!securityPromptFocused_) {
            ui_.focus("pin");
            securityPromptFocused_ = true;
        }
    }

    // ---- which assets ----------------------------------------------------
    if (chooseAssets) {
        int choice = 0;
        for (int i = 0; i < static_cast<int>(assets.size()); ++i) {
            if (assets[static_cast<size_t>(i)].id == assetProfileId_) { choice = i; break; }
        }
        std::vector<std::string> rows;
        rows.reserve(assets.size());
        for (const AssetChoice& one : assets) rows.push_back(one.label);

        ui_.text(col.at(), "Assets", labelSize, theme.inkSoft);
        col.gap(labelRow);
        const auto [a, b] = col.row(px(kFieldHeight));
        if (ui_.dropdown("assets", a, b, rows[static_cast<size_t>(choice)], rows, &choice)) {
            assetProfileId_ = assets[static_cast<size_t>(choice)].id;
            core::Application::getInstance().setAssetExpansionOverride(assetProfileId_);
            core::Application::getInstance().reloadExpansionData();
        }
        col.gap(px(kRowGap));
        if (!assetProfileId_.empty()) {
            ui_.text(col.at(), "Cross-expansion DBC and model formats may differ.", smallSize,
                     theme.pencil);
            col.gap(smallRow + px(4));
        }
    }

    // ---- what is installed ------------------------------------------------
    if (troubleH > 0.0f) {
        ui_.wrapped(col.at(), contentW, trouble.c_str(), smallSize, theme.crayonRed);
        col.gap(troubleH);
    } else if (haveH > 0.0f) {
        ui_.wrapped(col.at(), contentW, haveLine.c_str(), smallSize, theme.pencil);
        col.gap(haveH);
    }

    // ---- status ---------------------------------------------------------
    if (statusH > 0.0f) {
        ui_.wrapped(col.at(), contentW, statusMessage.c_str(), bodySize,
                    statusIsError ? theme.crayonRed : theme.crayonGreen);
        col.gap(statusH + px(10));
    }

    // ---- the one button --------------------------------------------------
    {
        const float w = px(200);
        const auto [a, b] = col.cell((contentW - w) * 0.5f, w, px(kButtonHeight));
        col.gap(px(kRowGap));

        char label[64];
        bool enabled = true;
        if (codeInMain) {
            std::snprintf(label, sizeof(label), "Submit Code");
        } else if (authenticating) {
            std::snprintf(label, sizeof(label), "Connecting  %.0fs", authTimer);
            enabled = false;
        } else {
            std::snprintf(label, sizeof(label), "Log In");
        }
        if (ui_.button("login", a, b, label, PaperUI::ButtonKind::Primary, enabled)) submit = true;
    }

    // ---- the disclosure --------------------------------------------------
    {
        const char* label = advancedOpen_ ? "fewer options" : "more options";
        const float w = ui_.textWidth(label, smallSize);
        if (ui_.link("advanced", ImVec2(centreX - w * 0.5f, col.y), label, smallSize)) {
            advancedOpen_ = !advancedOpen_;
        }
        col.gap(smallRow + px(kRowGap));
    }

    // ---- server, expansion, assets ---------------------------------------
    if (advancedOpen_) {
        ui_.rule(ImVec2(col.x0, col.y), ImVec2(col.x1, col.y), paperFade(theme.pencil, 0.6f),
                 px(1.0f), 0x2C71u);
        col.gap(px(kRowGap) + px(2));

        auto& app = core::Application::getInstance();

        // The server list itself is in the card now, above the account it
        // belongs with. What stays here is the address behind it, for the
        // server that is not on the list.

        // Address and port, on one row, because they are one address.
        {
            const float portW = px(84);
            const float gap = px(12);
            const float hostW = contentW - portW - gap;
            ui_.text(col.at(), "Address", labelSize, theme.inkSoft);
            ui_.text(ImVec2(col.x0 + hostW + gap, col.y), "Port", labelSize, theme.inkSoft);
            col.gap(labelRow);

            const float y = col.y;
            PaperUI::FieldOpts hostOpts;
            hostOpts.placeholder = "logon.example.com";
            const PaperUI::FieldResult h = ui_.field(
                "hostname", ImVec2(col.x0, y), ImVec2(col.x0 + hostW, y + px(kFieldHeight)),
                hostname_, hostOpts);

            PaperUI::FieldOpts portOpts;
            portOpts.digitsOnly = true;
            portOpts.placeholder = "3724";
            const PaperUI::FieldResult p = ui_.field(
                "port", ImVec2(col.x1 - portW, y), ImVec2(col.x1, y + px(kFieldHeight)),
                portText_, portOpts);
            col.gap(px(kFieldHeight) + px(kRowGap));

            if (h.changed || p.changed) selectedServerIndex_ = -1;
            if (h.submitted || p.submitted) submit = true;
            // An empty box is the default port rather than an error: it is a
            // field halfway through being retyped, and there is only one port
            // anyone means by leaving it blank.
            port = portText_.empty()
                ? 3724
                : std::clamp(std::atoi(portText_.c_str()), 1, 65535);
        }

        // Expansion.
        if (haveExpansions) {
            const auto& profiles = registry->getAllProfiles();

            ui_.text(col.at(), "Expansion", labelSize, theme.inkSoft);
            col.gap(labelRow);
            std::vector<std::string> rows;
            rows.reserve(profiles.size());
            for (const auto& p : profiles)
                rows.push_back(p.shortName + "  (" + p.versionString() + ")");
            const std::string preview =
                (expansionIndex >= 0 && expansionIndex < static_cast<int>(profiles.size()))
                    ? rows[static_cast<size_t>(expansionIndex)]
                    : std::string("choose one");
            {
                const auto [a, b] = col.row(px(kFieldHeight));
                if (ui_.dropdown("expansion", a, b, preview, rows, &expansionIndex)) {
                    registry->setActive(profiles[static_cast<size_t>(expansionIndex)].id);
                    app.reloadExpansionData();
                }
                col.gap(px(kRowGap));
            }

        } else {
            ui_.text(col.at(), "Expansion: WotLK 3.3.5a (default)", smallSize, theme.pencil);
            col.gap(smallRow + px(kRowGap));
        }

        if (codeInAdvanced) {
            ui_.text(col.at(), "Security code", labelSize, theme.inkSoft);
            ui_.textRight(col.x1, col.y + (labelSize - smallSize) * 0.5f, "if your realm asks",
                          smallSize, theme.pencil);
            col.gap(labelRow);
            const auto [a, b] = col.row(px(kFieldHeight));
            PaperUI::FieldOpts opts;
            opts.password = true;
            opts.digitsOnly = true;
            opts.placeholder = "PIN or authenticator";
            if (ui_.field("pin", a, b, pinCode_, opts).submitted) submit = true;
            col.gap(px(kRowGap));
        }

#ifndef __ANDROID__
        // Where the game data is read from, and a way to choose. The heading
        // and the link share a row, and the path wraps under them: the card is
        // already the tallest thing on the screen with this section open.
        // Android chooses its folder before the client starts.
        {
            auto& picker = dataFolderPicker();
            ui_.text(col.at(), "Game data", labelSize, theme.inkSoft);
            const char* chooseLabel = picker.isOpen() ? "choosing..." : "choose folder...";
            const float chooseW = ui_.textWidth(chooseLabel, smallSize);
            if (ui_.link("datafolder",
                         ImVec2(col.x1 - chooseW, col.y + (labelSize - smallSize) * 0.5f),
                         chooseLabel, smallSize) &&
                !picker.isOpen()) {
                auto* window = core::Application::getInstance().getWindow();
                picker.open(window ? window->getSDLWindow() : nullptr);
            }
            col.gap(labelRow);

            const std::string folder = DataFolderPicker::currentFolder();
            col.gap(ui_.wrapped(col.at(), contentW, folder.c_str(), smallSize, theme.pencil));
            if (DataFolderPicker::hasCustomFolder()) {
                const char* label = "use the default folder";
                if (ui_.link("datafolderdefault", col.at(), label, smallSize)) {
                    picker.useDefault();
                }
                col.gap(smallRow);
            }
            if (!picker.message().empty()) {
                col.gap(px(4));
                col.gap(ui_.wrapped(col.at(), contentW, picker.message().c_str(), smallSize,
                                    theme.inkSoft));
            }
            col.gap(px(kRowGap));
        }
#endif

#ifdef WOWEE_HAVE_ASSET_PANEL
        // The way back to the asset builder once there is already something
        // installed. It is the screen a first run opens on, and without a
        // door to it from here the only way to add a second game - or to
        // rebuild the one that is there - was to go and find the separate
        // program that draws the same panel.
        {
            // A link rather than a button, and nothing explaining it. With
            // this section open the card is already the tallest thing on the
            // screen - a button's worth of height pushed the footer off the
            // bottom edge - and the panel it opens says all of this at the
            // top of itself. Same shape as the disclosure above it.
            const char* label = "add or rebuild assets...";
            const float w = ui_.textWidth(label, smallSize);
            if (ui_.link("assets", ImVec2(centreX - w * 0.5f, col.y), label, smallSize)) {
                core::Application::getInstance().setState(core::AppState::FIRST_RUN);
            }
            col.gap(smallRow + px(kRowGap));
        }
#endif
    }

    // ---- footer ----------------------------------------------------------
    {
        col.gap(px(10));
        ui_.rule(ImVec2(col.x0, col.y - px(6)), ImVec2(col.x1, col.y - px(6)),
                 paperFade(theme.pencil, 0.5f), px(1.0f), 0x77A3u);

        std::string where = makeServerKey(hostname_.text(), port);
        if (auto* registry = core::Application::getInstance().getExpansionRegistry())
            if (const auto* active = registry->getActive())
                where += "  \xc2\xb7  " + active->shortName;
        ui_.text(col.at(), where.c_str(), smallSize, theme.pencil);

        const float r = smallSize * 0.86f;
        const float cy = col.y + smallRow * 0.5f - px(1);
        if (ui_.glyphButton("quit", ImVec2(col.x1 - r, cy), r, PaperUI::Glyph::Cross)) {
            if (auto* window = core::Application::getInstance().getWindow())
                window->setShouldClose(true);
        }
        if (ui_.glyphButton("settings", ImVec2(col.x1 - r * 3.4f, cy), r,
                            PaperUI::Glyph::Gear)) {
            openLoginSettingsSheet();
            // The sheet lands under the pointer, and one of its controls is
            // wherever this gear is. Without this the press carries into it:
            // at 1280x760 the gear sits on the ground-clutter slider, which
            // takes the value it was dropped on and drags while held.
            ui_.swallowPress();
        }
    }

    if (submit && !authenticating) {
        attemptAuth(authHandler);
    } else if (submit && waitingForSecurityCode) {
        const std::string code = trimAscii(pinCode_.text());
        if (!code.empty()) {
            authHandler.submitSecurityCode(code);
            pinCode_.clear();
            pinAutoSubmitted_ = true;
        }
    }
}

// ---------------------------------------------------------------------------
// Background art and login music
// ---------------------------------------------------------------------------

// The art is drawn straight into ImGui's background list rather than through
// PaperUI: it is behind everything by definition, and the realm and character
// screens ask for it before they have begun a frame of their own.

void AuthScreen::drawBackdrop() {
    // Kick the decode off once, then upload on whichever frame it lands.
    if (!bgDecodeStarted) {
        bgDecodeStarted = true;
        std::string imgPath = "assets/krayonsignin.png";
        if (!std::filesystem::exists(imgPath))
            imgPath = (std::filesystem::current_path() / imgPath).string();
        bgDecodeFuture = std::async(std::launch::async, [imgPath]() {
            DecodedBackground out;
            int channels = 0;
            stbi_set_flip_vertically_on_load(false);
            unsigned char* data = stbi_load(imgPath.c_str(), &out.width, &out.height, &channels, 4);
            if (data) {
                out.pixels.assign(data, data + static_cast<size_t>(out.width) * out.height * 4);
                stbi_image_free(data);
            }
            return out;
        });
    }
    if (!bgInitAttempted && bgDecodeFuture.valid() &&
        bgDecodeFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        bgInitAttempted = true;
        DecodedBackground decoded = bgDecodeFuture.get();
        if (!decoded.pixels.empty()) {
            bgWidth = decoded.width;
            bgHeight = decoded.height;
            uploadBackgroundImage(decoded.pixels.data());
        } else {
            LOG_WARNING("Auth screen: failed to decode background image");
        }
    }
    if (!bgDescriptorSet) return;

    const ImVec2 screen = ImGui::GetIO().DisplaySize;
    const float imgW = static_cast<float>(bgWidth);
    const float imgH = static_cast<float>(bgHeight);
    if (imgW <= 0.0f || imgH <= 0.0f) return;

    const float screenAspect = screen.x / screen.y;
    const float imgAspect = imgW / imgH;
    ImVec2 uv0(0.0f, 0.0f);
    ImVec2 uv1(1.0f, 1.0f);
    if (imgAspect > screenAspect) {
        const float crop = (1.0f - screenAspect / imgAspect) * 0.5f;
        uv0.x = crop;
        uv1.x = 1.0f - crop;
    } else if (imgAspect < screenAspect) {
        const float crop = (1.0f - imgAspect / screenAspect) * 0.5f;
        uv0.y = crop;
        uv1.y = 1.0f - crop;
    }
    ImGui::GetBackgroundDrawList()->AddImage(reinterpret_cast<ImTextureID>(bgDescriptorSet),
                                             ImVec2(0, 0), ImVec2(screen.x, screen.y), uv0, uv1);
}

void AuthScreen::updateMusic() {
    auto& app = core::Application::getInstance();
    auto* ac = app.getAudioCoordinator();
    if (!musicInitAttempted) {
        musicInitAttempted = true;
        auto* assets = app.getAssetManager();
        if (ac) {
            auto* music = ac->getMusicManager();
            if (music && assets && assets->isInitialized() && !music->isInitialized()) {
                music->initialize(assets);
            }
        }
    }
    if (!ac) return;
    auto* music = ac->getMusicManager();
    if (!music) return;

    if (!loginMusicVolumeAdjusted_) {
        savedMusicVolume_ = music->getVolume();
        // Reduce music to 80% during login so UI button clicks and error sounds
        // remain audible over the background track
        int loginVolume = (savedMusicVolume_ * 80) / 100;
        loginVolume = std::clamp(loginVolume, 0, 100);
        music->setVolume(loginVolume);
        loginMusicVolumeAdjusted_ = true;
    }
    music->update(ImGui::GetIO().DeltaTime);
    if (music->isPlaying() || music->isLoading()) return;

    if (!introTracksScanned_) {
        introTracksScanned_ = true;
        const std::filesystem::path relative =
            std::filesystem::path("assets") / "Original Music" / "TavernAllianceREMIX.mp3";
        if (std::filesystem::exists(relative)) {
            loginTrackPath_ = relative.string();
        } else {
            const auto absolute = std::filesystem::current_path() / relative;
            if (std::filesystem::exists(absolute)) {
                loginTrackPath_ = absolute.string();
            }
        }
    }

    if (!loginTrackPath_.empty()) {
        music->playFilePath(loginTrackPath_, true, 1800.0f);
        // The read runs on a worker, so the track is not playing yet.
        // Treat a load in flight as success or the track gets dropped below.
        musicPlaying = music->isPlaying() || music->isLoading();
        if (musicPlaying) {
            LOG_INFO("AuthScreen: Playing login intro track: ", loginTrackPath_);
        } else {
            // Avoid retrying a bad file every frame. There is deliberately
            // no fallback: the login screen is constrained to this track.
            loginTrackPath_.clear();
        }
    } else if (!missingIntroTracksLogged_) {
        LOG_WARNING("AuthScreen: Login track not found: assets/Original Music/TavernAllianceREMIX.mp3");
        missingIntroTracksLogged_ = true;
    }
}

void AuthScreen::stopLoginMusic() {
    auto& app = core::Application::getInstance();
    auto* ac = app.getAudioCoordinator();
    if (!ac) return;
    auto* music = ac->getMusicManager();
    if (!music) return;
    if (musicPlaying) {
        music->stopMusic(500.0f);
        musicPlaying = false;
    }
    if (loginMusicVolumeAdjusted_) {
        music->setVolume(savedMusicVolume_);
        loginMusicVolumeAdjusted_ = false;
    }
}

void AuthScreen::attemptAuth(auth::AuthHandler& authHandler) {
    // Validate inputs
    if (username_.empty()) {
        setStatus("Enter an account name", true);
        return;
    }

    // Check if using stored hash (password field contains placeholder)
    const bool useHash = usingStoredHash && password_.text() == PASSWORD_PLACEHOLDER;

    if (!useHash && password_.empty()) {
        setStatus("Enter a password", true);
        return;
    }

    if (hostname_.empty()) {
        setStatus("Enter an address to connect to", true);
        return;
    }

    // Build the auth-protocol candidate chain for this attempt. The profile's
    // own value is always tried first; vanilla-family profiles get the other
    // vanilla protocol byte appended as a fallback, because vanilla-family
    // servers disagree on it (Turtle/vmangos-derived: 8, older mangos/cmangos:
    // 3) and the profile can only name one. TBC/WotLK have no such ambiguity.
    authProtocols_.clear();
    authProtocolAttempt_ = 0;
    {
        uint8_t primary = 8;
        bool vanillaFamily = false;
        auto* reg = core::Application::getInstance().getExpansionRegistry();
        if (reg) {
            if (auto* profile = reg->getActive()) {
                primary = profile->protocolVersion;
                vanillaFamily = (profile->id == "classic" || profile->id == "turtle");
            }
        }
        authProtocols_.push_back(primary);
        if (vanillaFamily) {
            const uint8_t alternate = (primary == 3) ? uint8_t{8} : uint8_t{3};
            if (alternate != primary) authProtocols_.push_back(alternate);
        }
    }

    beginAuthAttempt(authHandler);
}

void AuthScreen::beginAuthAttempt(auth::AuthHandler& authHandler) {
    const bool useHash = usingStoredHash && password_.text() == PASSWORD_PLACEHOLDER;
    const uint8_t protocolVersion = (authProtocolAttempt_ < authProtocols_.size())
        ? authProtocols_[authProtocolAttempt_] : uint8_t{8};
    const bool isRetry = (authProtocolAttempt_ > 0);

    std::stringstream ss;
    if (isRetry) {
        ss << "Retrying " << hostname_.text() << ":" << port
           << " with auth protocol " << static_cast<int>(protocolVersion) << "...";
    } else {
        ss << "Connecting to " << hostname_.text() << ":" << port << "...";
    }
    setStatus(ss.str(), false);

    // Wire up failure callback to capture specific error reason
    failureReason.clear();
    authHandler.setOnFailure([this](const std::string& reason) {
        failureReason = reason;
    });

    // Configure client version from active expansion profile
    auto* reg = core::Application::getInstance().getExpansionRegistry();
    if (reg) {
        auto* profile = reg->getActive();
        if (profile) {
            auth::ClientInfo info;
            info.majorVersion = profile->majorVersion;
            info.minorVersion = profile->minorVersion;
            info.patchVersion = profile->patchVersion;
            info.build = profile->build;
            info.protocolVersion = protocolVersion;
            info.game = profile->game;
            info.platform = profile->platform;
            info.os = profile->os;
            info.locale = profile->locale;
            info.timezone = profile->timezone;
            // Vanilla-family servers send the legacy realm-list layout even
            // when the auth handshake itself uses protocol v8 (vmangos), so key
            // this on the expansion rather than on the protocol byte in play.
            info.legacyVanillaRealmList = (profile->id == "classic" ||
                                           profile->id == "turtle" ||
                                           profile->protocolVersion <= 3);
            authHandler.setClientInfo(info);
        }
    }

    if (authHandler.connect(hostname_.text(), static_cast<uint16_t>(port))) {
        authenticating = true;
        authTimer = 0.0f;
        setStatus(isRetry ? "Reconnected, authenticating..." : "Connected, authenticating...", false);
        pinAutoSubmitted_ = false;
        securityPromptFocused_ = false;

        // Save login info for next session
        saveLoginInfo(false);

        const std::string pinStr = trimAscii(pinCode_.text());

        // Send authentication credentials
        if (useHash) {
            auto hashBytes = hexDecode(savedPasswordHash);
            authHandler.authenticateWithHash(username_.text(), hashBytes, pinStr);
        } else {
            usingStoredHash = false;
            authHandler.authenticate(username_.text(), password_.text(), pinStr);
        }

        // Don't keep the code around longer than needed.
        pinCode_.clear();
    } else {
        std::stringstream errSs;
        errSs << "Failed to connect to " << hostname_.text() << ":" << port
              << " - check that the server is online and the address is correct";
        setStatus(errSs.str(), true);
        authenticating = false;
    }
}

void AuthScreen::setStatus(const std::string& message, bool isError, bool prominent) {
    statusMessage = message;
    statusIsError = isError;
    statusProminent = prominent;
}

std::string AuthScreen::getConfigPath() {
    return core::getConfigRoot() + "/login.cfg";
}

void AuthScreen::saveLoginInfo(bool includePasswordHash) {
    upsertCurrentServerProfile(includePasswordHash);

    std::string path = getConfigPath();
    std::filesystem::path dir = std::filesystem::path(path).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::ofstream out(path);
    if (!out.is_open()) {
        LOG_WARNING("Could not save login info to ", path);
        return;
    }

    out << "version=3\n";
    out << "active=" << makeServerKey(hostname_.text(), port) << "\n";

    for (const auto& s : servers_) {
        out << "\n[server " << makeServerKey(s.hostname, s.port) << "]\n";
        out << "username=" << s.username << "\n";
        if (!s.passwordHash.empty()) {
            out << "password_hash=" << s.passwordHash << "\n";
        }
        if (!s.expansionId.empty()) {
            out << "expansion=" << s.expansionId << "\n";
        }
        if (!s.assetProfileId.empty()) {
            out << "assets=" << s.assetProfileId << "\n";
        }
    }

    LOG_INFO("Login info saved to ", path);
}

void AuthScreen::loadLoginInfo() {
    std::string path = getConfigPath();
    std::ifstream in(path);
    if (!in.is_open()) return;

    std::string file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // If this looks like the old flat format, migrate it into a single server entry.
    if (file.find("[server ") == std::string::npos) {
        std::unordered_map<std::string, std::string> kv;
        std::istringstream ss(file);
        std::string line;
        while (std::getline(ss, line)) {
            line = trimAscii(line);
            if (line.empty() || line[0] == '#') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            kv[trimAscii(line.substr(0, eq))] = trimAscii(line.substr(eq + 1));
        }

        std::string host = kv["hostname"];
        int p = 3724;
        try { if (!kv["port"].empty()) p = std::stoi(kv["port"]); } catch (...) {}
        if (!host.empty()) {
            ServerProfile s;
            s.hostname = host;
            s.port = p;
            s.username = kv["username"];
            s.passwordHash = kv["password_hash"];
            s.expansionId = kv["expansion"];
            s.assetProfileId = kv["assets"];
            servers_.push_back(std::move(s));
            selectServerProfile(0);
        }

        LOG_INFO("Login info loaded from ", path, " (migrated legacy profile -> v3)");
        return;
    }

    servers_.clear();
    selectedServerIndex_ = -1;

    std::string activeKey;
    ServerProfile current;
    bool inServer = false;

    auto flushServer = [&]() {
        if (!inServer) return;
        if (!current.hostname.empty() && current.port > 0) {
            servers_.push_back(current);
        }
        current = ServerProfile{};
        inServer = false;
    };

    std::istringstream ss(file);
    std::string line;
    while (std::getline(ss, line)) {
        line = trimAscii(line);
        if (line.empty() || line[0] == '#') continue;

        if (line.front() == '[' && line.back() == ']') {
            flushServer();
            std::string inside = line.substr(1, line.size() - 2);
            inside = trimAscii(inside);
            const std::string prefix = "server ";
            if (inside.rfind(prefix, 0) == 0) {
                std::string key = trimAscii(inside.substr(prefix.size()));
                // Parse host:port (split on last ':', allow [ipv6]:port).
                std::string hostPart = key;
                int portPart = 3724;
                if (!key.empty() && key.front() == '[') {
                    auto rb = key.find(']');
                    if (rb != std::string::npos) {
                        hostPart = key.substr(1, rb - 1);
                        auto colon = key.find(':', rb);
                        if (colon != std::string::npos) {
                            try { portPart = std::stoi(key.substr(colon + 1)); } catch (...) {}
                        }
                    }
                } else {
                    auto colon = key.rfind(':');
                    if (colon != std::string::npos) {
                        hostPart = key.substr(0, colon);
                        try { portPart = std::stoi(key.substr(colon + 1)); } catch (...) {}
                    }
                }

                current.hostname = hostPart;
                current.port = portPart;
                inServer = true;
            }
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trimAscii(line.substr(0, eq));
        std::string val = trimAscii(line.substr(eq + 1));

        if (!inServer) {
            if (key == "active") activeKey = val;
            continue;
        }

        if (key == "username") current.username = val;
        else if (key == "password_hash") current.passwordHash = val;
        else if (key == "expansion") current.expansionId = val;
        else if (key == "assets") current.assetProfileId = val;
    }
    flushServer();

    if (!servers_.empty()) {
        std::sort(servers_.begin(), servers_.end(),
                  [](const ServerProfile& a, const ServerProfile& b) {
                      if (a.hostname != b.hostname) return a.hostname < b.hostname;
                      return a.port < b.port;
                  });

        if (!activeKey.empty()) {
            for (int i = 0; i < static_cast<int>(servers_.size()); ++i) {
                if (makeServerKey(servers_[i].hostname, servers_[i].port) == activeKey) {
                    selectServerProfile(i);
                    break;
                }
            }
        }

        if (selectedServerIndex_ < 0) {
            selectServerProfile(0);
        }
    }

    LOG_INFO("Login info loaded from ", path);
}

static uint32_t findMemType(VkPhysicalDevice pd, uint32_t filter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((filter & (1 << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    LOG_ERROR("AuthScreen: no suitable memory type found");
    return UINT32_MAX;
}

// Takes pixels already decoded on a worker thread and does the GPU-side work,
// which has to happen on the main thread.
bool AuthScreen::uploadBackgroundImage(const unsigned char* data) {
    auto& app = core::Application::getInstance();
    auto* renderer = app.getRenderer();
    if (!renderer) return false;
    bgVkCtx = renderer->getVkContext();
    if (!bgVkCtx) return false;
    if (!data) return false;

    VkDevice device = bgVkCtx->getDevice();
    VkPhysicalDevice physDevice = bgVkCtx->getPhysicalDevice();
    VkDeviceSize imageSize = static_cast<VkDeviceSize>(bgWidth) * bgHeight * 4;

    // Staging buffer
    VkBuffer stagingBuffer;
    VkDeviceMemory stagingMemory;
    {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = imageSize;
        bufInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bufInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        vkCreateBuffer(device, &bufInfo, nullptr, &stagingBuffer);

        VkMemoryRequirements memReqs;
        vkGetBufferMemoryRequirements(device, stagingBuffer, &memReqs);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemType(physDevice, memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkAllocateMemory(device, &allocInfo, nullptr, &stagingMemory);
        vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0);

        void* mapped;
        vkMapMemory(device, stagingMemory, 0, imageSize, 0, &mapped);
        memcpy(mapped, data, imageSize);
        vkUnmapMemory(device, stagingMemory);
    }
    // The pixels belong to the caller's decoded buffer, not to stb_image.

    // Create VkImage
    {
        VkImageCreateInfo imgInfo{};
        imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imgInfo.imageType = VK_IMAGE_TYPE_2D;
        imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imgInfo.extent = {.width = static_cast<uint32_t>(bgWidth), .height = static_cast<uint32_t>(bgHeight), .depth = 1};
        imgInfo.mipLevels = 1;
        imgInfo.arrayLayers = 1;
        imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        imgInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCreateImage(device, &imgInfo, nullptr, &bgImage);

        VkMemoryRequirements memReqs;
        vkGetImageMemoryRequirements(device, bgImage, &memReqs);
        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memReqs.size;
        allocInfo.memoryTypeIndex = findMemType(physDevice, memReqs.memoryTypeBits,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkAllocateMemory(device, &allocInfo, nullptr, &bgMemory);
        vkBindImageMemory(device, bgImage, bgMemory, 0);
    }

    // Transfer
    bgVkCtx->immediateSubmit([&](VkCommandBuffer cmd) {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = bgImage;
        barrier.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        VkDependencyInfo barrierDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        barrierDep.dependencyFlags = 0;
        barrierDep.imageMemoryBarrierCount = 1;
        barrierDep.pImageMemoryBarriers = &barrier;
        rendering::cmdPipelineBarrier2(cmd, barrierDep);

        VkBufferImageCopy region{};
        region.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1};
        region.imageExtent = {.width = static_cast<uint32_t>(bgWidth), .height = static_cast<uint32_t>(bgHeight), .depth = 1};
        vkCmdCopyBufferToImage(cmd, stagingBuffer, bgImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        barrier.srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkDependencyInfo toReadDep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        toReadDep.imageMemoryBarrierCount = 1;
        toReadDep.pImageMemoryBarriers = &barrier;
        rendering::cmdPipelineBarrier2(cmd, toReadDep);
    });

    vkDestroyBuffer(device, stagingBuffer, nullptr);
    vkFreeMemory(device, stagingMemory, nullptr);

    // Image view
    {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = bgImage;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewInfo.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .baseMipLevel = 0, .levelCount = 1, .baseArrayLayer = 0, .layerCount = 1};
        vkCreateImageView(device, &viewInfo, nullptr, &bgImageView);
    }

    // Sampler
    {
        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        bgSampler = bgVkCtx->getOrCreateSampler(samplerInfo);
    }

    bgDescriptorSet = ImGui_ImplVulkan_AddTexture(bgSampler, bgImageView,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    LOG_INFO("Auth screen background loaded: ", bgWidth, "x", bgHeight);
    return true;
}
// ---------------------------------------------------------------------------
// Login-screen graphics settings sheet
// ---------------------------------------------------------------------------

namespace {

/// Whether the client's words are German: the expansion profile's locale, the
/// same test the options pages make (WoweeSettingList).
bool clientSpeaksGerman() {
    if (auto* registry = core::Application::getInstance().getExpansionRegistry();
        registry && registry->getActive()) {
        return settingsLocaleGerman(registry->getActive()->locale);
    }
    return false;
}

/// The graphics pages in the order the game's Video frame lists them: the
/// schema's order, with the Debug pages last in their own (kLastOnVideo in
/// the options pages' Lua).
const std::vector<std::string>& sheetCategories() {
    static const std::vector<std::string> order = [] {
        static constexpr const char* kLast[] = {
            "Debug Graphics", "Debug Time", "Debug Lights", "Debug Glow", "Debug Shadows",
        };
        const auto isLast = [](const std::string& c) {
            for (const char* l : kLast) {
                if (c == l) return true;
            }
            return false;
        };
        std::vector<std::string> out;
        std::size_t count = 0;
        const SettingDesc* rows = clientSettingsSchema(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::string c = rows[i].category;
            if (!isGraphicsCategory(c) || isLast(c)) continue;
            if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
        }
        for (const char* l : kLast) {
            if (isGraphicsCategory(l)) out.emplace_back(l);
        }
        return out;
    }();
    return order;
}

/// Every row on the sheet's pages.
std::vector<const SettingDesc*> sheetRows() {
    std::vector<const SettingDesc*> out;
    std::size_t count = 0;
    const SettingDesc* rows = clientSettingsSchema(count);
    for (std::size_t i = 0; i < count; ++i) {
        if (isGraphicsCategory(rows[i].category) && rows[i].store[0] == '\0') out.push_back(&rows[i]);
    }
    return out;
}

std::vector<std::string> splitChoices(const std::string& text) {
    std::vector<std::string> out;
    std::size_t at = 0;
    while (true) {
        const std::size_t bar = text.find('|', at);
        out.push_back(text.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
        if (bar == std::string::npos) break;
        at = bar + 1;
    }
    return out;
}

/// As many decimals as the row's step has.
const char* sliderFormat(float step) {
    if (step >= 1.0f) return "%.0f";
    if (step >= 0.1f - 1e-4f) return "%.1f";
    return "%.2f";
}

/// The text the interface has for a GlobalStrings name, or "" before there
/// is an interface - the same lookup the client's own lines use.
std::string interfaceGlobal(const char* name) { return game::interfaceString(name, ""); }

/// What a greyed row waits on, in words: one line per clause of its
/// enabledWhen, as the options pages say it (clauseInWords).
std::string waitingInWords(const SettingDesc& desc, bool german) {
    std::string out;
    const std::string all = desc.enabledWhen;
    std::size_t start = 0;
    while (start <= all.size()) {
        const std::size_t amp = all.find('&', start);
        const std::string clause = all.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        start = amp == std::string::npos ? all.size() + 1 : amp + 1;
        if (clause.empty()) continue;
        std::string key = clause, want;
        bool negated = false, compare = false;
        if (const std::size_t bang = clause.find("!="); bang != std::string::npos) {
            key = clause.substr(0, bang);
            want = clause.substr(bang + 2);
            negated = compare = true;
        } else if (const std::size_t eq = clause.find('='); eq != std::string::npos) {
            key = clause.substr(0, eq);
            want = clause.substr(eq + 1);
            compare = true;
        }
        std::string name = key, choice = want;
        std::size_t count = 0;
        const SettingDesc* rows = clientSettingsSchema(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (key != rows[i].key) continue;
            const SettingText text = settingText(rows[i], german, interfaceGlobal);
            name = text.label;
            if (compare && !text.choices.empty()) {
                const auto choices = splitChoices(text.choices);
                const int index = std::atoi(want.c_str());
                if (index >= 0 && index < static_cast<int>(choices.size())) choice = choices[static_cast<size_t>(index)];
            }
            break;
        }
        if (!out.empty()) out += "\n";
        if (!compare) {
            out += german ? "Verfügbar, wenn " + name + " an ist." : "Available when " + name + " is on.";
        } else if (negated) {
            out += german ? "Verfügbar, außer bei " + name + ": " + choice + "."
                          : "Available when " + name + " is not " + choice + ".";
        } else {
            out += german ? "Verfügbar bei " + name + ": " + choice + "."
                          : "Available when " + name + " is " + choice + ".";
        }
    }
    return out;
}

}  // namespace

void AuthScreen::openLoginSettingsSheet() {
    settingsOpen_ = true;
    confirmGraphicsReset_ = false;
    if (sheetCategory_.empty() && !sheetCategories().empty()) sheetCategory_ = sheetCategories().front();
    // What Cancel puts back: every row, as it was.
    sheetOpened_.clear();
    if (settingsAccess_.get) {
        for (const SettingDesc* row : sheetRows()) sheetOpened_.emplace_back(row->key, settingsAccess_.get(row->key));
    }
}

void AuthScreen::closeLoginSettingsSheet(bool keep) {
    settingsOpen_ = false;
    confirmGraphicsReset_ = false;
    if (keep) {
        if (settingsAccess_.save) settingsAccess_.save();
    } else if (settingsAccess_.get && settingsAccess_.set) {
        // Only what differs, as the options pages' Cancel does: writing a
        // setting applies it, and full screen or vertical sync written back
        // unchanged would rebuild the window for nothing. The preset first,
        // since choosing one assigns the rows after it.
        for (int pass = 0; pass < 2; ++pass) {
            for (const auto& [key, was] : sheetOpened_) {
                if ((key == "graphicspreset") != (pass == 0)) continue;
                const std::string now = settingsAccess_.get(key);
                if (now == was) continue;
                const char* a = now.c_str();
                const char* b = was.c_str();
                char* endA = nullptr;
                char* endB = nullptr;
                const double x = std::strtod(a, &endA);
                const double y = std::strtod(b, &endB);
                if (endA != a && endB != b && std::abs(x - y) <= 1e-5 * std::max(1.0, std::abs(y))) continue;
                settingsAccess_.set(key, was);
            }
        }
    }
    sheetOpened_.clear();
}

void AuthScreen::renderRtCrashDialog(float screenW, float screenH) {
    namespace rtm = rendering::rt_memory;
    const bool german = clientSpeaksGerman();
    rtm::Refusal r;
    r.reason = rtm::Reason::PreviousSession;
    r.needed = rtMarkerTracingMB_ * rtm::kMiB;
    r.available = rtMarkerAvailableMB_ * rtm::kMiB;
    r.switchedOff = false;
    const std::string text = rtm::message(r, german);

    const PaperTheme& theme = ui_.theme();
    const auto px = [this](float units) { return ui_.px(units); };
    constexpr float kPad = 30.0f;
    constexpr float kLabel = 13.5f;
    constexpr float kButton = 42.0f;
    const float w = px(600);
    // The text wrapped to the dialog's width: lines of it, and the buttons.
    const float wrapW = w - px(kPad) * 2.0f;
    const float textH = ui_.wrappedHeight(wrapW, text.c_str(), px(kLabel));
    const float h = px(kPad) * 2.0f + textH + px(24) + px(kButton);

    ui_.setLayer(PaperLayer::Overlay);
    ui_.scrim(0.45f);
    const ImVec2 a((screenW - w) * 0.5f, (screenH - h) * 0.5f);
    const ImVec2 b(a.x + w, a.y + h);
    ui_.sheet(a, b, /*taped=*/false);
    ui_.claimMouse(a, b);
    ui_.wrapped(ImVec2(a.x + px(kPad), a.y + px(kPad)), wrapW, text.c_str(), px(kLabel), theme.ink);

    const std::string markerFile = core::getConfigRoot() + "/rt_session.marker";
    const float y = b.y - px(kPad) - px(kButton);
    // Each button as wide as its words and some, the German ones being longer.
    const char* keepLabel = german ? "Eingeschaltet lassen" : "Keep it on";
    const char* offLabel = german ? "Raytracing ausschalten" : "Turn ray tracing off";
    const float bw = std::max({px(170), ui_.textWidth(keepLabel, px(16)) + px(40),
                               ui_.textWidth(offLabel, px(16)) + px(40)});
    const float x1 = b.x - px(kPad);
    if (ui_.button("rtcrash.keep", ImVec2(x1 - bw * 2.0f - px(12), y), ImVec2(x1 - bw - px(12), y + px(kButton)),
                   keepLabel)) {
        rtMarkerShown_ = false;
        core::rt_session::clear(markerFile);
    }
    if (ui_.button("rtcrash.off", ImVec2(x1 - bw, y), ImVec2(x1, y + px(kButton)), offLabel,
                   PaperUI::ButtonKind::Primary)) {
        rtMarkerShown_ = false;
        core::rt_session::clear(markerFile);
        // Through the game's settings, which then save: they are what the
        // file is written from, and a value put into the file behind them was
        // written back over by the next save.
        if (settingsAccess_.set && settingsAccess_.save) {
            settingsAccess_.set("raytracedlighting", "0");
            settingsAccess_.save();
            ui_.setLayer(PaperLayer::Page);
            return;
        }
        // Into the settings file, which the game reads on the way in.
        std::map<std::string, std::string> cfg;
        {
            std::ifstream in(SettingsPanel::getSettingsPath());
            std::string line;
            while (std::getline(in, line)) {
                const auto eq = line.find('=');
                if (eq != std::string::npos) cfg[line.substr(0, eq)] = line.substr(eq + 1);
            }
        }
        cfg["ray_traced_lighting"] = "0";
        std::ofstream out(SettingsPanel::getSettingsPath());
        for (const auto& [k, v] : cfg) out << k << "=" << v << "\n";
    }
    ui_.setLayer(PaperLayer::Page);
}

void AuthScreen::renderLoginSettingsSheet(float screenW, float screenH) {
    const PaperTheme& theme = ui_.theme();
    const auto px = [this](float units) { return ui_.px(units); };
    const bool german = clientSpeaksGerman();
    const auto words = [german](const char* en, const char* de) { return german ? de : en; };

    constexpr float kPad = 26.0f;
    constexpr float kTitle = 30.0f;
    constexpr float kLabel = 13.5f;
    constexpr float kSmall = 12.5f;
    constexpr float kControl = 32.0f;
    constexpr float kCheckBox = 19.0f;
    constexpr float kChip = 30.0f;
    constexpr float kButton = 42.0f;

    const float w = std::min(screenW - px(20), px(900));
    const float h = std::min(screenH - px(20), px(720));

    ui_.setLayer(PaperLayer::Overlay);
    ui_.scrim(0.45f);

    const ImVec2 a((screenW - w) * 0.5f, (screenH - h) * 0.5f);
    const ImVec2 b(a.x + w, a.y + h);
    ui_.sheet(a, b, /*taped=*/false);
    // The sheet eats every click that lands on it, including the ones that
    // land on none of its controls.
    ui_.claimMouse(a, b);
    // The yes/no over it hears everything while it is up.
    const bool confirming = confirmGraphicsReset_;
    if (confirming) ui_.pushInert();

    Column col{a.x + px(kPad), b.x - px(kPad), a.y + px(kPad)};

    ui_.text(col.at(), words("Graphics", "Grafik"), px(kTitle), theme.ink, /*titleFace=*/true);
    {
        const float r = px(kSmall) * 0.95f;
        if (ui_.glyphButton("gfx.close", ImVec2(col.x1 - r, col.y + r), r, PaperUI::Glyph::Cross)) {
            closeLoginSettingsSheet(false);
        }
    }
    // The title face's capitals stand above its line; the line under it starts
    // below them, or an initial umlaut runs into the title.
    col.gap(ui_.lineHeight(px(kTitle), /*titleFace=*/true) + px(4));
    ui_.text(col.at(),
             words("Changes show as you make them; what is in the world, once you enter it.",
                   "Änderungen wirken sofort; was die Welt betrifft, sobald Ihr sie betretet."),
             px(kSmall), theme.pencil);
    col.gap(ui_.lineHeight(px(kSmall)) + px(8));
    ui_.rule(ImVec2(col.x0, col.y), ImVec2(col.x1, col.y), paperFade(theme.pencil, 0.6f), px(1.0f), 0x9A17u);
    col.gap(px(10));

    // ---- the pages, as chips wrapped across the sheet ----------------------
    {
        float x = col.x0;
        for (const std::string& category : sheetCategories()) {
            const std::string name = categoryText(category, german);
            const float cw = ui_.chipWidth(name.c_str(), px(kChip));
            if (x > col.x0 && x + cw > col.x1) {
                x = col.x0;
                col.gap(px(kChip) + px(6));
            }
            const std::string id = "gfx.page." + category;
            if (ui_.chip(id.c_str(), ImVec2(x, col.y), ImVec2(x + cw, col.y + px(kChip)), name.c_str(),
                         sheetCategory_ == category, theme.crayonBlue)) {
                if (sheetCategory_ != category) sheetPart_ = 0;
                sheetCategory_ = category;
            }
            x += cw + px(6);
        }
        col.gap(px(kChip) + px(14));
    }

    // ---- the page's rows, in two columns -----------------------------------
    //
    // The same rows the in-game Video pages are built from, with the same
    // labels, ranges, choices, greying and hover text, so neither screen can
    // offer a setting the other does not or say it differently.
    const float footerTop = b.y - px(kPad) - px(kButton);
    const float contentTop = col.y;
    const float contentBottom = footerTop - px(30);
    const float columnGap = px(28);
    const float columnW = (col.width() - columnGap) * 0.5f;
    const float labelSize = px(kLabel);

    const auto read = [this](const std::string& key) {
        return settingsAccess_.get ? settingsAccess_.get(key) : std::string();
    };
    // Why the ray traced rows are greyed on this device, and whether the
    // tracing runs, for the rows it makes moot.
    const std::string rtReason =
        services_.renderer ? services_.renderer->rtUnavailableReason(german) : std::string();
    const bool rtOn = rtReason.empty() && settingIsOn(read("raytracedlighting"));

    struct Item {
        const SettingDesc* desc = nullptr;  ///< nullptr for a heading
        SettingText text;
        float height = 0.0f;
        bool labelInline = false;  ///< a checkbox whose label fits beside it
    };
    std::vector<Item> items;
    {
        std::string heading;
        for (const SettingDesc* row : sheetRows()) {
            if (sheetCategory_ != row->category) continue;
            Item item;
            item.desc = row;
            item.text = settingText(*row, german, interfaceGlobal);
            if (row->section[0] != '\0' && heading != row->section) {
                heading = row->section;
                Item head;
                head.text.label = item.text.section;
                head.height = ui_.lineHeight(labelSize) + px(12);
                items.push_back(head);
            }
            if (row->kind == SettingKind::Bool) {
                const float box = px(kCheckBox);
                const float inlineW = box + px(9) + ui_.textWidth(item.text.label.c_str(), box * 0.78f);
                item.labelInline = inlineW <= columnW;
                const float labelH =
                    item.labelInline ? 0.0f : ui_.wrappedHeight(columnW - box - px(9), item.text.label.c_str(), labelSize);
                item.height = std::max(box, labelH) + px(11);
            } else {
                item.height = ui_.wrappedHeight(columnW, item.text.label.c_str(), labelSize) + px(3) + px(kControl) +
                              px(10);
            }
            items.push_back(std::move(item));
        }
    }

    // Laid down the first column and then the second; what is left over goes
    // on to the next part of the page.
    struct Placed {
        std::size_t item;
        float x;
        float y;
    };
    std::vector<std::vector<Placed>> parts(1);
    {
        int column = 0;
        float y = contentTop;
        for (std::size_t i = 0; i < items.size(); ++i) {
            // A heading keeps its first row with it.
            const float need =
                items[i].height + (!items[i].desc && i + 1 < items.size() ? items[i + 1].height : 0.0f);
            if (y + need > contentBottom && y > contentTop) {
                if (++column == 2) {
                    column = 0;
                    parts.emplace_back();
                }
                y = contentTop;
            }
            parts.back().push_back({i, column == 0 ? col.x0 : col.x0 + columnW + columnGap, y});
            y += items[i].height;
        }
    }
    sheetPart_ = std::clamp(sheetPart_, 0, static_cast<int>(parts.size()) - 1);
    // The wheel turns the parts of a long page.
    if (parts.size() > 1 && !ui_.popupOpen() &&
        ImGui::IsMouseHoveringRect(ImVec2(col.x0, contentTop), ImVec2(col.x1, contentBottom), false)) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel < 0.0f) sheetPart_ = std::min(sheetPart_ + 1, static_cast<int>(parts.size()) - 1);
        if (wheel > 0.0f) sheetPart_ = std::max(sheetPart_ - 1, 0);
    }

    std::string hoverTitle, hoverText;
    for (const Placed& place : parts[static_cast<size_t>(sheetPart_)]) {
        const Item& item = items[place.item];
        const ImVec2 at(place.x, place.y);
        if (!item.desc) {
            ui_.text(ImVec2(at.x, at.y + px(4)), item.text.label.c_str(), labelSize, theme.ink, /*titleFace=*/true);
            const float ry = at.y + ui_.lineHeight(labelSize) + px(6);
            ui_.rule(ImVec2(at.x, ry), ImVec2(at.x + columnW, ry), paperFade(theme.pencil, 0.5f), px(1.0f),
                     0x51A7u + static_cast<uint32_t>(place.item));
            continue;
        }
        const SettingDesc& d = *item.desc;
        const std::string key = d.key;
        const std::string id = "gfx." + key;

        // Greyed rather than hidden, for the same three reasons the in-game
        // pages grey a row, with the reason under its hover text.
        std::string reason;
        if (!rtReason.empty() && settingNeedsRayTracing(key)) {
            reason = rtReason;
        } else if (const char* moot = rtOn ? rayTracingIgnoredText(key, german) : nullptr) {
            reason = moot;
        } else if (!settingEnabled(d, read)) {
            reason = waitingInWords(d, german);
        }
        const bool enabled = reason.empty() && settingsAccess_.set;

        const std::string value = read(key);
        const double number = std::atof(value.c_str());
        const ImVec2 rowEnd(at.x + columnW, at.y + item.height - px(6));
        if (!enabled) ui_.pushInert();
        const ImU32 labelColour = enabled ? theme.inkSoft : theme.pencil;

        if (d.kind == SettingKind::Bool) {
            bool on = settingIsOn(value);
            const float box = px(kCheckBox);
            bool changed = false;
            if (item.labelInline) {
                changed = ui_.checkbox(id.c_str(), at, box, item.text.label.c_str(), &on);
            } else {
                changed = ui_.checkbox(id.c_str(), at, box, "", &on);
                ui_.wrapped(ImVec2(at.x + box + px(9), at.y), columnW - box - px(9), item.text.label.c_str(),
                            labelSize, labelColour);
            }
            if (changed && enabled) settingsAccess_.set(key, on ? "1" : "0");
        } else {
            const float labelH = ui_.wrapped(at, columnW, item.text.label.c_str(), labelSize, labelColour);
            const ImVec2 ca(at.x, at.y + labelH + px(3));
            const ImVec2 cb(at.x + columnW, ca.y + px(kControl));
            if (d.kind == SettingKind::Enum) {
                const std::vector<std::string> choices = splitChoices(item.text.choices);
                int index = std::clamp(static_cast<int>(number + 0.5), 0, static_cast<int>(choices.size()) - 1);
                if (ui_.dropdown(id.c_str(), ca, cb, choices[static_cast<size_t>(index)], choices, &index) &&
                    enabled) {
                    settingsAccess_.set(key, std::to_string(index));
                }
            } else {
                float v = static_cast<float>(number);
                if (ui_.sliderFloat(id.c_str(), ca, cb, &v, d.minValue, d.maxValue, sliderFormat(d.step)) &&
                    enabled) {
                    // To the row's own step, and only when that moved it.
                    double snapped = v;
                    if (d.step > 0.0f) {
                        snapped = d.minValue + std::round((v - d.minValue) / d.step) * d.step;
                        snapped = std::clamp(snapped, static_cast<double>(d.minValue), static_cast<double>(d.maxValue));
                    }
                    if (d.kind == SettingKind::Int) snapped = std::round(snapped);
                    if (std::abs(snapped - number) > 1e-6) settingsAccess_.set(key, settingNumberText(snapped));
                }
            }
        }
        if (!enabled) {
            ui_.popInert();
            ui_.veil(at, rowEnd, 0.45f);
        }

        if (ImGui::IsMouseHoveringRect(at, rowEnd, false)) {
            hoverTitle = item.text.label;
            hoverText = item.text.tooltip;
            if (!reason.empty()) hoverText += (hoverText.empty() ? "" : "\n\n") + reason;
        }
    }

    // ---- which part of a long page -----------------------------------------
    if (parts.size() > 1) {
        const float y = contentBottom + px(4);
        const float bh = px(24);
        const float bw = px(34);
        if (ui_.button("gfx.part.prev", ImVec2(col.x0, y), ImVec2(col.x0 + bw, y + bh), "<", PaperUI::ButtonKind::Quiet,
                       sheetPart_ > 0)) {
            --sheetPart_;
        }
        const std::string where = std::to_string(sheetPart_ + 1) + " / " + std::to_string(parts.size());
        ui_.textCentered(col.x0 + bw + px(30), y + px(3), where.c_str(), px(kSmall), theme.inkSoft);
        if (ui_.button("gfx.part.next", ImVec2(col.x0 + bw + px(60), y), ImVec2(col.x0 + bw * 2.0f + px(60), y + bh),
                       ">", PaperUI::ButtonKind::Quiet, sheetPart_ + 1 < static_cast<int>(parts.size()))) {
            ++sheetPart_;
        }
    }

    // ---- the way out ------------------------------------------------------
    {
        const float y = footerTop;
        const float bh = px(kButton);
        const float buttonText = px(16);
        const auto widthFor = [&](const char* label, float least) {
            return std::max(px(least), ui_.textWidth(label, buttonText) * 1.15f + px(48));
        };
        // Every graphics setting, not only the ones on this page: the ray
        // tracing's and the Debug pages' with the rest. Asked first, since it
        // cannot be undone.
        const char* resetLabel = words("Reset to default", "Auf Standard zurücksetzen");
        if (ui_.button("gfx.reset", ImVec2(col.x0, y), ImVec2(col.x0 + widthFor(resetLabel, 170), y + bh),
                       resetLabel)) {
            confirmGraphicsReset_ = true;
        }
        const char* applyLabel = words("Apply", "Übernehmen");
        const char* cancelLabel = words("Cancel", "Abbrechen");
        const float applyW = widthFor(applyLabel, 130);
        const float cancelW = widthFor(cancelLabel, 110);
        if (ui_.button("gfx.cancel", ImVec2(col.x1 - applyW - px(12) - cancelW, y),
                       ImVec2(col.x1 - applyW - px(12), y + bh), cancelLabel)) {
            closeLoginSettingsSheet(false);
        }
        if (ui_.button("gfx.apply", ImVec2(col.x1 - applyW, y), ImVec2(col.x1, y + bh), applyLabel,
                       PaperUI::ButtonKind::Primary)) {
            closeLoginSettingsSheet(true);
        }
    }
    if (confirming) ui_.popInert();

    // The hovered row's description, beside the pointer, as the game's own
    // tooltip is.
    if (settingsOpen_ && !confirming && !ui_.popupOpen() && !hoverText.empty()) {
        const float tw = std::min(px(380), screenW - px(20));
        const float inner = tw - px(28);
        const float titleH = ui_.wrappedHeight(inner, hoverTitle.c_str(), labelSize);
        const float bodyH = ui_.wrappedHeight(inner, hoverText.c_str(), px(kSmall));
        const float th = px(14) * 2.0f + titleH + px(6) + bodyH;
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        ImVec2 ta(mouse.x + px(18), mouse.y + px(18));
        if (ta.x + tw > screenW - px(6)) ta.x = std::max(px(6), mouse.x - tw - px(12));
        if (ta.y + th > screenH - px(6)) ta.y = std::max(px(6), screenH - px(6) - th);
        const ImVec2 tb(ta.x + tw, ta.y + th);
        ui_.sheet(ta, tb, /*taped=*/false);
        ui_.wrapped(ImVec2(ta.x + px(14), ta.y + px(14)), inner, hoverTitle.c_str(), labelSize, theme.ink);
        ui_.wrapped(ImVec2(ta.x + px(14), ta.y + px(14) + titleH + px(6)), inner, hoverText.c_str(), px(kSmall),
                    theme.inkSoft);
    }

    if (settingsOpen_ && confirmGraphicsReset_) {
        // Yes or no, over the sheet.
        const char* line1 = words("Reset all graphics settings to their defaults?",
                                  "Alle Grafikeinstellungen auf ihren Standard zurücksetzen?");
        const char* line2 = words("Every one, including ray tracing and the debug pages.",
                                  "Jede einzelne, auch Raytracing und die Debug-Seiten.");
        const char* line3 = words("Sound, controls, interface and chat stay as they are.",
                                  "Sound, Steuerung, Oberfläche und Chat bleiben, wie sie sind.");
        const float textW = std::max({ui_.textWidth(line1, px(kLabel)), ui_.textWidth(line2, px(kSmall)),
                                      ui_.textWidth(line3, px(kSmall))});
        const float cw = std::min(screenW - px(20), std::max(px(440), textW + px(kPad) * 2.0f));
        const float ch = px(kPad) * 2.0f + ui_.lineHeight(px(kLabel)) * 3.0f + px(28) + px(kButton);
        const ImVec2 ca((screenW - cw) * 0.5f, (screenH - ch) * 0.5f);
        const ImVec2 cb(ca.x + cw, ca.y + ch);
        ui_.sheet(ca, cb, /*taped=*/false);
        ui_.claimMouse(ca, cb);
        Column c{ca.x + px(kPad), cb.x - px(kPad), ca.y + px(kPad)};
        ui_.text(c.at(), line1, px(kLabel), theme.ink);
        c.gap(ui_.lineHeight(px(kLabel)) + px(4));
        ui_.text(c.at(), line2, px(kSmall), theme.pencil);
        c.gap(ui_.lineHeight(px(kSmall)) + px(4));
        ui_.text(c.at(), line3, px(kSmall), theme.pencil);
        const float y = cb.y - px(kPad) - px(kButton);
        const float bw = px(110);
        if (ui_.button("gfx.reset.no", ImVec2(c.x1 - bw * 2.0f - px(12), y), ImVec2(c.x1 - bw - px(12), y + px(kButton)),
                       words("No", "Nein"))) {
            confirmGraphicsReset_ = false;
        }
        if (ui_.button("gfx.reset.yes", ImVec2(c.x1 - bw, y), ImVec2(c.x1, y + px(kButton)), words("Yes", "Ja"),
                       PaperUI::ButtonKind::Primary)) {
            confirmGraphicsReset_ = false;
            // Applied and saved (GameScreen::resetGraphicsToDefaults): what
            // Cancel would put back is what there is now.
            if (onResetGraphicsDefaults_) onResetGraphicsDefaults_();
            openLoginSettingsSheet();
        }
    }

    ui_.setLayer(PaperLayer::Page);
}

}} // namespace wowee::ui
