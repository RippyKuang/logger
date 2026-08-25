cmake_minimum_required(VERSION 3.16)
project(rtplot_examples LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Installed rtplot package. Use CMAKE_PREFIX_PATH when it was installed
# into a custom prefix, e.g.:
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=$HOME/.local
find_package(rtplot CONFIG REQUIRED)

add_executable(rtplot_example full_demo.cpp)
target_link_libraries(rtplot_example PRIVATE rtplot::core)

add_executable(shm_recorder shm_recorder.cpp)
target_link_libraries(shm_recorder PRIVATE rtplot::core)

if(TARGET rtplot::gui)
  add_executable(shm_viewer shm_viewer.cpp)
  target_link_libraries(shm_viewer PRIVATE rtplot::gui)
endif()
