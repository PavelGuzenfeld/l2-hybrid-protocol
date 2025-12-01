# apps/CMakeLists.txt
# build targets for benchmark applications

# find libssh for remote benchmarking
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSSH libssh)

# remote node - deployed to remote machines for benchmarking
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

# remote benchmark orchestrator - needs libssh
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
            fmt::fmt
            ${LIBSSH_LIBRARIES}
    )

    target_compile_features(l2net_remote_benchmark PRIVATE cxx_std_23)

    set_target_properties(l2net_remote_benchmark PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

    # add libssh library directories
    target_link_directories(l2net_remote_benchmark
        PRIVATE
            ${LIBSSH_LIBRARY_DIRS}
    )

    message(STATUS "Building remote benchmark with libssh support")
else()
    message(WARNING "libssh not found - l2net_remote_benchmark will not be built")
    message(STATUS "Install libssh: apt install libssh-dev (Debian/Ubuntu)")
endif()

# install targets
install(TARGETS l2net_remote_node
    RUNTIME DESTINATION bin
)

if(LIBSSH_FOUND)
    install(TARGETS l2net_remote_benchmark
        RUNTIME DESTINATION bin
    )
endif()