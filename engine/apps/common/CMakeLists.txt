# mda::appcommon — code shared by the Kite-facing processes and tools.

find_package(spdlog CONFIG REQUIRED)

add_library(mda_appcommon STATIC
    src/backfill.cpp
    src/daily_instruments.cpp)
add_library(mda::appcommon ALIAS mda_appcommon)

target_include_directories(mda_appcommon PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_appcommon PUBLIC mda::bars mda::instruments mda::kite mda::runtime PRIVATE spdlog::spdlog)
mda_target_defaults(mda_appcommon)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
