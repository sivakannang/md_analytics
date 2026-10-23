find_package(Threads REQUIRED)

mda_add_test(mda_gateway_tests
    SOURCES gateway_test.cpp stream_test.cpp static_site_test.cpp system_monitor_test.cpp
    LIBS mda::gateway mda::appdb Threads::Threads)
