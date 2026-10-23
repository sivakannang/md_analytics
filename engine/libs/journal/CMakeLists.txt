# mda::journal — the tick journal's files: hourly raw-frame records (ADR-0026).

add_library(mda_journal STATIC
    src/journal.cpp)
add_library(mda::journal ALIAS mda_journal)

target_include_directories(mda_journal PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
mda_target_defaults(mda_journal)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
