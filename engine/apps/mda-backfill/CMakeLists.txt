add_executable(mda-backfill main.cpp)
target_link_libraries(mda-backfill PRIVATE mda::appcommon mda::appdb mda::bars mda::kite mda::runtime mda::storage
                                           mda::watchlists)
mda_target_defaults(mda-backfill)
