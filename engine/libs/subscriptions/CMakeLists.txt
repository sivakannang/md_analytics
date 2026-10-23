# mda::subscriptions — what mda-feedd subscribes: option-chain windows around ATM (ADR-0020).

add_library(mda_subscriptions STATIC
    src/chain_windows.cpp)
add_library(mda::subscriptions ALIAS mda_subscriptions)

target_include_directories(mda_subscriptions PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_subscriptions PUBLIC mda::core mda::instruments)
mda_target_defaults(mda_subscriptions)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
