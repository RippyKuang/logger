cmake_minimum_required(VERSION 3.16)
project(rtplot VERSION 1.0.0 LANGUAGES CXX C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(RTPLOT_BUILD_GUI "Build Qt5 widgets-based viewer" ON)
option(RTPLOT_BUILD_PYTHON "Build pybind11 Python module" ON)
option(RTPLOT_BUILD_DEMOS "Build demo applications" ON)
option(RTPLOT_BUILD_TESTS "Build unit tests" ON)
option(RTPLOT_USE_QT6 "Prefer Qt6 over Qt5" OFF)

set(RTPLOT_PUBLIC_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/include)
set(CORE_SOURCES
    src/logger.cpp
    src/storage.cpp
    src/shm_ipc.cpp
    src/udp.cpp
    src/export.cpp
)

add_library(rtplot_core STATIC ${CORE_SOURCES})
target_include_directories(rtplot_core PUBLIC
  $<BUILD_INTERFACE:${RTPLOT_PUBLIC_INCLUDES}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(rtplot_core PUBLIC RTPLOT_VERSION="${PROJECT_VERSION}")
target_compile_options(rtplot_core PRIVATE -Wall -Wextra)
set_target_properties(rtplot_core PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  EXPORT_NAME core)
find_package(Threads REQUIRED)
target_link_libraries(rtplot_core PUBLIC Threads::Threads)
# POSIX shared memory / sockets
target_link_libraries(rtplot_core PUBLIC rt)

# ---------------------------------------------------------------------------
# Qt GUI
# ---------------------------------------------------------------------------
if(RTPLOT_BUILD_GUI)
  if(RTPLOT_USE_QT6)
    find_package(Qt6 QUIET COMPONENTS Widgets)
    set(RTPLOT_QT_FOUND Qt6_FOUND)
  else()
    find_package(Qt5 QUIET COMPONENTS Widgets)
    set(RTPLOT_QT_FOUND Qt5_FOUND)
  endif()
  if(NOT RTPLOT_QT_FOUND AND RTPLOT_USE_QT6)
    find_package(Qt5 QUIET COMPONENTS Widgets)
    set(RTPLOT_QT_FOUND Qt5_FOUND)
  endif()
  if(RTPLOT_QT_FOUND)
    set(GUI_SOURCES gui/PlotWidget.cpp gui/TrajectoryWidget.cpp gui/MainWindow.cpp)
    add_library(rtplot_gui STATIC ${GUI_SOURCES})
    if(RTPLOT_USE_QT6 AND Qt6_FOUND)
      target_link_libraries(rtplot_gui PUBLIC Qt6::Widgets)
      target_compile_definitions(rtplot_gui PUBLIC RTPLOT_QT6=1)
      set(RTPLOT_QT_MAJOR 6)
    else()
      target_link_libraries(rtplot_gui PUBLIC Qt5::Widgets)
      set(RTPLOT_QT_MAJOR 5)
    endif()
    set_target_properties(rtplot_gui PROPERTIES EXPORT_NAME gui)
    target_link_libraries(rtplot_gui PUBLIC rtplot_core)
    target_include_directories(rtplot_gui PUBLIC
      $<BUILD_INTERFACE:${RTPLOT_PUBLIC_INCLUDES}>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/gui>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/rtplot/gui>)
    target_compile_options(rtplot_gui PRIVATE -Wall -Wextra)
  else()
    message(WARNING "Qt not found: GUI disabled")
  endif()
endif()

# ---------------------------------------------------------------------------
# Demos
# ---------------------------------------------------------------------------
if(RTPLOT_BUILD_DEMOS)
  add_executable(rtplot_example examples/full_demo.cpp)
  target_link_libraries(rtplot_example PRIVATE rtplot_core)

  add_executable(shm_recorder examples/shm_recorder.cpp)
  target_link_libraries(shm_recorder PRIVATE rtplot_core)

  if(TARGET rtplot_gui)
    add_executable(viewer apps/viewer.cpp)
    target_link_libraries(viewer PRIVATE rtplot_gui rtplot_core)

    add_executable(shm_viewer examples/shm_viewer.cpp)
    target_link_libraries(shm_viewer PRIVATE rtplot_gui rtplot_core)
  endif()
endif()

# ---------------------------------------------------------------------------
# Python bindings
# ---------------------------------------------------------------------------
if(RTPLOT_BUILD_PYTHON)
  find_package(Python3 QUIET COMPONENTS Interpreter Development)
  if(Python3_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c
                    "import pybind11; print(pybind11.get_include())"
                    OUTPUT_VARIABLE PYBIND11_INCLUDE
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
    find_package(pybind11 QUIET)
    if(NOT pybind11_FOUND)
      execute_process(COMMAND ${Python3_EXECUTABLE} -m pybind11 --includes
                      OUTPUT_VARIABLE PYBIND11_FLAGS
                      OUTPUT_STRIP_TRAILING_WHITESPACE
                      ERROR_QUIET)
      if(PYBIND11_FLAGS MATCHES "-I([^ ]+)")
        set(PYBIND11_INCLUDE ${CMAKE_MATCH_1})
      endif()
      if(NOT PYBIND11_INCLUDE)
        find_path(PYBIND11_INCLUDE pybind11/pybind11.h PATHS /usr/include /usr/local/include)
      endif()
    endif()
    if(pybind11_FOUND)
      pybind11_add_module(rtplot bindings/python.cpp)
      target_link_libraries(rtplot PRIVATE rtplot_core)
      target_compile_options(rtplot PRIVATE -Wall -Wextra)
    elseif(PYBIND11_INCLUDE)
      Python3_add_library(rtplot MODULE bindings/python.cpp)
      target_include_directories(rtplot PRIVATE ${PYBIND11_INCLUDE})
      target_link_libraries(rtplot PRIVATE rtplot_core)
      target_compile_options(rtplot PRIVATE -Wall -Wextra)
    else()
      message(WARNING "pybind11 not found: Python module disabled (pip install pybind11)")
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(RTPLOT_BUILD_TESTS)
  enable_testing()
  add_executable(test_core tests/test_core.cpp)
  target_link_libraries(test_core PRIVATE rtplot_core)
  add_test(NAME test_core COMMAND test_core)
endif()

# Convenience uninstall target: remove every file recorded by cmake --install.
if(NOT TARGET uninstall)
  add_custom_target(uninstall
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/scripts/uninstall.sh
                    ${CMAKE_BINARY_DIR}/install_manifest.txt
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Uninstalling rtplot files listed in install_manifest.txt")
endif()

# ---------------------------------------------------------------------------
# Installation and CMake package export
# ---------------------------------------------------------------------------
set(RTPLOT_GUI_INSTALLED OFF)
if(TARGET rtplot_gui)
  set(RTPLOT_GUI_INSTALLED ON)
endif()

set(RTPLOT_INSTALL_TARGETS rtplot_core)
if(TARGET rtplot_gui)
  list(APPEND RTPLOT_INSTALL_TARGETS rtplot_gui)
endif()
install(TARGETS ${RTPLOT_INSTALL_TARGETS}
        EXPORT rtplotTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(TARGET viewer)
  install(TARGETS viewer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TARGET shm_recorder)
  install(TARGETS shm_recorder RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TARGET shm_viewer)
  install(TARGETS shm_viewer RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(TARGET rtplot)
  install(TARGETS rtplot LIBRARY DESTINATION ${Python3_SITEARCH})
endif()

install(DIRECTORY include/rtplot DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if(TARGET rtplot_gui)
  install(DIRECTORY gui/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rtplot/gui
          FILES_MATCHING PATTERN "*.hpp")
endif()

configure_package_config_file(
  cmake/rtplotConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/rtplotConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rtplot)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/rtplotConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(EXPORT rtplotTargets
        FILE rtplotTargets.cmake
        NAMESPACE rtplot::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rtplot)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/rtplotConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/rtplotConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rtplot)
