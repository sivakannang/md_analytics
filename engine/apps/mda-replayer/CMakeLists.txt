add_executable(mda-replayer main.cpp)
target_link_libraries(mda-replayer PRIVATE mda::bus mda::instruments mda::journal mda::kite mda::messages mda::runtime)
mda_target_defaults(mda-replayer)
