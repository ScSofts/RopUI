cmake_minimum_required(VERSION 3.14)

project(linux_loop_core_test)

add_executable(linux_loop_core_test main.cc)

target_link_libraries(linux_loop_core_test PUBLIC ropui)