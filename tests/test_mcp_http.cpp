// The MCP server's HTTP: what is read off the wire, and who is let in.
//
// The reader is the part of the server anything on this machine can hand bytes
// to before it has shown a token, so most of this is bytes no client sends.

#include <catch_amalgamated.hpp>

#include <string>

#include "mcp/http_auth.hpp"
#include "mcp/http_message.hpp"

using namespace wowee::mcp;
using State = HttpRequestReader::State;

namespace {

std::string post(const std::string& body, const std::string& extra = "") {
    return "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:8642\r\nContent-Type: application/json\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\n" + extra + "\r\n" + body;
}

int statusOf(const std::string& bytes, HttpLimits limits = {}) {
    HttpRequestReader reader(limits);
    reader.feed(bytes);
    return reader.state() == State::Failed ? reader.errorStatus() : 0;
}

HttpRequest requestOf(const std::string& bytes) {
    HttpRequestReader reader;
    REQUIRE(reader.feed(bytes) == State::Complete);
    return reader.take();
}

}  // namespace

TEST_CASE("a whole request is read, in one piece or byte by byte", "[mcp][http]") {
    const std::string bytes = post("{\"a\":1}", "X-Other: some value \r\n");
    HttpRequestReader whole;
    REQUIRE(whole.feed(bytes) == State::Complete);
    HttpRequestReader slow;
    for (size_t i = 0; i + 1 < bytes.size(); ++i) {
        REQUIRE(slow.feed(bytes.substr(i, 1)) != State::Complete);
    }
    REQUIRE(slow.feed(bytes.substr(bytes.size() - 1)) == State::Complete);
    for (auto* reader : {&whole, &slow}) {
        const HttpRequest r = reader->take();
        CHECK(r.method == "POST");
        CHECK(r.target == "/mcp");
        CHECK(r.http11);
        CHECK(r.body == "{\"a\":1}");
        REQUIRE(r.header("x-other"));
        CHECK(*r.header("x-other") == "some value");
        CHECK_FALSE(r.header("x-none"));
    }
}

TEST_CASE("two requests in one read are two requests", "[mcp][http]") {
    HttpRequestReader reader;
    REQUIRE(reader.feed(post("1") + post("22")) == State::Complete);
    CHECK(reader.take().body == "1");
    REQUIRE(reader.state() == State::Complete);
    CHECK(reader.take().body == "22");
    CHECK(reader.state() == State::Headers);
    CHECK_FALSE(reader.midRequest());
}

TEST_CASE("a request without a body is whole at its blank line", "[mcp][http]") {
    CHECK(requestOf("GET /mcp HTTP/1.1\r\nHost: a\r\n\r\n").body.empty());
}

TEST_CASE("malformed heads are refused", "[mcp][http]") {
    CHECK(statusOf("\r\n\r\n") == 400);
    CHECK(statusOf("POST\r\n\r\n") == 400);
    CHECK(statusOf("POST /mcp\r\n\r\n") == 400);
    CHECK(statusOf("POST  /mcp HTTP/1.1\r\n\r\n") == 400);
    CHECK(statusOf("PO ST /mcp HTTP/1.1\r\n\r\n") == 400);
    CHECK(statusOf("P@ST /mcp HTTP/1.1\r\n\r\n") == 400);
    CHECK(statusOf("GET mcp HTTP/1.1\r\n\r\n") == 400);
    CHECK(statusOf("GET http://127.0.0.1/mcp HTTP/1.1\r\n\r\n") == 400);
    CHECK(statusOf("GET /m\tcp HTTP/1.1\r\n\r\n") == 400);
    CHECK(statusOf(std::string("GET /m\0cp HTTP/1.1\r\n\r\n", 25)) == 400);
    CHECK(statusOf("GET /mcp HTTP/2.0\r\n\r\n") == 505);
    CHECK(statusOf("GET /mcp HTTP/0.9\r\n\r\n") == 505);
    CHECK(statusOf("GET /mcp HTTP/1.1\r\nNoColon\r\n\r\n") == 400);
    CHECK(statusOf("GET /mcp HTTP/1.1\r\n: empty\r\n\r\n") == 400);
    CHECK(statusOf("GET /mcp HTTP/1.1\r\nBad Name: x\r\n\r\n") == 400);
    CHECK(statusOf("GET /mcp HTTP/1.1\r\nName : x\r\n\r\n") == 400);
    CHECK(statusOf("GET /mcp HTTP/1.1\r\nA: b\r\n folded\r\n\r\n") == 400);
    CHECK(statusOf("GET /mcp HTTP/1.1\nHost: a\n\n") == 400);
    CHECK(statusOf("GET /mcp HTTP/1.1\r\nA: b\nC: d\r\n\r\n") == 400);
    CHECK(statusOf("GET /mcp HTTP/1.1\r\nA: \x01\r\n\r\n") == 400);
    CHECK(statusOf("GET /mcp HTTP/1.1\r\nA: \xc3\xa4\r\n\r\n") == 400);
}

