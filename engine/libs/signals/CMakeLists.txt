# mda::signals — market state, factors and direction strategies run by mda-signald
# (ADR-0028, docs/design/signals.md).

add_library(mda_signals STATIC
    src/factors.cpp
    src/market_signal.cpp
    src/market_state.cpp
    src/oi_bias.cpp
    src/outcome.cpp
    src/signal_store.cpp
    src/strategies.cpp
    src/strategy.cpp)
add_library(mda::signals ALIAS mda_signals)

target_include_directories(mda_signals PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_signals PUBLIC mda::core mda::messages mda::bars mda::options mda::storage)
mda_target_defaults(mda_signals)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
