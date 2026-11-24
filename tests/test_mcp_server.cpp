// The MCP server's working parts together: the socket it listens on, the
// threads between it and the game's loop, the steps of a call, the token's
// file, and what the tools work out on their own.

#include <catch_amalgamated.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "mcp/http_server.hpp"
#include "mcp/image.hpp"
#include "mcp/main_queue.hpp"
#include "mcp/ring_log.hpp"
#include "mcp/script.hpp"
#include "mcp/token_file.hpp"
#include "mcp/walk_planner.hpp"
#include "mcp/world_probe.hpp"

using namespace wowee::mcp;
using namespace std::chrono_literals;

namespace {

constexpr const char* kToken = "0123456789abcdef";

socket_t connectTo(uint16_t port, uint32_t address = INADDR_LOOPBACK) {
    const socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_port = htons(port);
    at.sin_addr.s_addr = htonl(address);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) != 0) {
        wowee::net::closeSocket(s);
        return INVALID_SOCK;
    }
    return s;
}

/// What comes back until the server closes, or has answered once.
std::string talk(uint16_t port, const std::string& bytes, bool untilClosed = false) {
    const socket_t s = connectTo(port);
    if (s == INVALID_SOCK) return "no connection";
    ::send(s, bytes.data(), static_cast<int>(bytes.size()), 0);
    std::string out;
    char buffer[4096];
    wowee::net::setNonBlocking(s);
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < until) {
        const auto n = ::recv(s, buffer, sizeof(buffer), 0);
        if (n == 0) break;
        if (n < 0) {
            if (!wowee::net::isWouldBlock(wowee::net::lastError())) break;
            std::this_thread::sleep_for(5ms);
            continue;
        }
        out.append(buffer, static_cast<size_t>(n));
        if (!untilClosed && out.find("\r\n\r\n") != std::string::npos) break;
    }
    wowee::net::closeSocket(s);
    return out;
}

std::string request(uint16_t port, const std::string& body, const std::string& token = kToken,
                    const std::string& method = "POST", const std::string& path = "/mcp",
                    const std::string& extra = "Content-Type: application/json\r\n", const std::string& host = "") {
    std::string out = method + " " + path + " HTTP/1.1\r\n";
    out += "Host: " + (host.empty() ? "127.0.0.1:" + std::to_string(port) : host) + "\r\n";
    if (!token.empty()) out += "Authorization: Bearer " + token + "\r\n";
    out += extra + "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    return out;
}

int status(const std::string& answer) {
    return answer.size() > 12 ? std::atoi(answer.c_str() + 9) : 0;
}

struct Running {
    HttpServer server;
    std::atomic<int> calls{0};
    explicit Running(HttpServerLimits limits = {}) : server(limits) {
        std::string error;
        const bool ok = server.start(
            0, kToken,
            [this](const std::string& body) -> std::optional<std::string> {
                ++calls;
                if (body == "quiet") return std::nullopt;
                return "{\"got\":" + std::to_string(body.size()) + "}";
            },
            error);
        INFO(error);
        REQUIRE(ok);
        REQUIRE(server.port() != 0);
    }
};

}  // namespace

