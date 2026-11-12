#include "ui/movie_playback.hpp"

#include "audio/audio_engine.hpp"
#include "core/application.hpp"
#include "core/logger.hpp"
#include "pipeline/asset_manager.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>

#ifdef HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}
#endif

namespace fs = std::filesystem;

namespace wowee::ui::movie {

#ifndef HAVE_FFMPEG

bool available() { return false; }
bool start(uint32_t, const std::string&, int, std::string& why) {
    why = "this build has no FFmpeg";
    return false;
}
bool stop() { return false; }
void enableSubtitles(bool) {}
bool playing() { return false; }
uint32_t owner() { return 0; }
void poll(std::vector<Event>&) {}
bool takeFrame(std::vector<uint8_t>&, int&, int&) { return false; }
void pictureSize(int& w, int& h) { w = h = 0; }
uint32_t generation() { return 0; }

#else

namespace {

using Clock = std::chrono::steady_clock;

std::string lower(std::string v) {
    for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v;
}

fs::path childNoCase(const fs::path& base, const std::string& name) {
    std::error_code ec;
    if (base.empty()) return {};
    fs::path exact = base / name;
    if (fs::exists(exact, ec)) return exact;
    const std::string want = lower(name);
    for (const auto& e : fs::directory_iterator(base, ec)) {
        if (lower(e.path().filename().string()) == want) return e.path();
    }
    return {};
}

fs::path pathNoCase(const fs::path& base, const std::string& rel) {
    fs::path at = base;
    std::string part;
    for (char c : rel + "\\") {
        if (c == '\\' || c == '/') {
            if (!part.empty()) {
                at = childNoCase(at, part);
                if (at.empty()) return {};
            }
            part.clear();
        } else {
            part += c;
        }
    }
    std::error_code ec;
    return fs::is_regular_file(at, ec) ? at : fs::path();
}

/// The movie as a file on disk. The client's movies are loose files beside
/// the archives (Data\<locale>\Interface\Cinematics\*.avi), which Storm
/// finds before the archives; an extracted tree has them where the
/// extractor put them.
fs::path findLooseFile(const std::string& rel) {
    std::vector<fs::path> bases;
    auto add = [&](const fs::path& p) {
        std::error_code ec;
        if (!p.empty() && fs::is_directory(p, ec)) bases.push_back(p);
    };
    if (const char* d = std::getenv("WOW_DATA_PATH"); d && *d) add(d);
    if (const char* c = std::getenv("WOWEE_CLIENT_ROOT"); c && *c) {
        add(childNoCase(c, "Data"));
        add(c);
    }
    if (auto* am = core::Application::getInstance().getAssetManager()) add(am->getDataPath());
    std::vector<fs::path> roots;
    for (const auto& b : bases) {
        roots.push_back(b);
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(b, ec)) {
            if (!e.is_directory(ec)) continue;
            const std::string n = e.path().filename().string();
            // A locale folder (deDE, enUS ...), or an extracted tree's
            // expansions\<name>.
            if (n.size() == 4 && std::islower(static_cast<unsigned char>(n[0])) &&
                std::isupper(static_cast<unsigned char>(n[2]))) {
                roots.push_back(e.path());
            } else if (lower(n) == "expansions") {
                for (const auto& x : fs::directory_iterator(e.path(), ec)) {
                    if (x.is_directory(ec)) roots.push_back(x.path());
                }
            }
        }
    }
    for (const auto& r : roots) {
        if (fs::path p = pathNoCase(r, rel); !p.empty()) return p;
    }
    return {};
}

struct MemReader {
    std::shared_ptr<std::vector<uint8_t>> data;
    int64_t pos = 0;
};

int memRead(void* opaque, uint8_t* buf, int size) {
    auto* m = static_cast<MemReader*>(opaque);
    const int64_t left = static_cast<int64_t>(m->data->size()) - m->pos;
    if (left <= 0) return AVERROR_EOF;
    const int n = static_cast<int>(std::min<int64_t>(left, size));
    std::memcpy(buf, m->data->data() + m->pos, static_cast<size_t>(n));
    m->pos += n;
    return n;
}

int64_t memSeek(void* opaque, int64_t offset, int whence) {
    auto* m = static_cast<MemReader*>(opaque);
    const int64_t size = static_cast<int64_t>(m->data->size());
    if (whence == AVSEEK_SIZE) return size;
    whence &= ~AVSEEK_FORCE;
    int64_t to = offset;
    if (whence == SEEK_CUR) to = m->pos + offset;
    else if (whence == SEEK_END) to = size + offset;
    if (to < 0 || to > size) return -1;
    m->pos = to;
    return to;
}

/// One open AVI: a file, or bytes from the archives behind a custom reader.
struct Input {
    AVFormatContext* fmt = nullptr;
    AVIOContext* io = nullptr;
    std::unique_ptr<MemReader> mem;

