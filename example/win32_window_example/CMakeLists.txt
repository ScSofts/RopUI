cmake_minimum_required(VERSION 3.14)

project(win32_window_example)

add_executable(win32_window_example main.cc)

target_link_libraries(win32_window_example PUBLIC ropui)