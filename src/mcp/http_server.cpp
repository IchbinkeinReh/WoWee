#include "mcp/http_server.hpp"

#include <algorithm>
#include <array>
#include <utility>

#ifndef _WIN32
#include <sys/select.h>
#endif

#include "mcp/http_auth.hpp"

namespace wowee::mcp {

namespace {

constexpr auto kPoll = std::chrono::milliseconds(100);

/// Whether there is something to read (or room to write) within `wait`.
/// 1 yes, 0 not yet, -1 the socket is gone.
int ready(socket_t s, bool write, std::chrono::milliseconds wait) {
    fd_set set;
    FD_ZERO(&set);
#ifndef _WIN32
    // A descriptor past the set's end would be written beside it.
    if (s >= FD_SETSIZE) return -1;
#endif
    FD_SET(s, &set);
    timeval tv{};
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(wait.count() / 1000);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((wait.count() % 1000) * 1000);
    const int n = ::select(static_cast<int>(s) + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
    return n;
}

}  // namespace

HttpServer::~HttpServer() {
    stop();
}

bool HttpServer::start(uint16_t port, std::string token, Handler handler, std::string& error) {
    net::ensureInit();
    if (token.empty()) {
        error = "no token";
        return false;
    }
    token_ = std::move(token);
    handler_ = std::move(handler);
    listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == INVALID_SOCK) {
        error = "no socket";
        return false;
    }
#ifdef _WIN32
    // Nobody else's socket on the same port, before or after this one.
    const BOOL exclusive = TRUE;
    ::setsockopt(listener_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
                 sizeof(exclusive));
#else
    const int reuse = 1;
    ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener_, 8) != 0) {
        error = "port " + std::to_string(port) + " on 127.0.0.1 cannot be had (error " +
                std::to_string(net::lastError()) + ")";
        net::closeSocket(listener_);
        listener_ = INVALID_SOCK;
        return false;
    }
    sockaddr_in bound{};
#ifdef _WIN32
    int length = sizeof(bound);
#else
    socklen_t length = sizeof(bound);
#endif
    if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &length) == 0) port_ = ntohs(bound.sin_port);
    net::setNonBlocking(listener_);
    stopping_ = false;
    acceptThread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void HttpServer::stop() {
    stopping_ = true;
    if (acceptThread_.joinable()) acceptThread_.join();
    if (listener_ != INVALID_SOCK) {
        net::closeSocket(listener_);
        listener_ = INVALID_SOCK;
    }
    sweep(true);
}

void HttpServer::sweep(bool all) {
    std::list<std::unique_ptr<Connection>> finished;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = connections_.begin(); it != connections_.end();) {
            auto next = std::next(it);
            if (all || (*it)->done.load()) finished.splice(finished.end(), connections_, it);
            it = next;
        }
    }
    for (auto& c : finished) {
        if (c->thread.joinable()) c->thread.join();
        net::closeSocket(c->socket);
    }
}

void HttpServer::acceptLoop() {
    while (!stopping_) {
        sweep(false);
        const int n = ready(listener_, false, kPoll);
        if (n < 0) break;
        if (n == 0) continue;
        sockaddr_in peer{};
#ifdef _WIN32
        int length = sizeof(peer);
#else
        socklen_t length = sizeof(peer);
#endif
        const socket_t s = ::accept(listener_, reinterpret_cast<sockaddr*>(&peer), &length);
        if (s == INVALID_SOCK) continue;
        net::setNonBlocking(s);
#ifdef SO_NOSIGPIPE
        const int on = 1;
        ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
        size_t open = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            open = connections_.size();
        }
        // The address it came from as well, though nothing else can reach a
        // socket on the loopback.
        if (open >= limits_.connections || ntohl(peer.sin_addr.s_addr) >> 24 != 127) {
            ++refused_;
            turnAway(s);
            continue;
        }
        auto connection = std::make_unique<Connection>();
        connection->socket = s;
        Connection* c = connection.get();
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            connections_.push_back(std::move(connection));
        }
        c->thread = std::thread([this, c] {
            serve(*c);
            c->done = true;
        });
    }
}

bool HttpServer::sendAll(socket_t s, const std::string& bytes) {
    size_t at = 0;
    const auto until = std::chrono::steady_clock::now() + limits_.send;
    while (at < bytes.size()) {
        if (std::chrono::steady_clock::now() > until) return false;
        const int n = ready(s, true, kPoll);
        if (n < 0) return false;
        if (n == 0) continue;
#ifdef MSG_NOSIGNAL
        constexpr int kFlags = MSG_NOSIGNAL;
#else
        constexpr int kFlags = 0;
#endif
        const size_t chunk = std::min<size_t>(bytes.size() - at, 64 * 1024);
        const auto sent = ::send(s, bytes.data() + at, static_cast<int>(chunk), kFlags);
        if (sent < 0) {
            if (net::isWouldBlock(net::lastError())) continue;
            return false;
        }
        at += static_cast<size_t>(sent);
    }
    return true;
}

