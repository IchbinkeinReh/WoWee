#include "mcp/token_file.hpp"

#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#include <vector>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace wowee::mcp {

std::string makeToken() {
    std::array<unsigned char, 32> bytes{};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) return {};
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const unsigned char b : bytes) {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x", b);
        out += hex;
    }
    return out;
}

std::filesystem::path tokenFilePath(const std::string& configRoot) {
    return std::filesystem::path(configRoot) / "mcp_server.json";
}

namespace {

std::string fileText(const TokenFile& file) {
    const nlohmann::json j = {{"url", "http://127.0.0.1:" + std::to_string(file.port) + "/mcp"},
                              {"port", file.port},
                              {"token", file.token},
                              {"pid", file.pid}};
    return j.dump(2) + "\n";
}

#ifdef _WIN32

/// "D:P(A;;FA;;;SY)(A;;FA;;;<this user>)": the system and this user, and
/// nothing inherited from the folder. Empty when this user cannot be named.
std::wstring onlyThisUser() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> buffer(size);
    std::wstring out;
    if (size != 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        LPWSTR sid = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) {
            out = std::wstring(L"D:P(A;;FA;;;SY)(A;;FA;;;") + sid + L")";
            LocalFree(sid);
        }
    }
    CloseHandle(token);
    return out;
}

bool writePrivately(const std::filesystem::path& path, const std::string& text) {
    const std::wstring sddl = onlyThisUser();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    if (sddl.empty() || !ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                                              &descriptor, nullptr)) {
        return false;
    }
    attributes.lpSecurityDescriptor = descriptor;
    // Made new, so that the access list is this one and not what an older
    // file of the name had.
    DeleteFileW(path.c_str());
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(descriptor);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0 &&
                    written == text.size();
    CloseHandle(file);
    return ok;
}

#else

bool writePrivately(const std::filesystem::path& path, const std::string& text) {
    ::unlink(path.c_str());
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (fd < 0) return false;
    size_t at = 0;
    while (at < text.size()) {
        const ssize_t n = ::write(fd, text.data() + at, text.size() - at);
        if (n <= 0) break;
        at += static_cast<size_t>(n);
    }
    ::close(fd);
    return at == text.size();
}

#endif

}  // namespace

bool writeTokenFile(const std::filesystem::path& path, const TokenFile& file) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return writePrivately(path, fileText(file));
}

void removeTokenFile(const std::filesystem::path& path, const std::string& token) {
    std::string text;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return;
        std::ostringstream all;
        all << in.rdbuf();
        text = all.str();
    }
    if (token.empty() || text.find(token) == std::string::npos) return;
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

}  // namespace wowee::mcp
