#include "pipeline/chunk_cache.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace wowee {
namespace pipeline {

namespace fs = std::filesystem;

// ---- bitmap --------------------------------------------------------------------

namespace {
constexpr const char* kIndexMagic = "wowee-chunks 1";
}

void ChunkBitmap::reset(uint64_t fileSize, uint32_t chunkSize, const std::string& identity) {
    size_ = fileSize;
    chunk_ = chunkSize;
    count_ = chunkSize ? (fileSize + chunkSize - 1) / chunkSize : 0;
    identity_ = identity;
    bits_.assign(static_cast<size_t>((count_ + 7) / 8), 0);
}

bool ChunkBitmap::has(uint64_t c) const {
    return c < count_ && (bits_[static_cast<size_t>(c / 8)] & (1u << (c % 8))) != 0;
}

void ChunkBitmap::set(uint64_t c) {
    if (c < count_) bits_[static_cast<size_t>(c / 8)] |= static_cast<uint8_t>(1u << (c % 8));
}

uint64_t ChunkBitmap::present() const {
    uint64_t n = 0;
    for (uint8_t b : bits_) {
        for (; b; b &= static_cast<uint8_t>(b - 1)) ++n;
    }
    return n;
}

bool ChunkBitmap::save(const std::string& path) const {
    // Written beside and renamed over, so a crash mid-write leaves the old
    // bitmap rather than half of one.
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << kIndexMagic << '\n' << size_ << ' ' << chunk_ << ' ' << count_ << ' '
            << (identity_.empty() ? std::string("-") : identity_) << '\n';
        out.write(reinterpret_cast<const char*>(bits_.data()), static_cast<std::streamsize>(bits_.size()));
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
    }
    return !ec;
}

bool ChunkBitmap::load(const std::string& path, uint64_t fileSize, uint32_t chunkSize,
                       const std::string& identity) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string magic, line;
    if (!std::getline(in, magic) || magic != kIndexMagic) return false;
    if (!std::getline(in, line)) return false;
    std::istringstream hs(line);
    uint64_t size = 0, count = 0;
    uint32_t chunk = 0;
    std::string id;
    if (!(hs >> size >> chunk >> count >> id)) return false;
    if (id == "-") id.clear();
    if (size != fileSize || chunk != chunkSize || id != identity || chunk == 0 ||
        count != (size + chunk - 1) / chunk) {
        return false;
    }
    std::vector<uint8_t> bits(static_cast<size_t>((count + 7) / 8));
    in.read(reinterpret_cast<char*>(bits.data()), static_cast<std::streamsize>(bits.size()));
    if (in.gcount() != static_cast<std::streamsize>(bits.size())) return false;
    size_ = size;
    chunk_ = chunk;
    count_ = count;
    identity_ = id;
    bits_ = std::move(bits);
    return true;
}

// ---- cache ---------------------------------------------------------------------

ChunkCache::~ChunkCache() {
#ifdef _WIN32
    if (file_) CloseHandle(static_cast<HANDLE>(file_));
#else
    if (fd_ >= 0) ::close(fd_);
#endif
}

bool ChunkCache::open(const std::string& dataPath, uint64_t fileSize, uint32_t chunkSize,
                      const std::string& identity, Fetcher fetcher) {
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = dataPath;
    indexPath_ = dataPath + ".idx";
    size_ = fileSize;
    chunk_ = chunkSize;
    fetcher_ = std::move(fetcher);
    std::error_code ec;
    fs::create_directories(fs::path(dataPath).parent_path(), ec);

    const bool known = bitmap_.load(indexPath_, fileSize, chunkSize, identity);
    if (!known) {
        // Another version of the file, or none yet: what the copy holds is
        // not to be trusted, so it starts over empty.
        bitmap_.reset(fileSize, chunkSize, identity);
        fs::remove(dataPath, ec);
    }

#ifdef _WIN32
    HANDLE h = CreateFileW(fs::path(dataPath).wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    file_ = h;
    // Sparse, so a copy of a 3 GB archive takes the room of what was read.
    DWORD ret = 0;
    DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr);
    LARGE_INTEGER cur{};
    GetFileSizeEx(h, &cur);
    if (static_cast<uint64_t>(cur.QuadPart) != fileSize) {
        LARGE_INTEGER want{};
        want.QuadPart = static_cast<LONGLONG>(fileSize);
        if (!SetFilePointerEx(h, want, nullptr, FILE_BEGIN) || !SetEndOfFile(h)) return false;
    }
#else
    fd_ = ::open(dataPath.c_str(), O_RDWR | O_CREAT, 0600);
    if (fd_ < 0) return false;
    struct stat st{};
    if (fstat(fd_, &st) != 0) return false;
    if (static_cast<uint64_t>(st.st_size) != fileSize &&
        ftruncate(fd_, static_cast<off_t>(fileSize)) != 0) {
        return false;
    }
#endif
    if (!known) bitmap_.save(indexPath_);
    return true;
}

