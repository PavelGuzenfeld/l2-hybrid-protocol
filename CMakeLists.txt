cmake_minimum_required(VERSION 3.21)

project(l2net
    VERSION 1.0.0
    DESCRIPTION "High-performance Layer 2 raw socket networking library"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(L2NET_BUILD_TESTS "Build the test suite (requires root to run)" ON)
option(L2NET_BUILD_BENCHMARKS "Build benchmarks (requires root to run)" ON)
option(L2NET_BUILD_APPS "Build example applications" ON)
option(L2NET_BUILD_REMOTE_TESTS "Build remote network tests (requires ssh, sshpass)" ON)

include(FetchContent)

FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1
)

FetchContent_Declare(
    expected
    GIT_REPOSITORY https://github.com/TartanLlama/expected.git
    GIT_TAG v1.1.0
)

if(L2NET_BUILD_TESTS OR L2NET_BUILD_REMOTE_TESTS)
    FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
    )
    FetchContent_MakeAvailable(doctest)
endif()

if(L2NET_BUILD_BENCHMARKS)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG v1.8.3
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)
endif()

FetchContent_MakeAvailable(fmt expected)

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
# sanitizers for tests - ALWAYS ON regardless of build type
# because finding bugs at runtime is amateur hour, but we do it anyway
# =============================================================================
add_library(l2net_test_sanitizers INTERFACE)
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

# =============================================================================
# sanitizers for benchmarks - only in Debug because perf matters in Release
# don't want your benchmark numbers corrupted by sanitizer overhead
# =============================================================================
add_library(l2net_bench_sanitizers INTERFACE)
target_compile_options(l2net_bench_sanitizers INTERFACE
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<CONFIG:Debug>>:
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
    >
)
target_link_options(l2net_bench_sanitizers INTERFACE
    $<$<AND:$<CXX_COMPILER_ID:GNU,Clang>,$<CONFIG:Debug>>:
        -fsanitize=address,undefined
    >
)

# FIXED: Statically link stdlib to fix CXXABI errors on older OS
add_link_options(-static-libstdc++ -static-libgcc)

# =============================================================================
# main library - the actual useful code
# =============================================================================
add_library(l2net
    src/interface.cpp
    src/raw_socket.cpp
    src/frame.cpp
    src/vlan.cpp
    src/ipc_channel.cpp
    src/hybrid_chat.cpp
)

target_include_directories(l2net
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(l2net
    PUBLIC
        fmt::fmt
        tl::expected
    PRIVATE
        l2net_warnings
)

add_library(l2net::l2net ALIAS l2net)

# =============================================================================
# applications - stuff users actually run
# =============================================================================
if(L2NET_BUILD_APPS)
    add_executable(hybrid_chat apps/hybrid_chat_app.cpp)
    target_link_libraries(hybrid_chat PRIVATE l2net::l2net l2net_warnings)

    add_executable(ipc_l2 apps/ipc_l2_app.cpp)
    target_link_libraries(ipc_l2 PRIVATE l2net::l2net l2net_warnings)
endif()

# # remote node is needed for both apps and remote tests
# if(L2NET_BUILD_APPS OR L2NET_BUILD_REMOTE_TESTS)
#     add_executable(l2net_remote_node apps/remote_node.cpp)
#     target_link_libraries(l2net_remote_node PRIVATE l2net::l2net l2net_warnings)
# endif()

# =============================================================================
# unit and integration tests - with sanitizers ALWAYS enabled
# =============================================================================
if(L2NET_BUILD_TESTS)
    enable_testing()

    # unit tests - no root required, sanitizers ON
    add_executable(l2net_unit_tests
        tests/unit/main.cpp
        tests/unit/test_interface.cpp
        tests/unit/test_frame.cpp
        tests/unit/test_vlan.cpp
        tests/unit/test_edge_cases.cpp
    )
    target_link_libraries(l2net_unit_tests
        PRIVATE
            l2net::l2net
            doctest::doctest
            l2net_warnings
            l2net_test_sanitizers
    )
    add_test(NAME unit_tests COMMAND l2net_unit_tests)

    # integration tests - requires root, sanitizers ON
    add_executable(l2net_integration_tests
        tests/integration/main.cpp
        tests/integration/test_localhost.cpp
        tests/integration/test_network.cpp
    )
    target_link_libraries(l2net_integration_tests
        PRIVATE
            l2net::l2net
            doctest::doctest
            l2net_warnings
            l2net_test_sanitizers
    )
    add_test(NAME integration_tests COMMAND l2net_integration_tests)
endif()

# # =============================================================================
# # remote network tests - requires ssh access to remote machine
# # sanitizers ON for the test runner, remote node runs without sanitizers
# # =============================================================================
# if(L2NET_BUILD_REMOTE_TESTS)
#     enable_testing()

#     add_executable(l2net_remote_tests
#         tests/remote/main.cpp
#         tests/remote/test_remote_latency.cpp
#         tests/remote/test_remote_throughput.cpp
#     )
#     target_link_libraries(l2net_remote_tests
#         PRIVATE
#             l2net::l2net
#             doctest::doctest
#             l2net_warnings
#             l2net_test_sanitizers
#     )
#     # pass binary path to test so it knows where to find the node binary
#     target_compile_definitions(l2net_remote_tests PRIVATE
#         L2NET_REMOTE_NODE_PATH="$<TARGET_FILE:l2net_remote_node>"
#     )
#     add_test(NAME remote_tests COMMAND l2net_remote_tests)
# endif()

# =============================================================================
# benchmarks - for when you want to cry about performance
# NO sanitizers in Release, Debug-only sanitizers for correctness checking
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
            benchmark::benchmark
            l2net_warnings
            l2net_bench_sanitizers
    )
endif()

add_subdirectory(apps)


# =============================================================================
# installation - for the three people who will actually use this
# =============================================================================
include(GNUInstallDirs)
install(TARGETS l2net
    EXPORT l2netTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# =============================================================================
# print configuration summary so you know what you're getting into
# =============================================================================
message(STATUS "")
message(STATUS "============================================================")
message(STATUS "l2net configuration:")
message(STATUS "============================================================")
message(STATUS "  Build type:           ${CMAKE_BUILD_TYPE}")
message(STATUS "  Tests:                ${L2NET_BUILD_TESTS}")
message(STATUS "  Remote tests:         ${L2NET_BUILD_REMOTE_TESTS}")
message(STATUS "  Benchmarks:           ${L2NET_BUILD_BENCHMARKS}")
message(STATUS "  Apps:                 ${L2NET_BUILD_APPS}")
message(STATUS "------------------------------------------------------------")
message(STATUS "  Test sanitizers:      ALWAYS ON (ASan + UBSan)")
message(STATUS "  Bench sanitizers:     Debug only")
message(STATUS "============================================================")
message(STATUS "")