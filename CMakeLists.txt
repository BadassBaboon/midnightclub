
# midnightclub - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(midnightclub LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(MIDNIGHTCLUB_SOURCES
    src/main.cpp
    src/midnightclub_hooks.cpp
)

if(WIN32)
    add_executable(midnightclub WIN32 ${MIDNIGHTCLUB_SOURCES})
else()
    add_executable(midnightclub ${MIDNIGHTCLUB_SOURCES})
endif()

rexglue_setup_target(midnightclub GPU_PLUGINS xenos)

# Ship the SDL controller mapping database next to the executable.
#
# SDL looks for 'gamecontrollerdb.txt' in the working directory and logs
# "GameControllerDB: file 'gamecontrollerdb.txt' does not exist." on every boot
# without it. The file is zlib-licensed and freely redistributable
# (https://github.com/mdqinc/SDL_GameControllerDB), so vendoring it is fine -
# unlike the CodeX strings dictionary, which has no LICENSE and is therefore
# fetched by the user instead.
#
# Copied post-build rather than configure-time so it tracks edits to the source
# file without needing a reconfigure.
add_custom_command(TARGET midnightclub POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/assets/gamecontrollerdb.txt"
            "$<TARGET_FILE_DIR:midnightclub>/gamecontrollerdb.txt"
    COMMENT "Copying gamecontrollerdb.txt next to midnightclub.exe")
