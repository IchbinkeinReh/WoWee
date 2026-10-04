#pragma once

#include <SDL3/SDL.h>

#include <mutex>
#include <string>

namespace wowee::ui {

/// Choosing the folder the game data is read from, on a desktop.
///
/// The client finds its data by itself - WOW_DATA_PATH, the per-user data
/// folder, Data/ beside the executable - and that is all a player could
/// change it with. This puts a system folder dialog in front of the same
/// choice and remembers the answer (core::setCustomDataRoot), so data kept on
/// another drive, or shared between installs, can be pointed at.
///
/// The choice applies from the next start: the expansion profiles, the asset
/// manager and the fonts are all set up from the data folder at startup, and
/// pulling them out from under a running client is not something it does.
///
/// The dialog answers on whatever thread the platform likes, so the answer is
/// handed over under a lock and acted on in poll(), on the main thread.
/// Android chooses its folder before the client starts (DataFolderActivity),
/// so there this does nothing.
class DataFolderPicker {
public:
    /// Opens the folder dialog, unless one is already open.
    void open(SDL_Window* window);
    /// Forgets the chosen folder, so the default search applies again.
    void useDefault();
    /// Picks up the dialog's answer. Once a frame, from the main thread.
    void poll();

    [[nodiscard]] bool isOpen() const;
    /// What the last choice came to, for the screen to show. Empty until
    /// something has been chosen or refused.
    [[nodiscard]] const std::string& message() const { return message_; }
    /// The folder the client is reading now, as it will be shown.
    [[nodiscard]] static std::string currentFolder();
    /// Whether a folder has been chosen, rather than the default search.
    [[nodiscard]] static bool hasCustomFolder();

private:
    static void SDLCALL onChosen(void* userdata, const char* const* files, int filter);

    mutable std::mutex mutex_;
    bool open_ = false;
    bool answered_ = false;
    bool failed_ = false;
    std::string chosen_;
    std::string error_;
    std::string message_;
    // Where the dialog starts. Held here because the dialog may read it after
    // open() has returned.
    std::string startIn_;
};

/// The one both screens use, so a dialog opened from one is not opened again
/// from the other.
DataFolderPicker& dataFolderPicker();

}  // namespace wowee::ui