TEST_CASE("the socket answers who may ask, and nobody else", "[mcp][server]") {
    Running r;
    const uint16_t port = r.server.port();
    CHECK(status(talk(port, request(port, "{}"))) == 200);
    CHECK(talk(port, request(port, "{}")).find("{\"got\":2}") != std::string::npos);
    CHECK(status(talk(port, request(port, "quiet"))) == 202);
    CHECK(status(talk(port, request(port, "{}", kToken, "POST", "/mcp", "Content-Type: application/json\r\n",
                                    "localhost:" + std::to_string(port)))) == 200);
    const int before = r.calls;
    CHECK(status(talk(port, request(port, "{}", ""))) == 401);
    CHECK(status(talk(port, request(port, "{}", "wrong"))) == 401);
    CHECK(talk(port, request(port, "{}", "")).find("WWW-Authenticate: Bearer") != std::string::npos);
    CHECK(status(talk(port, request(port, "{}", kToken, "POST", "/mcp", "Content-Type: application/json\r\n",
                                    "evil.example"))) == 403);
    CHECK(status(talk(port, request(port, "{}", kToken, "POST", "/mcp",
                                    "Content-Type: application/json\r\nOrigin: http://evil.example\r\n"))) == 403);
    CHECK(status(talk(port, request(port, "", kToken, "GET"))) == 405);
    CHECK(talk(port, request(port, "", kToken, "GET")).find("Allow: POST") != std::string::npos);
    CHECK(status(talk(port, request(port, "", kToken, "DELETE"))) == 405);
    CHECK(status(talk(port, request(port, "", kToken, "OPTIONS"))) == 405);
    CHECK(status(talk(port, request(port, "{}", kToken, "POST", "/"))) == 404);
    CHECK(status(talk(port, request(port, "{}", kToken, "POST", "/mcp/../etc/passwd"))) == 404);
    CHECK(status(talk(port, request(port, "{}", kToken, "POST", "/mcp", "Content-Type: text/plain\r\n"))) == 415);
    CHECK(status(talk(port, request(port, "{}", kToken, "POST", "/mcp", ""))) == 415);
    CHECK(status(talk(port, "garbage\r\n\r\n")) == 400);
    // Refused before the method, the path and the body are looked at.
    CHECK(status(talk(port, request(port, "{}", "", "GET", "/secret"))) == 401);
    CHECK(r.calls == before);
    CHECK(r.server.refused() >= 13);
}

TEST_CASE("a body too large is refused by its head", "[mcp][server]") {
    HttpServerLimits limits;
    limits.message.bodyBytes = 100;
    Running r(limits);
    const uint16_t port = r.server.port();
    CHECK(status(talk(port, request(port, std::string(100, 'a')))) == 200);
    CHECK(status(talk(port, request(port, std::string(101, 'a')), true)) == 413);
    // The whole of it sent: the answer is had all the same.
    CHECK(status(talk(port, request(port, std::string(200000, 'a')), true)) == 413);
    CHECK(r.calls == 1);
}

TEST_CASE("a connection is kept, and several requests answered on it", "[mcp][server]") {
    Running r;
    const uint16_t port = r.server.port();
    const std::string two =
        talk(port, request(port, "1") + request(port, "22", kToken, "POST", "/mcp",
                                                "Content-Type: application/json\r\nConnection: close\r\n"),
             true);
    CHECK(two.find("{\"got\":1}") != std::string::npos);
    CHECK(two.find("{\"got\":2}") != std::string::npos);
    // After a refusal nothing more is read.
    const std::string refused = talk(port, request(port, "1", "wrong") + request(port, "22"), true);
    CHECK(refused.find("{\"got\":2}") == std::string::npos);
}

TEST_CASE("slow requests and too many connections are let go of", "[mcp][server]") {
    HttpServerLimits limits;
    limits.connections = 2;
    limits.request = 300ms;
    limits.idle = 1500ms;
    Running r(limits);
    const uint16_t port = r.server.port();
    const auto begun = std::chrono::steady_clock::now();
    CHECK(status(talk(port, "POST /mcp HTTP/1.1\r\nHost:", true)) == 408);
    CHECK(std::chrono::steady_clock::now() - begun < 3s);
    // Two that say nothing are all there is room for.
    const socket_t a = connectTo(port);
    const socket_t b = connectTo(port);
    std::this_thread::sleep_for(400ms);
    CHECK(status(talk(port, request(port, "{}"), true)) == 503);
    wowee::net::closeSocket(a);
    wowee::net::closeSocket(b);
    std::this_thread::sleep_for(900ms);
    CHECK(status(talk(port, request(port, "{}"))) == 200);
}