    bool open(const fs::path& file, const std::shared_ptr<std::vector<uint8_t>>& bytes, std::string& why) {
        int r = 0;
        if (!file.empty()) {
            r = avformat_open_input(&fmt, file.string().c_str(), nullptr, nullptr);
        } else {
            mem = std::make_unique<MemReader>();
            mem->data = bytes;
            constexpr int kBuf = 64 * 1024;
            auto* buffer = static_cast<uint8_t*>(av_malloc(kBuf));
            io = avio_alloc_context(buffer, kBuf, 0, mem.get(), memRead, nullptr, memSeek);
            fmt = avformat_alloc_context();
            if (!io || !fmt) { why = "out of memory"; return false; }
            fmt->pb = io;
            fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
            r = avformat_open_input(&fmt, nullptr, nullptr, nullptr);
        }
        if (r < 0) {
            char buf[AV_ERROR_MAX_STRING_SIZE] = {};
            av_strerror(r, buf, sizeof(buf));
            why = std::string("cannot open: ") + buf;
            fmt = nullptr;  // freed by avformat_open_input on failure
            return false;
        }
        if (avformat_find_stream_info(fmt, nullptr) < 0) { why = "no stream info"; return false; }
        return true;
    }
    ~Input() {
        if (fmt) avformat_close_input(&fmt);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }
};

struct Picture {
    double pts = 0.0;
    std::vector<uint8_t> rgba;
};

struct Movie {
    uint32_t widget = 0;
    fs::path file;
    std::shared_ptr<std::vector<uint8_t>> bytes;
    float volume = 1.0f;
    int width = 0;
    int height = 0;
    double frameSeconds = 1.0 / 24.0;
    std::vector<Subtitle> subtitles;
    int shownSubtitle = -1;

    std::thread worker;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Picture> queue;
    std::atomic<bool> quit{false};
    bool ready = false;         // audio track read, pictures coming
    bool decoderDone = false;
    std::string failure;
    std::shared_ptr<std::vector<uint8_t>> mp3;

    bool started = false;
    Clock::time_point t0;
    uint32_t audioId = 0;
    double lastPts = 0.0;
    bool hasNewPicture = false;
    Picture current;

