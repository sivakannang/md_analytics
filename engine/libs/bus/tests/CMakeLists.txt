find_package(Threads REQUIRED)

mda_add_test(mda_bus_tests
    SOURCES bus_test.cpp multiprocess_test.cpp ring_test.cpp wait_strategy_test.cpp
    LIBS mda::bus Threads::Threads)