TEST_CASE("only the loopback is listened on, and not after the end", "[mcp][server]") {
    uint16_t port = 0;
    {
        Running r;
        port = r.server.port();
        // The same port on any address of this machine but the loopback.
        char name[256] = {};
        if (::gethostname(name, sizeof(name)) == 0) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* list = nullptr;
            if (::getaddrinfo(name, nullptr, &hints, &list) == 0) {
                for (auto* a = list; a; a = a->ai_next) {
                    const auto address =
                        ntohl(reinterpret_cast<const sockaddr_in*>(a->ai_addr)->sin_addr.s_addr);
                    if ((address >> 24) == 127) continue;
                    CHECK(connectTo(port, address) == INVALID_SOCK);
                }
                ::freeaddrinfo(list);
            }
        }
        // Nor a second server on it.
        HttpServer second;
        std::string error;
        CHECK_FALSE(second.start(port, kToken, [](const std::string&) { return std::nullopt; }, error));
        CHECK_FALSE(error.empty());
    }
    CHECK(connectTo(port) == INVALID_SOCK);
    HttpServer none;
    std::string error;
    CHECK_FALSE(none.start(0, "", [](const std::string&) { return std::nullopt; }, error));
}

TEST_CASE("a call is run by the loop, step by step", "[mcp][queue]") {
    MainQueue queue;
    std::atomic<bool> stop{false};
    std::thread loop([&] {
        double now = 0.0;
        while (!stop) {
            queue.pump(now += 0.016, 4ms);
            std::this_thread::sleep_for(1ms);
        }
    });
    const auto answer = queue.submit(
        [] {
            auto s = std::make_shared<Script>();
            s->then(Script::frames(3));
            s->then(Script::seconds(0.1));
            s->then([](Script& script) { return script.done(script.elapsed()); });
            return s;
        },
        false, 5s);
    REQUIRE(answer.kind == MainQueue::Outcome::Kind::Finished);
    CHECK(answer.script->result.get<double>() >= 0.1);
    const auto thrown = queue.submit([]() -> std::shared_ptr<Script> { throw std::runtime_error("oh"); }, false, 5s);
    REQUIRE(thrown.kind == MainQueue::Outcome::Kind::Finished);
    CHECK(thrown.script->failed);
    stop = true;
    loop.join();
}

TEST_CASE("calls that press something wait their turn; calls that look do not", "[mcp][queue]") {
    MainQueue queue;
    std::atomic<bool> stop{false};
    std::atomic<int> pressing{0};
    std::atomic<int> most{0};
    std::thread loop([&] {
        double now = 0.0;
        while (!stop) {
            queue.pump(now += 0.016, 4ms);
            std::this_thread::sleep_for(1ms);
        }
    });
    const auto press = [&] {
        queue.submit(
            [&] {
                auto s = std::make_shared<Script>();
                s->then([&](Script&) {
                    most = std::max(most.load(), ++pressing);
                    return Script::Step::Next;
                });
                s->then(Script::frames(20));
                s->then([&](Script&) {
                    --pressing;
                    return Script::Step::Next;
                });
                return s;
            },
            true, 10s);
    };
    std::thread a(press);
    std::thread b(press);
    std::this_thread::sleep_for(15ms);
    const auto began = std::chrono::steady_clock::now();
    const auto look = queue.submit(
        [] {
            auto s = std::make_shared<Script>();
            s->then([](Script& script) { return script.done(1); });
            return s;
        },
        false, 5s);
    CHECK(look.kind == MainQueue::Outcome::Kind::Finished);
    CHECK(std::chrono::steady_clock::now() - began < 2s);
    a.join();
    b.join();
    CHECK(most == 1);
    stop = true;
    loop.join();
}

TEST_CASE("a call nobody waits for any more is ended, and lets go", "[mcp][queue]") {
    MainQueue queue;
    bool ended = false;
    const auto make = [&] {
        auto s = std::make_shared<Script>();
        s->then([](Script&) { return Script::Step::Again; });
        s->onEnd = [&] { ended = true; };
        return s;
    };
    std::thread caller([&] {
        CHECK(queue.submit(make, true, 1500ms).kind == MainQueue::Outcome::Kind::TimedOut);
    });
    for (int i = 0; i < 100 && !queue.handsBusy(); ++i) {
        queue.pump(i, 4ms);
        std::this_thread::sleep_for(5ms);
    }
    CHECK(queue.handsBusy());
    caller.join();
    queue.pump(101, 4ms);
    CHECK(ended);
    CHECK_FALSE(queue.handsBusy());
}

