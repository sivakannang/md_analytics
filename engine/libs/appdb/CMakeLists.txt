# mda::appdb — the app database schema (all migrations) and the notification outbox.

add_library(mda_appdb STATIC
    src/outbox.cpp
    src/schema.cpp)
add_library(mda::appdb ALIAS mda_appdb)

target_include_directories(mda_appdb PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_appdb PUBLIC mda::storage)
mda_target_defaults(mda_appdb)
