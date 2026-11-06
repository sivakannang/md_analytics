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

add_executable(mda-backtest mda-backtest/main.cpp)
target_link_libraries(mda-backtest PRIVATE mda::signals mda::bars mda::instruments mda::options mda::messages mda::runtime)
mda_target_defaults(mda-backtest)

add_executable(mda-paperctl mda-paperctl/main.cpp)
target_link_libraries(mda-paperctl PRIVATE mda::broker mda::bus mda::instruments mda::runtime)
mda_target_defaults(mda-paperctl)
