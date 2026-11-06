add_executable(mda-signald main.cpp)
target_link_libraries(mda-signald PRIVATE mda::signals mda::bars mda::bus mda::instruments mda::options mda::messages mda::runtime)
mda_target_defaults(mda-signald)
