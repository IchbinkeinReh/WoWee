#include "mcp/http_auth.hpp"

#include <cctype>

namespace wowee::mcp {

namespace {

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

bool allDigits(std::string_view s) {
    if (s.empty()) return false;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

}  // namespace

bool hostIsLocal(std::string_view host, uint16_t port) {
    const std::string at = ":" + std::to_string(port);
    return equalsIgnoreCase(host, "127.0.0.1" + at) || equalsIgnoreCase(host, "localhost" + at);
}

bool originIsLocal(std::string_view origin) {
    std::string_view rest;
    if (origin.substr(0, 7) == "http://") rest = origin.substr(7);
    else if (origin.substr(0, 8) == "https://") rest = origin.substr(8);
    else return false;
    for (const std::string_view host : {std::string_view("127.0.0.1"), std::string_view("localhost"),
                                        std::string_view("[::1]")}) {
        if (equalsIgnoreCase(rest, host)) return true;
        if (rest.size() > host.size() + 1 && equalsIgnoreCase(rest.substr(0, host.size()), host) &&
            rest[host.size()] == ':' && allDigits(rest.substr(host.size() + 1))) {
            return true;
        }
    }
    return false;
}

bool sameSecret(std::string_view a, std::string_view b) {
    // The lengths are no secret; what is in them is.
    if (a.size() != b.size() || a.empty()) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]));
    }
    return diff == 0;
}

AuthVerdict authorize(const HttpRequest& request, uint16_t port, std::string_view token) {
    const std::string* host = request.header("host");
    if (!host || !hostIsLocal(*host, port)) return {.status = 403, .text = "Host is not this server"};
    if (const std::string* origin = request.header("origin"); origin && !originIsLocal(*origin)) {
        return {.status = 403, .text = "Origin is not local"};
    }
    const std::string* auth = request.header("authorization");
    if (!auth) return {.status = 401, .text = "bearer token wanted"};
    constexpr std::string_view kScheme = "Bearer ";
    const std::string_view value = *auth;
    if (value.size() <= kScheme.size() || !equalsIgnoreCase(value.substr(0, kScheme.size()), kScheme) ||
        !sameSecret(value.substr(kScheme.size()), token)) {
        return {.status = 401, .text = "bearer token not valid"};
    }
    return {};
}

}  // namespace wowee::mcp
