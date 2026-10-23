# mda::bus — single-writer broadcast ring (ADR-0007, docs/design/market-data-bus.md).

add_library(mda_bus STATIC
    src/bus.cpp
    src/error.cpp
    src/ring.cpp
    src/ring_reader.cpp
    src/ring_writer.cpp
    src/shared_memory.cpp)
add_library(mda::bus ALIAS mda_bus)

target_include_directories(mda_bus
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE src)
target_link_libraries(mda_bus PUBLIC mda::core)
mda_target_defaults(mda_bus)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
