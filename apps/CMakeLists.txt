# apps/CMakeLists.txt
# build targets for benchmark applications

# find libssh for remote benchmarking
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSSH libssh)

# remote node - deployed to remote machines for benchmarking
# this one can be built with musl/static for maximum portability
add_executable(l2net_remote_node
    remote_node.cpp
)

target_link_libraries(l2net_remote_node
    PRIVATE
        l2net::l2net
        fmt::fmt
)

target_compile_features(l2net_remote_node PRIVATE cxx_std_23)

set_target_properties(l2net_remote_node PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# apply static build settings if enabled
if(TARGET l2net_static_build)
    target_link_libraries(l2net_remote_node PRIVATE l2net_static_build)
endif()

# remote benchmark orchestrator - needs libssh
# NOTE: for static builds, we skip this because libssh+openssl static linking is a nightmare
# the remote_node (which is what you deploy) builds fine statically
if(LIBSSH_FOUND AND NOT L2NET_USE_MUSL AND NOT L2NET_STATIC)
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
            fmt::fmt
            ${LIBSSH_LIBRARIES}
    )

    target_compile_features(l2net_remote_benchmark PRIVATE cxx_std_23)

    set_target_properties(l2net_remote_benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    target_link_directories(l2net_remote_benchmark
        PRIVATE
            ${LIBSSH_LIBRARY_DIRS}
    )

    message(STATUS "Building remote benchmark with libssh support")
    
    install(TARGETS l2net_remote_benchmark
        RUNTIME DESTINATION bin
    )
elseif(L2NET_USE_MUSL OR L2NET_STATIC)
    message(STATUS "Skipping l2net_remote_benchmark (static builds use remote_node only)")
elseif(NOT LIBSSH_FOUND)
    message(WARNING "libssh not found - l2net_remote_benchmark will not be built")
    message(STATUS "Install libssh: apt install libssh-dev (Debian/Ubuntu)")
endif()

# install targets
install(TARGETS l2net_remote_node
    RUNTIME DESTINATION bin
)