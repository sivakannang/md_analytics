# mda::messages — normalized events carried on the buses (ADR-0010,
# docs/design/domain-model.md §3). Header-only: fixed-layout structs.

add_library(mda_messages INTERFACE)
add_library(mda::messages ALIAS mda_messages)

target_include_directories(mda_messages INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_messages INTERFACE mda::core)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
