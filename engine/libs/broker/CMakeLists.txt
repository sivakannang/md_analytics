# mda::broker — the paper broker: order and execution messages, paper.db and the Broker
# state machine run by mda-broker (ADR-0029, docs/design/paper-trading.md).

add_library(mda_broker STATIC
    src/broker.cpp
    src/instrument_catalog.cpp
    src/paper_store.cpp)
add_library(mda::broker ALIAS mda_broker)

target_include_directories(mda_broker PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_broker PUBLIC mda::instruments mda::oms mda::rms mda::messages mda::storage)
mda_target_defaults(mda_broker)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
