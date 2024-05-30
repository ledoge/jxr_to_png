cmake_minimum_required(VERSION 3.26)
project(jxr_to_png C)

set(CMAKE_C_STANDARD 17)

add_compile_options(-ffast-math -DSPNG_USE_MINIZ -DSPNG_STATIC)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_executable(jxr_to_png main.c spng.c miniz.c)
target_link_libraries(jxr_to_png "$<$<CONFIG:Release>:-s -static>")
