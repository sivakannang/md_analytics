add_executable(mda-recorder main.cpp)
target_link_libraries(mda-recorder PRIVATE mda::bus mda::journal mda::messages mda::runtime)
mda_target_defaults(mda-recorder)
