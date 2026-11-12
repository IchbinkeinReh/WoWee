#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace wowee {
namespace pipeline {

/**
 * MpqArchiveSet - the original client's Data folder read in place.
 *
 * The archives of a 3.3.5a install (Data\*.MPQ and Data\<locale>\*.MPQ) are
 * opened in the client's own order (common, common-2, expansion, lichking,
 * the locale archives, then patch / patch-<locale> / patch-2 / ... - the
 * table at 0x00405b40 in Wow.exe 12340, base before locale within a tier) and
 * a file is taken from the last archive that holds it. A deletion marker in a
 * later patch hides the file in every earlier archive, as in the client.
 *
 * The archives are memory-mapped and their hash and block tables are read
 * once at open; a lookup is a hash probe per archive and a read decodes the
 * file's sectors straight out of the mapping, so any number of threads read
 * at once without a lock. What 3.3.5a's archives hold is v1 headers, zlib
 * sectors (with or without the sector CRC table), single-unit files, plain
 * stored files and deletion markers; encryption (and the fix-key variant) is
 * handled as well. Other compressions (bzip2, implode, Huffman, ADPCM) do not
 * occur in a 3.3.5a install; a file using one is refused and named once.
 *
 * Paths are the asset manager's normalised form: lower case, backslashes.
 */
class MpqArchiveSet {
public:
    MpqArchiveSet();
    ~MpqArchiveSet();
    MpqArchiveSet(const MpqArchiveSet&) = delete;
    MpqArchiveSet& operator=(const MpqArchiveSet&) = delete;

    /// Whether `dir` (or a "Data" folder inside it) holds MPQ archives.
    /// Returns the folder that holds them in `archiveDir` when it does.
    static bool findArchiveDir(const std::string& dir, std::string* archiveDir = nullptr);

    /// The archives of `archiveDir` in load order (lowest priority first) and
    /// the locale folder found beside them ("" when none).
    static std::vector<std::string> discoverArchives(const std::string& archiveDir,
                                                     std::string& localeOut);

    /// Opens every archive of the Data folder `dir` (or `dir`\Data).
    bool open(const std::string& dir);
    void close();

    [[nodiscard]] bool isOpen() const { return !archives_.empty(); }
    [[nodiscard]] const std::string& archiveDir() const { return archiveDir_; }
    [[nodiscard]] const std::string& locale() const { return locale_; }
    [[nodiscard]] size_t archiveCount() const { return archives_.size(); }

    /// Whether the newest archive that names the file holds it (not a
    /// deletion marker).
    [[nodiscard]] bool contains(const std::string& normalizedPath) const;

    /// The file's bytes, or empty when no archive holds it (or it cannot be
    /// decoded).
    [[nodiscard]] std::vector<uint8_t> read(const std::string& normalizedPath) const;

    /// Every file under a normalised prefix (e.g. "interface\\icons\\"),
    /// known from the archives' (listfile)s and present in the set. The name
    /// index is built on the first call.
    [[nodiscard]] std::vector<std::string> list(const std::string& normalizedPrefix) const;

    struct Stats {
        uint64_t reads = 0;
        uint64_t bytes = 0;
        uint64_t failures = 0;
    };
    [[nodiscard]] Stats stats() const;

private:
    struct Archive;
    struct Hit {
        const Archive* archive = nullptr;
        uint32_t block = 0;
    };
    // The newest archive's entry for the name; archive null when none or
    // when that entry is a deletion marker.
    [[nodiscard]] Hit find(const std::string& normalizedPath) const;
    [[nodiscard]] bool decode(const Archive& a, uint32_t block, const std::string& name,
                              std::vector<uint8_t>& out) const;
    void buildNameIndex() const;

    std::vector<std::unique_ptr<Archive>> archives_;  // load order, newest last
    std::string archiveDir_;
    std::string locale_;

    mutable std::once_flag nameIndexOnce_;
    mutable std::vector<std::string> names_;  // sorted, normalised, present

    struct Counters;
    std::unique_ptr<Counters> counters_;
};

} // namespace pipeline
} // namespace wowee
