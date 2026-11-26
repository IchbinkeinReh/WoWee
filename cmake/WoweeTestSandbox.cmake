# Tests never write into the player's own folders.
#
# A throwaway test once wrote its Lua error into the player's
# %APPDATA%\wowee\lua_errors.txt (and rewrote interface_state.cfg beside it),
# and every replay test that raises a Lua error did the same: the client finds
# its per-user folders through APPDATA, LOCALAPPDATA, HOME, the XDG variables
# and WOWEE_CONFIG_ROOT, and ctest passed the developer's own values through.
#
# Every test of a directory gets a corner of its own under
# <build>/test_sandbox/<test> for each of those variables - only those it does
# not already set itself (a few name their own config root in the build tree).
# A fixture empties the sandbox before a run and records the player's real
# folders; another compares them after the run and fails, naming the files,
# if anything in them changed. Catch2 executables started by hand get the same
# from their main (tests/catch_main.cpp).

set(WOWEE_TEST_SANDBOX_ROOT "${CMAKE_BINARY_DIR}/test_sandbox")

# The player's folders as this configure sees them: what the guard watches.
set(_wowee_real_roots "")
if(WIN32)
    if(DEFINED ENV{APPDATA})
        file(TO_CMAKE_PATH "$ENV{APPDATA}/wowee" _r)
        list(APPEND _wowee_real_roots "${_r}")
    endif()
    if(DEFINED ENV{LOCALAPPDATA})
        file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/Wowee" _r)
        list(APPEND _wowee_real_roots "${_r}")
    endif()
elseif(DEFINED ENV{HOME})
    list(APPEND _wowee_real_roots "$ENV{HOME}/.wowee" "$ENV{HOME}/.local/share/wowee"
         "$ENV{HOME}/.local/state/wowee" "$ENV{HOME}/Library/Application Support/wowee"
         "$ENV{HOME}/Library/Application Support/Wowee" "$ENV{HOME}/Library/Logs/wowee")
endif()
set(WOWEE_TEST_REAL_USER_ROOTS "${_wowee_real_roots}" CACHE INTERNAL
    "The player's per-user wowee folders, which no test may change")

# Python finds its own per-user packages through APPDATA/LOCALAPPDATA; the
# script tests keep reading them from where they are (read only).
set(_wowee_python_user_base "")
if(Python3_EXECUTABLE)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import site; print(site.getuserbase())"
                    OUTPUT_VARIABLE _wowee_python_user_base
                    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()

string(REPLACE ";" "|" _wowee_roots_bar "${WOWEE_TEST_REAL_USER_ROOTS}")
if(WOWEE_BUILD_TESTS AND NOT TEST wowee_user_paths_setup)
    add_test(NAME wowee_user_paths_setup
             COMMAND ${CMAKE_COMMAND} -DMODE=setup "-DSANDBOX=${WOWEE_TEST_SANDBOX_ROOT}"
                     "-DROOTS=${_wowee_roots_bar}"
                     -P ${CMAKE_SOURCE_DIR}/cmake/test_user_paths.cmake)
    add_test(NAME wowee_user_paths_untouched
             COMMAND ${CMAKE_COMMAND} -DMODE=check "-DSANDBOX=${WOWEE_TEST_SANDBOX_ROOT}"
                     "-DROOTS=${_wowee_roots_bar}"
                     -P ${CMAKE_SOURCE_DIR}/cmake/test_user_paths.cmake)
    set_tests_properties(wowee_user_paths_setup PROPERTIES FIXTURES_SETUP wowee_user_paths)
    set_tests_properties(wowee_user_paths_untouched PROPERTIES FIXTURES_CLEANUP wowee_user_paths)
endif()

# Give every test of the calling directory its own per-user folders.
function(wowee_sandbox_directory_tests)
    get_property(_tests DIRECTORY PROPERTY TESTS)
    foreach(_t IN LISTS _tests)
        if(_t STREQUAL "wowee_user_paths_setup" OR _t STREQUAL "wowee_user_paths_untouched")
            continue()
        endif()
        set(_box "${WOWEE_TEST_SANDBOX_ROOT}/${_t}")
        get_test_property(${_t} ENVIRONMENT _env)
        if(NOT _env)
            set(_env "")
        endif()
        set(_add
            "APPDATA=${_box}/AppData/Roaming"
            "LOCALAPPDATA=${_box}/AppData/Local"
            "HOME=${_box}/home"
            "XDG_CONFIG_HOME=${_box}/home/.config"
            "XDG_DATA_HOME=${_box}/home/.local/share"
            "XDG_STATE_HOME=${_box}/home/.local/state"
            "XDG_CACHE_HOME=${_box}/home/.cache"
            "WOWEE_CONFIG_ROOT=${_box}/config"
            "WOWEE_TEST_SANDBOX=${_box}")
        if(WOWEE_TEST_REAL_USER_ROOTS)
            string(REPLACE ";" "|" _roots "${WOWEE_TEST_REAL_USER_ROOTS}")
            list(APPEND _add "WOWEE_TEST_REAL_USER_ROOTS=${_roots}")
        endif()
        if(_wowee_python_user_base)
            list(APPEND _add "PYTHONUSERBASE=${_wowee_python_user_base}")
        endif()
        foreach(_kv IN LISTS _add)
            string(REGEX MATCH "^[^=]+" _name "${_kv}")
            set(_have FALSE)
            foreach(_e IN LISTS _env)
                if(_e MATCHES "^${_name}=")
                    set(_have TRUE)
                endif()
            endforeach()
            if(NOT _have)
                set_property(TEST ${_t} APPEND PROPERTY ENVIRONMENT "${_kv}")
            endif()
        endforeach()
        set_property(TEST ${_t} APPEND PROPERTY FIXTURES_REQUIRED wowee_user_paths)
        # The setup script makes these, so a test that only reads finds them.
        set_property(GLOBAL APPEND PROPERTY WOWEE_SANDBOXED_TESTS ${_t})
    endforeach()
    get_property(_all GLOBAL PROPERTY WOWEE_SANDBOXED_TESTS)
    string(REPLACE ";" "\n" _lines "${_all}")
    file(WRITE "${WOWEE_TEST_SANDBOX_ROOT}.tests.txt" "${_lines}\n")
endfunction()
