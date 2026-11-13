#include "pipeline/data_stream.hpp"
#include "pipeline/mpq_archive_set.hpp"

#include "core/config_paths.hpp"
#include "core/data_paths.hpp"
#include "core/logger.hpp"
#include "core/tls_trust.hpp"
#include "network/net_platform.hpp"

#include <nlohmann/json.hpp>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

namespace wowee {
namespace pipeline {

namespace fs = std::filesystem;

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isLoopback(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1" || host == "[::1]";
}

std::string hexSha256(const uint8_t* data, size_t n) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_Digest(data, n, md, &len, EVP_sha256(), nullptr);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < len; ++i) {
        out += hex[md[i] >> 4];
        out += hex[md[i] & 15];
    }
    return out;
}

/// Percent-encodes a relative path for the URL, keeping '/'.
std::string encodePath(const std::string& rel) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : rel) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// ---- a small keep-alive HTTP(S) client -----------------------------------------
//
// Not https_get's one-shot GET: a zone load asks for hundreds of ranges, and a
// TLS handshake for each would cost more than the bytes. Connections are kept
// in a pool and reused; a broken one is dropped and the request tried again on
// a new one, a few times, with a pause that grows.

struct Connection {
    socket_t sock = INVALID_SOCK;
    SSL* ssl = nullptr;
    std::string pending;   // bytes read past the last reply
    bool closed = false;   // the server said it closes after this reply
    bool reused = false;   // taken from the pool rather than just opened
    std::chrono::steady_clock::time_point idleSince = std::chrono::steady_clock::now();
    ~Connection() {
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        if (sock != INVALID_SOCK) net::closeSocket(sock);
    }
    int read(char* buf, int n) {
        if (ssl) return SSL_read(ssl, buf, n);
        return static_cast<int>(::recv(sock, buf, n, 0));
    }
    bool writeAll(const std::string& s) {
        size_t done = 0;
        while (done < s.size()) {
            int n = ssl ? SSL_write(ssl, s.data() + done, static_cast<int>(s.size() - done))
                        : static_cast<int>(::send(sock, s.data() + done, static_cast<int>(s.size() - done), 0));
            if (n <= 0) return false;
            done += static_cast<size_t>(n);
        }
        return true;
    }
};

class HttpClient {
public:
    HttpClient(StreamUrl url, std::string key) : url_(std::move(url)), key_(std::move(key)) {}
    ~HttpClient() {
        if (ctx_) SSL_CTX_free(ctx_);
    }

    struct Reply {
        int status = 0;
        std::string contentSha256;
        std::vector<uint8_t> body;
    };

    /// GET basePath + path (with a byte range when `to` > `from`). Retries a
    /// broken connection; false with `error` when it could not get a reply.
    bool get(const std::string& path, uint64_t from, uint64_t to, Reply& reply, std::string& error) {
        static const int kPauseMs[] = {0, 250, 1000, 3000};
        for (int attempt = 0; attempt < 4; ++attempt) {
            if (kPauseMs[attempt]) std::this_thread::sleep_for(std::chrono::milliseconds(kPauseMs[attempt]));
            std::unique_ptr<Connection> c = take(error);
            if (!c) continue;
            if (once(*c, path, from, to, reply, error)) {
                if (!c->closed) give(std::move(c));
                return true;
            }
            // A kept connection the server had closed meanwhile (its
            // keep-alive timeout, its request limit) is no failure of the
            // server: the same try again on a new one, at once.
            if (c->reused) {
                c = connect(error);
                if (c && once(*c, path, from, to, reply, error)) {
                    give(std::move(c));
                    return true;
                }
            }
            // The connection is dropped with c; the next try opens another.
        }
        return false;
    }

private:
    std::unique_ptr<Connection> take(std::string& error) {
        {
            std::lock_guard<std::mutex> lock(poolMutex_);
            // Newest first; one idle for longer than a web server keeps an
            // idle connection open (Apache: 5 s) is closed rather than tried.
            const auto now = std::chrono::steady_clock::now();
            while (!pool_.empty()) {
                auto c = std::move(pool_.back());
                pool_.pop_back();
                if (now - c->idleSince > std::chrono::seconds(4)) continue;
                c->reused = true;
                return c;
            }
        }
        return connect(error);
    }

    void give(std::unique_ptr<Connection> c) {
        if (c->closed) return;
        c->idleSince = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(poolMutex_);
        if (pool_.size() < 8) pool_.push_back(std::move(c));
    }

