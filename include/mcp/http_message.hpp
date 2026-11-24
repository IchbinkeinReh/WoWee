#pragma once

// One HTTP/1.1 request, read as it arrives, and the answer to one.
//
// Only what an MCP client sends: a request line, headers, and a body whose
// length is given. Everything else is refused rather than guessed at - no
// chunked bodies, no folded headers, no line that ends in a bare LF, no header
// that decides something given twice - because what the two ends of a
// connection disagree about is what request smuggling is made of. Free of
// sockets, so that malformed input can be tried on it in a test.

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wowee::mcp {

struct HttpLimits {
    size_t headerBytes = 16 * 1024;
    size_t bodyBytes = 256 * 1024;
    size_t headerCount = 64;
};

struct HttpRequest {
    std::string method;
    std::string target;
    bool http11 = true;
    /// Names in lower case, values without the blanks round them.
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    /// The header's value, or null; the first where it was given twice.
    [[nodiscard]] const std::string* header(std::string_view lowerName) const;
    /// Whether the connection stays open after the answer.
    [[nodiscard]] bool keepAlive() const;
};

class HttpRequestReader {
public:
    explicit HttpRequestReader(HttpLimits limits = {}) : limits_(limits) {}

    enum class State {
        Headers,      ///< more bytes wanted
        Body,         ///< the headers are in, more of the body wanted
        Complete,     ///< request() is whole; take() it
        Failed        ///< errorStatus() says with which status
    };

    /// Takes bytes off the wire. What arrives after a whole request is kept
    /// for the next one (take()).
    State feed(std::string_view bytes);
    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] const HttpRequest& request() const { return request_; }
    [[nodiscard]] int errorStatus() const { return errorStatus_; }
    [[nodiscard]] const std::string& errorText() const { return errorText_; }
    /// Whether anything of a request has arrived: a connection kept open with
    /// nothing on it is idle, one in the middle of a request is slow.
    [[nodiscard]] bool midRequest() const { return state_ == State::Body || !buffer_.empty(); }
    /// The client waits for a word before it sends the body
    /// (Expect: 100-continue); answered once.
    [[nodiscard]] bool takeExpectContinue();
    /// The whole request, and the reader ready for the next.
    HttpRequest take();

private:
    State fail(int status, const char* text);
    State advance();
    State parseHead(std::string_view head);

    HttpLimits limits_;
    State state_ = State::Headers;
    std::string buffer_;
    HttpRequest request_;
    size_t bodyLength_ = 0;
    bool expectContinue_ = false;
    int errorStatus_ = 0;
    std::string errorText_;
};

struct HttpResponse {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

const char* httpReason(int status);
/// The bytes of an answer. `close`: the connection ends after it.
std::string writeResponse(const HttpResponse& response, bool close);

}  // namespace wowee::mcp
