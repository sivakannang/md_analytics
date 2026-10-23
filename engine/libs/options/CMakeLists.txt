# mda::options — option pricing (Black-76, IV, Greeks) and option-chain analytics (ADR-0022).

add_library(mda_options STATIC
    src/black76.cpp
    src/chain_analytics.cpp)
add_library(mda::options ALIAS mda_options)

target_include_directories(mda_options PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_options PUBLIC mda::core mda::messages)
mda_target_defaults(mda_options)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
