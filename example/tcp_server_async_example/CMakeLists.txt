cmake_minimum_required(VERSION 3.14)

project(tcp_server_async_example)

add_library(tcp_server_async_example_lib STATIC
    library/src/asyncnet.cc
    library/src/server_runner.cc
)
target_include_directories(tcp_server_async_example_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/library/include
)
target_link_libraries(tcp_server_async_example_lib PUBLIC ropui)

add_executable(tcp_server_async_example
    app.cc
)

target_link_libraries(tcp_server_async_example PRIVATE tcp_server_async_example_lib)
