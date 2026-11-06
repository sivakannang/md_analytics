# mda::oms — paper trading core: orders, positions, the account ledger and Indian charges
# (ADR-0029, docs/design/paper-trading.md). Pure logic, no I/O.

add_library(mda_oms STATIC
    src/account.cpp
    src/charges.cpp
    src/order.cpp
    src/paper_venue.cpp
    src/position.cpp
    src/types.cpp)
add_library(mda::oms ALIAS mda_oms)

target_include_directories(mda_oms PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_oms PUBLIC mda::core)
mda_target_defaults(mda_oms)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
