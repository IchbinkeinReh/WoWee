// How the catalog searches reach a file's contents.
//
// They do not parse .w* files. They re-invoke this executable with the
// format's --info flag and read the JSON back, so the one exporter that
// already knows each format is the only thing that reads it. Four commands
// carried the same four steps to do that, copied byte for byte in three of
// them and reformatted in the fourth.
//
// Every failure on that path is a file silently skipped, which is correct for
// an asset format and invisible when it is caused by something else: seven
// rows of the format table named an --info flag no handler answers, and the
// item catalog was unsearchable by all four commands without a word of
// complaint.
//
// The oracle for the quoting is POSIX sh itself, which the test runs.
#include <catch_amalgamated.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "cli_catalog_subprocess.hpp"

using wowee::editor::cli::runAndCapture;
using wowee::editor::cli::shellQuote;

namespace {

#ifdef _WIN32
// cmd.exe has no printf, and its echo prints an argument as it was written,
// quotes and all. So this is what reaches the child once cmd has parsed the
// line - nothing split off at a & or |, nothing redirected - with the quotes
// the child's own argv parsing removes taken off here.
std::string throughTheShell(const std::string& argument) {
    int rc = -1;
    std::string out = runAndCapture("echo " + shellQuote(argument), rc);
    CHECK(rc == 0);
    if (!out.empty() && out.back() == '\n') out.pop_back();
    if (out.size() >= 2 && out.front() == '"' && out.back() == '"') {
        out = out.substr(1, out.size() - 2);
    }
    return out;
}
#else
// What a POSIX shell actually passes to a child for this argument. The point
// of shellQuote is that this is the identity function.
std::string throughTheShell(const std::string& argument) {
    int rc = -1;
    std::string out =
        runAndCapture("printf %s " + shellQuote(argument), rc);
    CHECK(rc == 0);
    return out;
}
#endif

std::filesystem::path scratchDir() {
    const auto dir = std::filesystem::temp_directory_path() /
                     "wowee_catalog_subprocess_test";
    std::filesystem::create_directories(dir);
    return dir;
}

}  // namespace

TEST_CASE("a quoted argument reaches the child unchanged", "[catalog]") {
    // Data paths are named by whoever made the zone.
    CHECK(throughTheShell("plain") == "plain");
    CHECK(throughTheShell("with space") == "with space");
    CHECK(throughTheShell("Bob's zone") == "Bob's zone");
#ifndef _WIN32
    // echo shows the escaped form; the argv rules below cover it on Windows.
    CHECK(throughTheShell("quote\"and'both") == "quote\"and'both");
#endif
    CHECK(throughTheShell("./Data/terrain/azeroth_32_48.whm") ==
          "./Data/terrain/azeroth_32_48.whm");
}

#ifdef _WIN32
TEST_CASE("a quote inside follows the child's argv rules", "[catalog]") {
    // CommandLineToArgvW: a quote is escaped by a backslash, and only
    // backslashes that run up to a quote are doubled.
    CHECK(shellQuote("a b") == "\"a b\"");
    CHECK(shellQuote("a\"b") == "\"a\\\"b\"");
    CHECK(shellQuote("C:\\dir\\file") == "\"C:\\dir\\file\"");
    CHECK(shellQuote("C:\\dir\\") == "\"C:\\dir\\\\\"");
    CHECK(shellQuote("a\\\"b") == "\"a\\\\\\\"b\"");
}
#endif

TEST_CASE("shell metacharacters are data, not syntax", "[catalog]") {
    // The command is handed to popen, so an unquoted path containing any of
    // these would run as a command. Nothing here is exotic in a filename.
    CHECK(throughTheShell("a;b") == "a;b");
    CHECK(throughTheShell("$(echo no)") == "$(echo no)");
    CHECK(throughTheShell("`echo no`") == "`echo no`");
    CHECK(throughTheShell("a|b") == "a|b");
    CHECK(throughTheShell("a&b") == "a&b");
    CHECK(throughTheShell("*") == "*");
    CHECK(throughTheShell("~") == "~");
#ifndef _WIN32
    // Not a character a Windows file name can hold.
    CHECK(throughTheShell("a\nb") == "a\nb");
#endif
}

TEST_CASE("a child that fails is reported as failing", "[catalog]") {
    // Every caller treats a nonzero status as "skip this file", so a command
    // that cannot run must not look like one that produced nothing to search.
    int rc = -1;
    CHECK(runAndCapture("exit 3", rc).empty());
    CHECK(rc == 3);

    rc = -1;
#ifdef _WIN32
    const std::string text = runAndCapture("echo a& echo b", rc);
#else
    const std::string text = runAndCapture("printf 'a\\nb\\n'", rc);
#endif
    CHECK(rc == 0);
    CHECK(text == "a\nb\n");
}

TEST_CASE("output longer than one read is captured whole", "[catalog]") {
    // A catalog's JSON runs to hundreds of kilobytes and the capture buffer is
    // four. Truncated output parses as invalid JSON and the file is skipped,
    // which reads as an empty catalog rather than an error.
    int rc = -1;
#ifdef _WIN32
    // 2000 lines of nine x's and a newline.
    const std::string text =
        runAndCapture("for /L %i in (1,1,2000) do @echo xxxxxxxxx", rc);
#else
    const std::string text =
        runAndCapture("printf 'x%.0s' $(seq 1 20000)", rc);
#endif
    CHECK(rc == 0);
    CHECK(text.size() == 20000);
}
