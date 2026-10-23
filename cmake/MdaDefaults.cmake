# Project-wide target defaults: warnings and sanitizers (ADR-0004).
#
# Applied per target instead of globally, so third-party code (vcpkg packages)
# is never compiled with our warning flags.

include_guard(GLOBAL)

set(_mda_warnings
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wold-style-cast
    -Wnon-virtual-dtor
    -Woverloaded-virtual
    -Wnull-dereference
    -Wimplicit-fallthrough
)

# mda_target_defaults(<target>)
#   Applies warnings and the configured sanitizers to one of our compiled targets.
function(mda_target_defaults target)
    target_compile_options(${target} PRIVATE ${_mda_warnings})
    if(MDA_WARNINGS_AS_ERRORS)
        target_compile_options(${target} PRIVATE -Werror)
    endif()

    if(MDA_SANITIZER)
        set(_san -fsanitize=${MDA_SANITIZER} -fno-omit-frame-pointer)
        if(MDA_SANITIZER MATCHES "undefined")
            # Make UBSan findings fail the test instead of only printing.
            list(APPEND _san -fno-sanitize-recover=undefined)
        endif()
        target_compile_options(${target} PRIVATE ${_san})
        target_link_options(${target} PRIVATE ${_san})
    endif()
endfunction()

# mda_add_test(<name> SOURCES <files...> LIBS <targets...>)
#   Creates a GoogleTest executable and registers each test case with CTest.
function(mda_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} GTest::gtest_main)
    mda_target_defaults(${name})
    gtest_discover_tests(${name})
endfunction()
