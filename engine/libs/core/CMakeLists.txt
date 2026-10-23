# mda::core — foundational vocabulary types and utilities shared by every module.
# Header-only for now; becomes a compiled library once it has .cpp files.

add_library(mda_core INTERFACE)
add_library(mda::core ALIAS mda_core)

target_include_directories(mda_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
