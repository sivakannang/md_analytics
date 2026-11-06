add_executable(mda-autotrader main.cpp)
target_link_libraries(mda-autotrader PRIVATE mda::autotrade mda::broker mda::bus mda::instruments mda::messages
                      mda::runtime mda::signals mda::storage)
mda_target_defaults(mda-autotrader)
