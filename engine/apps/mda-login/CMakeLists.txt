add_executable(mda-login main.cpp)
target_link_libraries(mda-login PRIVATE mda::kite)
mda_target_defaults(mda-login)
