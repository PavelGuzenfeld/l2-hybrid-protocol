# apps/CMakeLists.txt
# build targets for applications

# =============================================================================
# remote node - deployed to remote machines for benchmarking
# this one can be built with musl/static for maximum portability
# =============================================================================
add_executable(l2net_remote_node
    remote_node.cpp
)

target_link_libraries(l2net_remote_node
    PRIVATE
        l2net::l2net
        l2net_warnings
)

target_link_options(l2net_remote_node PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
)

if(TARGET l2net_static_build)
    target_link_libraries(l2net_remote_node PRIVATE l2net_static_build)
endif()

install(TARGETS l2net_remote_node
    RUNTIME DESTINATION bin
)

# =============================================================================
# remote benchmark orchestrator - needs libssh (dynamic builds only)
# =============================================================================
# NOTE: for static builds, we skip this because libssh+openssl static linking
# is a mass grave of linker errors. the remote_node is what you deploy anyway.
if(NOT L2NET_USE_MUSL AND NOT L2NET_STATIC)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSSH QUIET libssh)
    endif()

    if(LIBSSH_FOUND)
        add_executable(l2net_remote_benchmark
            remote_benchmark.cpp
            ${CMAKE_SOURCE_DIR}/src/ssh_session.cpp
        )

        target_include_directories(l2net_remote_benchmark
            PRIVATE
                ${CMAKE_SOURCE_DIR}/include
                ${LIBSSH_INCLUDE_DIRS}
        )

        target_link_libraries(l2net_remote_benchmark
            PRIVATE
                l2net::l2net
                l2net_warnings
                ${LIBSSH_LIBRARIES}
        )

        target_link_directories(l2net_remote_benchmark
            PRIVATE
                ${LIBSSH_LIBRARY_DIRS}
        )

        message(STATUS "Building remote benchmark with libssh support")

        install(TARGETS l2net_remote_benchmark
            RUNTIME DESTINATION bin
        )
    else()
        message(STATUS "libssh not found - l2net_remote_benchmark will not be built")
        message(STATUS "  Install: apt install libssh-dev (Debian/Ubuntu)")
    endif()
else()
    message(STATUS "Skipping l2net_remote_benchmark (static/musl build)")
endif()
