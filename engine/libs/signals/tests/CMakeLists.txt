mda_add_test(mda_signals_tests
    SOURCES market_state_test.cpp factors_test.cpp oi_bias_test.cpp outcome_test.cpp market_signal_test.cpp signal_store_test.cpp flow_test.cpp
    LIBS mda::signals)
