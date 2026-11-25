#pragma once

/// The files of addons: whatever stands under Interface\\AddOns is read from the
/// disk.

#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace wowee::pipeline {

/// "interface\\addons\\" in any case, with either slash, and what stands behind
/// it; "" for a path that is elsewhere, or that leaves the folder.
inline std::string addonRelativePath(const std::string& path) {
    static const std::string kPrefix = "interface/addons/";
    if (path.size() <= kPrefix.size()) return {};
    std::string rest;
    for (size_t i = 0; i < path.size(); ++i) {
        const char c = path[i] == '\\' ? '/' : path[i];
        if (i < kPrefix.size()) {
            if (static_cast<char>(std::tolower(static_cast<unsigned char>(c))) != kPrefix[i]) return {};
        } else {
            rest += c;
        }
    }
    if (rest.find("..") != std::string::npos || rest.find(':') != std::string::npos) return {};
    return rest;
}

/// The file of one of the folders addons are in, whatever case its name is in on
/// the disk; "" for none.
inline std::string addonFile(const std::vector<std::string>& roots, const std::string& path) {
    namespace fs = std::filesystem;
    const std::string rest = addonRelativePath(path);
    if (rest.empty()) return {};
    const auto lowered = [](std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    for (const std::string& root : roots) {
        std::error_code ec;
        if (const fs::path straight = fs::path(root) / rest; fs::is_regular_file(straight, ec)) return straight.string();
#ifndef _WIN32
        fs::path at = root;
        bool found = true;
        for (size_t from = 0; found && from <= rest.size();) {
            const size_t slash = rest.find('/', from);
            const std::string want = lowered(rest.substr(from, slash == std::string::npos ? std::string::npos : slash - from));
            found = false;
            for (const auto& entry : fs::directory_iterator(at, ec)) {
                if (lowered(entry.path().filename().string()) != want) continue;
                at = entry.path();
                found = true;
                break;
            }
            from = slash == std::string::npos ? rest.size() + 1 : slash + 1;
        }
        if (found && fs::is_regular_file(at, ec)) return at.string();
#else
        (void)lowered;
#endif
    }
    return {};
}

}  // namespace wowee::pipeline
