#pragma once

#include "pipeline/chunk_cache.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wowee {
namespace pipeline {

/**
 * DataStream - the "streaming" data mode: the original client's Data folder
 * read from a server (tools/data_stream_server) instead of a local disk.
 *
 * At start it fetches the server's file list (sizes, SHA-256) and lays out a
 * mirror of the folder under the cache directory: every file a sparse local
 * copy of its full size (ChunkCache). The MPQ reader opens the mirror like a
 * local install and asks for each byte range before it touches it
 * (MpqArchiveSet::Archive::need), so only what is read is fetched - the
 * archive headers and the hash and block tables at open, then the files the
 * game asks for. What was fetched stays, with a bitmap per file, so the next
 * start fetches nothing it already has.
 *
 * The access key goes in the Authorization header of each request, never in
 * a URL, and is never logged. Plain http:// is accepted for a loopback host
 * only (an SSH tunnel); anywhere else it must be https://, verified.
 *
 * Without the server (offline, a broken connection) the client starts from
 * the file list it kept and reads what the cache holds; a range it cannot
 * fetch reads as a missing file, as on a damaged install.
 */
struct StreamConfig {
    std::string url;   ///< e.g. https://example.org/wowee-data/
    std::string key;
};

/// The parts of a stream URL. False when it is not one this client accepts
/// (`error` says why): a scheme other than https, or http to a host that is
/// not this machine.
struct StreamUrl {
    bool tls = true;
    std::string host;
    std::string port;
    std::string basePath;  ///< always ends in '/'
};
bool parseStreamUrl(const std::string& url, StreamUrl& out, std::string& error);

class DataStream {
public:
    static DataStream& instance();

    /// Fetches the file list (or uses the one kept from last time when the
    /// server cannot be reached), lays out the mirror under `cacheRoot` and
    /// opens every file's chunk cache. Returns the mirror's root - the folder
    /// to read as the game folder - or empty with `error` set.
    std::string start(const StreamConfig& config, const std::string& cacheRoot, std::string& error);

    [[nodiscard]] bool active() const { return active_; }

    /// The chunk cache behind a mirror file, by the path the reader opened;
    /// null for any other file.
    ChunkCache* cacheFor(const std::string& localPath) const;

    struct Totals {
        uint64_t files = 0;
        uint64_t totalBytes = 0;   ///< every listed file
        uint64_t mpqBytes = 0;     ///< the .MPQ files among them
        uint64_t fetchedBytes = 0; ///< this run
        uint64_t requests = 0;     ///< this run
        uint64_t localBytes = 0;   ///< chunks held in the cache (all runs)
    };
    [[nodiscard]] Totals totals() const;

    /// The default cache folder for a stream URL: <per-user data>/stream-cache/<host>
    /// (WOWEE_STREAM_CACHE overrides the folder).
    static std::string defaultCacheRoot(const StreamUrl& url);

private:
    struct Entry {
        std::string rel;
        uint64_t size = 0;
        uint32_t chunk = 0;
        std::string sha256;
        std::unique_ptr<ChunkCache> cache;
    };
    std::vector<std::unique_ptr<Entry>> entries_;
    std::map<std::string, Entry*> byLocal_;   // normalised local path -> entry
    bool active_ = false;
};

/// Normalised form of a local path for the lookups above (absolute, generic
/// separators, lower case on Windows).
std::string streamPathKey(const std::string& path);

} // namespace pipeline
} // namespace wowee
