cmake_minimum_required(VERSION 3.21)

project(l2net
    VERSION 0.0.1
    DESCRIPTION "High-performance Layer 2 raw socket networking library"
    LANGUAGES CXX C  # C needed for libssh
)

# =============================================================================
# Build options
# =============================================================================
option(L2NET_USE_MUSL "Build with musl libc for portable static binaries" OFF)
option(L2NET_STATIC "Build fully static binaries with glibc" OFF)
option(L2NET_BUILD_TESTS "Build the test suite (requires root to run)" ON)
option(L2NET_BUILD_BENCHMARKS "Build benchmarks (requires root to run)" ON)
option(L2NET_BUILD_APPS "Build example applications" ON)
option(L2NET_ENABLE_SANITIZERS "Enable ASan/UBSan for tests" ON)
option(L2NET_FETCH_LIBSSH "Fetch and build libssh (requires OpenSSL dev headers)" ON)
option(L2NET_STATIC_OPENSSL "Link OpenSSL statically (for portable binaries)" OFF)

include(cmake/MuslSupport.cmake OPTIONAL)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# =============================================================================
# Static linking setup
# =============================================================================
# NOTE: fully static builds require static OpenSSL (libssl.a, libcrypto.a)
# on debian/ubuntu: not available in packages, must build from source
# on alpine: apk add openssl-libs-static
# if static OpenSSL not found, we fall back to dynamic linking with warning
set(L2NET_FULL_STATIC OFF)

if(L2NET_STATIC OR L2NET_USE_MUSL)
    set(L2NET_STATIC_OPENSSL ON)
    set(BUILD_SHARED_LIBS OFF)
    # don't set -static yet - we'll check if static openssl exists first
    message(STATUS "Static build requested - looking for static OpenSSL...")
endif()

# =============================================================================
# Dependencies via FetchContent
# =============================================================================
include(FetchContent)

# fmt - formatting library
FetchContent_Declare(
    fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 10.2.1
)

# doctest - testing framework
if(L2NET_BUILD_TESTS)
    FetchContent_Declare(
        doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.4.11
    )
    set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(doctest)
endif()

# nanobench - microbenchmark framework
if(L2NET_BUILD_BENCHMARKS)
    FetchContent_Declare(
        nanobench
        GIT_REPOSITORY https://github.com/martinus/nanobench.git
        GIT_TAG v4.3.11
    )
    FetchContent_MakeAvailable(nanobench)
endif()

FetchContent_MakeAvailable(fmt)

# =============================================================================
# OpenSSL - required by libssh
# =============================================================================
# for static builds, we need to find static OpenSSL libs
# install with: apt install libssl-dev (dynamic) or build from source (static)
# =============================================================================
set(L2NET_HAS_OPENSSL OFF)
set(L2NET_HAS_STATIC_OPENSSL OFF)

if(L2NET_STATIC_OPENSSL)
    # look for static OpenSSL first
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    find_package(OpenSSL QUIET)
    
    if(OpenSSL_FOUND)
        # check if we actually got static libraries
        if(OPENSSL_CRYPTO_LIBRARY MATCHES "\\.a$")
            set(L2NET_HAS_STATIC_OPENSSL ON)
            message(STATUS "Static OpenSSL found: ${OPENSSL_VERSION}")
        else()
            # reset and try again without static requirement
            message(STATUS "Static OpenSSL requested but only dynamic found")
            message(STATUS "  found: ${OPENSSL_CRYPTO_LIBRARY}")
            unset(OPENSSL_FOUND)
            unset(OPENSSL_SSL_LIBRARY)
            unset(OPENSSL_CRYPTO_LIBRARY)
            set(OPENSSL_USE_STATIC_LIBS FALSE)
        endif()
    endif()
endif()

# if static not found (or not requested), try dynamic
if(NOT OpenSSL_FOUND)
    set(OPENSSL_USE_STATIC_LIBS FALSE)
    find_package(OpenSSL QUIET)
endif()

if(OpenSSL_FOUND)
    set(L2NET_HAS_OPENSSL ON)
    message(STATUS "OpenSSL found: ${OPENSSL_VERSION}")
    message(STATUS "  SSL lib: ${OPENSSL_SSL_LIBRARY}")
    message(STATUS "  Crypto lib: ${OPENSSL_CRYPTO_LIBRARY}")
    
    # now we can decide if full static linking is possible
    if((L2NET_STATIC OR L2NET_USE_MUSL) AND L2NET_HAS_STATIC_OPENSSL)
        set(L2NET_FULL_STATIC ON)
        set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
        message(STATUS "Full static linking enabled")
    elseif(L2NET_STATIC OR L2NET_USE_MUSL)
        message(WARNING "Static build requested but static OpenSSL not available")
        message(WARNING "  Falling back to dynamic linking")
        message(WARNING "  For static builds, install static OpenSSL:")
        message(WARNING "    Alpine: apk add openssl-libs-static")
        message(WARNING "    Debian/Ubuntu: build OpenSSL from source")
    endif()
