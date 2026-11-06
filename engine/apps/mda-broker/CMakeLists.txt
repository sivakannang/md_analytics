find_package(boost_json CONFIG REQUIRED)

add_executable(mda-broker main.cpp)
target_link_libraries(mda-broker PRIVATE mda::broker mda::appdb mda::bars mda::bus mda::messages mda::runtime mda::storage
                      Boost::json)
mda_target_defaults(mda-broker)
