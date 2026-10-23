# Benchmarks: run release builds only (cmake --preset release).

find_package(Threads REQUIRED)

add_executable(mda-bus-bench bus_bench.cpp)
target_link_libraries(mda-bus-bench PRIVATE mda::bus Threads::Threads)
mda_target_defaults(mda-bus-bench)