TEST_CASE("a body's length is given once, in digits, and held to the limit", "[mcp][http]") {
    const std::string head = "POST /mcp HTTP/1.1\r\nHost: a\r\n";
    CHECK(statusOf(head + "\r\n") == 411);
    CHECK(statusOf(head + "Transfer-Encoding: chunked\r\n\r\n") == 411);
    CHECK(statusOf(head + "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n") == 411);
    CHECK(statusOf(head + "Content-Length: -1\r\n\r\n") == 400);
    CHECK(statusOf(head + "Content-Length: +1\r\n\r\n") == 400);
    CHECK(statusOf(head + "Content-Length: 1 2\r\n\r\n") == 400);
    CHECK(statusOf(head + "Content-Length: 0x10\r\n\r\n") == 400);
    CHECK(statusOf(head + "Content-Length: \r\n\r\n") == 400);
    CHECK(statusOf(head + "Content-Length: 99999999999999999999\r\n\r\n") == 400);
    CHECK(statusOf(head + "Content-Length: 1\r\nContent-Length: 2\r\n\r\n") == 400);
    CHECK(statusOf(head + "Content-Length: 262145\r\n\r\n") == 413);
    CHECK(statusOf(head + "Content-Length: 262144\r\n\r\n") == 0);
}

TEST_CASE("what decides is not given twice", "[mcp][http]") {
    for (const char* name : {"Host", "Origin", "Authorization", "Content-Type", "Connection"}) {
        const std::string line = std::string(name) + ": a\r\n";
        CHECK(statusOf("GET /mcp HTTP/1.1\r\n" + line + line + "\r\n") == 400);
    }
    CHECK(statusOf("GET /mcp HTTP/1.1\r\nAccept: a\r\nAccept: b\r\n\r\n") == 0);
}

TEST_CASE("heads without end and without number are cut off", "[mcp][http]") {
    HttpLimits limits;
    limits.headerBytes = 256;
    limits.headerCount = 4;
    CHECK(statusOf("GET /" + std::string(300, 'a'), limits) == 431);
    CHECK(statusOf("GET /" + std::string(300, 'a') + " HTTP/1.1\r\n\r\n", limits) == 431);
    std::string many = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 5; ++i) many += "A" + std::to_string(i) + ": b\r\n";
    CHECK(statusOf(many + "\r\n", limits) == 431);
}

TEST_CASE("nothing more is taken from a request that failed", "[mcp][http]") {
    HttpRequestReader reader;
    REQUIRE(reader.feed("nonsense\r\n\r\n") == State::Failed);
    CHECK(reader.feed(post("{}")) == State::Failed);
    CHECK(reader.errorStatus() == 400);
}

TEST_CASE("a client that waits for a word before its body is given one", "[mcp][http]") {
    HttpRequestReader reader;
    reader.feed("POST /mcp HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\nContent-Length: 2\r\n\r\n");
    REQUIRE(reader.state() == State::Body);
    CHECK(reader.takeExpectContinue());
    CHECK_FALSE(reader.takeExpectContinue());
    CHECK(reader.feed("{}") == State::Complete);
    CHECK(statusOf("POST /mcp HTTP/1.1\r\nExpect: something\r\nContent-Length: 2\r\n\r\n") == 417);
}

TEST_CASE("a connection stays open as the request has it", "[mcp][http]") {
    CHECK(requestOf("GET / HTTP/1.1\r\n\r\n").keepAlive());
    CHECK_FALSE(requestOf("GET / HTTP/1.1\r\nConnection: Close\r\n\r\n").keepAlive());
    CHECK_FALSE(requestOf("GET / HTTP/1.0\r\n\r\n").keepAlive());
    CHECK(requestOf("GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n").keepAlive());
}

