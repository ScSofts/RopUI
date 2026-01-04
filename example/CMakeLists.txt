add_subdirectory(log_test)

if (UNIX AND NOT APPLE)
add_subdirectory(linux_loop_core_test)
add_subdirectory(tcp_client_example)
add_subdirectory(rop_hive_epoll_demo)
add_subdirectory(egl_window)
endif()

if(WIN32)
add_subdirectory(iocp_tcp_server_example)
add_subdirectory(win32_window_example)
endif()