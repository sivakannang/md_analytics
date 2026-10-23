# mda::admin — queries behind the admin dashboard and mda-useradm (ADR-0019).

add_library(mda_admin STATIC
    src/admin.cpp)
add_library(mda::admin ALIAS mda_admin)

target_include_directories(mda_admin PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_admin PUBLIC mda::storage)
mda_target_defaults(mda_admin)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
