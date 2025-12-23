cmake_minimum_required(VERSION 3.14)

project(egl_window LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(EXECUTE_NAME egl_window)
set(NANOUI_BUILD_WAYLAND ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
pkg_check_modules(WAYLAND_EGL REQUIRED wayland-egl)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLESv2 REQUIRED glesv2)

# 源文件
add_executable(${EXECUTE_NAME}
    main.cpp
    egl_backend.cpp
)

target_include_directories(${EXECUTE_NAME} PRIVATE
    .
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
    ${EGL_INCLUDE_DIRS}
    ${GLES2_INCLUDE_DIRS}
    ${WAYLAND_EGL_INCLUDE_DIRS}
)

# 链接
target_link_libraries(${EXECUTE_NAME} PRIVATE
    ropui
    ${WAYLAND_CLIENT_LIBRARIES}
    ${EGL_LIBRARIES}
    ${GLES2_LIBRARIES}
    ${WAYLAND_EGL_LIBRARIES}
)
