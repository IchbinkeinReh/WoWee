// Warden check answers, graded the way AzerothCore grades them.
//
// A wrong answer to a cheat check is not an error anyone sees: the server
// applies its penalty - a kick, or a ban on the account - and closes the
// connection without a reason. These put AzerothCore's own check list to the
// responder, laid out as its WardenWin::RequestChecks sends them, and grade
// the reply by its WardenWin::HandleData, so a wrong answer fails here instead
// of on someone's account.
#include <catch_amalgamated.hpp>
#include "game/warden_check_responder.hpp"
#include "game/warden_cr_file.hpp"
#include "game/warden_memory.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <openssl/hmac.h>
#include <openssl/sha.h>

using namespace wowee::game;

namespace {

struct AcCheck {
    int id;
    int type;
    const char* data;    // hex
    const char* str;
    uint32_t address;
    int length;
    const char* result;  // hex
};

const AcCheck kAcChecks[] = {
#include "warden_azerothcore_checks.inc"
};

// AzerothCore's WardenCheckType
constexpr uint8_t MEM_CHECK = 0xF3, PAGE_CHECK_A = 0xB2, PAGE_CHECK_B = 0xBF, MPQ_CHECK = 0x98,
                  LUA_EVAL_CHECK = 0x8B, DRIVER_CHECK = 0x71, TIMING_CHECK = 0x57, MODULE_CHECK = 0xD9;

constexpr char kLuaEvalPrefix[] = "local S,T,R=SendAddonMessage,function()";
constexpr char kLuaEvalMidfix[] = " end R=S and T()if R then S('_TW',";
constexpr char kLuaEvalPostfix[] = ",'GUILD')end";

std::vector<uint8_t> fromHex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

void le32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

// WardenWin::RequestChecks: the string table, TIMING first, then each check.
std::vector<uint8_t> buildRequest(const std::vector<const AcCheck*>& checks, uint8_t xorByte) {
    std::vector<uint8_t> buff{0x02};
    for (const AcCheck* c : checks) {
        if (c->type == LUA_EVAL_CHECK) {
            const std::string s = std::string(kLuaEvalPrefix) + c->str + kLuaEvalMidfix + "0001" + kLuaEvalPostfix;
            buff.push_back(static_cast<uint8_t>(s.size()));
            buff.insert(buff.end(), s.begin(), s.end());
        } else if (c->type == MPQ_CHECK || c->type == DRIVER_CHECK) {
            const std::string s = c->str;
            buff.push_back(static_cast<uint8_t>(s.size()));
            buff.insert(buff.end(), s.begin(), s.end());
        }
    }
    buff.push_back(0x00);
    buff.push_back(TIMING_CHECK ^ xorByte);

    uint8_t index = 1;
    for (const AcCheck* c : checks) {
        buff.push_back(static_cast<uint8_t>(c->type ^ xorByte));
        switch (c->type) {
            case MEM_CHECK:
                buff.push_back(0x00);
                le32(buff, c->address);
                buff.push_back(static_cast<uint8_t>(c->length));
                break;
            case PAGE_CHECK_A:
            case PAGE_CHECK_B: {
                const auto data = fromHex(c->data);
                buff.insert(buff.end(), data.begin(), data.end());
                le32(buff, c->address);
                buff.push_back(static_cast<uint8_t>(c->length));
                break;
            }
            case MPQ_CHECK:
            case LUA_EVAL_CHECK:
                buff.push_back(index++);
                break;
            case DRIVER_CHECK: {
                const auto data = fromHex(c->data);
                buff.insert(buff.end(), data.begin(), data.end());
                buff.push_back(index++);
                break;
            }
            case MODULE_CHECK: {
                const uint8_t seed[4] = {0x13, 0x37, 0xC0, 0xDE};
                uint8_t digest[SHA_DIGEST_LENGTH];
                unsigned int len = 0;
                HMAC(EVP_sha1(), seed, 4, reinterpret_cast<const uint8_t*>(c->str), std::strlen(c->str), digest, &len);
                buff.insert(buff.end(), seed, seed + 4);
                buff.insert(buff.end(), digest, digest + 20);
                break;
            }
        }
    }
    buff.push_back(xorByte);
    return buff;
}

// WardenWin::HandleData. Returns what failed, one line each; empty is a pass.
std::vector<std::string> grade(const std::vector<uint8_t>& response, const std::vector<const AcCheck*>& checks) {
    std::vector<std::string> failures;
    if (response.size() < 7 || response[0] != 0x02) return {"not a CHEAT_CHECKS_RESULT"};
    const uint16_t length = static_cast<uint16_t>(response[1] | (response[2] << 8));
    const uint32_t checksum = uint32_t(response[3]) | (uint32_t(response[4]) << 8) |
                              (uint32_t(response[5]) << 16) | (uint32_t(response[6]) << 24);
    const uint8_t* buf = response.data() + 7;
    const size_t size = response.size() - 7;
    if (length != size) return {"Failed size checks in HandleData"};

    uint8_t digest[SHA_DIGEST_LENGTH];
    SHA1(buf, size, digest);
    uint32_t expected = 0;
    for (int i = 0; i < 5; ++i) {
        uint32_t w;
        std::memcpy(&w, digest + i * 4, 4);
        expected ^= w;
    }
    if (checksum != expected) return {"Failed checksum in HandleData"};

    size_t pos = 0;
    auto need = [&](size_t n) { return pos + n <= size; };
    if (!need(5) || buf[pos] == 0x00) return {"TIMING result 0x00 - the server stops reading here"};
    pos += 5;

    for (const AcCheck* c : checks) {
        const std::string name = "check " + std::to_string(c->id);
        switch (c->type) {
            case MEM_CHECK: {
                if (!need(1)) return failures.push_back(name + ": reply ends early"), failures;
                if (buf[pos++] != 0x00) { failures.push_back(name + ": MEM result not 0x00"); break; }
                const auto want = fromHex(c->result);
                if (!need(want.size())) return failures.push_back(name + ": reply ends early"), failures;
                if (std::memcmp(buf + pos, want.data(), want.size()) != 0) {
                    std::string got;
                    for (size_t i = 0; i < want.size(); ++i) { char h[4]; std::snprintf(h, 4, "%02X", buf[pos + i]); got += h; }
                    failures.push_back(name + ": MEM got " + got + ", expected " + c->result);
                }
                pos += want.size();
                break;
            }
            case PAGE_CHECK_A:
            case PAGE_CHECK_B:
            case DRIVER_CHECK:
            case MODULE_CHECK:
                if (!need(1)) return failures.push_back(name + ": reply ends early"), failures;
                if (buf[pos] != 0xE9) {
                    char h[4];
                    std::snprintf(h, 4, "%02X", buf[pos]);
                    failures.push_back(name + ": scan answered 0x" + h + ", only 0xE9 passes");
                }
                pos++;
                break;
            case LUA_EVAL_CHECK: {
                if (!need(1)) return failures.push_back(name + ": reply ends early"), failures;
                const uint8_t result = buf[pos++];
                if (result == 0) {
                    if (!need(1)) return failures.push_back(name + ": reply ends early"), failures;
                    pos += 1 + buf[pos];
                }
                break;
            }
            case MPQ_CHECK:
                if (!need(1)) return failures.push_back(name + ": reply ends early"), failures;
                if (buf[pos++] != 0x00) { failures.push_back(name + ": MPQ result not 0x00"); break; }
                pos += 20;
                break;
        }
    }
    if (pos != size) failures.push_back("reply carries " + std::to_string(size - pos) + " bytes no check read");
    return failures;
}

// Each request kept under the 500 bytes AzerothCore holds one to.
std::vector<std::vector<const AcCheck*>> batches(const std::vector<const AcCheck*>& checks) {
    std::vector<std::vector<const AcCheck*>> out(1);
    for (const AcCheck* c : checks) {
        out.back().push_back(c);
        if (buildRequest(out.back(), 0).size() > 500) {
            out.back().pop_back();
            out.push_back({c});
        }
    }
    return out;
}

std::vector<std::string> run(const std::vector<const AcCheck*>& checks, WardenMemory* memory, uint8_t xorByte) {
    uint8_t opcodes[9];
    std::vector<WardenCRRecord> records;
    REQUIRE(knownWardenCR("79c0768d657977d697e10bad956cced1", opcodes, records));
    WardenCheckContext ctx;
    ctx.opcodes = opcodes;
    ctx.memory = memory;

    std::vector<std::string> failures;
    for (const auto& batch : batches(checks)) {
        const auto response = buildWardenCheckResponse(buildRequest(batch, xorByte), ctx, "test");
        for (auto& f : grade(response, batch)) failures.push_back(std::move(f));
    }
    return failures;
}

std::vector<const AcCheck*> checksWhere(bool (*keep)(const AcCheck&)) {
    std::vector<const AcCheck*> out;
    for (const auto& c : kAcChecks) {
        if (keep(c)) out.push_back(&c);
    }
    return out;
}

}  // namespace

