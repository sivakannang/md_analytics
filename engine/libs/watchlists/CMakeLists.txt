# mda::watchlists — per-user watchlists in the app DB.

add_library(mda_watchlists STATIC
    src/watchlists.cpp)
add_library(mda::watchlists ALIAS mda_watchlists)

target_include_directories(mda_watchlists PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_watchlists PUBLIC mda::catalog mda::storage)
mda_target_defaults(mda_watchlists)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