    std::unique_ptr<Connection> connect(std::string& error) {
        net::ensureInit();
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* found = nullptr;
        if (getaddrinfo(url_.host.c_str(), url_.port.c_str(), &hints, &found) != 0 || !found) {
            error = "cannot resolve " + url_.host;
            return nullptr;
        }
        auto c = std::make_unique<Connection>();
        for (addrinfo* at = found; at; at = at->ai_next) {
            socket_t s = ::socket(at->ai_family, at->ai_socktype, at->ai_protocol);
            if (s == INVALID_SOCK) continue;
#ifdef _WIN32
            const DWORD timeout = 20000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
            timeval timeout{};
            timeout.tv_sec = 20;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
            if (::connect(s, at->ai_addr, static_cast<int>(at->ai_addrlen)) == 0) {
                c->sock = s;
                break;
            }
            net::closeSocket(s);
        }
        freeaddrinfo(found);
        if (c->sock == INVALID_SOCK) {
            error = "cannot reach " + url_.host;
            return nullptr;
        }
        if (!url_.tls) return c;

        {
            std::lock_guard<std::mutex> lock(poolMutex_);
            if (!ctx_) {
                ctx_ = SSL_CTX_new(TLS_client_method());
                if (ctx_) {
                    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
                    core::loadTrustStore(ctx_);
                    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
                }
            }
        }
        if (!ctx_) {
            error = "no TLS context";
            return nullptr;
        }
        c->ssl = SSL_new(ctx_);
        if (!c->ssl) {
            error = "no TLS session";
            return nullptr;
        }
        SSL_set_tlsext_host_name(c->ssl, url_.host.c_str());
        X509_VERIFY_PARAM_set1_host(SSL_get0_param(c->ssl), url_.host.c_str(), 0);
        SSL_set_fd(c->ssl, static_cast<int>(c->sock));
        ERR_clear_error();
        if (SSL_connect(c->ssl) != 1) {
            const unsigned long code = ERR_get_error();
            char buf[256] = "no detail";
            if (code) ERR_error_string_n(code, buf, sizeof(buf));
            error = std::string("TLS handshake failed: ") + buf;
            return nullptr;
        }
        return c;
    }

    bool once(Connection& c, const std::string& path, uint64_t from, uint64_t to, Reply& reply,
              std::string& error) {
        std::string req = "GET " + url_.basePath + path + " HTTP/1.1\r\nHost: " + url_.host +
                          "\r\nUser-Agent: wowee-stream/1\r\nAuthorization: Bearer " + key_ +
                          "\r\nConnection: keep-alive\r\n";
        if (to > from) req += "Range: bytes=" + std::to_string(from) + "-" + std::to_string(to - 1) + "\r\n";
        req += "\r\n";
        if (!c.writeAll(req)) {
            error = "the connection broke while sending";
            return false;
        }
        std::string& buf = c.pending;
        char tmp[65536];
        size_t split;
        while ((split = buf.find("\r\n\r\n")) == std::string::npos) {
            if (buf.size() > 65536) {
                error = "reply headers too long";
                return false;
            }
            const int n = c.read(tmp, sizeof(tmp));
            if (n <= 0) {
                error = "the connection broke before the reply";
                return false;
            }
            buf.append(tmp, static_cast<size_t>(n));
        }
        const std::string head = buf.substr(0, split);
        buf.erase(0, split + 4);
        if (head.rfind("HTTP/1.", 0) != 0 || head.size() < 12) {
            error = "not an HTTP reply";
            return false;
        }
        reply.status = std::atoi(head.c_str() + 9);
        const std::string lowerHead = lowerAscii(head);
        auto header = [&](const std::string& name) -> std::string {
            const size_t at = lowerHead.find("\r\n" + name + ":");
            if (at == std::string::npos) return {};
            size_t v = at + name.size() + 3;
            const size_t end = head.find("\r\n", v);
            std::string out = head.substr(v, end == std::string::npos ? std::string::npos : end - v);
            while (!out.empty() && out.front() == ' ') out.erase(out.begin());
            while (!out.empty() && out.back() == ' ') out.pop_back();
            return out;
        };
        const std::string lenText = header("content-length");
        if (lenText.empty()) {
            error = "reply without Content-Length";
            return false;
        }
        const uint64_t len = std::strtoull(lenText.c_str(), nullptr, 10);
        if (len > 64ull * 1024 * 1024) {
            error = "reply too large";
            return false;
        }
        reply.contentSha256 = lowerAscii(header("x-content-sha256"));
        reply.body.clear();
        reply.body.reserve(static_cast<size_t>(len));
        const size_t fromBuf = std::min<size_t>(buf.size(), static_cast<size_t>(len));
        reply.body.insert(reply.body.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(fromBuf));
        buf.erase(0, fromBuf);
        while (reply.body.size() < len) {
            const int want = static_cast<int>(std::min<uint64_t>(sizeof(tmp), len - reply.body.size()));
            const int n = c.read(tmp, want);
            if (n <= 0) {
                error = "the connection broke during the reply";
                return false;
            }
            reply.body.insert(reply.body.end(), tmp, tmp + n);
        }
        if (lowerHead.find("\r\nconnection: close") != std::string::npos) c.closed = true;
        return true;
    }

