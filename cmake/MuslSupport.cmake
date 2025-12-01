# cmake/MuslSupport.cmake
# Static binary support - works with both musl and glibc

if(DEFINED _L2NET_MUSL_SUPPORT_INCLUDED)
    return()
endif()
set(_L2NET_MUSL_SUPPORT_INCLUDED TRUE)

if(NOT DEFINED L2NET_USE_MUSL)
    option(L2NET_USE_MUSL "Build with musl libc (requires musl toolchain)" OFF)
endif()

if(NOT DEFINED L2NET_STATIC)
    option(L2NET_STATIC "Build fully static binaries with glibc" OFF)
endif()

# =============================================================================
# Common settings
# =============================================================================
if(L2NET_USE_MUSL OR L2NET_STATIC)
    set(FMT_TEST OFF CACHE BOOL "" FORCE)
    set(FMT_DOC OFF CACHE BOOL "" FORCE)
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
endif()

# =============================================================================
# OPTION 1: Full musl build
# =============================================================================
if(L2NET_USE_MUSL)
    message(STATUS "")
    message(STATUS "============================================================")
    message(STATUS "MUSL BUILD ENABLED")
    message(STATUS "============================================================")
    
    execute_process(
        COMMAND ldd --version
        OUTPUT_VARIABLE LDD_OUTPUT
        ERROR_VARIABLE LDD_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    
    string(FIND "${LDD_OUTPUT}" "musl" MUSL_IN_LDD)
    
    if(MUSL_IN_LDD GREATER -1)
        message(STATUS "Detected musl-based system")
        set(MUSL_NATIVE TRUE)
    else()
        message(WARNING "Not a musl system. Falling back to L2NET_STATIC=ON...")
        set(L2NET_USE_MUSL OFF)
        set(L2NET_STATIC ON)
    endif()
endif()

if(L2NET_USE_MUSL AND MUSL_NATIVE)
    set(L2NET_BUILD_TESTS OFF CACHE BOOL "Tests disabled for musl" FORCE)

    add_library(l2net_static_build INTERFACE)
    target_compile_options(l2net_static_build INTERFACE -static -D_GNU_SOURCE)
    target_link_options(l2net_static_build INTERFACE -static -Wl,--gc-sections)
    add_link_options(-static)

    message(STATUS "  System:          musl-native")
    message(STATUS "  Static linking:  ON")
    message(STATUS "============================================================")
endif()

# =============================================================================
# OPTION 2: Static glibc build
# =============================================================================
if(L2NET_STATIC AND NOT L2NET_USE_MUSL)
    message(STATUS "")
    message(STATUS "============================================================")
    message(STATUS "STATIC GLIBC BUILD ENABLED")
    message(STATUS "============================================================")
    
    set(L2NET_BUILD_TESTS OFF CACHE BOOL "Tests disabled for static" FORCE)

    add_library(l2net_static_build INTERFACE)
    target_link_options(l2net_static_build INTERFACE -static -Wl,--gc-sections)
    add_link_options(-static)

    message(STATUS "  System:          glibc (static)")
    message(STATUS "  Static linking:  ON")
    message(STATUS "============================================================")
endif()

function(target_use_static target)
    if(TARGET l2net_static_build)
        target_link_libraries(${target} PRIVATE l2net_static_build)
    endif()
endfunction()