// ReportBug and ReportSuggestion (0x00510930, 0x00510980 -> 0x006b22a0):
// CMSG_BUG with the report the client writes about the machine and the game
// in front of the player's own text.

#include "game/game_handler.hpp"
#include "game/character.hpp"
#include "game/opcode_table.hpp"
#include "game/update_field_table.hpp"
#include "network/world_socket.hpp"
#include "pipeline/asset_manager.hpp"
#include "rendering/renderer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <intrin.h>
#else
#  include <unistd.h>
#  include <pwd.h>
#  if defined(__APPLE__)
#    include <sys/sysctl.h>
#  endif
#  if defined(__x86_64__) || defined(__i386__)
#    include <cpuid.h>
#  endif
#endif

namespace wowee {
namespace game {

namespace {

/// The user the client runs as (0x0086b4a0).
std::string userName() {
#if defined(_WIN32)
    char buf[256];
    DWORD n = sizeof(buf);
    if (GetUserNameA(buf, &n)) return buf;
    return {};
#else
    if (const char* u = std::getenv("USER"); u && *u) return u;
    if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_name) return pw->pw_name;
    return {};
#endif
}

/// The machine's name (0x0086b480).
std::string computerName() {
#if defined(_WIN32)
    char buf[256];
    DWORD n = sizeof(buf);
    if (GetComputerNameA(buf, &n)) return buf;
    return {};
#else
    char buf[256] = {};
    if (gethostname(buf, sizeof(buf) - 1) == 0) return buf;
    return {};
#endif
}

/// 0x0086b9a0's vendor, as the client's table names it (0x00ad93c0).
const char* processorVendor() {
#if defined(_WIN32) && (defined(_M_X64) || defined(_M_IX86))
    int regs[4] = {};
    __cpuid(regs, 0);
    char v[13] = {};
    std::memcpy(v, &regs[1], 4);
    std::memcpy(v + 4, &regs[3], 4);
    std::memcpy(v + 8, &regs[2], 4);
#elif defined(__x86_64__) || defined(__i386__)
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(0, &a, &b, &c, &d)) return "Unknown";
    char v[13] = {};
    std::memcpy(v, &b, 4);
    std::memcpy(v + 4, &d, 4);
    std::memcpy(v + 8, &c, 4);
#else
    const char* v = "";
#endif
    const std::string vendor(v);
    if (vendor == "GenuineIntel") return "intel";
    if (vendor == "AuthenticAMD") return "AMD";
    return "Unknown";
}

/// The processor's clock in hertz (0x0086ae70), 0 where it cannot be read.
double processorHz() {
#if defined(_WIN32)
    LARGE_INTEGER f;
    HKEY key;
    DWORD mhz = 0, size = sizeof(mhz);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0,
                      KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExA(key, "~MHz", nullptr, nullptr, reinterpret_cast<LPBYTE>(&mhz), &size);
        RegCloseKey(key);
    }
    (void)f;
    return static_cast<double>(mhz) * 1e6;
#elif defined(__APPLE__)
    uint64_t hz = 0;
    size_t len = sizeof(hz);
    if (sysctlbyname("hw.cpufrequency", &hz, &len, nullptr, 0) == 0) return static_cast<double>(hz);
    return 0.0;
#else
    std::ifstream in("/proc/cpuinfo");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("cpu MHz", 0) == 0) {
            const auto colon = line.find(':');
            if (colon != std::string::npos) return std::atof(line.c_str() + colon + 1) * 1e6;
        }
    }
    return 0.0;
#endif
}

/// Physical memory in megabytes (0x0086b4c0).
unsigned memoryMB() {
#if defined(_WIN32)
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) return static_cast<unsigned>(m.ullTotalPhys / (1024 * 1024));
    return 0;
#elif defined(__APPLE__)
    uint64_t bytes = 0;
    size_t len = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &len, nullptr, 0) == 0) return static_cast<unsigned>(bytes / (1024 * 1024));
    return 0;
#else
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long size = sysconf(_SC_PAGE_SIZE);
    if (pages <= 0 || size <= 0) return 0;
    return static_cast<unsigned>(static_cast<unsigned long long>(pages) * static_cast<unsigned long long>(size) /
                                 (1024ull * 1024ull));
#endif
}

/// The operating system, by the client's table (0x00b1d2b0).
const char* osName() {
#if defined(_WIN32)
    return "WinVista";
#elif defined(__APPLE__)
    return "MacOSX";
#elif defined(__linux__)
    return "Linux";
#else
    return "Unknown";
#endif
}

std::string line(const char* fmt, const std::string& value) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), fmt, value.c_str());
    return buf;
}

}  // namespace

