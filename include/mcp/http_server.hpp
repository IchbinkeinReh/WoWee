#pragma once

// The listening socket of the MCP server and the threads behind it.
//
// Bound to 127.0.0.1 and nothing else. One thread takes connections and one
// each serves them, a few at a time; a request that is not for this server, or
// not from who may ask (http_auth.hpp), is answered here, and what is left is
// handed to the one function that knows what MCP is.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "mcp/http_message.hpp"
#include "network/net_platform.hpp"

namespace wowee::mcp {

struct HttpServerLimits {
    HttpLimits message;
    size_t connections = 8;
    /// A connection with nothing on it.
    std::chrono::milliseconds idle{60000};
    /// A request from its first byte to its last.
    std::chrono::milliseconds request{10000};
    std::chrono::milliseconds send{10000};
};

class HttpServer {
public:
    /// The body of a POST to /mcp in; the answer out, or nothing for what is
    /// not answered. On the connection's thread.
    using Handler = std::function<std::optional<std::string>(const std::string& body)>;

    explicit HttpServer(HttpServerLimits limits = {}) : limits_(limits) {}
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /// False, with why in `error`, when the port cannot be had. Port 0 takes
    /// a free one.
    bool start(uint16_t port, std::string token, Handler handler, std::string& error);
    /// Ends every connection and waits for the threads.
    void stop();

    [[nodiscard]] uint16_t port() const { return port_; }
    [[nodiscard]] uint64_t refused() const { return refused_.load(std::memory_order_relaxed); }

private:
    struct Connection {
        socket_t socket = INVALID_SOCK;
        std::thread thread;
        std::atomic<bool> done{false};
    };

    void acceptLoop();
    void serve(Connection& connection);
    /// What a whole request is answered with.
    HttpResponse answer(const HttpRequest& request);
    bool sendAll(socket_t s, const std::string& bytes);
    /// There is no room: said so, and closed.
    void turnAway(socket_t s);
    void sweep(bool all);

    HttpServerLimits limits_;
    uint16_t port_ = 0;
    std::string token_;
    Handler handler_;
    socket_t listener_ = INVALID_SOCK;
    std::thread acceptThread_;
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> refused_{0};
    std::mutex mutex_;
    std::list<std::unique_ptr<Connection>> connections_;
};

}  // namespace wowee::mcp
