#pragma once

// Whose doing what the client does now is.
//
// A packet does not say why it is sent. What is known is who acted last: a
// key, a button or typed text from the devices, or the MCP server. From the
// server's first act until a real one, everything the client sends is counted
// as the server's - also what comes later, out of a timer, an addon or an
// answer from the realm - and is held to send_guard.hpp. While one of the
// server's acts is still under way (a key held, a walk) a real key press does
// not change that.

#include <atomic>

namespace wowee::mcp {

class Origin {
public:
    /// The server pressed something, or is about to.
    void actedByServer() { server_.store(true, std::memory_order_relaxed); }
    void beginAct() {
        ++running_;
        actedByServer();
    }
    void endAct() { --running_; }
    /// A key, a mouse button or text from a device.
    void actedByHand() {
        if (running_.load(std::memory_order_relaxed) == 0) server_.store(false, std::memory_order_relaxed);
    }
    [[nodiscard]] bool isServer() const { return server_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> server_{false};
    std::atomic<int> running_{0};
};

}  // namespace wowee::mcp
