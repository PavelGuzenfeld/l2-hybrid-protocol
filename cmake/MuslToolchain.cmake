# cmake/MuslSupport.cmake
# include this in your main CMakeLists.txt AFTER project() declaration
# usage: include(cmake/MuslSupport.cmake)

# guard against double inclusion
if(DEFINED _L2NET_MUSL_SUPPORT_INCLUDED)
    return()
endif()
set(_L2NET_MUSL_SUPPORT_INCLUDED TRUE)

# option should be defined before including this file, but define default if not
if(NOT DEFINED L2NET_USE_MUSL)
    option(L2NET_USE_MUSL "Build with musl libc for portable static binaries" OFF)
endif()

if(L2NET_USE_MUSL)
    message(STATUS "")
    message(STATUS "============================================================")
    message(STATUS "MUSL BUILD ENABLED")
    message(STATUS "============================================================")

    # find musl compilers
    find_program(MUSL_GCC musl-gcc)
    find_program(MUSL_CLANG musl-clang)

    if(NOT MUSL_GCC AND NOT MUSL_CLANG)
        message(FATAL_ERROR 
            "L2NET_USE_MUSL is ON but no musl compiler found!\n"
            "Install musl-tools:\n"
            "  Debian/Ubuntu: sudo apt install musl-tools musl-dev\n"
            "  Fedora:        sudo dnf install musl-gcc musl-devel\n"
            "  Arch:          sudo pacman -S musl\n"
            "  Alpine:        already using musl!\n"
            "\n"
            "Or specify the compiler directly:\n"
            "  CC=musl-gcc CXX=musl-gcc cmake ..\n"
            "\n"
            "Or use the toolchain file:\n"
            "  cmake -DCMAKE_TOOLCHAIN_FILE=cmake/MuslToolchain.cmake ..")
    endif()

    # prefer clang if available (better C++ support with musl)
    if(MUSL_CLANG)
        message(STATUS "Found musl-clang: ${MUSL_CLANG}")
        set(MUSL_CC "${MUSL_CLANG}")
        set(MUSL_CXX "${MUSL_CLANG}")
    else()
        message(STATUS "Found musl-gcc: ${MUSL_GCC}")
        set(MUSL_CC "${MUSL_GCC}")
        set(MUSL_CXX "${MUSL_GCC}")
    endif()

    # create a target for musl-specific settings
    add_library(l2net_musl INTERFACE)
    
    # static linking for fully portable binaries
    target_compile_options(l2net_musl INTERFACE
        -static
        -D_GNU_SOURCE
    )
    target_link_options(l2net_musl INTERFACE
        -static
        -Wl,--gc-sections
    )

    # also set global link options for targets that don't explicitly use l2net_musl
    add_link_options(-static)

    message(STATUS "  Static linking: ON")
    message(STATUS "  Musl compiler:  ${MUSL_CC}")
    message(STATUS "")
    message(STATUS "NOTE: For best results, configure with:")
    message(STATUS "  CC=${MUSL_CC} CXX=${MUSL_CXX} cmake -DL2NET_USE_MUSL=ON ..")
    message(STATUS "============================================================")
    message(STATUS "")
endif()

# function to apply musl settings to a target
function(target_use_musl target)
    if(L2NET_USE_MUSL AND TARGET l2net_musl)
        target_link_libraries(${target} PRIVATE l2net_musl)
    endif()
endfunction()