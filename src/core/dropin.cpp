#include "core/dropin.hpp"
#include "core/env.hpp"
#include "pipeline/mpq_archive_set.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace wowee {
namespace core {

namespace {

constexpr char kMagic[8] = {'W', 'O', 'W', 'E', 'E', 'P', 'K', '1'};

fs::path executablePath() {
#if defined(_WIN32)
    std::wstring buf(MAX_PATH, L'\0');
    DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    while (len == buf.size()) {
        buf.resize(buf.size() * 2);
        len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    }
    return len ? fs::path(buf.substr(0, len)) : fs::path();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    return fs::path(buf.c_str());
#else
    char buf[4096];
    const ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf));
    return len > 0 ? fs::path(std::string(buf, static_cast<size_t>(len))) : fs::path();
#endif
}

template <typename T>
bool readAt(std::ifstream& in, uint64_t at, T& v) {
    in.seekg(static_cast<std::streamoff>(at));
    in.read(reinterpret_cast<char*>(&v), sizeof(T));
    return in.good();
}

} // namespace

std::string unpackPayload(const std::string& exePath, const std::string& root) {
    std::ifstream in(fs::path(exePath), std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const uint64_t exeSize = static_cast<uint64_t>(in.tellg());
    if (exeSize < 24) return {};
    char magic[8];
    uint64_t indexAt = 0, indexSize = 0;
    in.seekg(static_cast<std::streamoff>(exeSize - 24));
    in.read(magic, 8);
    if (!in.good() || std::memcmp(magic, kMagic, 8) != 0) return {};
    if (!readAt(in, exeSize - 16, indexAt) || !readAt(in, exeSize - 8, indexSize)) return {};
    if (indexAt + indexSize > exeSize - 24) return "payload index out of range - not unpacked";

    std::ostringstream stampText;
    stampText << "payload " << exeSize << " " << indexAt << " " << indexSize << "\n";
    const std::string stamp = stampText.str();
    const fs::path stampPath = fs::path(root) / ".payload";
    {
        std::ifstream s(stampPath, std::ios::binary);
        const std::string have((std::istreambuf_iterator<char>(s)), std::istreambuf_iterator<char>());
        if (have == stamp) return "own files up to date in " + root;
    }

    std::vector<char> index(indexSize);
    in.seekg(static_cast<std::streamoff>(indexAt));
    in.read(index.data(), static_cast<std::streamsize>(indexSize));
    if (!in.good() || indexSize < 4) return "payload index unreadable - not unpacked";
    size_t at = 0;
    auto take = [&](void* dst, size_t n) {
        if (at + n > index.size()) return false;
        std::memcpy(dst, index.data() + at, n);
        at += n;
        return true;
    };
    uint32_t count = 0;
    take(&count, 4);
    uint64_t total = 0;
    std::vector<char> buf;
    std::error_code ec;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t len = 0;
        uint64_t off = 0, size = 0;
        if (!take(&len, 2) || at + len > index.size()) return "payload index truncated";
        std::string rel(index.data() + at, len);
        at += len;
        if (!take(&off, 8) || !take(&size, 8)) return "payload index truncated";
        if (rel.find("..") != std::string::npos || off + size > indexAt) return "payload entry refused: " + rel;
        const fs::path out = fs::path(root) / fs::path(rel);
        fs::create_directories(out.parent_path(), ec);
        buf.resize(size);
        in.seekg(static_cast<std::streamoff>(off));
        in.read(buf.data(), static_cast<std::streamsize>(size));
        std::ofstream o(out, std::ios::binary | std::ios::trunc);
        o.write(buf.data(), static_cast<std::streamsize>(size));
        if (!o.good()) return "cannot write " + out.string();
        total += size;
    }
    std::ofstream(stampPath, std::ios::binary | std::ios::trunc) << stamp;
    return "unpacked " + std::to_string(count) + " own files (" + std::to_string(total / 1024) + " KB) into " +
           root;
}

std::string enterDropInRoot() {
    const char* env = std::getenv("WOWEE_DROPIN");
    const std::string want = env ? env : "";
    if (want == "0") return {};
    const fs::path exe = executablePath();
    if (exe.empty()) return {};
    const fs::path exeDir = exe.parent_path();
    std::error_code ec;
    std::string archiveDir;
    const bool clientFolder = pipeline::MpqArchiveSet::findArchiveDir(exeDir.string(), &archiveDir);
    const bool devBuild = fs::is_directory(exeDir / "assets" / "shaders", ec);
    if (want != "1" && (!clientFolder || devBuild)) return {};

    const fs::path root = exeDir / "WoWee";
    fs::create_directories(root, ec);
    if (ec) return "drop-in: cannot create " + root.string() + " (" + ec.message() + ") - not taken";
    const std::string unpacked = unpackPayload(exe.string(), root.string());
    fs::current_path(root, ec);
    if (ec) return "drop-in: cannot enter " + root.string() + " - not taken";
    if (!archiveDir.empty()) setEnvVar("WOW_DATA_PATH", archiveDir.c_str(), /*overwrite=*/false);
    const std::string config = (root / "config").string();
    setEnvVar("WOWEE_CONFIG_ROOT", config.c_str(), /*overwrite=*/false);
#ifdef _WIN32
    // A few files are kept under %APPDATA%\wowee by name (the last world, the
    // guild name cache, the pipeline cache, the Warden cache); in this process
    // that is WoWee\appdata, so they stay with the rest.
    const std::string appData = (root / "appdata").string();
    setEnvVar("APPDATA", appData.c_str(), /*overwrite=*/true);
#endif
    std::string note = "drop-in: running in " + exeDir.string() + ", writing to " + root.string();
    if (!unpacked.empty()) note += "; " + unpacked;
    return note;
}

} // namespace core
} // namespace wowee
