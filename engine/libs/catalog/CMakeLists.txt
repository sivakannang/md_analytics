# mda::catalog — instrument catalog in the app DB: daily sync and search.

add_library(mda_catalog STATIC
    src/catalog.cpp)
add_library(mda::catalog ALIAS mda_catalog)

target_include_directories(mda_catalog PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_catalog PUBLIC mda::storage mda::instruments PRIVATE mda::appdb)
mda_target_defaults(mda_catalog)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
