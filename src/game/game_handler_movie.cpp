// The server's movies: SMSG_TRIGGER_MOVIE to PLAY_MOVIE, and
// GameMovieFinished back to CMSG_COMPLETE_MOVIE.

#include "game/game_handler.hpp"
#include "game/opcode_table.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "core/window.hpp"
#include "network/packet.hpp"
#include "network/world_socket.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/dbc_loader.hpp"
#include "ui/movie_playback.hpp"

#include <string>
#include <vector>

namespace wowee {
namespace game {

namespace {

struct MovieFile {
    std::string name;  // path and file, without the extension
    uint32_t volume = 0;
};

/// Movie.dbc (ID, filename, volume) -> MovieVariation (ID, movie,
/// fileDataID) -> MovieFileData (fileDataID, resolution) -> FileData (ID,
/// filename, path); the variation for this screen as 0x00518700 picks it,
/// named "%s%s" (path, file) up to its '.' (0x0051d710).
bool findMovieFile(pipeline::AssetManager& am, uint32_t movieId, int screenWidth, MovieFile& out) {
    auto movies = am.loadDBCOptional("Movie.dbc");
    auto variations = am.loadDBCOptional("MovieVariation.dbc");
    auto fileData = am.loadDBCOptional("MovieFileData.dbc");
    auto files = am.loadDBCOptional("FileData.dbc");
    if (!movies || !variations || !fileData || !files || !movies->isLoaded() || !variations->isLoaded() ||
        !fileData->isLoaded() || !files->isLoaded()) {
        return false;
    }
    const int32_t movieRow = movies->findRecordById(movieId);
    if (movieRow < 0) return false;
    out.volume = movies->getFieldCount() > 2 ? movies->getUInt32(static_cast<uint32_t>(movieRow), 2) : 0;

    std::vector<uint32_t> ids;
    std::vector<int> resolutions;
    for (uint32_t i = 0; i < variations->getRecordCount(); ++i) {
        if (variations->getUInt32(i, 1) != movieId) continue;
        const uint32_t fileDataId = variations->getUInt32(i, 2);
        const int32_t fdRow = fileData->findRecordById(fileDataId);
        if (fdRow < 0 || files->findRecordById(fileDataId) < 0) continue;
        ids.push_back(fileDataId);
        resolutions.push_back(static_cast<int>(fileData->getUInt32(static_cast<uint32_t>(fdRow), 1)));
    }
    const int pick = ui::movie::pickResolution(resolutions, screenWidth);
    if (pick < 0) return false;
    const int32_t fileRow = files->findRecordById(ids[static_cast<size_t>(pick)]);
    std::string name = files->getString(static_cast<uint32_t>(fileRow), 2) +
                       files->getString(static_cast<uint32_t>(fileRow), 1);
    if (const auto dot = name.find('.'); dot != std::string::npos) name.resize(dot);
    if (name.empty()) return false;
    out.name = name;
    return true;
}

}  // namespace

void GameHandler::triggerMovie(uint32_t movieId) {
    int width = 0;
    if (auto* window = core::Application::getInstance().getWindow()) width = window->getWidth();
    MovieFile movie;
    auto* am = services_.assetManager;
    moviePlaying_ = true;
    if (am && findMovieFile(*am, movieId, width, movie) && addonEventCallback_) {
        LOG_INFO("SMSG_TRIGGER_MOVIE ", movieId, ": PLAY_MOVIE ", movie.name, " volume ", movie.volume,
                 " (screen width ", width, ")");
        fireAddonEvent("PLAY_MOVIE", {movie.name, std::to_string(movie.volume)});
        return;
    }
    // No file for it (0x0051d710 calls 0x00518870): done at once.
    LOG_INFO("SMSG_TRIGGER_MOVIE ", movieId, ": no movie file - completed at once");
    gameMovieFinished();
}

void GameHandler::gameMovieFinished() {
    if (!moviePlaying_) return;
    moviePlaying_ = false;
    const uint16_t wire = wireOpcode(Opcode::CMSG_COMPLETE_MOVIE);
    if (wire == 0xFFFF || !socket) return;
    network::Packet ack(wire);
    socket->send(ack);
    LOG_INFO("GameMovieFinished: sent CMSG_COMPLETE_MOVIE");
}

}  // namespace game
}  // namespace wowee
