#pragma once

/**
 * data_prompt.hpp - when the client has to ask where the game data is, and
 * what it says while it asks.
 *
 * Android asks before the client starts: DataFolderActivity is a Java screen
 * in front of the native code, shown while the chosen folder holds no data.
 * iOS has no such screen - the app is the native client from its first frame -
 * and the client's own way to choose a folder was compiled out on every
 * phone (WOWEE_MOBILE), on the assumption that the phone had already asked.
 * So an iPhone with nothing in its Documents folder went straight to a login
 * screen that could not work, and never said where the data had to go.
 *
 * These are the decisions, free of any window or platform call, so they can be
 * tested on any machine; the screen (ui/data_folder_screen) and startup act on
 * them.
 */

#include "core/data_paths.hpp"

#include <filesystem>
#include <string>

namespace wowee {
namespace core {

/// Whether this build has a screen in front of the client that chooses the
/// data folder (Android's DataFolderActivity). Where it does, the client never
/// asks itself.
#if defined(__ANDROID__)
inline constexpr bool kPreLaunchDataMask = true;
#else
inline constexpr bool kPreLaunchDataMask = false;
#endif

/// Why the data folder screen is shown at startup, if it is.
enum class DataPrompt {
    None,              ///< Data is there; straight to the login screen.
    NoData,            ///< Nothing usable where the client looked.
    ChosenFolderGone,  ///< A folder was chosen and is not there any more.
    StreamFailed,      ///< A streaming server was chosen and could not be read.
};

/// Whether this start reads the game data from a streaming server: when
/// WOWEE_DATA_MODE (`modeEnv`, empty when unset) says "streaming", or when a
/// server is kept (`streamUrl`) and WOWEE_DATA_MODE says nothing else.
inline bool streamingChosen(const std::string& modeEnv, const std::string& streamUrl) {
    return modeEnv == "streaming" || (modeEnv.empty() && !streamUrl.empty());
}

/// What startup does about the data folder.
///
/// `preLaunchMask`: the platform asked already (Android). `dataUsable`: the
/// client found game data - an extraction or the original archives.
/// `chosenFolder`: the folder the player chose, empty for the default search;
/// `chosenFolderPresent`: whether that folder exists now.
///
/// `builderAvailable`: this build has the asset builder (FIRST_RUN), which
/// asks the same question with more to offer; it wins over this screen.
///
/// `streamFailed`: a streaming server was chosen and did not start. That wins
/// over everything: this screen holds the server's fields, Android's activity
/// let the server through without being able to try it, and the asset
/// builder has nothing to say about a server. Without it the phones came
/// back to the screen (or, on Android, to a login with no data) with no word
/// of what had gone wrong.
inline DataPrompt dataPromptAtStartup(bool preLaunchMask, bool builderAvailable, bool dataUsable,
                                      const std::string& chosenFolder,
                                      bool chosenFolderPresent, bool streamFailed) {
    if (streamFailed) return DataPrompt::StreamFailed;
    if (preLaunchMask || builderAvailable) return DataPrompt::None;
    if (!chosenFolder.empty() && !chosenFolderPresent) return DataPrompt::ChosenFolderGone;
    if (!dataUsable) return DataPrompt::NoData;
    return DataPrompt::None;
}

/// The data folder a picked folder stands for: the folder itself when it
/// holds an extraction, else a Data folder inside it that does - picking the
/// folder around Data is the natural mistake, and it can only have meant one
/// thing. Empty when neither holds one.
inline std::filesystem::path resolveDataFolderChoice(const std::filesystem::path& picked) {
    if (picked.empty()) return {};
    if (holdsExtraction(picked)) return picked;
    if (holdsExtraction(picked / "Data")) return picked / "Data";
    return {};
}

/// What the data folder screen says, by state. One place, so the screen and
/// its test cannot drift apart.
struct DataFolderView {
    std::string status;      ///< The line about the folder being read.
    std::string help;        ///< What to do about it.
    bool found = false;      ///< Data is there now; reopening the app uses it.
    bool offerDefault = false;  ///< A chosen folder can be forgotten.
};

/// What the screen says when the streaming server could not be read: a
/// heading short enough for a button-sized line in any language this client
/// speaks (24 characters or fewer), and the rest as body text.
struct StreamFailureView {
    std::string heading;
    std::string body;
};

/// `server`: the server's host, as the player gave it; `error`: what the
/// streaming client said (pipeline::DataStream::start).
inline StreamFailureView describeStreamFailure(const std::string& server, const std::string& error) {
    StreamFailureView v;
    v.heading = "Streaming failed";
    v.body = "The game data could not be streamed from " +
             (server.empty() ? std::string("the server") : server) + ": " +
             (error.empty() ? std::string("no reason was given") : error) + ".";
    if (error.find("access key") != std::string::npos) {
        v.body += " Check the access key";
    } else if (error.find("TLS") != std::string::npos ||
               error.find("certificate") != std::string::npos) {
        v.body += " The server's certificate could not be checked. Check the address";
    } else if (error.find("status 5") != std::string::npos) {
        // 502/503: the web server answers, the data service behind it not.
        v.body += " The server's data service is not answering. Try again later, or check the address";
    } else {
        v.body += " Check the address and the access key";
    }
    v.body += ", then choose \"Use this server\" again.";
    return v;
}

/// `folder`: the folder the client reads (or would read); `chosen`: whether
/// the player chose it rather than the default; `found`: whether it holds data
/// now; `platformFolderHelp`: how data gets into the default folder on this
/// platform (on iOS: the Files app).
inline DataFolderView describeDataFolder(const std::string& folder, bool chosen, bool found,
                                         DataPrompt reason,
                                         const std::string& platformFolderHelp) {
    DataFolderView v;
    v.found = found;
    v.offerDefault = chosen;
    if (found) {
        v.status = "Game data found in " + folder + ".";
        v.help = "Close WoWee and open it again to play with it.";
        return v;
    }
    if (reason == DataPrompt::ChosenFolderGone) {
        v.status = "The chosen game data folder " + folder + " is not there.";
    } else {
        v.status = "No game data in " + folder + ".";
    }
    v.help = platformFolderHelp.empty()
                 ? std::string("Copy an extraction into that folder, or choose the folder that "
                               "holds one.")
                 : platformFolderHelp;
    return v;
}

}  // namespace core
}  // namespace wowee