else()
    message(STATUS "OpenSSL not found - SSH support will be disabled")
    message(STATUS "  install with: apt install libssl-dev (debian/ubuntu)")
    message(STATUS "              : apk add openssl-dev openssl-libs-static (alpine)")
endif()

# =============================================================================
# libssh - SSH protocol library (with SFTP support)
# =============================================================================
# libssh 0.10.x deprecated SCP API, we use SFTP instead
# =============================================================================
set(L2NET_HAS_SSH OFF)
set(LIBSSH_FROM_FETCH OFF)

if(L2NET_FETCH_LIBSSH AND L2NET_HAS_OPENSSL)
    message(STATUS "Fetching libssh 0.10.6...")
    
    # libssh build options - disable stuff we don't need
    set(WITH_SERVER OFF CACHE BOOL "" FORCE)
    set(WITH_SFTP ON CACHE BOOL "" FORCE)      # SFTP is required (replaced SCP)
    set(WITH_ZLIB OFF CACHE BOOL "" FORCE)     # optional compression
    set(WITH_PCAP OFF CACHE BOOL "" FORCE)     # packet capture debug
    set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)  # always build static libssh
    set(UNIT_TESTING OFF CACHE BOOL "" FORCE)
    set(CLIENT_TESTING OFF CACHE BOOL "" FORCE)
    set(SERVER_TESTING OFF CACHE BOOL "" FORCE)
    set(WITH_GSSAPI OFF CACHE BOOL "" FORCE)   # kerberos - usually not needed
    set(WITH_NACL OFF CACHE BOOL "" FORCE)     # NaCl crypto - OpenSSL is enough
    set(WITH_MBEDTLS OFF CACHE BOOL "" FORCE)  # we use OpenSSL
    
    FetchContent_Declare(
        libssh
        GIT_REPOSITORY https://git.libssh.org/projects/libssh.git
        GIT_TAG libssh-0.10.6
        GIT_SHALLOW TRUE
    )
    
    # save our output directories (libssh likes to override them)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_BACKUP ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_BACKUP ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_BACKUP ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY})
    
    FetchContent_MakeAvailable(libssh)
    
    # restore our output directories
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY_BACKUP})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY_BACKUP})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_ARCHIVE_OUTPUT_DIRECTORY_BACKUP})
    
    set(L2NET_HAS_SSH ON)
    set(LIBSSH_FROM_FETCH ON)
endif()

# fallback to system libssh if fetch disabled or failed
if(NOT L2NET_HAS_SSH AND L2NET_HAS_OPENSSL)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        if(L2NET_STATIC OR L2NET_USE_MUSL)
            pkg_check_modules(LIBSSH QUIET IMPORTED_TARGET libssh>=0.10.0)
            if(LIBSSH_FOUND)
                # for static linking, we need the static lib
                find_library(LIBSSH_STATIC_LIB 
                    NAMES libssh.a ssh
                    HINTS ${LIBSSH_LIBRARY_DIRS}
                )
                if(LIBSSH_STATIC_LIB)
                    set(L2NET_HAS_SSH ON)
                    message(STATUS "Using system libssh (static): ${LIBSSH_STATIC_LIB}")
                endif()
            endif()
        else()
            pkg_check_modules(LIBSSH QUIET libssh>=0.10.0)
            if(LIBSSH_FOUND)
                set(L2NET_HAS_SSH ON)
                message(STATUS "Using system libssh ${LIBSSH_VERSION}")
            endif()
        endif()
    endif()
endif()

# status summary
if(L2NET_HAS_SSH)
    if(LIBSSH_FROM_FETCH)
        message(STATUS "SSH support: ON (fetched libssh 0.10.6, static)")
    else()
        message(STATUS "SSH support: ON (system libssh ${LIBSSH_VERSION})")
    endif()
else()
    message(STATUS "SSH support: OFF")
    if(NOT L2NET_HAS_OPENSSL)
        message(STATUS "  reason: OpenSSL not found")
    else()
        message(STATUS "  reason: libssh not found")
    endif()
endif()

# =============================================================================
# Compiler warnings
# =============================================================================
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
# Sanitizers (for tests only)
# =============================================================================
add_library(l2net_test_sanitizers INTERFACE)
if(L2NET_ENABLE_SANITIZERS AND NOT L2NET_STATIC AND NOT L2NET_USE_MUSL)
    # sanitizers don't work with fully static builds
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

