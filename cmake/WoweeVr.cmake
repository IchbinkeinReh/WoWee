# ---- wowee_vr: the client in a headset, through OpenXR ----
#
# A second executable beside wowee, not a mode of it. Everything the two share
# is compiled once: wowee_vr links wowee's own object files, all but the four
# that carry the headset's hooks (the frame loop, the renderer's last pass, the
# Vulkan context and the loading screen's own loop), which it compiles again
# with WOWEE_VR defined, plus the sources under src/vr. So wowee itself is
# built exactly as it was - the hooks are not in it at all, not even as a
# branch.
#
# No header may test WOWEE_VR. The shared objects were compiled without it,
# and a class laid out one way in them and another in the recompiled
# files would be two classes under one name.
#
# OpenXR's loader is built from Khronos' own SDK at a pinned release, linked
# statically: a headset's runtime (Meta Quest Link, SteamVR) is found through
# it at run time, and nothing has to be installed beside the exe.

if(WIN32 AND NOT ANDROID)
    set(_wowee_vr_default ON)
else()
    set(_wowee_vr_default OFF)
endif()
option(WOWEE_BUILD_VR "Build wowee_vr, the OpenXR client" ${_wowee_vr_default})

if(NOT WOWEE_BUILD_VR OR NOT TARGET wowee)
    return()
endif()

include(FetchContent)
# Only the loader: no layers, no tests, no conformance suite, nothing installed.
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr_sdk
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG        release-1.1.63
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(openxr_sdk)

# The four files with WOWEE_VR blocks in them, as wowee compiled them and as
# wowee_vr compiles them.
set(WOWEE_VR_HOOKED_SOURCES
    src/core/application.cpp
    src/rendering/renderer.cpp
    src/rendering/vk_context.cpp
    src/rendering/loading_screen.cpp
)
set(WOWEE_VR_SOURCES
    src/vr/vr_math.cpp
    src/vr/vr_runtime.cpp
)

add_executable(wowee_vr
    ${WOWEE_VR_HOOKED_SOURCES}
    ${WOWEE_VR_SOURCES}
    # wowee's objects, less those four.
    "$<FILTER:$<TARGET_OBJECTS:wowee>,EXCLUDE,/src/(core/application|rendering/renderer|rendering/vk_context|rendering/loading_screen)\\.cpp\\.(o|obj)$>"
)

foreach(_prop INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES
              LINK_DIRECTORIES LINK_OPTIONS)
    get_target_property(_val wowee ${_prop})
    if(_val)
        set_property(TARGET wowee_vr PROPERTY ${_prop} "${_val}")
    endif()
endforeach()
target_compile_definitions(wowee_vr PRIVATE WOWEE_VR)
target_include_directories(wowee_vr SYSTEM PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/extern
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/vk-bootstrap/src)
target_link_libraries(wowee_vr PRIVATE openxr_loader OpenXR::headers)
target_precompile_headers(wowee_vr REUSE_FROM wowee)
# The objects, the shaders and the generated headers are wowee's.
add_dependencies(wowee_vr wowee)
if(WIN32)
    # A console is no use to someone in a headset; the log and a message box
    # say what went wrong. Same subsystem as wowee, whatever that is.
    get_target_property(_wowee_win32 wowee WIN32_EXECUTABLE)
    if(_wowee_win32)
        set_target_properties(wowee_vr PROPERTIES WIN32_EXECUTABLE ON)
    endif()
endif()
message(STATUS "  wowee_vr (OpenXR ${openxr_sdk_SOURCE_DIR}): ENABLED")
