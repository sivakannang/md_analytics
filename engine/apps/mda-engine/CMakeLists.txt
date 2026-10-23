add_executable(mda-engine main.cpp)
target_link_libraries(mda-engine PRIVATE mda::bars mda::bus mda::instruments mda::options mda::messages mda::quotestore mda::runtime)
mda_target_defaults(mda-engine)