# static libstdc++/libgcc for semi-portable binaries
if(NOT L2NET_USE_MUSL AND NOT L2NET_STATIC)
    add_link_options(-static-libstdc++ -static-libgcc)
endif()

# =============================================================================
# Main Library
# =============================================================================

# base sources
set(L2NET_SOURCES
    src/interface.cpp
    src/raw_socket.cpp
    src/frame.cpp
    src/vlan.cpp
    src/ipc_channel.cpp
    src/hybrid_chat.cpp
)

# add SSH support sources
if(L2NET_HAS_SSH)
    list(APPEND L2NET_SOURCES src/ssh_session.cpp)
endif()

add_library(l2net ${L2NET_SOURCES})

target_include_directories(l2net
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

find_package(Threads REQUIRED)

target_link_libraries(l2net
    PUBLIC
        fmt::fmt
        Threads::Threads
    PRIVATE
        l2net_warnings
)

# =============================================================================
# libssh linking
# =============================================================================
if(L2NET_HAS_SSH)
    if(LIBSSH_FROM_FETCH)
        # link against fetched static libssh
        # the 'ssh' target dependency ensures libssh is fully built (including
        # generated headers like libssh_version.h) before l2net compiles
        target_link_libraries(l2net PRIVATE ssh)
        target_include_directories(l2net PRIVATE 
            ${libssh_SOURCE_DIR}/include
            ${libssh_BINARY_DIR}/include
        )
        
        # ensure libssh configure runs before we try to compile
        # this generates libssh_version.h and other headers
        add_dependencies(l2net ssh)
        
        # libssh needs OpenSSL - link it explicitly for static builds
        target_link_libraries(l2net PRIVATE 
            OpenSSL::SSL 
            OpenSSL::Crypto
        )
        
        # on linux, OpenSSL crypto needs -ldl for dlopen (used by engine support)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            target_link_libraries(l2net PRIVATE ${CMAKE_DL_LIBS})
        endif()
    else()
        # system libssh
        if(L2NET_FULL_STATIC)
            # static linking (only if static OpenSSL was found)
            target_link_libraries(l2net PRIVATE 
                ${LIBSSH_STATIC_LIB}
                OpenSSL::SSL 
                OpenSSL::Crypto
            )
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
                target_link_libraries(l2net PRIVATE ${CMAKE_DL_LIBS})
            endif()
        else()
            # dynamic linking
            target_include_directories(l2net PRIVATE ${LIBSSH_INCLUDE_DIRS})
            target_link_libraries(l2net PRIVATE ${LIBSSH_LIBRARIES})
            target_link_directories(l2net PRIVATE ${LIBSSH_LIBRARY_DIRS})
        endif()
    endif()
    
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

    # remote_node and remote_benchmark
    add_subdirectory(apps)
endif()

# =============================================================================
# Tests (Doctest)
# =============================================================================
if(L2NET_BUILD_TESTS)
    enable_testing()

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

# =============================================================================
# clang-tidy target
# =============================================================================
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
# Configuration summary
# =============================================================================
message(STATUS "")
message(STATUS "============================================================")
message(STATUS "l2net configuration:")
message(STATUS "============================================================")
message(STATUS "  Version:              ${PROJECT_VERSION}")
message(STATUS "  Build type:           ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard:         ${CMAKE_CXX_STANDARD}")
message(STATUS "  Compiler:             ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "------------------------------------------------------------")
message(STATUS "  Static build:         ${L2NET_STATIC}")
message(STATUS "  Musl build:           ${L2NET_USE_MUSL}")
message(STATUS "  Static OpenSSL:       ${L2NET_STATIC_OPENSSL}")
message(STATUS "------------------------------------------------------------")
message(STATUS "  SSH support:          ${L2NET_HAS_SSH}")
if(L2NET_HAS_SSH)
    if(LIBSSH_FROM_FETCH)
        message(STATUS "    libssh:             fetched 0.10.6 (static)")
    else()
        message(STATUS "    libssh:             system ${LIBSSH_VERSION}")
    endif()
endif()
message(STATUS "------------------------------------------------------------")
message(STATUS "  Tests:                ${L2NET_BUILD_TESTS}")
message(STATUS "  Benchmarks:           ${L2NET_BUILD_BENCHMARKS}")
message(STATUS "  Apps:                 ${L2NET_BUILD_APPS}")
if(L2NET_ENABLE_SANITIZERS AND NOT L2NET_STATIC AND NOT L2NET_USE_MUSL)
    message(STATUS "  Test sanitizers:      ON (ASan + UBSan)")
else()
    message(STATUS "  Test sanitizers:      OFF")
endif()
message(STATUS "============================================================")
message(STATUS "")