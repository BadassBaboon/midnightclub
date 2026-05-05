
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
)

if(WIN32)
    add_executable(midnightclub WIN32 ${MIDNIGHTCLUB_SOURCES})
else()
    add_executable(midnightclub ${MIDNIGHTCLUB_SOURCES})
endif()

rexglue_setup_target(midnightclub)
