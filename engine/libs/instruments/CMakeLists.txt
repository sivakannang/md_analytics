# mda::instruments — ContractKey, the permanent instrument registry (ADR-0008,
# docs/design/instruments.md). Broker-independent.

add_library(mda_instruments STATIC
    src/contract_key.cpp
    src/registry.cpp)
add_library(mda::instruments ALIAS mda_instruments)

target_include_directories(mda_instruments PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_instruments PUBLIC mda::core)
mda_target_defaults(mda_instruments)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
