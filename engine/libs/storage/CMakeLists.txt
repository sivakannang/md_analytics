# mda::storage — SQLite app database wrapper (ADR-0012).

find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_library(mda_storage STATIC
    src/database.cpp)
add_library(mda::storage ALIAS mda_storage)

target_include_directories(mda_storage PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_storage PRIVATE unofficial::sqlite3::sqlite3)
mda_target_defaults(mda_storage)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
