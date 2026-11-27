# Writes core/version.hpp from `git describe`, so the version shown in the client
# is the last tag reachable from HEAD - or, in a CI branch build, the tag that
# build's release will get (below).
#
# Run as a script (cmake -P) from a build-time custom target, not just at configure
# time - otherwise tagging a release would not change the binary until someone
# happened to re-run cmake.
#
# Expects: SRC_DIR, IN_FILE, OUT_FILE

# With -P nothing else sets the policies (cmake/check_script_policies.cmake).
cmake_minimum_required(VERSION 3.15)

find_package(Git QUIET)

set(WOWEE_GIT_VERSION "unknown")
# A CI build of a branch push names itself what the release job will tag it
# (.github/workflows/build.yml: build-<run number>-<8 hex of the commit>). The
# tag is made after the builds, so `git describe` here found the previous
# push's: the IPA of commit 58f35462 reported build-508-315532be, another
# commit, and the update check offered each build its own release as newer. A
# tag build (release.yml, v*) keeps its tag; WOWEE_BUILD_VERSION overrides both.
set(_ci_version "")
if(DEFINED ENV{WOWEE_BUILD_VERSION} AND NOT "$ENV{WOWEE_BUILD_VERSION}" STREQUAL "")
    set(_ci_version "$ENV{WOWEE_BUILD_VERSION}")
elseif("$ENV{GITHUB_ACTIONS}" STREQUAL "true" AND NOT "$ENV{GITHUB_REF_TYPE}" STREQUAL "tag"
       AND DEFINED ENV{GITHUB_RUN_NUMBER} AND DEFINED ENV{GITHUB_SHA})
    string(SUBSTRING "$ENV{GITHUB_SHA}" 0 8 _ci_sha)
    set(_ci_version "build-$ENV{GITHUB_RUN_NUMBER}-${_ci_sha}")
endif()
if(_ci_version)
    set(WOWEE_GIT_VERSION "${_ci_version}")
elseif(GIT_FOUND)
    # The last tag reachable from HEAD - the released version this build descends from.
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
        WORKING_DIRECTORY ${SRC_DIR}
        OUTPUT_VARIABLE _git_tag
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _git_result
    )
    if(_git_result EQUAL 0 AND _git_tag)
        set(WOWEE_GIT_VERSION "${_git_tag}")
    else()
        # No tags (shallow clone, fresh fork): fall back to the short commit.
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${SRC_DIR}
            OUTPUT_VARIABLE _git_sha
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _sha_result
        )
        if(_sha_result EQUAL 0 AND _git_sha)
            set(WOWEE_GIT_VERSION "${_git_sha}")
        endif()
    endif()
endif()

# The commit itself, whatever the version: /ver names the running build by it
# (T369, the user's wish), as the deployed builds are named p-<commit>.
set(WOWEE_GIT_COMMIT "unknown")
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=9 HEAD
        WORKING_DIRECTORY ${SRC_DIR}
        OUTPUT_VARIABLE _git_commit
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _commit_result
    )
    if(_commit_result EQUAL 0 AND _git_commit)
        set(WOWEE_GIT_COMMIT "${_git_commit}")
    endif()
endif()

# Date only, no clock time: a timestamp would differ on every build and force a
# recompile of everything including this header.
string(TIMESTAMP WOWEE_BUILD_DATE "%Y-%m-%d" UTC)

configure_file(${IN_FILE} ${OUT_FILE}.tmp @ONLY)

# Only touch the real header when the version actually changed; rewriting it every
# build would rebuild every translation unit that includes it.
execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OUT_FILE}.tmp ${OUT_FILE}
)
file(REMOVE ${OUT_FILE}.tmp)
