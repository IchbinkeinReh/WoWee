#pragma once

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

// A file that says a session is running with the ray tracing on, and the
// last memory numbers it saw.
//
// A phone's out-of-memory killer gives the app no last word: the process is
// gone, and the player is left with nothing to say why. So while the tracing
// runs this file is kept up to date, and a clean exit takes it away. Found at
// the next start, it means the last session ended while tracing - most
// likely for want of memory - and the login screen says so with the numbers,
// and offers to leave the tracing off.
namespace wowee::core::rt_session {

struct Numbers {
    uint64_t availableMB = 0;  ///< the system's available RAM
    uint64_t tracingMB = 0;    ///< what the ray tracing held, GPU and CPU
    uint64_t privateMB = 0;    ///< the process's private bytes
};

inline void write(const std::string& file, const Numbers& n) {
    std::ofstream out(file, std::ios::trunc);
    if (!out) return;
    out << "available_mb=" << n.availableMB << "\ntracing_mb=" << n.tracingMB << "\nprivate_mb=" << n.privateMB
        << "\n";
}

inline void clear(const std::string& file) { std::remove(file.c_str()); }

inline std::optional<Numbers> read(const std::string& file) {
    std::ifstream in(file);
    if (!in) return std::nullopt;
    Numbers n;
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        uint64_t v = 0;
        std::istringstream(line.substr(eq + 1)) >> v;
        if (key == "available_mb") n.availableMB = v;
        else if (key == "tracing_mb") n.tracingMB = v;
        else if (key == "private_mb") n.privateMB = v;
    }
    return n;
}

}  // namespace wowee::core::rt_session
