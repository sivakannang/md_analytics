add_executable(mda-busctl mda-busctl/main.cpp)
target_link_libraries(mda-busctl PRIVATE mda::bus)
mda_target_defaults(mda-busctl)

add_executable(mda-kite-check mda-kite-check/main.cpp)
target_link_libraries(mda-kite-check PRIVATE mda::kite mda::instruments)
mda_target_defaults(mda-kite-check)

add_executable(mda-tail mda-tail/main.cpp)
target_link_libraries(mda-tail PRIVATE mda::bus mda::instruments mda::messages mda::runtime)
mda_target_defaults(mda-tail)

add_executable(mda-useradm mda-useradm/main.cpp)
target_link_libraries(mda-useradm PRIVATE mda::admin mda::appdb mda::auth mda::runtime mda::storage)
mda_target_defaults(mda-useradm)
