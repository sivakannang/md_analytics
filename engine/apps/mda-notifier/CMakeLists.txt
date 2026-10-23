add_executable(mda-notifier main.cpp)
target_link_libraries(mda-notifier PRIVATE mda::notify mda::appdb mda::kite mda::runtime mda::storage)
mda_target_defaults(mda-notifier)