std::string GameHandler::bugReportText() const {
    std::string out;
    if (const std::string u = userName(); !u.empty()) out += line("Username:\t%s\n", u);
    if (const std::string c = computerName(); !c.empty()) out += line("Computer:\t%s\n", c);
    char buf[256];
    std::snprintf(buf, sizeof(buf), "Processors:\t%u\n", std::max(1u, std::thread::hardware_concurrency()));
    out += buf;
    out += line("Processor vendor:\t%s\n", processorVendor());
    // Below 990 MHz in megahertz, else in gigahertz (0x00ad93b8).
    const double hz = processorHz();
    if (hz < 990000000.0) std::snprintf(buf, sizeof(buf), "Processor speed:\t%6.2f%cHz\n", hz / 1000.0 * 0.001, 'M');
    else std::snprintf(buf, sizeof(buf), "Processor speed:\t%6.2f%cHz\n", hz / 1000000.0 * 0.001, 'G');
    out += buf;
    std::snprintf(buf, sizeof(buf), "Memory:\t%uMB\n", memoryMB());
    out += buf;
    out += "OS:\t\t";
    out += osName();
    out += "\n";
    out += "Version:\tWoW [Release] Build 12340 (Jun 24 2010 23:54:57)\n";
    // The game's half, once logged in to a realm.
    if (accountName.empty() || realmName_.empty()) return out;
    out += line("Realm:\t%s\n", realmName_);
    out += line("Account:\t%s\n", accountName);
    const auto me = getEntityManager().getEntity(playerGuid);
    if (state != WorldState::IN_WORLD || !me) return out;

    auto* am = services_.assetManager;
    const auto races = am && am->isInitialized() ? am->loadDBCOptional("ChrRaces.dbc") : nullptr;
    const auto classes = am && am->isInitialized() ? am->loadDBCOptional("ChrClasses.dbc") : nullptr;
    const uint16_t bytes0 = fieldIndex(UF::UNIT_FIELD_BYTES_0);
    const uint32_t b0 = bytes0 != 0xFFFF ? me->getField(bytes0) : 0u;
    const uint32_t race = b0 & 0xFFu, cls = (b0 >> 8) & 0xFFu, gender = (b0 >> 16) & 0xFFu;
    std::string raceName = "Unknown", className = "Unknown";
    if (races) {
        if (const int32_t r = races->findRecordById(race); r >= 0 && races->getFieldCount() > 14)
            raceName = races->getString(static_cast<uint32_t>(r), 14);
    }
    if (classes) {
        if (const int32_t r = classes->findRecordById(cls); r >= 0 && classes->getFieldCount() > 4)
            className = classes->getString(static_cast<uint32_t>(r), 4);
    }
    static const char* kGender[3] = {"Male", "Female", "NOSEX"};
    std::snprintf(buf, sizeof(buf), "Character:\t%s (level %i %s %s %s)\n", lookupName(playerGuid).c_str(),
                  static_cast<int>(getPlayerLevel()), raceName.c_str(), kGender[std::min<uint32_t>(gender, 2)],
                  className.c_str());
    out += buf;
    const std::string mapName = getMapName(currentMapId_);
    if (mapName.empty()) std::snprintf(buf, sizeof(buf), "Map:\t\t%u\n", currentMapId_);
    else std::snprintf(buf, sizeof(buf), "Map:\t\t%u (%s)\n", currentMapId_, mapName.c_str());
    out += buf;
    if (const auto* r = services_.renderer) {
        if (const auto* t = r->getZoneTexts(); t && !t->zone.empty()) {
            out += line("Zone:\t\t%s", t->zone);
            if (!t->subzone.empty()) out += line(" - %s", t->subzone);
            out += "\n";
        }
    }
    std::snprintf(buf, sizeof(buf), "Position:\t%f %f %f, %f\n", movementInfo.x, movementInfo.y, movementInfo.z,
                  movementInfo.orientation);
    out += buf;
    if (getTargetGuid() != 0 && getEntityManager().getEntity(getTargetGuid()))
        out += line("Target:\t%s\n", lookupName(getTargetGuid()));
    out += "Auras:\n";
    for (const auto& aura : getPlayerAuras()) {
        if (aura.spellId == 0) continue;
        std::snprintf(buf, sizeof(buf), "\t%s(%d)\n", getSpellName(aura.spellId).c_str(),
                      static_cast<int>(aura.spellId));
        out += buf;
    }
    return out;
}

void GameHandler::sendBugReport(uint32_t type, const std::string& text) {
    // 0x006b22a0: nothing for an empty text; type 0 a bug, 1 a suggestion,
    // then the report and the text, each with its length and its NUL.
    if (text.empty() || !socket || wireOpcode(Opcode::CMSG_BUG) == 0xFFFF) return;
    const std::string report = bugReportText();
    network::Packet packet(wireOpcode(Opcode::CMSG_BUG));
    packet.writeUInt32(type);
    packet.writeUInt32(static_cast<uint32_t>(report.size() + 1));
    packet.writeString(report);
    packet.writeUInt32(static_cast<uint32_t>(text.size() + 1));
    packet.writeString(text);
    socket->send(packet);
}

}  // namespace game
}  // namespace wowee
