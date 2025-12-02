cmake_minimum_required(VERSION 3.21)

project(l2net
    VERSION 1.0.0
    DESCRIPTION "High-performance Layer 2 raw socket networking library"
    LANGUAGES CXX
)

option(L2NET_USE_MUSL "Build with musl libc for portable static binaries" OFF)
option(L2NET_STATIC "Build fully static binaries with glibc" OFF)
include(cmake/MuslSupport.cmake)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(L2NET_BUILD_TESTS "Build the test suite (requires root to run)" ON)
option(L2NET_BUILD_BENCHMARKS "Build benchmarks (requires root to run)" ON)
option(L2NET_BUILD_APPS "Build example applications" ON)
option(L2NET_ENABLE_SANITIZERS "Enable ASan/UBSan for tests" ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

include(FetchContent)

FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1
)

# 1. Fetch doctest (Testing)
if(L2NET_BUILD_TESTS)
    FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
    )
    # Disable doctest's own tests to save build time
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(doctest)
endif()

# 2. Fetch nanobench (Benchmarking)
if(L2NET_BUILD_BENCHMARKS)
    FetchContent_Declare(
        nanobench
        GIT_REPOSITORY https://github.com/martinus/nanobench.git
        GIT_TAG v4.3.11
    )
    FetchContent_MakeAvailable(nanobench)
endif()

FetchContent_MakeAvailable(fmt)

add_library(l2net_warnings INTERFACE)
target_compile_options(l2net_warnings INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:
        -Wall -Wextra -Wpedantic -Werror
        -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor -Wold-style-cast
        -Wcast-align -Wunused -Woverloaded-virtual
        -Wshadow -Wformat=2 -Wnull-dereference
    >
)

# =============================================================================
# Sanitizers
# =============================================================================
add_library(l2net_test_sanitizers INTERFACE)
if(L2NET_ENABLE_SANITIZERS)
    target_compile_options(l2net_test_sanitizers INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang>:
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
            -fno-optimize-sibling-calls
        >
    )
    target_link_options(l2net_test_sanitizers INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang>:
            -fsanitize=address,undefined
        >
    )
endif()

if(NOT L2NET_USE_MUSL AND NOT L2NET_STATIC)
    add_link_options(-static-libstdc++ -static-libgcc)
endif()

# =============================================================================
# SSH Support Detection
# =============================================================================
# ssh is only available for dynamic builds - static linking libssh+openssl
# is a mass grave of linker errors that nobody should have to endure
set(L2NET_HAS_SSH OFF)

if(NOT L2NET_USE_MUSL AND NOT L2NET_STATIC)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSSH QUIET libssh)
        if(LIBSSH_FOUND)
            set(L2NET_HAS_SSH ON)
        endif()
    endif()
endif()

if(L2NET_HAS_SSH)
    message(STATUS "SSH support: ON (libssh ${LIBSSH_VERSION})")
else()
    if(L2NET_USE_MUSL OR L2NET_STATIC)
        message(STATUS "SSH support: OFF (static/musl build - intentionally skipped)")
    else()
        message(STATUS "SSH support: OFF (libssh not found)")
    endif()
endif()

# =============================================================================
# Main Library
# =============================================================================

# base sources - always compiled
set(L2NET_SOURCES
    src/interface.cpp
    src/raw_socket.cpp
    src/frame.cpp
    src/vlan.cpp
    src/ipc_channel.cpp
    src/hybrid_chat.cpp
)

# conditionally add ssh support
if(L2NET_HAS_SSH)
    list(APPEND L2NET_SOURCES src/ssh_session.cpp)
endif()

add_library(l2net ${L2NET_SOURCES})