TEST_CASE("an answer says its length, also of nothing", "[mcp][http]") {
    const std::string some = writeResponse({.status = 200, .contentType = "application/json", .body = "{}", .headers = {}}, false);
    CHECK(some.find("HTTP/1.1 200 OK\r\n") == 0);
    CHECK(some.find("Content-Length: 2\r\n") != std::string::npos);
    CHECK(some.find("Connection: close") == std::string::npos);
    CHECK(some.substr(some.size() - 6) == "\r\n\r\n{}");
    const std::string none = writeResponse({.status = 202, .contentType = "", .body = "", .headers = {}}, true);
    CHECK(none.find("Content-Length: 0\r\n") != std::string::npos);
    CHECK(none.find("Connection: close\r\n") != std::string::npos);
    CHECK(none.find("Access-Control") == std::string::npos);
}

TEST_CASE("only this server's own names are its host", "[mcp][auth]") {
    CHECK(hostIsLocal("127.0.0.1:8642", 8642));
    CHECK(hostIsLocal("localhost:8642", 8642));
    CHECK(hostIsLocal("LOCALHOST:8642", 8642));
    for (const char* host : {"", "127.0.0.1", "localhost", "127.0.0.1:8643", "evil.example:8642", "127.0.0.1:8642.evil",
                             "127.0.0.1:08642", "127.0.0.2:8642", "localhost.evil:8642", "evil.localhost:8642",
                             "[::1]:8642", "0.0.0.0:8642", " 127.0.0.1:8642"}) {
        INFO(host);
        CHECK_FALSE(hostIsLocal(host, 8642));
    }
}

TEST_CASE("a page's origin is local or nobody's", "[mcp][auth]") {
    for (const char* origin : {"http://localhost", "http://localhost:3000", "https://127.0.0.1:8642",
                               "http://127.0.0.1", "http://[::1]:5173"}) {
        INFO(origin);
        CHECK(originIsLocal(origin));
    }
    for (const char* origin : {"", "null", "http://evil.example", "http://localhost.evil.example",
                               "http://127.0.0.1.evil.example", "http://127.0.0.1:80@evil.example",
                               "http://localhost:", "http://localhost:80/", "file://", "ftp://localhost",
                               "http://localhostx"}) {
        INFO(origin);
        CHECK_FALSE(originIsLocal(origin));
    }
}

TEST_CASE("host, origin and token are asked in that order", "[mcp][auth]") {
    const auto verdict = [](const std::string& headers) {
        return authorize(requestOf("GET /mcp HTTP/1.1\r\n" + headers + "\r\n"), 8642, "secret").status;
    };
    const std::string host = "Host: 127.0.0.1:8642\r\n";
    CHECK(verdict("") == 403);
    CHECK(verdict("Host: evil.example\r\nAuthorization: Bearer secret\r\n") == 403);
    CHECK(verdict(host + "Origin: http://evil.example\r\nAuthorization: Bearer secret\r\n") == 403);
    CHECK(verdict(host + "Origin: null\r\nAuthorization: Bearer secret\r\n") == 403);
    CHECK(verdict(host) == 401);
    CHECK(verdict(host + "Authorization: secret\r\n") == 401);
    CHECK(verdict(host + "Authorization: Basic secret\r\n") == 401);
    CHECK(verdict(host + "Authorization: Bearer\r\n") == 401);
    CHECK(verdict(host + "Authorization: Bearer wrong!\r\n") == 401);
    CHECK(verdict(host + "Authorization: Bearer secret2\r\n") == 401);
    CHECK(verdict(host + "Authorization: Bearer secret\r\n") == 0);
    CHECK(verdict(host + "Authorization: bearer secret\r\n") == 0);
    CHECK(verdict(host + "Origin: http://localhost:3000\r\nAuthorization: Bearer secret\r\n") == 0);
}

TEST_CASE("an empty token lets nobody in", "[mcp][auth]") {
    CHECK_FALSE(sameSecret("", ""));
    CHECK_FALSE(sameSecret("a", ""));
    CHECK(sameSecret("abc", "abc"));
    CHECK_FALSE(sameSecret("abc", "abd"));
    CHECK_FALSE(sameSecret("abc", "abcd"));
}