TEST_CASE("every AzerothCore scan, module, driver and Lua check is answered clean", "[wardengrader]") {
    // The answers here do not depend on the executable: a clean client finds
    // none of these cheats. Without one the code scans are never searched,
    // which is the answer a genuine executable gives them too.
    const auto checks = checksWhere([](const AcCheck& c) { return c.type != MEM_CHECK; });
    REQUIRE(checks.size() > 60);
    for (uint8_t xorByte : {uint8_t{0x00}, uint8_t{0x5C}, uint8_t{0xF3}}) {
        INFO("xorByte " << int(xorByte));
        const auto failures = run(checks, nullptr, xorByte);
        for (const auto& f : failures) UNSCOPED_INFO(f);
        CHECK(failures.empty());
    }
}

TEST_CASE("every AzerothCore memory check reads what a genuine 3.3.5a client holds", "[wardengrader]") {
    // Needs the build 12340 WoW.exe, which is not distributed with the client:
    // name it with WOWEE_TEST_WOW_EXE.
    const char* exe = std::getenv("WOWEE_TEST_WOW_EXE");
    if (!exe || !std::filesystem::exists(exe)) SKIP("WOWEE_TEST_WOW_EXE does not name a WoW.exe");
    {
        std::ifstream f(exe, std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (WardenMemory::fileBuildOf(bytes.data(), bytes.size()) != 12340) SKIP("WOWEE_TEST_WOW_EXE is not build 12340");
    }
    WardenMemory memory;
    REQUIRE(memory.loadFromFile(exe));

    const auto checks = checksWhere([](const AcCheck& c) { return c.type == MEM_CHECK; });
    REQUIRE(checks.size() == 34);
    const auto failures = run(checks, &memory, 0x5C);
    for (const auto& f : failures) UNSCOPED_INFO(f);
    CHECK(failures.empty());
}

TEST_CASE("AzerothCore's code scans find nothing in a genuine 3.3.5a client", "[wardengrader]") {
    // The same answer as without an executable, but by searching one: a scan
    // that matched genuine code by mistake would answer 0x4A and fail.
    const char* exe = std::getenv("WOWEE_TEST_WOW_EXE");
    if (!exe || !std::filesystem::exists(exe)) SKIP("WOWEE_TEST_WOW_EXE does not name a WoW.exe");
    {
        std::ifstream f(exe, std::ios::binary);
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (WardenMemory::fileBuildOf(bytes.data(), bytes.size()) != 12340) SKIP("WOWEE_TEST_WOW_EXE is not build 12340");
    }
    WardenMemory memory;
    REQUIRE(memory.loadFromFile(exe));

    const auto checks = checksWhere([](const AcCheck& c) { return c.type == PAGE_CHECK_A || c.type == PAGE_CHECK_B; });
    REQUIRE(checks.size() >= 20);
    const auto failures = run(checks, &memory, 0x5C);
    for (const auto& f : failures) UNSCOPED_INFO(f);
    CHECK(failures.empty());
}

TEST_CASE("the grader fails the answers that got an account banned", "[wardengrader]") {
    // 0x00 for a scan that found nothing, which vmangos takes as clean and
    // AzerothCore penalises; and a reply whose checksum does not match.
    const AcCheck* module = nullptr;
    for (const auto& c : kAcChecks) {
        if (c.type == MODULE_CHECK) { module = &c; break; }
    }
    REQUIRE(module != nullptr);
    const std::vector<const AcCheck*> checks{module};

    auto respond = [](std::vector<uint8_t> results) {
        uint8_t digest[SHA_DIGEST_LENGTH];
        SHA1(results.data(), results.size(), digest);
        uint32_t checksum = 0;
        for (int i = 0; i < 5; ++i) { uint32_t w; std::memcpy(&w, digest + i * 4, 4); checksum ^= w; }
        std::vector<uint8_t> r{0x02, static_cast<uint8_t>(results.size()), 0};
        le32(r, checksum);
        r.insert(r.end(), results.begin(), results.end());
        return r;
    };
    const std::vector<uint8_t> timing{0x01, 1, 2, 3, 4};

    auto zero = timing;
    zero.push_back(0x00);
    CHECK_FALSE(grade(respond(zero), checks).empty());

    auto clean = timing;
    clean.push_back(0xE9);
    CHECK(grade(respond(clean), checks).empty());

    auto badSum = respond(clean);
    badSum[3] ^= 0xFF;
    CHECK_FALSE(grade(badSum, checks).empty());
}

TEST_CASE("CMaNGOS build 5875 memory checks match the supplied 1.12.1 executable", "[wardengrader][classic]") {
    const char* exe = std::getenv("WOWEE_TEST_CLASSIC_WOW_EXE");
    if (!exe || !std::filesystem::exists(exe)) SKIP("WOWEE_TEST_CLASSIC_WOW_EXE does not name a WoW.exe");
    std::ifstream f(exe, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    REQUIRE(WardenMemory::fileBuildOf(bytes.data(), bytes.size()) == 5875);
    WardenMemory memory;
    REQUIRE(memory.loadFromFile(exe));
    const AcCheck classicChecks[] = {
#include "warden_cmangos_5875_checks.inc"
    };
    std::vector<const AcCheck*> checks;
    for (const auto& c : classicChecks) checks.push_back(&c);
    REQUIRE(checks.size() > 20);
    for (uint8_t key : {uint8_t{0}, uint8_t{0x5C}, uint8_t{0xF3}}) {
        const auto failures = run(checks, &memory, key);
        for (const auto& failure : failures) UNSCOPED_INFO(failure);
        CHECK(failures.empty());
    }
}

TEST_CASE("vmangos build 5875 memory checks match the supplied 1.12.1 executable", "[wardengrader][classic]") {
    const char* exe = std::getenv("WOWEE_TEST_CLASSIC_WOW_EXE");
    if (!exe || !std::filesystem::exists(exe)) SKIP("WOWEE_TEST_CLASSIC_WOW_EXE does not name a WoW.exe");
    std::ifstream f(exe, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    REQUIRE(WardenMemory::fileBuildOf(bytes.data(), bytes.size()) == 5875);
    WardenMemory memory;
    REQUIRE(memory.loadFromFile(exe));
    const AcCheck classicChecks[] = {
#include "warden_vmangos_5875_checks.inc"
    };
    std::vector<const AcCheck*> checks;
    for (const auto& c : classicChecks) checks.push_back(&c);
    REQUIRE(checks.size() > 20);
    for (uint8_t key : {uint8_t{0}, uint8_t{0x5C}, uint8_t{0xF3}}) {
        const auto failures = run(checks, &memory, key);
        for (const auto& failure : failures) UNSCOPED_INFO(failure);
        CHECK(failures.empty());
    }
}

TEST_CASE("CMaNGOS build 8606 memory checks match the supplied 2.4.3 executable", "[wardengrader][tbc]") {
    const char* exe = std::getenv("WOWEE_TEST_TBC_WOW_EXE");
    if (!exe || !std::filesystem::exists(exe)) SKIP("WOWEE_TEST_TBC_WOW_EXE does not name a WoW.exe");
    std::ifstream f(exe, std::ios::binary);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    REQUIRE(WardenMemory::fileBuildOf(bytes.data(), bytes.size()) == 8606);
    WardenMemory memory;
    REQUIRE(memory.loadFromFile(exe));
    const AcCheck tbcChecks[] = {
#include "warden_cmangos_8606_checks.inc"
    };
    std::vector<const AcCheck*> checks;
    for (const auto& c : tbcChecks) checks.push_back(&c);
    REQUIRE(checks.size() > 20);
    for (uint8_t key : {uint8_t{0}, uint8_t{0x5C}, uint8_t{0xF3}}) {
        const auto failures = run(checks, &memory, key);
        for (const auto& failure : failures) UNSCOPED_INFO(failure);
        CHECK(failures.empty());
    }
}
