mda_add_test(mda_notify_tests
    SOURCES codes_test.cpp email_channel_test.cpp outbox_test.cpp secrets_test.cpp system_alerts_test.cpp telegram_test.cpp templates_test.cpp
    LIBS mda::notify mda::appdb)
target_compile_definitions(mda_notify_tests PRIVATE
    MDA_NOTIFIER_EXAMPLE="${PROJECT_SOURCE_DIR}/config/notifier.example.json")
