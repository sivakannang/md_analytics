add_executable(mda-feedd main.cpp)
target_link_libraries(mda-feedd PRIVATE mda::bus mda::kite mda::instruments mda::messages mda::runtime mda::appdb mda::catalog mda::storage mda::watchlists mda::subscriptions mda::appcommon mda::bars mda::notify)
mda_target_defaults(mda-feedd)
