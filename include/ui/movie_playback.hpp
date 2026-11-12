#pragma once

// The MovieFrame's movie (CSimpleMovieFrame, 0x0095dc80 and on).
//
// StartMovie(name, volume) opens "<name>.avi" (0x0095dc80) and "<name>.sbt"
// (0x0095e880), plays the picture full screen with its sound at volume/255
// (0x0095e600), and tells the frame when a subtitle line starts and ends and
// when the movie is over. StopMovie (0x0095eba0) ends it early and fires
// OnMovieFinished the same way.
//
// One movie at a time, owned by the MovieFrame widget that started it. The
// file is demuxed and decoded on a thread of its own (FFmpeg: the AVI
// demuxer, MPEG-4 Part 2 and MP3); the sound is the AVI's MP3 track played
// as one stream through the audio engine. Without FFmpeg StartMovie fails,
// and FrameXML then calls GameMovieFinished at once, as before.

#include <cstdint>
#include <string>
#include <vector>

#include "ui/movie_rules.hpp"

namespace wowee::ui::movie {

struct Event {
    enum class Kind { Finished, ShowSubtitle, HideSubtitle };
    Kind kind;
    uint32_t widget = 0;
    std::string text;
};

/// Whether this build can play movies at all.
bool available();

/// StartMovie: false when the file is not there or cannot be decoded.
bool start(uint32_t widget, const std::string& name, int volume, std::string& why);
/// StopMovie: true when a movie was playing (OnMovieFinished is then due).
bool stop();
/// EnableSubtitles (+0x2a8). Lines also show when the movie has no sound.
void enableSubtitles(bool on);
bool playing();
uint32_t owner();

/// Once a frame, from the interface's update: what happened since the last
/// call, in order. The movie releases itself after Finished.
void poll(std::vector<Event>& out);

/// For the drawing: the newest picture when there is one not yet taken
/// (RGBA, width*height*4). The picture size is reported either way.
bool takeFrame(std::vector<uint8_t>& rgba, int& width, int& height);
void pictureSize(int& width, int& height);
/// Bumped by every new start, so the drawing knows to drop the last movie's
/// picture.
uint32_t generation();

} // namespace wowee::ui::movie
