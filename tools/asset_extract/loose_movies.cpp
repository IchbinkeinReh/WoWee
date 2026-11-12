#include "loose_movies.hpp"
#include "path_mapper.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace fs = std::filesystem;

namespace wowee {
namespace tools {

namespace {

std::string toLowerStr(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string findCaseInsensitiveDirectory(const std::string& parent, const std::string& name) {
    std::error_code ec;
    const std::string want = toLowerStr(name);
    for (const auto& e : fs::directory_iterator(parent, ec)) {
        if (e.is_directory(ec) && toLowerStr(e.path().filename().string()) == want) {
            return e.path().filename().string();
        }
    }
    return {};
}

/// A locale folder beside the archives: four letters, xxYY (deDE, enUS ...).
std::string findLocale(const std::string& dataDir) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dataDir, ec)) {
        if (!e.is_directory(ec)) continue;
        const std::string n = e.path().filename().string();
        if (n.size() == 4 && std::islower(static_cast<unsigned char>(n[0])) &&
            std::islower(static_cast<unsigned char>(n[1])) && std::isupper(static_cast<unsigned char>(n[2])) &&
            std::isupper(static_cast<unsigned char>(n[3]))) {
            return n;
        }
    }
    return {};
}

std::string normalizeWowPath(std::string p) {
    std::replace(p.begin(), p.end(), '/', '\\');
    return toLowerStr(p);
}

}  // namespace

// The client's movies are not in its archives: the installer leaves them
// loose beside the locale's archives, as Data\<locale>\Interface\Cinematics\*.avi
// (WOW_Wrathgate_1024.avi), where Storm finds them as files. They are
// copied into the tree like any extracted file, so the movie player finds
// them in an extracted install too.
std::vector<ManifestWriter::FileEntry> copyLooseMovies(const std::string& mpqDirIn, const std::string& localeIn,
                                                       const std::string& outputDir) {
    std::vector<ManifestWriter::FileEntry> entries;
    std::vector<std::string> dataDirs = {mpqDirIn};
    const std::string inner = findCaseInsensitiveDirectory(mpqDirIn, "Data");
    if (!inner.empty()) dataDirs.push_back((fs::path(mpqDirIn) / inner).string());
    for (const auto& dataDir : dataDirs) {
        const std::string locale = localeIn.empty() ? findLocale(dataDir) : localeIn;
        if (locale.empty()) continue;
        const std::string localeDir = findCaseInsensitiveDirectory(dataDir, locale);
        if (localeDir.empty()) continue;
        fs::path at = fs::path(dataDir) / localeDir;
        const std::string iface = findCaseInsensitiveDirectory(at.string(), "Interface");
        if (iface.empty()) continue;
        at /= iface;
        const std::string cine = findCaseInsensitiveDirectory(at.string(), "Cinematics");
        if (cine.empty()) continue;
        at /= cine;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(at, ec)) {
            if (!e.is_regular_file() || toLowerStr(e.path().extension().string()) != ".avi") continue;
            const std::string wowPath = "Interface\\Cinematics\\" + e.path().filename().string();
            const std::string mapped = PathMapper::mapPath(wowPath);
            const fs::path out = fs::path(outputDir) / mapped;
            std::ifstream in(e.path(), std::ios::binary);
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (data.empty()) continue;
            fs::create_directories(out.parent_path(), ec);
            std::ofstream o(out, std::ios::binary | std::ios::trunc);
            o.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!o.good()) {
                std::cerr << "  FAILED write: " << out << "\n";
                continue;
            }
            ManifestWriter::FileEntry entry;
            entry.wowPath = normalizeWowPath(wowPath);
            entry.filesystemPath = mapped;
            entry.size = data.size();
            entry.crc32 = ManifestWriter::computeCRC32(data.data(), data.size());
            entries.push_back(std::move(entry));
            std::cout << "  Loose movie: " << e.path().string() << " -> " << mapped << "\n";
        }
        return entries;
    }
    return entries;
}

}  // namespace tools
}  // namespace wowee
