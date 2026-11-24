#pragma once

// Who may speak to the server: a program on this machine that can read the
// token file, and no page in a browser.
//
// The socket is bound to 127.0.0.1, which keeps other machines out but not a
// web page: a browser on this machine sends its requests from this machine,
// and a name its owner points at 127.0.0.1 after the page has loaded (DNS
// rebinding) makes them same-origin ones. Such a request names its own host in
// Host and its page in Origin, so both are read before the token is.

#include <cstdint>
#include <string>
#include <string_view>

#include "mcp/http_message.hpp"

namespace wowee::mcp {

/// "127.0.0.1:PORT" or "localhost:PORT".
bool hostIsLocal(std::string_view host, uint16_t port);
/// http(s)://localhost, 127.0.0.1 or [::1], any port.
bool originIsLocal(std::string_view origin);
/// Takes the same time whatever the difference.
bool sameSecret(std::string_view a, std::string_view b);

struct AuthVerdict {
    int status = 0;  ///< 0 is let in
    const char* text = "";
};
/// Host, then Origin, then the bearer token.
AuthVerdict authorize(const HttpRequest& request, uint16_t port, std::string_view token);

}  // namespace wowee::mcp
