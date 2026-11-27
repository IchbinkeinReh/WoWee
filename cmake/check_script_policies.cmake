# ctest cmake_scripts_set_policies: every script the build or the tests run
# with `cmake -P` sets its own policies first.
#
# A script run with -P has no project above it, so nothing sets the policies:
# CMake 3.x leaves them unset and an `IN_LIST` there is "Unknown arguments"
# (the Linux CI, 3.28, at 40c22780b), while CMake 4 only has the new
# behaviour and runs the same script without a word - so a host with CMake 4
# cannot see it by running the script. This reads them instead: each script
# named after -P in the build's CMake files must open with
# cmake_minimum_required.
#
# Invoked with -DSRC=<source dir>.
cmake_minimum_required(VERSION 3.15)

if(NOT IS_DIRECTORY "${SRC}")
    message(FATAL_ERROR "SRC must name the source tree")
endif()

set(_lists "${SRC}/CMakeLists.txt" "${SRC}/tests/CMakeLists.txt")
file(GLOB _modules "${SRC}/cmake/*.cmake")
list(APPEND _lists ${_modules})

set(_scripts "")
foreach(_f IN LISTS _lists)
    if(NOT EXISTS "${_f}")
        continue()
    endif()
    file(STRINGS "${_f}" _lines REGEX "-P[ \t]+[^ \t)\"]")
    foreach(_l IN LISTS _lines)
        string(REGEX MATCHALL "-P[ \t]+[^ \t)\"]+" _found "${_l}")
        foreach(_m IN LISTS _found)
            string(REGEX REPLACE "^-P[ \t]+" "" _p "${_m}")
            string(REPLACE "\${CMAKE_SOURCE_DIR}" "${SRC}" _p "${_p}")
            string(REPLACE "\${CMAKE_CURRENT_SOURCE_DIR}" "${SRC}" _p "${_p}")
            if(_p MATCHES "[.]cmake$")
                list(APPEND _scripts "${_p}")
            endif()
        endforeach()
    endforeach()
endforeach()
list(REMOVE_DUPLICATES _scripts)

# This one is among them once it is registered; fewer than that means the
# reading above has stopped finding the calls.
list(LENGTH _scripts _count)
if(_count LESS 4)
    message(FATAL_ERROR "found only ${_count} cmake -P scripts (${_scripts}) - the search is broken")
endif()

set(_bad "")
foreach(_s IN LISTS _scripts)
    if(NOT EXISTS "${_s}")
        list(APPEND _bad "  ${_s}: run with -P, and not there")
        continue()
    endif()
    file(STRINGS "${_s}" _body)
    set(_first "")
    foreach(_l IN LISTS _body)
        string(STRIP "${_l}" _t)
        if(_t STREQUAL "" OR _t MATCHES "^#")
            continue()
        endif()
        set(_first "${_t}")
        break()
    endforeach()
    if(NOT _first MATCHES "^cmake_minimum_required[ \t]*[(]")
        list(APPEND _bad "  ${_s}: first command is '${_first}'")
    endif()
endforeach()

if(_bad)
    string(REPLACE ";" "\n" _text "${_bad}")
    message(FATAL_ERROR
        "cmake -P scripts that do not set their policies first (cmake_minimum_required):\n${_text}")
endif()
message(STATUS "${_count} cmake -P scripts set their policies")
