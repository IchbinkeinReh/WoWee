#pragma once

/// The handful of things a test that reads a file out of the tree needs.
///
/// Each of these was written out in two test files, and they are the shape
/// that drifts quietly: a test whose helper stops finding a file does not
/// fail, it reads an empty buffer and asserts nothing about it, and a green
/// run means the file was never opened rather than that its contents were
/// right. `dbcPathFor` is the one where that matters most - it matches the
/// table name case-insensitively because the extracted archives disagree
/// about capitalisation, so a copy that lost the fold would find nothing on
/// one machine and everything on another.
///
/// The tree's root is found as the test runs, never compiled in: a path built
/// into an object is the checkout it was first compiled in, and a compiler
/// cache hands that object to every other checkout, whose tests then read
/// (or fail to find) another tree's files. CTest gives each test the tree in
/// WOWEE_SOURCE_DIR (tests/CMakeLists.txt, at its end); a test run by hand
/// from inside the tree finds it by walking up to this file. WOWEE_SOURCE_DIR
/// is the macro the tests have always read; it names sourceDir() now.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace wowee::test {

/// The tree's root without a trailing slash: the environment's
/// WOWEE_SOURCE_DIR, else the first directory up from the working one that
/// holds tests/test_support.hpp, else empty (paths stay relative).
inline const std::string& sourceDir() {
    static const std::string dir = [] {
        if (const char* env = std::getenv("WOWEE_SOURCE_DIR"); env != nullptr && *env != '\0') {
            std::string d(env);
            while (d.size() > 1 && (d.back() == '/' || d.back() == '\\')) d.pop_back();
            return d;
        }
        std::error_code ec;
        std::filesystem::path at = std::filesystem::current_path(ec);
        while (!ec && !at.empty()) {
            if (std::filesystem::exists(at / "tests" / "test_support.hpp", ec)) return at.generic_string();
            const std::filesystem::path up = at.parent_path();
            if (up == at) break;
            at = up;
        }
        return std::string{};
    }();
    return dir;
}

/// The tree's root with a trailing slash, or empty when it was not found.
inline const std::string& sourceRoot() {
    static const std::string root = sourceDir().empty() ? std::string{} : sourceDir() + "/";
    return root;
}

/// A whole text file, or an empty string when it is not there.
inline std::string slurp(const std::string& relativePath) {
    std::ifstream in(sourceRoot() + relativePath);
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
}

/// A whole binary file, or an empty vector when it is not there.
inline std::vector<uint8_t> readFile(const std::string& relativePath) {
    std::ifstream in(sourceRoot() + relativePath, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                std::istreambuf_iterator<char>());
}

/// A little-endian uint32 at `offset`.
///
/// Fails the test on a short buffer rather than answering zero, which is what
/// both copies did: this reads a packet the test has just built, so a buffer
/// too short to hold the field means the builder is wrong, and a zero there
/// would be compared against an expected value and reported as the wrong
/// number rather than as a missing one.
///
/// Include this header *after* catch_amalgamated.hpp - REQUIRE comes from it.
inline uint32_t readU32(const std::vector<uint8_t>& bytes, size_t offset) {
    REQUIRE(bytes.size() >= offset + 4);
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

/// The .dbc for a table, found case-insensitively, or an empty path.
///
/// The extracted archives do not agree about capitalisation - Spell.dbc,
/// spell.dbc and SPELL.DBC all occur - so the name is folded on both sides.
/// A copy that lost the fold finds the file on one machine and not another,
/// and the test that uses it then passes by skipping.
inline std::filesystem::path dbcPathFor(const std::string& table) {
    std::string wanted = table;
    for (char& c : wanted) {
        if (c >= 'A' && c <= 'Z') c += 32;
    }
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(sourceRoot() + "Data/db", ec)) {
        if (entry.path().extension() != ".dbc") continue;
        std::string stem = entry.path().stem().string();
        for (char& c : stem) {
            if (c >= 'A' && c <= 'Z') c += 32;
        }
        if (stem == wanted) return entry.path();
    }
    return {};
}

}  // namespace wowee::test

#ifndef WOWEE_SOURCE_DIR
#define WOWEE_SOURCE_DIR (::wowee::test::sourceDir())
#endif
