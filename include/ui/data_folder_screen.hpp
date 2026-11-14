#pragma once

/// Where the game data is, asked by the client itself.
///
/// Android asks before the client starts (DataFolderActivity). iOS has nothing
/// in front of the client, and builds without the asset builder (iOS among
/// them) had no FIRST_RUN screen either - so with no data the client opened a
/// login that could not work and never said where the data had to go. This is
/// the same screen as Android's, drawn by the client: the folder it reads, what
/// to do to fill it, a way to choose another folder, and a way back to the
/// default. The decisions are core/data_prompt.hpp's.
///
/// Or no folder at all: a streaming server's address and access key (the
/// "streaming" data mode, pipeline/data_stream), kept like the folder choice
/// (core::setDataStream) and started at once - the login screen follows
/// without a restart (Application::startStreamingNow). When the server does
/// not answer, at startup or here, the screen says why rather than simply
/// coming back. Android's own screen offers the same two fields; this one is
/// shown there only when the server it was given failed, with only them.

#include "core/data_prompt.hpp"

#include <string>

namespace wowee::core {
class Application;
}

namespace wowee::ui {

class DataFolderScreen {
public:
    /// Draw into the frame already open.
    void render();

private:
    /// Look at the folder again (the player may have copied data in).
    void recheck();
    /// "Use this server": the fields checked, kept, and the server started.
    void useServer(core::Application& app);

    bool checked_ = false;
    bool found_ = false;
    std::string folder_;
    std::string lastMessage_;
    // The streaming server's fields, filled from what is kept.
    bool streamLoaded_ = false;
    char streamUrl_[512] = {};
    char streamKey_[256] = {};
    std::string streamMessage_;
    std::string streamHost_;  ///< The server being started, for what is said meanwhile.
    bool diagPressDone_ = false;  ///< WOWEE_DIAG_STREAM_PRESS_URL looked at.
};

}  // namespace wowee::ui
