# mda::autotrade — the auto trader's strategies: runs, guards and the orders they want
# (ADR-0030, docs/design/auto-trader.md). Pure logic, no I/O.

add_library(mda_autotrade STATIC
    src/decision_store.cpp
    src/metrics.cpp
    src/options_run.cpp
    src/run.cpp)
add_library(mda::autotrade ALIAS mda_autotrade)

target_include_directories(mda_autotrade PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_autotrade PUBLIC mda::broker mda::signals mda::storage)
mda_target_defaults(mda_autotrade)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
