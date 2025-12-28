# apps/CMakeLists.txt
# build targets for applications

# =============================================================================
# remote node - deployed to remote machines for benchmarking
# =============================================================================
add_executable(l2net_remote_node
    remote_node.cpp
)

target_link_libraries(l2net_remote_node
    PRIVATE
        l2net::l2net
        l2net_warnings
)

# only add static flags if we're actually doing a full static build
if(L2NET_FULL_STATIC)
    target_link_options(l2net_remote_node PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
    )
endif()

install(TARGETS l2net_remote_node
    RUNTIME DESTINATION bin
)

# =============================================================================
# remote benchmark orchestrator - requires SSH support from l2net library
# =============================================================================
# the l2net library already includes ssh_session.cpp and links libssh
# we just need to link against l2net::l2net

if(L2NET_HAS_SSH)
    add_executable(l2net_remote_benchmark
        remote_benchmark.cpp
    )

    target_link_libraries(l2net_remote_benchmark
        PRIVATE
            l2net::l2net
            l2net_warnings
    )

    # for fetched libssh, we need to add include paths for generated headers
    if(LIBSSH_FROM_FETCH)
        target_include_directories(l2net_remote_benchmark
            PRIVATE
                ${libssh_SOURCE_DIR}/include
                ${libssh_BINARY_DIR}/include
        )
    endif()

    message(STATUS "Building remote benchmark with SSH support")

    install(TARGETS l2net_remote_benchmark
        RUNTIME DESTINATION bin
    )
else()
    message(STATUS "SSH support disabled - l2net_remote_benchmark will not be built")
endif()