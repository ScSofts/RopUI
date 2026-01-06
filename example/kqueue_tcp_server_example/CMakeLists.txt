cmake_minimum_required(VERSION 3.14)

project(kqueue_tcp_server_example)

add_executable(kqueue_tcp_server_example main.cc)

target_link_libraries(kqueue_tcp_server_example PUBLIC ropui)