    StreamUrl url_;
    std::string key_;
    SSL_CTX* ctx_ = nullptr;
    std::mutex poolMutex_;
    std::vector<std::unique_ptr<Connection>> pool_;
};

std::unique_ptr<HttpClient> g_client;
std::atomic<uint64_t> g_fetched{0};
std::atomic<uint64_t> g_requests{0};
std::atomic<uint64_t> g_filesFetched{0};
std::atomic<int64_t> g_lastFetchMs{-1};   // steady clock, ms

int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// The stream cache's two rows of settings.cfg, read before the settings
/// panel exists (as update_check reads its own row).
struct CacheSettings {
    int limitChoice = 0;
    bool clear = false;
};
CacheSettings readCacheSettings() {
    CacheSettings out;
    std::ifstream in(core::getConfigRoot() + "/settings.cfg");
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string val = line.substr(eq + 1);
        if (key == "stream_cache_limit") out.limitChoice = std::atoi(val.c_str());
        else if (key == "stream_cache_clear") out.clear = std::atoi(val.c_str()) != 0;
    }
    return out;
}

/// The clear is asked for once: done, its row goes back to 0, so the next
/// start keeps what this one fetches.
void resetCacheClearSetting() {
    const std::string path = core::getConfigRoot() + "/settings.cfg";
    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return;
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string key = "stream_cache_clear=";
    size_t at = std::string::npos;
    if (text.compare(0, key.size(), key) == 0) {
        at = 0;
    } else {
        const size_t nl = text.find("\n" + key);
        if (nl != std::string::npos) at = nl + 1;
    }
    if (at == std::string::npos) return;
    const size_t end = text.find_first_of("\r\n", at);
    text.replace(at, (end == std::string::npos ? text.size() : end) - at, key + "0");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

}  // namespace

bool parseStreamUrl(const std::string& url, StreamUrl& out, std::string& error) {
    std::string rest;
    if (url.rfind("https://", 0) == 0) {
        out.tls = true;
        out.port = "443";
        rest = url.substr(8);
    } else if (url.rfind("http://", 0) == 0) {
        out.tls = false;
        out.port = "80";
        rest = url.substr(7);
    } else {
        error = "the address must start with https://";
        return false;
    }
    const size_t slash = rest.find('/');
    std::string hostPort = rest.substr(0, slash);
    out.basePath = slash == std::string::npos ? "/" : rest.substr(slash);
    if (out.basePath.empty() || out.basePath.back() != '/') out.basePath += '/';
    const size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos && hostPort.find(']') == std::string::npos) {
        out.port = hostPort.substr(colon + 1);
        hostPort = hostPort.substr(0, colon);
    }
    out.host = hostPort;
    if (out.host.empty()) {
        error = "no host in the address";
        return false;
    }
    if (!out.tls && !isLoopback(out.host)) {
        error = "plain http:// only to this machine; use https://";
        return false;
    }
    return true;
}

std::string streamPathKey(const std::string& path) {
    std::error_code ec;
    fs::path p = fs::absolute(fs::path(path), ec).lexically_normal();
    std::string s = p.generic_string();
#ifdef _WIN32
    s = lowerAscii(s);
#endif
    return s;
}

DataStream& DataStream::instance() {
    static DataStream ds;
    return ds;
}

std::string DataStream::defaultCacheRoot(const StreamUrl& url) {
    if (const char* env = std::getenv("WOWEE_STREAM_CACHE"); env && *env) return env;
    std::string host = url.host + (url.port == "443" || url.port == "80" ? "" : "_" + url.port);
    for (char& c : host) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-') c = '_';
    }
    return (core::userDataRoot() / "stream-cache" / host).string();
}

