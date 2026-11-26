// Where the tree is, as a test finds it: told by CTest as the test runs, never
// compiled in. A path built into an object is the checkout it was first
// compiled in, and a compiler cache hands that object to every other checkout:
// fourteen tests once read the files of a worktree that was gone.
#include <catch_amalgamated.hpp>
#include "test_support.hpp"

#include <cstdlib>
#include <filesystem>
#include <string>

TEST_CASE("the tree is the one CTest names, found as the test runs", "[test_support]") {
    const char* told = std::getenv("WOWEE_SOURCE_DIR");
    REQUIRE(told != nullptr);
    std::string want(told);
    while (want.size() > 1 && (want.back() == '/' || want.back() == '\\')) want.pop_back();
    CHECK(wowee::test::sourceDir() == want);
    CHECK(std::string(WOWEE_SOURCE_DIR) == want);
    CHECK(wowee::test::sourceRoot() == want + "/");
    CHECK(std::filesystem::exists(std::filesystem::path(want) / "tests" / "test_support.hpp"));
}

TEST_CASE("no test is given the tree when it is compiled", "[test_support]") {
    const std::string cmake = wowee::test::slurp("tests/CMakeLists.txt");
    REQUIRE_FALSE(cmake.empty());
    CHECK(cmake.find("WOWEE_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"") == std::string::npos);
    CHECK(cmake.find("PROPERTY ENVIRONMENT \"WOWEE_SOURCE_DIR=${CMAKE_SOURCE_DIR}\"") != std::string::npos);
}
