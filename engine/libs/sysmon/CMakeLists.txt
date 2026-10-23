# mda::sysmon — machine, process and disk statistics for the admin system monitor
# (docs/design/system-monitor.md §3). One interface, a macOS and a Linux implementation.

if(APPLE)
    set(MDA_SYSMON_HOST src/host_macos.cpp)
else()
    set(MDA_SYSMON_HOST src/host_linux.cpp)
endif()

add_library(mda_sysmon STATIC
    src/disk.cpp
    ${MDA_SYSMON_HOST})
add_library(mda::sysmon ALIAS mda_sysmon)

target_include_directories(mda_sysmon PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
if(APPLE)
    # Power sources (battery / AC) come from IOKit.
    target_link_libraries(mda_sysmon PRIVATE "-framework IOKit" "-framework CoreFoundation")
endif()
mda_target_defaults(mda_sysmon)

if(MDA_BUILD_TESTS)
    add_subdirectory(tests)
endif()
