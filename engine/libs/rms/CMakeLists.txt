# mda::rms — order validation and pre-trade risk for paper trading (ADR-0029,
# docs/design/paper-trading.md §4, §8, §9). Pure logic, no I/O.

add_library(mda_rms STATIC
    src/risk.cpp)
add_library(mda::rms ALIAS mda_rms)

target_include_directories(mda_rms PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_rms PUBLIC mda::oms)
mda_target_defaults(mda_rms)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
