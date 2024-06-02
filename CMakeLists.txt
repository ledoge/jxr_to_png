cmake_minimum_required(VERSION 3.26)
project(jxr_to_png C)

set(CMAKE_C_STANDARD 17)

add_compile_options(-ffast-math)

find_library(PNG_LIBRARY png PATHS ${PROJECT_SOURCE_DIR}/lib REQUIRED NO_DEFAULT_PATH)
find_library(Z_LIBRARY z PATHS ${PROJECT_SOURCE_DIR}/lib REQUIRED NO_DEFAULT_PATH)

add_executable(jxr_to_png main.c)
target_link_libraries(jxr_to_png ${PNG_LIBRARY} ${Z_LIBRARY} "$<$<CONFIG:Release>:-s -static>")
