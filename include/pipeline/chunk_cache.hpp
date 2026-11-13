#pragma once

#include <condition_variable>
#include <cstdint>
#include <set>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace wowee {
namespace pipeline {

/**
 * ChunkCache - one remote file kept as a local sparse copy, filled chunk by
 * chunk as reads need it (the "streaming" data mode, pipeline/data_stream).
 *
 * The local copy has the remote file's full size from the start, so it can be
 * memory-mapped like any archive; only the chunks that were read hold data.
 * Which chunks are there is a bitmap beside it (<file>.idx), saved after each
 * fetch, so a restart does not fetch them again. The bitmap belongs to one
 * version of the remote file - its size, chunk size and SHA-256 from the
 * server's file list - and is thrown away (with the copy's contents) when
 * that changes.
 *
 * A chunk's bit is set only after its bytes are written to the copy, so a
 * crash of the client or a broken connection leaves at worst a chunk to fetch
 * again, never a chunk marked present that is not. (A power cut can lose
 * written pages the OS had not flushed yet; flushing after every chunk cost
 * too much while a zone loads.) Readers of one file fetch different runs at
 * once; a reader that needs a chunk another one is fetching waits for it.
 */
class ChunkBitmap {
public:
    void reset(uint64_t fileSize, uint32_t chunkSize, const std::string& identity);
    [[nodiscard]] uint64_t chunkCount() const { return count_; }
    [[nodiscard]] bool has(uint64_t chunk) const;
    void set(uint64_t chunk);
    void unset(uint64_t chunk);
    [[nodiscard]] uint64_t present() const;
    [[nodiscard]] const std::string& identity() const { return identity_; }

    /// Text header + raw bits. False when the file could not be written.
    bool save(const std::string& path) const;
    /// False (and nothing changed) when the file is missing, damaged, or for
    /// another size, chunk size or identity.
    bool load(const std::string& path, uint64_t fileSize, uint32_t chunkSize,
              const std::string& identity);

private:
    uint64_t size_ = 0;
    uint32_t chunk_ = 0;
    uint64_t count_ = 0;
    std::string identity_;
    std::vector<uint8_t> bits_;
};

class ChunkCache {
public:
    /// Fetches [offset, offset + length) of the remote file into `out`, which
    /// it must fill exactly and verify (the HTTP layer checks the server's
    /// SHA-256 of the body). False with `error` set when it could not.
    using Fetcher = std::function<bool(uint64_t offset, uint64_t length, std::vector<uint8_t>& out,
                                       std::string& error)>;

    ChunkCache() = default;
    ~ChunkCache();
    ChunkCache(const ChunkCache&) = delete;
    ChunkCache& operator=(const ChunkCache&) = delete;

    /// Opens (or creates, sparse, at full size) the local copy `dataPath` and
    /// its bitmap `dataPath + ".idx"`. A bitmap for another version of the
    /// file is dropped. False when the copy cannot be created.
    bool open(const std::string& dataPath, uint64_t fileSize, uint32_t chunkSize,
              const std::string& identity, Fetcher fetcher);

    /// Makes [offset, offset + length) present locally, fetching the missing
    /// chunks (consecutive ones in one request, up to maxRequestBytes). True
    /// when all of it is present. Thread-safe; blocks while fetching.
    bool ensure(uint64_t offset, uint64_t length);

    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] uint64_t size() const { return size_; }
    [[nodiscard]] uint64_t chunksPresent() const;
    [[nodiscard]] uint64_t chunkCount() const;
    [[nodiscard]] uint64_t bytesFetched() const { return fetched_; }
    [[nodiscard]] uint64_t requests() const { return requests_; }
    /// The bytes the local copy holds (present chunks, the last one short).
    [[nodiscard]] uint64_t bytesPresent() const;

    /// Drops every chunk: the bitmap is emptied and saved, and the copy's
    /// room given back (it is cut to nothing and grown again, sparse). Only
    /// while nothing reads the copy - at start, before an archive maps it.
    bool clear();

    /// Fetches that this thread completed, in all caches, ever. A caller
    /// compares it before and after ensure() to learn whether that read had
    /// to go to the server.
    static uint64_t threadFetchCount();

    // ---- the size limit ------------------------------------------------
    //
    // Every chunk has the number of the run that last read it, kept beside
    // the copy (<file>.use) and saved by saveUse(). Over the limit, the
    // chunks read longest ago go first, a whole run's worth at a time
    // (chooseUseCutoff), and their room is given back (a hole punched in the
    // sparse copy). An archive's tables are read at every start, so they
    // are never the oldest.

    /// The number of this run (DataStream counts starts per cache folder).
    static void setRun(uint32_t run);
    /// Saves the chunks' last-read runs when they changed. False when the
    /// file could not be written.
    bool saveUse();
    /// Bytes held, by the run that last read them.
    void addBytesByLastUse(std::map<uint32_t, uint64_t>& out) const;
    /// Drops the chunks last read before run `cutoff`. Only while nothing
    /// reads the copy - at start, before an archive maps it. Returns the
    /// bytes dropped.
    uint64_t dropReadBefore(uint32_t cutoff);

    /// Largest single request, in bytes.
    static constexpr uint64_t kMaxRequestBytes = 8ull * 1024 * 1024;

private:
    bool writeAt(uint64_t offset, const uint8_t* data, size_t length);

    mutable std::mutex mutex_;
    std::condition_variable inFlightDone_;
    std::set<uint64_t> inFlight_;   // chunks a reader is fetching now
    uint64_t failedEpoch_ = 0;      // fetches that failed, ever
    std::string path_;
    std::string indexPath_;
    uint64_t size_ = 0;
    uint32_t chunk_ = 0;
    ChunkBitmap bitmap_;
    Fetcher fetcher_;
    uint64_t fetched_ = 0;
    uint64_t requests_ = 0;
    bool loggedFailure_ = false;
    std::string usePath_;
    std::vector<uint32_t> lastUse_;   // per chunk: the run that last read it
    bool useDirty_ = false;
    /// Gives back the room of [offset, offset + length) of the copy.
    void punchHole(uint64_t offset, uint64_t length);
#ifdef _WIN32
    void* file_ = nullptr;
#else
    int fd_ = -1;
#endif
};

/// For the size limit: given the bytes held by the run that last read them,
/// the run before which everything is dropped so that the rest fits in nine
/// tenths of `limit` - room for a session to grow without dropping again at
/// every start. 0 (drop nothing) when it fits already or `limit` is 0. What
/// the newest start (or this one, `current`) read stays, even over the limit.
uint32_t chooseUseCutoff(const std::map<uint32_t, uint64_t>& bytesByRun, uint64_t limit,
                         uint32_t current);

} // namespace pipeline
} // namespace wowee
