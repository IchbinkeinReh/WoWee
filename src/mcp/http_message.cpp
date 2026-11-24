#include "mcp/http_message.hpp"

#include <algorithm>
#include <cctype>

namespace wowee::mcp {

namespace {

/// RFC 9110 token: what a method and a header's name are made of.
bool isToken(std::string_view s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](char c) {
        if (std::isalnum(static_cast<unsigned char>(c)) && static_cast<unsigned char>(c) < 0x80) return true;
        return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
    });
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trimBlanks(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

/// A header's value: printable, blanks and tabs; no control characters, and
/// nothing above ASCII, which no header this server reads has in it.
bool isFieldValue(std::string_view s) {
    return std::all_of(s.begin(), s.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return c == '\t' || (u >= 0x20 && u < 0x7f);
    });
}

/// The headers that decide whether a request is taken, or where it ends.
/// Given twice, the two ends may each believe a different one.
bool mustBeSingle(std::string_view name) {
    return name == "host" || name == "origin" || name == "authorization" || name == "content-length" ||
           name == "transfer-encoding" || name == "content-type" || name == "expect" || name == "connection";
}

}  // namespace

const std::string* HttpRequest::header(std::string_view lowerName) const {
    for (const auto& [name, value] : headers) {
        if (name == lowerName) return &value;
    }
    return nullptr;
}

bool HttpRequest::keepAlive() const {
    const std::string* c = header("connection");
    const std::string v = c ? lower(*c) : std::string();
    if (v.find("close") != std::string::npos) return false;
    return http11 || v.find("keep-alive") != std::string::npos;
}

HttpRequestReader::State HttpRequestReader::fail(int status, const char* text) {
    state_ = State::Failed;
    errorStatus_ = status;
    errorText_ = text;
    buffer_.clear();
    return state_;
}

HttpRequestReader::State HttpRequestReader::feed(std::string_view bytes) {
    if (state_ == State::Failed) return state_;
    buffer_.append(bytes);
    return advance();
}

HttpRequestReader::State HttpRequestReader::advance() {
    if (state_ == State::Headers) {
        const size_t end = buffer_.find("\r\n\r\n");
        if (end == std::string::npos) {
            // A head whose lines end in bare LFs would never be found by the
            // search above; it is refused now rather than waited on.
            if (buffer_.find("\n\n") != std::string::npos) return fail(400, "line ended in a bare LF");
            if (buffer_.size() > limits_.headerBytes) return fail(431, "headers too large");
            return state_;
        }
        if (end + 4 > limits_.headerBytes) return fail(431, "headers too large");
        const std::string head = buffer_.substr(0, end);
        buffer_.erase(0, end + 4);
        if (parseHead(head) == State::Failed) return state_;
    }
    if (state_ == State::Body) {
        if (buffer_.size() < bodyLength_) return state_;
        request_.body = buffer_.substr(0, bodyLength_);
        buffer_.erase(0, bodyLength_);
        expectContinue_ = false;
        state_ = State::Complete;
    }
    return state_;
}

HttpRequestReader::State HttpRequestReader::parseHead(std::string_view head) {
    request_ = {};
    bodyLength_ = 0;
    expectContinue_ = false;

    const size_t lineEnd = head.find("\r\n");
    const std::string_view line = head.substr(0, lineEnd);
    const size_t sp1 = line.find(' ');
    const size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos) return fail(400, "malformed request line");
    const std::string_view method = line.substr(0, sp1);
    const std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string_view version = line.substr(sp2 + 1);
    if (!isToken(method) || method.size() > 16) return fail(400, "malformed method");
    if (target.empty() || target.front() != '/' ||
        !std::all_of(target.begin(), target.end(), [](char c) {
            const auto u = static_cast<unsigned char>(c);
            return u > 0x20 && u < 0x7f;
        })) {
        return fail(400, "malformed target");
    }
    if (version == "HTTP/1.1") request_.http11 = true;
    else if (version == "HTTP/1.0") request_.http11 = false;
    else return fail(505, "HTTP/1.1 only");
    request_.method = method;
    request_.target = target;

    size_t at = lineEnd == std::string_view::npos ? head.size() : lineEnd + 2;
    while (at < head.size()) {
        size_t end = head.find("\r\n", at);
        if (end == std::string_view::npos) end = head.size();
        const std::string_view field = head.substr(at, end - at);
        at = end + 2;
        if (field.find('\n') != std::string_view::npos || field.find('\r') != std::string_view::npos) {
            return fail(400, "line ended in a bare CR or LF");
        }
        if (field.empty() || field.front() == ' ' || field.front() == '\t') {
            return fail(400, "folded header");
        }
        const size_t colon = field.find(':');
        if (colon == std::string_view::npos || !isToken(field.substr(0, colon))) {
            return fail(400, "malformed header");
        }
        const std::string name = lower(field.substr(0, colon));
        const std::string_view value = trimBlanks(field.substr(colon + 1));
        if (!isFieldValue(value)) return fail(400, "malformed header value");
        if (mustBeSingle(name) && request_.header(name)) return fail(400, "header given twice");
        if (request_.headers.size() >= limits_.headerCount) return fail(431, "too many headers");
        request_.headers.emplace_back(name, std::string(value));
    }

    if (request_.header("transfer-encoding")) return fail(411, "Content-Length wanted");
    if (const std::string* length = request_.header("content-length")) {
        if (length->empty() || length->size() > 12 ||
            !std::all_of(length->begin(), length->end(), [](char c) { return c >= '0' && c <= '9'; })) {
            return fail(400, "malformed Content-Length");
        }
        const unsigned long long n = std::stoull(*length);
        if (n > limits_.bodyBytes) return fail(413, "body too large");
        bodyLength_ = static_cast<size_t>(n);
    } else if (request_.method == "POST" || request_.method == "PUT") {
        return fail(411, "Content-Length wanted");
    }
    if (const std::string* expect = request_.header("expect")) {
        if (lower(*expect) != "100-continue") return fail(417, "expectation failed");
        expectContinue_ = bodyLength_ > 0;
    }
    state_ = State::Body;
    return state_;
}

bool HttpRequestReader::takeExpectContinue() {
    if (state_ != State::Body || !expectContinue_) return false;
    expectContinue_ = false;
    return true;
}

HttpRequest HttpRequestReader::take() {
    HttpRequest out = std::move(request_);
    request_ = {};
    bodyLength_ = 0;
    state_ = State::Headers;
    // A second request may already be here.
    if (!buffer_.empty()) advance();
    return out;
}

const char* httpReason(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 411: return "Length Required";
        case 413: return "Content Too Large";
        case 415: return "Unsupported Media Type";
        case 417: return "Expectation Failed";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 503: return "Service Unavailable";
        case 505: return "HTTP Version Not Supported";
        default: return "Error";
    }
}

std::string writeResponse(const HttpResponse& response, bool close) {
    std::string out = "HTTP/1.1 " + std::to_string(response.status) + ' ' + httpReason(response.status) + "\r\n";
    if (!response.body.empty()) out += "Content-Type: " + response.contentType + "\r\n";
    // Said for an empty one too: a client that reads to the end of the body
    // must know where that is.
    out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
    out += "Cache-Control: no-store\r\n";
    out += "X-Content-Type-Options: nosniff\r\n";
    for (const auto& [name, value] : response.headers) out += name + ": " + value + "\r\n";
    if (close) out += "Connection: close\r\n";
    out += "\r\n";
    out += response.body;
    return out;
}

}  // namespace wowee::mcp