TEST_CASE("the end of the client is the end of every call", "[mcp][queue]") {
    MainQueue queue;
    std::thread caller([&] {
        CHECK(queue.submit([] { return std::make_shared<Script>(); }, false, 10s).kind ==
              MainQueue::Outcome::Kind::Closed);
    });
    std::this_thread::sleep_for(50ms);
    queue.close();
    caller.join();
    CHECK(queue.submit([] { return std::make_shared<Script>(); }, false, 10s).kind ==
          MainQueue::Outcome::Kind::Closed);
}

TEST_CASE("steps put first come first", "[mcp][script]") {
    Script s;
    std::string order;
    const auto say = [&](char c) {
        return [&order, c](Script&) {
            order += c;
            return Script::Step::Next;
        };
    };
    s.then([&](Script& script) {
        order += 'a';
        script.first({say('b'), say('c')});
        return Script::Step::Next;
    });
    s.then(say('d'));
    CHECK(s.run(0.0));
    CHECK(order == "abcd");
    Script failing;
    failing.then([](Script& script) { return script.fail("why"); });
    failing.then(say('x'));
    CHECK(failing.run(0.0));
    CHECK(failing.failed);
    CHECK(failing.result == "why");
    CHECK(order == "abcd");
}

TEST_CASE("a cursor is given what came after it", "[mcp][ring]") {
    RingLog<int> ring(3);
    CHECK(ring.last() == 0);
    for (int i = 1; i <= 5; ++i) ring.push(i);
    const auto all = [](int) { return true; };
    CHECK(ring.last() == 5);
    CHECK(ring.oldest() == 3);
    REQUIRE(ring.after(0, 10, all).size() == 3);
    CHECK(ring.after(0, 10, all).front().value == 3);
    CHECK(ring.after(4, 10, all).size() == 1);
    CHECK(ring.after(5, 10, all).empty());
    REQUIRE(ring.after(0, 2, all).size() == 2);
    CHECK(ring.after(0, 2, all).front().value == 4);
    CHECK(ring.after(0, 10, [](int v) { return v % 2 == 0; }).size() == 1);
}

