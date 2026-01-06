cmake_minimum_required(VERSION 3.14)

project(cocoa_metal_bad_apple)

# Create executable
add_executable(cocoa_metal_bad_apple main.mm bad_apple_data.h)

# # Set include directories
# target_include_directories(cocoa_metal_bad_apple PRIVATE
#     ${CMAKE_CURRENT_SOURCE_DIR}
#     ${CMAKE_SOURCE_DIR}
#     ${CMAKE_SOURCE_DIR}/utils
# )

# Link libraries
target_link_libraries(cocoa_metal_bad_apple PUBLIC ropui)

# Find and link Cocoa and Metal frameworks
find_library(COCOA_LIBRARY Cocoa REQUIRED)
find_library(METAL_LIBRARY Metal REQUIRED)
find_library(METALKIT_LIBRARY MetalKit REQUIRED)
find_library(QUARTZCORE_LIBRARY QuartzCore REQUIRED)

target_link_libraries(cocoa_metal_bad_apple
    PRIVATE
    ${COCOA_LIBRARY}
    ${METAL_LIBRARY}
    ${METALKIT_LIBRARY}
    ${QUARTZCORE_LIBRARY}
)

# Set Objective-C++ properties
set_target_properties(cocoa_metal_bad_apple PROPERTIES
    XCODE_ATTRIBUTE_CLANG_ENABLE_OBJC_ARC YES
)
