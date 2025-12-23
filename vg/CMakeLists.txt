add_library(vg OBJECT
    nanovg/nanovg.c
)

target_include_directories(vg
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/nanovg
)
