#pragma once

// The player's per-user wowee folders, and a run's own stand-ins for them.
//
// See tests/catch_main.cpp and cmake/WoweeTestSandbox.cmake. Header-only: the
// Catch2 main compiles it, and test_user_paths_sandboxed.cpp checks with the
// same rules what the client's own path functions answer under a test.

#include "core/env.hpp"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

namespace wowee::test {

namespace fs = std::filesystem;

inline std::string envOr(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

/// The folders the client keeps per user - its config root, its data and log
/// folders - as the environment names them for the player. Under ctest the
/// environment is already the sandbox's, so the configure step hands the real
/// ones over in WOWEE_TEST_REAL_USER_ROOTS (separated by |).
inline std::vector<fs::path> realUserRoots() {
    std::vector<fs::path> roots;
    const std::string given = envOr("WOWEE_TEST_REAL_USER_ROOTS");
    if (!given.empty()) {
        size_t start = 0;
        while (start <= given.size()) {
            const size_t bar = given.find('|', start);
            const std::string one = given.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
            if (!one.empty()) roots.emplace_back(one);
            if (bar == std::string::npos) break;
            start = bar + 1;
        }
        return roots;
    }
#ifdef _WIN32
    if (const std::string a = envOr("APPDATA"); !a.empty()) roots.push_back(fs::path(a) / "wowee");
    if (const std::string l = envOr("LOCALAPPDATA"); !l.empty()) roots.push_back(fs::path(l) / "Wowee");
#else
    if (const std::string h = envOr("HOME"); !h.empty()) {
        roots.push_back(fs::path(h) / ".wowee");
        roots.push_back(fs::path(h) / ".local" / "share" / "wowee");
        roots.push_back(fs::path(h) / ".local" / "state" / "wowee");
        roots.push_back(fs::path(h) / "Library" / "Application Support" / "wowee");
        roots.push_back(fs::path(h) / "Library" / "Application Support" / "Wowee");
        roots.push_back(fs::path(h) / "Library" / "Logs" / "wowee");
    }
#endif
    return roots;
}

/// Whether `p` is `root` or lies inside it (lexically, case-insensitive on
/// Windows, where the file system is).
inline bool pathIsUnder(const fs::path& p, const fs::path& root) {
    auto norm = [](const fs::path& x) {
        std::error_code ec;
        fs::path a = fs::absolute(x, ec);
        if (ec) a = x;
        std::string s = a.lexically_normal().generic_string();
        while (s.size() > 1 && s.back() == '/') s.pop_back();
#ifdef _WIN32
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
#endif
        return s;
    };
    const std::string ps = norm(p);
    const std::string rs = norm(root);
    if (rs.empty()) return false;
    return ps == rs || (ps.size() > rs.size() && ps.compare(0, rs.size(), rs) == 0 && ps[rs.size()] == '/');
}

/// Every file under the roots, with its size and a hash of its bytes.
inline std::map<std::string, std::pair<std::uintmax_t, std::uint64_t>>
snapshotUserRoots(const std::vector<fs::path>& roots) {
    std::map<std::string, std::pair<std::uintmax_t, std::uint64_t>> out;
    for (const fs::path& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::uint64_t h = 1469598103934665603ull;  // FNV-1a
            std::ifstream in(it->path(), std::ios::binary);
            char buf[8192];
            while (in.read(buf, sizeof buf) || in.gcount() > 0) {
                for (std::streamsize i = 0; i < in.gcount(); ++i) {
                    h ^= static_cast<unsigned char>(buf[i]);
                    h *= 1099511628211ull;
                }
            }
            out[it->path().string()] = {it->file_size(ec), h};
        }
    }
    return out;
}

/// For the length of a run started outside ctest: every per-user variable
/// points into a temporary folder, removed afterwards, and the player's real
/// folders are recorded so a change to them can be named.
class UserPathSandbox {
public:
    UserPathSandbox() {
        realRoots_ = realUserRoots();
        if (!envOr("WOWEE_TEST_SANDBOX").empty()) return;  // ctest gave one already

        std::error_code ec;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = fs::temp_directory_path(ec) / ("wowee-test-" + std::to_string(stamp));
        if (ec) dir_ = fs::current_path(ec) / ("wowee-test-" + std::to_string(stamp));
        fs::create_directories(dir_, ec);

        std::string roots;
        for (const fs::path& r : realRoots_) roots += (roots.empty() ? "" : "|") + r.string();
        core::setEnvVar("WOWEE_TEST_REAL_USER_ROOTS", roots.c_str());

        const struct { const char* name; fs::path path; } vars[] = {
            {"APPDATA", dir_ / "AppData" / "Roaming"},
            {"LOCALAPPDATA", dir_ / "AppData" / "Local"},
            {"HOME", dir_ / "home"},
            {"XDG_CONFIG_HOME", dir_ / "home" / ".config"},
            {"XDG_DATA_HOME", dir_ / "home" / ".local" / "share"},
            {"XDG_STATE_HOME", dir_ / "home" / ".local" / "state"},
            {"XDG_CACHE_HOME", dir_ / "home" / ".cache"},
            {"WOWEE_CONFIG_ROOT", dir_ / "config"},
            {"WOWEE_TEST_SANDBOX", dir_},
        };
        for (const auto& v : vars) {
            fs::create_directories(v.path, ec);
            core::setEnvVar(v.name, v.path.string().c_str());
        }
        managed_ = true;
        before_ = snapshotUserRoots(realRoots_);
    }

    ~UserPathSandbox() {
        if (!managed_ || !envOr("WOWEE_TEST_KEEP_SANDBOX").empty()) return;
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    UserPathSandbox(const UserPathSandbox&) = delete;
    UserPathSandbox& operator=(const UserPathSandbox&) = delete;

    /// Files of the player's folders that differ from the start of the run.
    std::vector<std::string> touchedRealFiles() const {
        std::vector<std::string> out;
        if (!managed_) return out;
        const auto after = snapshotUserRoots(realRoots_);
        for (const auto& [path, info] : after) {
            auto it = before_.find(path);
            if (it == before_.end() || it->second != info) out.push_back(path);
        }
        for (const auto& [path, info] : before_) {
            if (after.find(path) == after.end()) out.push_back(path + " (gone)");
        }
        return out;
    }

private:
    std::vector<fs::path> realRoots_;
    fs::path dir_;
    bool managed_ = false;
    std::map<std::string, std::pair<std::uintmax_t, std::uint64_t>> before_;
};

}  // namespace wowee::test
