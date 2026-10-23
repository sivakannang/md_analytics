# mda::quotestore — last-known quotes per instrument in data/last_quotes.db (ADR-0024).

add_library(mda_quotestore STATIC
    src/last_quote_store.cpp)
add_library(mda::quotestore ALIAS mda_quotestore)

target_include_directories(mda_quotestore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_quotestore PUBLIC mda::messages mda::storage)
mda_target_defaults(mda_quotestore)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
