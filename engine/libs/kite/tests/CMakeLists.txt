mda_add_test(mda_kite_tests
    SOURCES instrument_import_test.cpp login_test.cpp rest_client_test.cpp tick_decoder_test.cpp
            ws_client_test.cpp
    LIBS mda::kite)
