add_executable(mda-gateway main.cpp)
target_link_libraries(mda-gateway PRIVATE mda::gateway mda::admin mda::bars mda::auth mda::storage mda::runtime mda::watchlists)
mda_target_defaults(mda-gateway)