void HttpServer::serve(Connection& connection) {
    using Clock = std::chrono::steady_clock;
    HttpRequestReader reader(limits_.message);
    auto idleSince = Clock::now();
    auto requestSince = Clock::time_point{};
    std::array<char, 16 * 1024> buffer{};
    while (!stopping_) {
        if (reader.state() == HttpRequestReader::State::Complete) {
            const HttpRequest request = reader.take();
            requestSince = {};
            const HttpResponse response = answer(request);
            // A refusal ends the connection: what follows it is not read.
            const bool close = !request.keepAlive() || response.status >= 400;
            if (!sendAll(connection.socket, writeResponse(response, close)) || close) break;
            idleSince = Clock::now();
            continue;
        }
        if (reader.state() == HttpRequestReader::State::Failed) {
            ++refused_;
            sendAll(connection.socket, writeResponse({.status = reader.errorStatus(),
                                                      .contentType = "text/plain",
                                                      .body = reader.errorText() + "\n",
                                                      .headers = {}},
                                                     true));
            break;
        }
        const auto now = Clock::now();
        if (requestSince == Clock::time_point{} && reader.midRequest()) requestSince = now;
        if (requestSince != Clock::time_point{} && now - requestSince > limits_.request) {
            ++refused_;
            sendAll(connection.socket, writeResponse({.status = 408, .contentType = "text/plain", .body = "too slow\n",
                                                      .headers = {}},
                                                     true));
            break;
        }
        if (requestSince == Clock::time_point{} && now - idleSince > limits_.idle) break;

        const int n = ready(connection.socket, false, kPoll);
        if (n < 0) break;
        if (n == 0) continue;
        const auto got = ::recv(connection.socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (got == 0) break;
        if (got < 0) {
            if (net::isWouldBlock(net::lastError())) continue;
            break;
        }
        reader.feed(std::string_view(buffer.data(), static_cast<size_t>(got)));
        // Only for who may send a body at all: the head is in, so the
        // question can be answered before the body is.
        if (reader.takeExpectContinue()) {
            const AuthVerdict verdict = authorize(reader.request(), port_, token_);
            if (verdict.status != 0) {
                ++refused_;
                sendAll(connection.socket, writeResponse({.status = verdict.status, .contentType = "text/plain",
                                                          .body = std::string(verdict.text) + "\n", .headers = {}},
                                                         true));
                break;
            }
            if (!sendAll(connection.socket, "HTTP/1.1 100 Continue\r\n\r\n")) break;
        }
    }
    // Said to be over, and what the client is still sending - the body of a
    // request refused by its head - read for a moment: closed with bytes
    // unread, a socket is reset, and the answer may be lost with it.
#ifdef _WIN32
    ::shutdown(connection.socket, SD_SEND);
#else
    ::shutdown(connection.socket, SHUT_WR);
#endif
    const auto until = Clock::now() + std::chrono::milliseconds(500);
    while (!stopping_ && Clock::now() < until) {
        const int n = ready(connection.socket, false, kPoll);
        if (n < 0) break;
        if (n == 0) continue;
        if (::recv(connection.socket, buffer.data(), static_cast<int>(buffer.size()), 0) <= 0) break;
    }
}

void HttpServer::turnAway(socket_t s) {
    sendAll(s, writeResponse({.status = 503, .contentType = "text/plain", .body = "busy\n", .headers = {}}, true));
#ifdef _WIN32
    ::shutdown(s, SD_SEND);
#else
    ::shutdown(s, SHUT_WR);
#endif
    std::array<char, 2048> buffer{};
    for (int i = 0; i < 4 && ready(s, false, std::chrono::milliseconds(50)) > 0; ++i) {
        if (::recv(s, buffer.data(), static_cast<int>(buffer.size()), 0) <= 0) break;
    }
    net::closeSocket(s);
}

HttpResponse HttpServer::answer(const HttpRequest& request) {
    const auto refuse = [&](int status, const char* text) {
        ++refused_;
        HttpResponse out{.status = status, .contentType = "text/plain", .body = std::string(text) + "\n", .headers = {}};
        if (status == 401) out.headers.emplace_back("WWW-Authenticate", "Bearer");
        if (status == 405) out.headers.emplace_back("Allow", "POST");
        return out;
    };
    const AuthVerdict verdict = authorize(request, port_, token_);
    if (verdict.status != 0) return refuse(verdict.status, verdict.text);
    const std::string path = request.target.substr(0, request.target.find('?'));
    if (path != "/mcp") return refuse(404, "POST /mcp");
    // No stream from the server and no session to end.
    if (request.method != "POST") return refuse(405, "POST only");
    const std::string* type = request.header("content-type");
    if (!type || type->substr(0, type->find(';')).find("application/json") == std::string::npos) {
        return refuse(415, "application/json wanted");
    }
    if (const auto body = handler_(request.body)) {
        return {.status = 200, .contentType = "application/json", .body = *body, .headers = {}};
    }
    return {.status = 202, .contentType = "", .body = "", .headers = {}};
}

}  // namespace wowee::mcp
