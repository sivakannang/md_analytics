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

# GCC only:
# - -Wextra flags designated initialisers that leave members out; we rely on those being
#   value-initialised, and Clang doesn't warn about them.
# - -Wnull-dereference and -Wfree-nonheap-object misfire at -O2 on code inlined from
#   Boost.Asio/Beast and std::expected (GCC 14), so they can't be errors there.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    list(APPEND _mda_warnings
        -Wno-missing-field-initializers
        -Wno-null-dereference
        -Wno-free-nonheap-object)
endif()

# Older libc++ (Xcode 16 and earlier) keeps std::jthread and std::stop_token behind
# -fexperimental-library; newer ones don't need it.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles([=[
#include <stop_token>
#include <thread>
int main() { std::jthread t([](std::stop_token) {}); }
]=] MDA_HAVE_JTHREAD)
set(_mda_std_options)
if(NOT MDA_HAVE_JTHREAD AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(_mda_std_options -fexperimental-library)
endif()

# mda_target_defaults(<target>)
#   Applies warnings and the configured sanitizers to one of our compiled targets.
function(mda_target_defaults target)
    target_compile_options(${target} PRIVATE ${_mda_warnings} ${_mda_std_options})
    target_link_options(${target} PRIVATE ${_mda_std_options})
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