target_include_directories(l2net
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(l2net
    PUBLIC
        fmt::fmt
    PRIVATE
        l2net_warnings
)

# link libssh if available
if(L2NET_HAS_SSH)
    target_include_directories(l2net PRIVATE ${LIBSSH_INCLUDE_DIRS})
    target_link_libraries(l2net PRIVATE ${LIBSSH_LIBRARIES})
    target_link_directories(l2net PRIVATE ${LIBSSH_LIBRARY_DIRS})
    target_compile_definitions(l2net PUBLIC L2NET_HAS_SSH=1)
else()
    target_compile_definitions(l2net PUBLIC L2NET_HAS_SSH=0)
endif()

add_library(l2net::l2net ALIAS l2net)

# =============================================================================
# Applications
# =============================================================================
if(L2NET_BUILD_APPS)
    add_executable(hybrid_chat apps/hybrid_chat_app.cpp)
    target_link_libraries(hybrid_chat PRIVATE l2net::l2net l2net_warnings)

    add_executable(ipc_l2 apps/ipc_l2_app.cpp)
    target_link_libraries(ipc_l2 PRIVATE l2net::l2net l2net_warnings)

    # include the rest of the apps (remote_node, remote_benchmark)
    add_subdirectory(apps)
endif()

# =============================================================================
# Tests (Doctest)
# =============================================================================
if(L2NET_BUILD_TESTS)
    enable_testing()

    # Unit Tests
    add_executable(l2net_unit_tests
        tests/unit/main.cpp
        tests/unit/test_interface.cpp
        tests/unit/test_frame.cpp
        tests/unit/test_vlan.cpp
        tests/unit/test_edge_cases.cpp
    )

    target_include_directories(l2net_unit_tests PRIVATE tests/common)

    target_link_libraries(l2net_unit_tests
        PRIVATE
            l2net::l2net
            doctest::doctest
            l2net_warnings
            l2net_test_sanitizers
    )
    add_test(NAME unit_tests COMMAND l2net_unit_tests)

    # Integration Tests
    add_executable(l2net_integration_tests
        tests/integration/main.cpp
        tests/integration/test_localhost.cpp
        tests/integration/test_network.cpp
        tests/integration/test_vlan.cpp
    )

    target_include_directories(l2net_integration_tests PRIVATE tests/common)

    target_link_libraries(l2net_integration_tests
        PRIVATE
            l2net::l2net
            doctest::doctest
            l2net_warnings
            l2net_test_sanitizers
    )
    add_test(NAME integration_tests COMMAND l2net_integration_tests)
endif()

# =============================================================================
# Benchmarks (Nanobench)
# =============================================================================
if(L2NET_BUILD_BENCHMARKS)
    add_executable(l2net_benchmarks
        bench/main.cpp
        bench/bench_localhost.cpp
        bench/bench_network.cpp
    )
    target_link_libraries(l2net_benchmarks
        PRIVATE
            l2net::l2net
            nanobench
            l2net_warnings
    )
endif()

# =============================================================================
# Installation
# =============================================================================
include(GNUInstallDirs)
install(TARGETS l2net
    EXPORT l2netTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

find_program(CLANG_TIDY clang-tidy)
if(CLANG_TIDY)
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY} -p ${CMAKE_BINARY_DIR}
            ${CMAKE_SOURCE_DIR}/src/*.cpp
            ${CMAKE_SOURCE_DIR}/apps/*.cpp
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-tidy"
    )
endif()

# =============================================================================
# summary
# =============================================================================
message(STATUS "")
message(STATUS "============================================================")
message(STATUS "l2net configuration:")
message(STATUS "============================================================")
message(STATUS "  Build type:           ${CMAKE_BUILD_TYPE}")
message(STATUS "  Tests:                ${L2NET_BUILD_TESTS}")
message(STATUS "  Remote tests:         ${L2NET_BUILD_REMOTE_TESTS}")
message(STATUS "  Musl build:           ${L2NET_USE_MUSL}")
message(STATUS "  Static build:         ${L2NET_STATIC}")
message(STATUS "  Benchmarks:           ${L2NET_BUILD_BENCHMARKS}")
message(STATUS "  Apps:                 ${L2NET_BUILD_APPS}")
message(STATUS "  SSH support:          ${L2NET_HAS_SSH}")
message(STATUS "------------------------------------------------------------")
if(L2NET_ENABLE_SANITIZERS)
    message(STATUS "  Test sanitizers:      ON (ASan + UBSan)")
    message(STATUS "  Bench sanitizers:     Debug only")
else()
    message(STATUS "  Test sanitizers:      OFF")
    message(STATUS "  Bench sanitizers:     OFF")
endif()
message(STATUS "============================================================")
message(STATUS "")
