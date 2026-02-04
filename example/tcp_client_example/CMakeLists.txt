cmake_minimum_required(VERSION 3.14)

project(tcp_client_example)

add_executable(tcp_client_example main.cc)

target_link_libraries(tcp_client_example PUBLIC ropui)
