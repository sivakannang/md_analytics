# mda::bars — 1-minute bars: builder (from snapshots), the bars.db store (ADR-0021) and
# the OI buildup state (docs/design/oi-buildup-chart.md).

add_library(mda_bars STATIC
    src/aggregate.cpp
    src/bar_builder.cpp
    src/buildup.cpp
    src/day_snapshot.cpp
    src/bar_store.cpp)
add_library(mda::bars ALIAS mda_bars)

target_include_directories(mda_bars PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_bars PUBLIC mda::core mda::messages mda::storage)
mda_target_defaults(mda_bars)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
