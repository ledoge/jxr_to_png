cmake_minimum_required(VERSION 3.26)
project(jxr_to_png)

set(CMAKE_CXX_STANDARD 17)

add_compile_options(/fp:fast /std:c++latest)

add_executable(jxr_to_png main.cpp)
target_link_libraries(jxr_to_png windowscodecs Shlwapi ${PROJECT_SOURCE_DIR}/lib/libpng.lib ${PROJECT_SOURCE_DIR}/lib/zlibstatic.lib)
