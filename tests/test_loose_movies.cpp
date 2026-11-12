// The extractor copies the client's loose movies (Data/<locale>/Interface/
// Cinematics/*.avi, not in any archive) into the extracted tree.

#include <catch_amalgamated.hpp>

#include "loose_movies.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

TEST_CASE("loose movies beside the locale archives are copied with manifest entries") {
    const fs::path root = fs::temp_directory_path() /
        ("wowee_loose_movies_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path cine = root / "client" / "Data" / "deDE" / "Interface" / "Cinematics";
    fs::create_directories(cine);
    std::ofstream(cine / "WOW_Wrathgate_1024.avi", std::ios::binary) << "RIFFxxxxAVI movie bytes";
    std::ofstream(cine / "readme.txt", std::ios::binary) << "not a movie";
    const fs::path out = root / "out";

    // Given the client folder (the Data folder is found inside it), no locale.
    const auto entries = wowee::tools::copyLooseMovies((root / "client").string(), "", out.string());
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].wowPath == "interface\\cinematics\\wow_wrathgate_1024.avi");
    CHECK(entries[0].size == 23);
    const fs::path copied = out / entries[0].filesystemPath;
    REQUIRE(fs::exists(copied));
    std::ifstream in(copied, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(bytes == "RIFFxxxxAVI movie bytes");

    // Given the Data folder itself, with the locale named.
    CHECK(wowee::tools::copyLooseMovies((root / "client" / "Data").string(), "deDE", out.string()).size() == 1);
    // No locale folder: nothing.
    CHECK(wowee::tools::copyLooseMovies((root / "out").string(), "", out.string()).empty());
    std::error_code ec;
    fs::remove_all(root, ec);
}
