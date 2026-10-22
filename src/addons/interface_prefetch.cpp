#include "addons/interface_prefetch.hpp"

#include "core/logger.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

namespace wowee::addons::interface_prefetch {

namespace {

namespace fs = std::filesystem;

std::mutex g_mutex;
std::unordered_map<std::string, std::string> g_files;
std::thread g_thread;
std::atomic<bool> g_stop{false};

bool wanted(const fs::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".lua" || ext == ".xml" || ext == ".toc";
}

void run(std::vector<std::string> roots) {
    const auto started = std::chrono::steady_clock::now();
    size_t files = 0, bytes = 0;
    for (const std::string& root : roots) {
        std::error_code ec;
        if (root.empty() || !fs::is_directory(root, ec)) continue;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
             it != end && !g_stop.load(std::memory_order_relaxed); it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec) || !wanted(it->path())) continue;
            std::ifstream in(it->path(), std::ios::binary);
            if (!in) continue;
            std::stringstream ss;
            ss << in.rdbuf();
            std::string content = ss.str();
            bytes += content.size();
            ++files;
            std::lock_guard<std::mutex> lock(g_mutex);
            g_files.emplace(key(it->path().string()), std::move(content));
        }
    }
    LOG_WARNING("Interface prefetch: ", files, " files, ", bytes / 1024, " KB in ",
             std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(), " s");
}

}  // namespace

std::string key(const std::string& path) {
    std::string s = path;
    std::replace(s.begin(), s.end(), '\\', '/');
    s = fs::path(s).lexically_normal().generic_string();
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void start(std::vector<std::string> roots) {
    stop();
    g_stop = false;
    g_thread = std::thread(run, std::move(roots));
}

bool take(const std::string& path, std::string& out) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_files.find(key(path));
    if (it == g_files.end()) return false;
    out = it->second;
    return true;
}

void wait() {
    if (g_thread.joinable()) g_thread.join();
}

void stop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace wowee::addons::interface_prefetch