    void run();
    // Joined however it ends - also by the static's destructor at exit,
    // when a movie is still playing.
    ~Movie() {
        quit.store(true);
        cv.notify_all();
        if (worker.joinable()) worker.join();
    }
};

std::mutex gLock;  // the main thread's state below; the drawing reads it too
std::unique_ptr<Movie> gMovie;
bool gSubtitles = false;
std::atomic<uint32_t> gGeneration{0};

void Movie::run() {
    // The sound first: the AVI's MP3 track, packet by packet, is an MP3
    // stream the audio engine plays as it plays any other.
    {
        Input in;
        std::string why;
        if (!in.open(file, bytes, why)) {
            std::lock_guard<std::mutex> l(mutex);
            failure = why;
            decoderDone = true;
            cv.notify_all();
            return;
        }
        int audioIndex = -1;
        for (unsigned i = 0; i < in.fmt->nb_streams; ++i) {
            const auto* par = in.fmt->streams[i]->codecpar;
            if (par->codec_type == AVMEDIA_TYPE_AUDIO && par->codec_id == AV_CODEC_ID_MP3) {
                audioIndex = static_cast<int>(i);
                break;
            }
        }
        auto track = std::make_shared<std::vector<uint8_t>>();
        if (audioIndex >= 0) {
            AVPacket* pkt = av_packet_alloc();
            while (!quit.load() && av_read_frame(in.fmt, pkt) >= 0) {
                if (pkt->stream_index == audioIndex) track->insert(track->end(), pkt->data, pkt->data + pkt->size);
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }
        std::lock_guard<std::mutex> l(mutex);
        mp3 = std::move(track);
        ready = true;
    }
    cv.notify_all();

    // Then the pictures, a few ahead of the clock.
    Input in;
    std::string why;
    AVCodecContext* dec = nullptr;
    SwsContext* sws = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    int videoIndex = -1;
    double timeBase = 0.0;
    auto finish = [&](const std::string& error) {
        std::lock_guard<std::mutex> l(mutex);
        if (!error.empty() && failure.empty()) failure = error;
        decoderDone = true;
        cv.notify_all();
    };
    auto emit = [&]() -> bool {
        const double pts = frame->best_effort_timestamp != AV_NOPTS_VALUE
                               ? static_cast<double>(frame->best_effort_timestamp) * timeBase
                               : lastPts + frameSeconds;
        Picture pic;
        pic.pts = pts;
        pic.rgba.resize(static_cast<size_t>(width) * height * 4);
        uint8_t* dst[1] = {pic.rgba.data()};
        const int dstStride[1] = {width * 4};
        sws_scale(sws, frame->data, frame->linesize, 0, height, dst, dstStride);
        std::unique_lock<std::mutex> l(mutex);
        cv.wait(l, [&] { return quit.load() || queue.size() < 4; });
        if (quit.load()) return false;
        queue.push_back(std::move(pic));
        lastPts = pts;
        return true;
    };
    do {
        if (!in.open(file, bytes, why)) { finish(why); break; }
        for (unsigned i = 0; i < in.fmt->nb_streams; ++i) {
            if (in.fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                videoIndex = static_cast<int>(i);
                break;
            }
        }
        if (videoIndex < 0) { finish("no picture in the file"); break; }
        AVStream* st = in.fmt->streams[videoIndex];
        timeBase = av_q2d(st->time_base);
        const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
        dec = codec ? avcodec_alloc_context3(codec) : nullptr;
        if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0 || avcodec_open2(dec, codec, nullptr) < 0) {
            finish("no decoder for the picture");
            break;
        }
        sws = sws_getContext(width, height, dec->pix_fmt, width, height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                             nullptr, nullptr);
        if (!sws) { finish("no colour conversion"); break; }
        bool going = true;
        while (going && !quit.load()) {
            const int r = av_read_frame(in.fmt, pkt);
            if (r < 0) break;
            if (pkt->stream_index == videoIndex && avcodec_send_packet(dec, pkt) >= 0) {
                while (going && avcodec_receive_frame(dec, frame) >= 0) going = emit();
            }
            av_packet_unref(pkt);
        }
        if (going && !quit.load()) {
            avcodec_send_packet(dec, nullptr);
            while (avcodec_receive_frame(dec, frame) >= 0 && emit()) {}
        }
        finish("");
    } while (false);
    if (sws) sws_freeContext(sws);
    if (dec) avcodec_free_context(&dec);
    av_frame_free(&frame);
    av_packet_free(&pkt);
}

void release(std::unique_ptr<Movie>& m) {
    if (!m) return;
    m->quit.store(true);
    m->cv.notify_all();
    if (m->worker.joinable()) m->worker.join();
    if (m->audioId != 0) audio::AudioEngine::instance().stopStream(m->audioId);
    m.reset();
}

}  // namespace

bool available() { return true; }

bool start(uint32_t widget, const std::string& name, int volume, std::string& why) {
    std::lock_guard<std::mutex> l(gLock);
    release(gMovie);
    auto m = std::make_unique<Movie>();
    m->widget = widget;
    // volume_0_to_255, clamped to 0..1 (0x0095e600).
    m->volume = std::clamp(static_cast<float>(volume) * 0.003921569f, 0.0f, 1.0f);
    const std::string rel = name + ".avi";
    m->file = findLooseFile(rel);
    auto* am = core::Application::getInstance().getAssetManager();
    if (m->file.empty() && am) {
        std::string norm = lower(rel);
        std::replace(norm.begin(), norm.end(), '/', '\\');
        if (std::string disk = am->resolveFile(norm); !disk.empty()) m->file = disk;
        else {
            auto bytes = am->readFileOptional(rel);
            if (!bytes.empty()) m->bytes = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
        }
    }
    if (m->file.empty() && !m->bytes) {
        why = rel + " not found";
        return false;
    }
    // The header now, so a file that will not play fails StartMovie.
    {
        Input in;
        if (!in.open(m->file, m->bytes, why)) return false;
        bool video = false;
        for (unsigned i = 0; i < in.fmt->nb_streams; ++i) {
            const auto* st = in.fmt->streams[i];
            if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) continue;
            if (!avcodec_find_decoder(st->codecpar->codec_id)) {
                why = "no decoder for its picture";
                return false;
            }
            m->width = st->codecpar->width;
            m->height = st->codecpar->height;
            if (st->avg_frame_rate.num > 0) m->frameSeconds = 1.0 / av_q2d(st->avg_frame_rate);
            video = true;
            break;
        }
        if (!video || m->width <= 0 || m->height <= 0) {
            why = "no picture in it";
            return false;
        }
    }
    if (am) {
        const auto sbt = am->readFileOptional(name + ".sbt");
        m->subtitles = parseSubtitles(std::string(sbt.begin(), sbt.end()));
    }
    LOG_INFO("Movie: ", name, " from ", m->file.empty() ? std::string("the archives") : m->file.string(), ", ",
             m->width, "x", m->height, ", ", m->subtitles.size(), " subtitle lines, volume ", volume);
    Movie* raw = m.get();
    m->worker = std::thread([raw] { raw->run(); });
    gMovie = std::move(m);
    gGeneration.fetch_add(1);
    return true;
}

