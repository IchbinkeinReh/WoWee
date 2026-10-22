// The interface's files, read ahead into memory (addons/interface_prefetch.hpp).
#include <catch_amalgamated.hpp>

#include "addons/interface_prefetch.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace prefetch = wowee::addons::interface_prefetch;
namespace fs = std::filesystem;

TEST_CASE("a file is found however its path is spelt", "[interface_prefetch]") {
    // FrameXML's paths are a directory, a manifest's line and an include's
    // attribute put together, with both slashes and any case.
    CHECK(prefetch::key(R"(G:\wowee-data/expansions\wotlk/interface/FrameXML\GlobalStrings.lua)") ==
          prefetch::key("g:/wowee-data/expansions/wotlk/Interface/framexml/globalstrings.lua"));
    CHECK(prefetch::key("a/b/../c/./d.xml") == prefetch::key("A/C/D.XML"));
    CHECK(prefetch::key("a/b.xml") != prefetch::key("a/c.xml"));
}

TEST_CASE("what was read ahead is handed out, and nothing else", "[interface_prefetch]") {
    const fs::path root = fs::temp_directory_path() / "wowee_prefetch_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "Sub", ec);
    { std::ofstream(root / "Sub" / "Frame.xml") << "<Ui/>"; }
    { std::ofstream(root / "Code.lua") << "x = 1"; }
    { std::ofstream(root / "Picture.blp") << "not text"; }

    prefetch::start({root.string()});
    prefetch::wait();

    std::string out;
    REQUIRE(prefetch::take((root / "sub" / "frame.xml").string(), out));
    CHECK(out == "<Ui/>");
    REQUIRE(prefetch::take((root / "Code.lua").string(), out));
    CHECK(out == "x = 1");
    CHECK_FALSE(prefetch::take((root / "Picture.blp").string(), out));
    CHECK_FALSE(prefetch::take((root / "Missing.lua").string(), out));
    fs::remove_all(root, ec);
}
