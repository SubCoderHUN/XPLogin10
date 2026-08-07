cmake_minimum_required(VERSION 3.16)
project(XPLogin10
        VERSION 1.0.0
        DESCRIPTION "Windows XP LogonUI replacement for Windows 10/11"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(XPLOGIN_BUILD_TESTS     "Build the automated test suite"            ON)
option(XPLOGIN_BUILD_WIN32     "Build the Windows-only components"         ${WIN32})
option(XPLOGIN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"      OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
    if(XPLOGIN_WARNINGS_AS_ERRORS)
        add_compile_options(/WX)
    endif()
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

    # Static CRT, for three separate reasons - none of them a preference:
    #
    #  * XPLoginProvider.dll is loaded by LogonUI.exe on the secure Winlogon
    #    desktop. If MSVCP140.dll is absent there, or is a different build from
    #    the one this was compiled against, LogonUI declines to load the
    #    provider and reports nothing at all. The logon screen is simply the
    #    Windows one, with no clue why.
    #  * XPLogin10-Setup.exe is a bootstrapper. An installer that needs a
    #    redistributable installed before it will run is not one, and "out of
    #    the box, no dependencies" is the whole promise.
    #  * XPLoginWatchdog.exe starts in session 0 early in boot, where less is
    #    guaranteed to be present than most code assumes.
    #
    # Safe here because nothing crosses a module boundary that belongs to a
    # CRT: XPLoginProvider.dll links xplogin_core statically and talks to
    # Windows through COM, whose strings and buffers are CoTaskMemAlloc'd.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(XPLOGIN_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
endif()

# Multi-config generators (Visual Studio) vs single-config (Ninja/Make)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_subdirectory(src/core)
add_subdirectory(src/ui)
add_subdirectory(src/assets)

# Portable tooling. Neither of these is shipped; both exist because the XP
# screen is recovered from Microsoft's own binaries rather than guessed at, and
# without them that claim could not be checked or rebuilt.
add_subdirectory(tools/xp-extract)
add_subdirectory(tools/xp-bake)

# Turns a developer's own logonui.exe into the runtime artwork pack. Must come
# after xp-bake, which it runs.
include(cmake/BakeAssets.cmake)
include(cmake/BakeSounds.cmake)

if(XPLOGIN_BUILD_WIN32)
    if(NOT WIN32)
        message(FATAL_ERROR "XPLOGIN_BUILD_WIN32=ON requires a Windows toolchain.")
    endif()
    add_subdirectory(src/provider)
    add_subdirectory(src/service)
    add_subdirectory(src/installer)
    # Last: the setup embeds the outputs of everything above.
    add_subdirectory(src/setup)
endif()

if(XPLOGIN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "")
message(STATUS "XPLogin10 ${PROJECT_VERSION}")
message(STATUS "  Win32 components : ${XPLOGIN_BUILD_WIN32}")
message(STATUS "  Tests            : ${XPLOGIN_BUILD_TESTS}")
message(STATUS "  Compiler         : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Baked artwork    : ${XPLOGIN_HAVE_ASSETS}")
message(STATUS "")
