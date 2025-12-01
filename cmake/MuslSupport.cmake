# cmake/MuslSupport.cmake
# Static binary support - works with both musl and glibc
#
# Two modes:
#   L2NET_USE_MUSL=ON   - Full musl build (requires Alpine or musl toolchain)
#   L2NET_STATIC=ON     - Static glibc build (works on most Linux distros)

# guard against double inclusion
if(DEFINED _L2NET_MUSL_SUPPORT_INCLUDED)
    return()
endif()
set(_L2NET_MUSL_SUPPORT_INCLUDED TRUE)

# define options if not already defined
if(NOT DEFINED L2NET_USE_MUSL)
    option(L2NET_USE_MUSL "Build with musl libc (requires musl toolchain)" OFF)
endif()

if(NOT DEFINED L2NET_STATIC)
    option(L2NET_STATIC "Build fully static binaries with glibc" OFF)
endif()

# =============================================================================
# Common settings for any static build (musl or glibc)
# =============================================================================
if(L2NET_USE_MUSL OR L2NET_STATIC)
    # disable all dependency tests - we just want the libs
    set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(FMT_TEST OFF CACHE BOOL "" FORCE)
    set(FMT_DOC OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
endif()

# =============================================================================
# OPTION 1: Full musl build (Alpine Linux or musl cross-toolchain)
# =============================================================================
if(L2NET_USE_MUSL)
    message(STATUS "")
    message(STATUS "============================================================")
    message(STATUS "MUSL BUILD ENABLED")
    message(STATUS "============================================================")
    
    # check if we're actually using a musl-based system (like Alpine)
    # or if we have a proper musl C++ toolchain
    execute_process(
        COMMAND ldd --version
        OUTPUT_VARIABLE LDD_OUTPUT
        ERROR_VARIABLE LDD_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    
    string(FIND "${LDD_OUTPUT}" "musl" MUSL_IN_LDD)
    
    if(MUSL_IN_LDD GREATER -1)
        message(STATUS "Detected musl-based system (Alpine/Void/etc)")
        set(MUSL_NATIVE TRUE)
    else()
        message(WARNING 
            "L2NET_USE_MUSL=ON but not running on a musl-based system.\n"
            "musl-gcc on glibc systems doesn't support C++!\n"
            "\n"
            "Options:\n"
            "  1. Use Docker with Alpine Linux (recommended)\n"
            "  2. Use L2NET_STATIC=ON instead for glibc static builds\n"
            "  3. Install a musl cross-toolchain with C++ support\n"
            "\n"
            "Falling back to L2NET_STATIC=ON...")
        set(L2NET_USE_MUSL OFF)
        set(L2NET_STATIC ON)
    endif()
endif()

# actually do musl config if we're on a musl system
if(L2NET_USE_MUSL AND MUSL_NATIVE)
    # FORCE disable benchmarks - google benchmark doesn't work with musl
    set(L2NET_BUILD_BENCHMARKS OFF CACHE BOOL "Benchmarks disabled for musl" FORCE)
    set(L2NET_BUILD_TESTS OFF CACHE BOOL "Tests disabled for musl" FORCE)

    # create interface target
    add_library(l2net_static_build INTERFACE)
    target_compile_options(l2net_static_build INTERFACE -static -D_GNU_SOURCE)
    target_link_options(l2net_static_build INTERFACE -static -Wl,--gc-sections)
    add_link_options(-static)

    message(STATUS "  System:          musl-native")
    message(STATUS "  Static linking:  ON")
    message(STATUS "  Benchmarks:      OFF (incompatible)")
    message(STATUS "  Tests:           OFF (incompatible)")
    message(STATUS "============================================================")
    message(STATUS "")
endif()

# =============================================================================
# OPTION 2: Static glibc build (works on Ubuntu/Debian/Fedora/etc)
# =============================================================================
if(L2NET_STATIC AND NOT L2NET_USE_MUSL)
    message(STATUS "")
    message(STATUS "============================================================")
    message(STATUS "STATIC GLIBC BUILD ENABLED")
    message(STATUS "============================================================")
    
    # FORCE disable benchmarks and tests for static builds
    set(L2NET_BUILD_BENCHMARKS OFF CACHE BOOL "Benchmarks disabled for static" FORCE)
    set(L2NET_BUILD_TESTS OFF CACHE BOOL "Tests disabled for static" FORCE)

    # create interface target
    add_library(l2net_static_build INTERFACE)
    target_link_options(l2net_static_build INTERFACE
        -static
        -Wl,--gc-sections
    )
    
    # global static linking
    add_link_options(-static)

    message(STATUS "  System:          glibc (static)")
    message(STATUS "  Static linking:  ON")
    message(STATUS "  Benchmarks:      OFF")
    message(STATUS "  Tests:           OFF")
    message(STATUS "  remote_benchmark: SKIPPED (libssh static linking is broken)")
    message(STATUS "  remote_node:      BUILT (this is what you deploy)")
    message(STATUS "============================================================")
    message(STATUS "")
endif()

# =============================================================================
# Helper function
# =============================================================================
function(target_use_static target)
    if(TARGET l2net_static_build)
        target_link_libraries(${target} PRIVATE l2net_static_build)
    endif()
endfunction()