bool stop() {
    std::lock_guard<std::mutex> l(gLock);
    if (!gMovie) return false;
    LOG_INFO("Movie: stopped");
    release(gMovie);
    return true;
}

void enableSubtitles(bool on) {
    std::lock_guard<std::mutex> l(gLock);
    gSubtitles = on;
}

bool playing() {
    std::lock_guard<std::mutex> l(gLock);
    return gMovie != nullptr;
}

uint32_t owner() {
    std::lock_guard<std::mutex> l(gLock);
    return gMovie ? gMovie->widget : 0;
}

void poll(std::vector<Event>& out) {
    std::lock_guard<std::mutex> l(gLock);
    Movie* m = gMovie.get();
    if (!m) return;
    std::unique_lock<std::mutex> ml(m->mutex);
    if (!m->started) {
        if (!m->ready && !m->decoderDone) return;
        m->started = true;
        m->t0 = Clock::now();
        if (m->mp3 && !m->mp3->empty()) {
            m->audioId = audio::AudioEngine::instance().playStream(m->mp3, m->volume, false);
        }
        LOG_INFO("Movie: playing, sound ", m->mp3 ? m->mp3->size() : 0, " bytes",
                 m->audioId ? "" : " (no sound stream)");
    }
    const double t = std::chrono::duration<double>(Clock::now() - m->t0).count();
    bool popped = false;
    while (!m->queue.empty() && m->queue.front().pts <= t) {
        m->current = std::move(m->queue.front());
        m->queue.pop_front();
        popped = true;
    }
    if (popped) {
        m->hasNewPicture = true;
        m->cv.notify_all();
    }
    const bool done = m->decoderDone && m->queue.empty() && t >= m->lastPts + m->frameSeconds;
    const std::string failure = m->failure;
    ml.unlock();

    // The subtitle lines (0x0095ebf0): the shown one hidden at its end, then
    // the line whose time it is, while subtitles are on or there is no sound.
    const uint32_t nowMs = static_cast<uint32_t>(t * 1000.0);
    if (m->shownSubtitle >= 0 && m->subtitles[static_cast<size_t>(m->shownSubtitle)].endMs <= nowMs) {
        out.push_back({Event::Kind::HideSubtitle, m->widget, {}});
        m->shownSubtitle = -1;
    }
    if (m->shownSubtitle < 0 && (gSubtitles || m->audioId == 0)) {
        for (size_t i = 0; i < m->subtitles.size(); ++i) {
            const auto& s = m->subtitles[i];
            if (s.startMs <= nowMs && nowMs < s.endMs) {
                out.push_back({Event::Kind::ShowSubtitle, m->widget, s.text});
                m->shownSubtitle = static_cast<int>(i);
                break;
            }
        }
    }
    if (done) {
        if (!failure.empty()) LOG_WARNING("Movie: ended early: ", failure);
        LOG_INFO("Movie: finished after ", t, " s");
        const uint32_t widget = m->widget;
        release(gMovie);
        out.push_back({Event::Kind::Finished, widget, {}});
    }
}

bool takeFrame(std::vector<uint8_t>& rgba, int& width, int& height) {
    std::lock_guard<std::mutex> l(gLock);
    Movie* m = gMovie.get();
    if (!m) return false;
    width = m->width;
    height = m->height;
    std::lock_guard<std::mutex> ml(m->mutex);
    if (!m->hasNewPicture) return false;
    m->hasNewPicture = false;
    rgba.swap(m->current.rgba);
    return !rgba.empty();
}

void pictureSize(int& width, int& height) {
    std::lock_guard<std::mutex> l(gLock);
    width = gMovie ? gMovie->width : 0;
    height = gMovie ? gMovie->height : 0;
}

uint32_t generation() { return gGeneration.load(); }

#endif  // HAVE_FFMPEG

}  // namespace wowee::ui::movie
