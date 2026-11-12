#include "ui/data_folder_picker.hpp"
#include "core/platform.hpp"

#include "core/config_paths.hpp"
#include "core/data_paths.hpp"
#include "core/logger.hpp"

#include "core/data_prompt.hpp"

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace wowee::ui {

namespace fs = std::filesystem;

DataFolderPicker& dataFolderPicker() {
    static DataFolderPicker picker;
    return picker;
}

std::string DataFolderPicker::currentFolder() {
    if (const char* env = std::getenv("WOW_DATA_PATH"); env && *env) return env;
    std::error_code ec;
    const fs::path local = fs::absolute("Data", ec);
    return ec ? std::string("Data") : local.string();
}

bool DataFolderPicker::hasCustomFolder() {
    return !core::getCustomDataRoot().empty();
}

bool DataFolderPicker::isOpen() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return open_;
}

#if defined(WOWEE_IOS)
/// data_folder_picker_ios.mm: presents UIDocumentPickerViewController for a
/// folder over the window's view controller; answers through deliver(). False
/// when there is nothing to present it over.
bool iosPresentFolderPicker(SDL_Window* window);
#endif

void DataFolderPicker::open([[maybe_unused]] SDL_Window* window) {
#if defined(__ANDROID__)
    // Chosen before the client starts; see DataFolderActivity.
    return;
#elif defined(WOWEE_IOS)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_) return;
        open_ = true;
        answered_ = false;
    }
    if (!iosPresentFolderPicker(window)) deliver("", "", "the folder picker could not be shown");
#else
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (open_) return;
        open_ = true;
        answered_ = false;
    }
    std::error_code ec;
    startIn_ = currentFolder();
    const bool startThere = fs::is_directory(startIn_, ec);
    SDL_ShowOpenFolderDialog(&DataFolderPicker::onChosen, this, window,
                             startThere ? startIn_.c_str() : nullptr, /*allow_many=*/false);
#endif
}

void SDLCALL DataFolderPicker::onChosen(void* userdata, const char* const* files,
                                        [[maybe_unused]] int filter) {
    auto* self = static_cast<DataFolderPicker*>(userdata);
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->open_ = false;
    self->answered_ = true;
    self->failed_ = (files == nullptr);
    self->error_ = self->failed_ ? SDL_GetError() : "";
    // A pointer to NULL is the dialog cancelled, which chooses nothing.
    self->chosen_ = (files && files[0]) ? files[0] : "";
    self->bookmark_.clear();
}

void DataFolderPicker::deliver(const std::string& path, const std::string& bookmark,
                               const std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    answered_ = true;
    failed_ = !error.empty();
    error_ = error;
    chosen_ = path;
    bookmark_ = bookmark;
}

void DataFolderPicker::poll() {
    std::string chosen, bookmark;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!answered_) return;
        answered_ = false;
        if (failed_) {
            message_ = "The folder dialog could not be opened: " + error_;
            LOG_WARNING("Data folder dialog failed: ", error_);
            return;
        }
        chosen = chosen_;
        bookmark = bookmark_;
    }
    if (chosen.empty()) return;

    // The folder itself, or the Data folder inside it: whichever holds an
    // extraction. Picking the folder around Data is the natural mistake, and
    // there is only one thing it can have meant.
    const fs::path root = core::resolveDataFolderChoice(fs::path(chosen));
    if (root.empty()) {
        message_ = "No game data in " + chosen +
                   ". Choose the folder that holds manifest.json, or expansions/ "
                   "with a game in it.";
        return;
    }

    if (!core::setCustomDataRoot(root.string()) || !core::setCustomDataBookmark(bookmark)) {
        message_ = "Could not save the choice to " + core::getConfigRoot() + ".";
        return;
    }
    LOG_WARNING("Game data folder set to ", root.string(), " from the next start");
    message_ = "Game data will be read from " + root.string() +
               " when WoWee next starts.";
}

void DataFolderPicker::useDefault() {
    if (!core::setCustomDataRoot("") || !core::setCustomDataBookmark("")) {
        message_ = "Could not save the choice to " + core::getConfigRoot() + ".";
        return;
    }
    message_ = "The default game data folder will be used when WoWee next starts.";
}

}  // namespace wowee::ui
