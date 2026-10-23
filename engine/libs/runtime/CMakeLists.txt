# mda::runtime — shared process skeleton: configuration and logging (ADR-0017).

find_package(spdlog CONFIG REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)

add_library(mda_runtime STATIC
    src/config.cpp
    src/logging.cpp)
add_library(mda::runtime ALIAS mda_runtime)

target_include_directories(mda_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(mda_runtime PUBLIC spdlog::spdlog PRIVATE yaml-cpp::yaml-cpp)
mda_target_defaults(mda_runtime)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
