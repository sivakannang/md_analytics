cmake_minimum_required(VERSION 3.28)

project(md_analytics
    VERSION 0.1.0
    DESCRIPTION "Real-time market-data and derivatives analytics engine"
    LANGUAGES CXX)

# Single source of truth for toolchain-level versions (C++ standard, minimum CMake,
# project version). Library versions live only in vcpkg.json; CI compilers only in ci.yml.

# C++20 baseline, compiled as C++23 so pre-approved C++23 features are available (ADR-0002).
# Applies to every target in the tree; modules must not set their own standard.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++20 modules: skip the clang-scan-deps step CMake otherwise adds for Clang.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(MDA_BUILD_TESTS "Build unit tests" ON)
option(MDA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (on in CI)" OFF)
set(MDA_SANITIZER "" CACHE STRING
    "Sanitizers to enable: empty, 'address,undefined' or 'thread'")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(MdaDefaults)

if(MDA_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)
endif()

add_subdirectory(engine)