std::string DataStream::start(const StreamConfig& config, const std::string& cacheRoot,
                              std::string& error) {
    StreamUrl url;
    if (!parseStreamUrl(config.url, url, error)) return {};
    if (config.key.empty()) {
        error = "no access key";
        return {};
    }
    g_client = std::make_unique<HttpClient>(url, config.key);
    std::error_code ec;
    fs::create_directories(cacheRoot, ec);
    const fs::path listCopy = fs::path(cacheRoot) / "files.json";

    // The file list: from the server, kept for a start without it.
    std::string listText;
    HttpClient::Reply reply;
    std::string netError;
    if (g_client->get("v1/files", 0, 0, reply, netError) && reply.status == 200) {
        listText.assign(reply.body.begin(), reply.body.end());
        std::ofstream(listCopy, std::ios::binary | std::ios::trunc) << listText;
    } else {
        if (reply.status == 401) {
            error = "the server refused the access key";
            return {};
        }
        std::ifstream in(listCopy, std::ios::binary);
        listText.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (listText.empty()) {
            error = "cannot get the file list: " + (netError.empty() ? "status " + std::to_string(reply.status) : netError);
            return {};
        }
        LOG_WARNING("STREAM: server not reachable (", netError, "); using the file list from the last start");
    }

    nlohmann::json list = nlohmann::json::parse(listText, nullptr, false);
    if (list.is_discarded() || !list.contains("files") || !list["files"].is_array()) {
        error = "the server's file list is not readable";
        return {};
    }
    const uint32_t chunk = list.value("chunkSize", 262144u);

    // This start's number in this cache folder: the chunks it reads are
    // stamped with it, for the size limit (ChunkCache::setRun).
    {
        const fs::path runFile = fs::path(cacheRoot) / "runs.txt";
        uint64_t runs = 0;
        std::ifstream(runFile) >> runs;
        runs = std::min<uint64_t>(runs + 1, 0xFFFFFFF0u);
        std::ofstream(runFile, std::ios::trunc) << runs << '\n';
        run_ = static_cast<uint32_t>(runs);
        ChunkCache::setRun(run_);
    }
    const fs::path mirror = fs::path(cacheRoot) / "mirror";

    entries_.clear();
    byLocal_.clear();
    for (const auto& f : list["files"]) {
        auto e = std::make_unique<Entry>();
        e->rel = f.value("path", std::string());
        e->size = f.value("size", uint64_t{0});
        e->chunk = chunk;
        e->sha256 = f.contains("sha256") && f["sha256"].is_string() ? f["sha256"].get<std::string>() : "";
        // Only paths under Data/, never leaving the mirror.
        if (e->rel.rfind("Data/", 0) != 0 || e->rel.find("..") != std::string::npos || e->size == 0) continue;
        const std::string identity =
            e->sha256.empty() ? "size" + std::to_string(e->size) + "-mtime" + std::to_string(f.value("mtime", 0ll))
                              : e->sha256;
        const fs::path local = mirror / fs::path(e->rel);
        const std::string rel = e->rel;
        e->cache = std::make_unique<ChunkCache>();
        auto fetch = [rel](uint64_t off, uint64_t len, std::vector<uint8_t>& out, std::string& err) {
            HttpClient::Reply r;
            if (!g_client || !g_client->get("v1/data/" + encodePath(rel), off, off + len, r, err)) return false;
            if (r.status != 206) {
                err = "status " + std::to_string(r.status);
                return false;
            }
            if (r.body.size() != len) {
                err = "short reply";
                return false;
            }
            // The server's hash of what it sent, against what arrived.
            if (r.contentSha256.empty() || hexSha256(r.body.data(), r.body.size()) != r.contentSha256) {
                err = "the chunk failed its SHA-256 check";
                return false;
            }
            g_fetched += r.body.size();
            ++g_requests;
            g_lastFetchMs = steadyMs();
            {
                static std::mutex logMutex;
                static auto lastLog = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> lock(logMutex);
                const auto now = std::chrono::steady_clock::now();
                if (now - lastLog > std::chrono::seconds(5)) {
                    lastLog = now;
                    LOG_WARNING("STREAM: fetching, ", g_fetched.load(), " bytes in ", g_requests.load(),
                                " requests so far");
                }
            }
            out = std::move(r.body);
            return true;
        };
        if (!e->cache->open(local.string(), e->size, chunk, identity, fetch)) {
            LOG_WARNING("STREAM: cannot create the cache copy ", local.string());
            continue;
        }
        byLocal_[streamPathKey(local.string())] = e.get();
        entries_.push_back(std::move(e));
    }
    active_ = !entries_.empty();
    applyCacheSettings();
    MpqArchiveSet::setRangeSource([this](const std::string& path) -> MpqArchiveSet::RangeFetch {
        ChunkCache* cache = cacheFor(path);
        if (!cache) return {};
        return [cache](uint64_t offset, uint64_t length) {
            // One read: an archive's tables at open, or one game file.
            const uint64_t before = ChunkCache::threadFetchCount();
            const bool ok = cache->ensure(offset, length);
            if (ChunkCache::threadFetchCount() != before) ++g_filesFetched;
            return ok;
        };
    });
    if (!active_) {
        error = "the server lists no files";
        return {};
    }
    const Totals t = totals();
    LOG_WARNING("STREAM: ", t.files, " files, ", t.mpqBytes, " bytes of MPQ (", t.totalBytes,
                " in all), ", t.localBytes, " bytes already cached, from ", url.host, url.basePath);
    return mirror.string();
}

