cmake_minimum_required(VERSION 3.14)

project(tcp_server_example)

add_executable(tcp_server_example main.cc)
add_executable(tcp_server_multithread_example main_multithread.cc)

target_link_libraries(tcp_server_example PUBLIC ropui)
target_link_libraries(tcp_server_multithread_example PUBLIC ropui)

# `burst_clients.py` is a helper script to stress-test the server.