TEST_CASE("the token is new each time, and its file this user's", "[mcp][token]") {
    const std::string a = makeToken();
    const std::string b = makeToken();
    CHECK(a.size() == 64);
    CHECK(a != b);
    const auto dir = std::filesystem::temp_directory_path() / ("wowee_mcp_test_" + a.substr(0, 12));
    const auto path = tokenFilePath((dir / "config").string());
    REQUIRE(writeTokenFile(path, {.port = 1234, .token = a, .pid = 7}));
    const auto text = [&] {
        std::ifstream in(path);
        std::ostringstream all;
        all << in.rdbuf();
        return all.str();
    };
    CHECK(text().find(a) != std::string::npos);
    CHECK(text().find("1234") != std::string::npos);
#ifndef _WIN32
    const auto mode = std::filesystem::status(path).permissions();
    CHECK((mode & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
          std::filesystem::perms::none);
#endif
    // A second client's takes its place, and is not taken away by the first.
    REQUIRE(writeTokenFile(path, {.port = 1235, .token = b, .pid = 8}));
    CHECK(text().find(a) == std::string::npos);
    removeTokenFile(path, a);
    CHECK(std::filesystem::exists(path));
    removeTokenFile(path, "");
    CHECK(std::filesystem::exists(path));
    removeTokenFile(path, b);
    CHECK_FALSE(std::filesystem::exists(path));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("pictures are cut, made smaller and written as text", "[mcp][image]") {
    // Four by two, blue first: the first row blue, the second red.
    std::vector<uint8_t> bgra;
    for (int i = 0; i < 4; ++i) bgra.insert(bgra.end(), {255, 0, 0, 9});
    for (int i = 0; i < 4; ++i) bgra.insert(bgra.end(), {0, 0, 255, 9});
    const Picture whole = cutFrame(bgra, 4, 2, 0, 0, 0, 0);
    REQUIRE(whole.width == 4);
    REQUIRE(whole.height == 2);
    CHECK(whole.rgb[2] == 255);
    CHECK(whole.rgb[12] == 255);
    const Picture part = cutFrame(bgra, 4, 2, 3, 1, 10, 10);
    CHECK(part.width == 1);
    CHECK(part.height == 1);
    CHECK(part.rgb == std::vector<uint8_t>{255, 0, 0});
    const Picture small = shrink(whole, 2);
    CHECK(small.width == 2);
    CHECK(small.height == 1);
    CHECK(small.rgb[0] == 128);
    CHECK(shrink(whole, 0).width == 4);
    CHECK(shrink(whole, 100).width == 4);
    CHECK(cutFrame(bgra, 400, 200, 0, 0, 0, 0).rgb.empty());
    CHECK(base64({}) == "");
    CHECK(base64({'f'}) == "Zg==");
    CHECK(base64({'f', 'o'}) == "Zm8=");
    CHECK(base64({'f', 'o', 'o', 'b', 'a', 'r'}) == "Zm9vYmFy");
}

namespace {

using probe::Facet;

void quad(std::vector<Facet>& out, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d, Facet::Kind kind) {
    const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
    for (const auto& t : {std::array{a, b, c}, std::array{a, c, d}}) {
        Facet f = wowee::rendering::collision::makeFacet(n, t[0], t[1], t[2]);
        f.kind = kind;
        out.push_back(f);
    }
}

/// Level ground 60 yards across, at z 0.
std::vector<Facet> plain() {
    std::vector<Facet> out;
    quad(out, {-30, -30, 0}, {30, -30, 0}, {30, 30, 0}, {-30, 30, 0}, Facet::Kind::Ground);
    return out;
}

/// A wall of both sides across x = 5, from y = lo to y = hi, three yards high.
void wall(std::vector<Facet>& out, float lo, float hi) {
    quad(out, {5, lo, 0}, {5, lo, 3}, {5, hi, 3}, {5, hi, 0}, Facet::Kind::Building);
    quad(out, {5, lo, 0}, {5, hi, 0}, {5, hi, 3}, {5, lo, 3}, Facet::Kind::Building);
}

}  // namespace

TEST_CASE("rays and ground are met as the character meets them", "[mcp][probe]") {
    auto world = plain();
    wall(world, -10, 10);
    const auto hit = probe::castRay(world, {0, 0, 1}, {1, 0, 0}, 20);
    REQUIRE(hit);
    CHECK(hit->distance == Catch::Approx(5.0f));
    CHECK(hit->kind == Facet::Kind::Building);
    CHECK_FALSE(probe::castRay(world, {0, 0, 1}, {1, 0, 0}, 4.9f));
    CHECK_FALSE(probe::castRay(world, {0, 20, 1}, {1, 0, 0}, 20));
    const auto ground = probe::groundAt(world, 1, 1, 0.5f, 1.0f);
    REQUIRE(ground);
    CHECK(ground->z == Catch::Approx(0.0f).margin(1e-4));
    CHECK(ground->slopeDegrees == Catch::Approx(0.0f).margin(0.1));
    CHECK_FALSE(probe::groundAt(world, 100, 0, 0, 1));
    // Of two floors, the one that can be stood on from here.
    quad(world, {-5, -5, 4}, {5, -5, 4}, {5, 5, 4}, {-5, 5, 4}, Facet::Kind::Building);
    CHECK(probe::groundAt(world, 0, 0, 0.0f, 1.0f)->z == Catch::Approx(0.0f).margin(1e-4));
    CHECK(probe::groundAt(world, 0, 0, 4.0f, 1.0f)->z == Catch::Approx(4.0f));
}

TEST_CASE("a grid of the ground lies as a map does", "[mcp][probe]") {
    std::vector<Facet> slope;
    // Rising to the north (+x) by one in two.
    quad(slope, {-30, -30, -15}, {30, -30, 15}, {30, 30, 15}, {-30, 30, -15}, Facet::Kind::Ground);
    const auto grid = probe::groundGrid(slope, {0, 0, 0}, 4, 2, 100);
    REQUIRE(grid.side == 5);
    CHECK(grid.place(0, 0) == glm::vec2(4, 4));
    CHECK(grid.place(4, 4) == glm::vec2(-4, -4));
    REQUIRE(grid.at(0, 2));
    CHECK(grid.at(0, 2)->z == Catch::Approx(2.0f));
    CHECK(grid.at(4, 2)->z == Catch::Approx(-2.0f));
    CHECK(grid.at(2, 2)->slopeDegrees == Catch::Approx(26.565f).margin(0.1));
}

TEST_CASE("how far one can go", "[mcp][probe]") {
    auto world = plain();
    wall(world, -10, 10);
    const auto ahead = probe::wayAlong(world, {0, 0, 0}, {1, 0}, 20, 5);
    CHECK(ahead.why == probe::Way::Why::Wall);
    CHECK(ahead.clear == Catch::Approx(5.0f).margin(0.01));
    const auto back = probe::wayAlong(world, {0, 0, 0}, {-1, 0}, 20, 5);
    CHECK(back.why == probe::Way::Why::Reach);
    CHECK(back.clear == Catch::Approx(20.0f));
    const auto edge = probe::wayAlong(world, {0, 20, 0}, {0, 1}, 20, 5);
    CHECK(edge.why == probe::Way::Why::Drop);
    CHECK(edge.clear == Catch::Approx(10.0f).margin(1.1));
}

TEST_CASE("a way goes round what is in the way", "[mcp][walk]") {
    auto world = plain();
    wall(world, -10, 10);
    const WalkPlan plan = planWalk(FacetIndex(world), {0, 0, 0}, {10, 0, 0});
    REQUIRE(plan.reaches);
    REQUIRE(plan.points.size() >= 2);
    bool round = false;
    for (const auto& p : plan.points) round = round || std::fabs(p.y) > 10.0f;
    CHECK(round);
    // Each leg of it clear.
    FacetIndex index(world);
    glm::vec3 at(0.0f);
    for (const auto& p : plan.points) {
        CHECK(index.clear(at, p, 0.3f, 2.0f));
        at = p;
    }
    // Straight where nothing is.
    const WalkPlan straight = planWalk(FacetIndex(world), {0, 0, 0}, {-10, 3, 0});
    REQUIRE(straight.reaches);
    CHECK(straight.points.size() == 1);
}

TEST_CASE("what hangs over the way stops a rider, and a place known to be blocked anybody", "[mcp][walk]") {
    auto world = plain();
    // A beam two and a half yards up, across x = 5 from y = -10 to 10.
    quad(world, {5, -10, 2.4f}, {5, -10, 3}, {5, 10, 3}, {5, 10, 2.4f}, Facet::Kind::Building);
    quad(world, {5, -10, 2.4f}, {5, 10, 2.4f}, {5, 10, 3}, {5, -10, 3}, Facet::Kind::Building);
    const FacetIndex index(world);
    CHECK(planWalk(index, {0, 0, 0}, {10, 0, 0}).points.size() == 1);
    WalkLimits rider;
    rider.bodyHeight = 3.0f;
    const WalkPlan ridden = planWalk(index, {0, 0, 0}, {10, 0, 0}, rider);
    REQUIRE(ridden.reaches);
    CHECK(ridden.points.size() > 1);
    WalkLimits stuckBefore;
    stuckBefore.avoid.push_back({5, 0, 0});
    const WalkPlan round = planWalk(FacetIndex(plain()), {0, 0, 0}, {10, 0, 0}, stuckBefore);
    REQUIRE(round.reaches);
    CHECK(round.points.size() > 1);
    for (const auto& p : round.points) CHECK(glm::length(glm::vec2(p) - glm::vec2(5, 0)) >= 0.9f);
}

TEST_CASE("where there is no way, it goes as near as it can", "[mcp][walk]") {
    auto world = plain();
    wall(world, -30, 30);
    const WalkPlan plan = planWalk(FacetIndex(world), {0, 0, 0}, {10, 0, 0});
    CHECK_FALSE(plan.reaches);
    CHECK(plan.left > 4.0f);
    for (const auto& p : plan.points) CHECK(p.x < 5.0f);
    const WalkPlan nowhere = planWalk(FacetIndex({}), {0, 0, 0}, {10, 0, 0});
    CHECK_FALSE(nowhere.reaches);
    CHECK(nowhere.points.empty());
}