uint64_t DataStream::cacheLimitForChoice(int choice) {
    constexpr uint64_t kGB = 1024ull * 1024 * 1024;
    switch (choice) {
        case 1: return 1 * kGB;
        case 2: return 2 * kGB;
        case 3: return 5 * kGB;
        case 4: return 10 * kGB;
        default: return 0;
    }
}

void DataStream::applyCacheSettings() {
    // Here, at start, and nowhere later: once the archives are open they are
    // mapped, and a chunk taken from under a reader would read as zeros. So
    // the limit holds at each start; a session can go past it until the next.
    const CacheSettings settings = readCacheSettings();
    if (settings.clear) {
        uint64_t freed = 0;
        size_t failed = 0;
        for (const auto& e : entries_) {
            const uint64_t held = e->cache->bytesPresent();
            if (held == 0) continue;
            if (e->cache->clear()) freed += held;
            else ++failed;
        }
        LOG_WARNING("STREAM: cache cleared as asked in the settings: ", freed, " bytes freed",
                    failed ? (", " + std::to_string(failed) + " files could not be cleared") : std::string());
        resetCacheClearSetting();
    }
    const uint64_t limit = cacheLimitForChoice(settings.limitChoice);
    if (limit == 0) return;
    std::map<uint32_t, uint64_t> byRun;
    for (const auto& e : entries_) e->cache->addBytesByLastUse(byRun);
    uint64_t held = 0;
    for (const auto& [run, bytes] : byRun) held += bytes;
    const uint32_t cutoff = chooseUseCutoff(byRun, limit, run_);
    if (cutoff == 0) {
        LOG_WARNING("STREAM: cache holds ", held, " bytes, within its limit of ", limit);
        return;
    }
    // The chunks read longest ago, a whole start's worth at a time.
    uint64_t freed = 0;
    size_t files = 0;
    for (const auto& e : entries_) {
        const uint64_t dropped = e->cache->dropReadBefore(cutoff);
        if (dropped == 0) continue;
        freed += dropped;
        ++files;
        LOG_WARNING("STREAM: cache over its limit: ", dropped, " bytes of ", e->rel, " dropped");
    }
    LOG_WARNING("STREAM: cache held ", held, " bytes, limit ", limit, "; dropped what was last read before start ",
                cutoff, " of ", run_, ": ", freed, " bytes in ", files, " files");
}

void DataStream::saveUse() {
    for (const auto& e : entries_) e->cache->saveUse();
}

ChunkCache* DataStream::cacheFor(const std::string& localPath) const {
    if (!active_) return nullptr;
    auto it = byLocal_.find(streamPathKey(localPath));
    return it == byLocal_.end() ? nullptr : it->second->cache.get();
}

DataStream::Totals DataStream::totals() const {
    Totals t;
    for (const auto& e : entries_) {
        ++t.files;
        t.totalBytes += e->size;
        const std::string lower = lowerAscii(e->rel);
        if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".mpq") == 0) t.mpqBytes += e->size;
        t.localBytes += e->cache->bytesPresent();
    }
    t.fetchedBytes = g_fetched.load();
    t.requests = g_requests.load();
    t.filesFetched = g_filesFetched.load();
    const int64_t last = g_lastFetchMs.load();
    t.secondsSinceFetch = last < 0 ? -1.0 : static_cast<double>(steadyMs() - last) / 1000.0;
    return t;
}

} // namespace pipeline
} // namespace wowee
