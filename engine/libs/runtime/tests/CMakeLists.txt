mda_add_test(mda_runtime_tests
    SOURCES config_test.cpp
    LIBS mda::runtime)
target_compile_definitions(mda_runtime_tests PRIVATE
    MDA_REPO_CONFIG="${PROJECT_SOURCE_DIR}/config/mda.yaml"
    MDA_REPLAY_CONFIG="${PROJECT_SOURCE_DIR}/config/replay.yaml")