bool ChunkCache::writeAt(uint64_t offset, const uint8_t* data, size_t length) {
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(file_);
    size_t done = 0;
    while (done < length) {
        OVERLAPPED ov{};
        const uint64_t at = offset + done;
        ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        const DWORD want = static_cast<DWORD>(std::min<size_t>(length - done, 1u << 30));
        DWORD wrote = 0;
        if (!WriteFile(h, data + done, want, &wrote, &ov) || wrote == 0) return false;
        done += wrote;
    }
    return true;
#else
    size_t done = 0;
    while (done < length) {
        const ssize_t n = ::pwrite(fd_, data + done, length - done, static_cast<off_t>(offset + done));
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
#endif
}

bool ChunkCache::ensure(uint64_t offset, uint64_t length) {
    if (length == 0) return true;
    std::unique_lock<std::mutex> lock(mutex_);
    if (offset >= size_ || length > size_ - offset || chunk_ == 0) return false;
    const uint64_t first = offset / chunk_;
    const uint64_t last = (offset + length - 1) / chunk_;
    const uint64_t perRequest = std::max<uint64_t>(1, kMaxRequestBytes / chunk_);
    const uint64_t epoch0 = failedEpoch_;
    for (;;) {
        // The first chunk still missing that nobody is fetching: claim the
        // run of missing chunks from there and fetch it without the lock, so
        // other readers of this file go on (and fetch other runs) meanwhile.
        bool waiting = false;
        uint64_t c = first;
        for (; c <= last; ++c) {
            if (bitmap_.has(c)) continue;
            if (inFlight_.count(c)) { waiting = true; continue; }
            break;
        }
        if (c > last) {
            if (!waiting) return true;
            inFlightDone_.wait(lock);
            // Another reader's fetch of these chunks failed: this read takes
            // them on itself (the loop claims them), a few times at most.
            if (failedEpoch_ - epoch0 > 2) return false;
            continue;
        }
        uint64_t end = c;
        while (end + 1 <= last && !bitmap_.has(end + 1) && !inFlight_.count(end + 1) &&
               end + 1 - c < perRequest) {
            ++end;
        }
        for (uint64_t k = c; k <= end; ++k) inFlight_.insert(k);
        const uint64_t from = c * chunk_;
        const uint64_t to = std::min<uint64_t>((end + 1) * chunk_, size_);
        lock.unlock();

        std::vector<uint8_t> bytes;
        std::string error;
        bool ok = fetcher_ && fetcher_(from, to - from, bytes, error) && bytes.size() == to - from;
        if (ok && !writeAt(from, bytes.data(), bytes.size())) {
            ok = false;
            error = "could not write the cache copy";
        }

        lock.lock();
        for (uint64_t k = c; k <= end; ++k) inFlight_.erase(k);
        if (ok) {
            ++requests_;
            fetched_ += bytes.size();
            for (uint64_t k = c; k <= end; ++k) bitmap_.set(k);
            bitmap_.save(indexPath_);
        } else {
            ++failedEpoch_;
            if (!loggedFailure_) {
                LOG_WARNING("STREAM: could not fetch ", fs::path(path_).filename().string(), " bytes ",
                            from, "-", to, ": ", error.empty() ? std::string("short reply") : error);
                loggedFailure_ = true;
            }
        }
        inFlightDone_.notify_all();
        if (!ok) return false;
    }
}

uint64_t ChunkCache::chunksPresent() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bitmap_.present();
}

uint64_t ChunkCache::chunkCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bitmap_.chunkCount();
}

} // namespace pipeline
} // namespace wowee
