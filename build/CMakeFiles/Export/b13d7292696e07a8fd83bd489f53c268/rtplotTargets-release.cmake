#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "rtplot::core" for configuration "Release"
set_property(TARGET rtplot::core APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(rtplot::core PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/librtplot_core.a"
  )

list(APPEND _cmake_import_check_targets rtplot::core )
list(APPEND _cmake_import_check_files_for_rtplot::core "${_IMPORT_PREFIX}/lib/librtplot_core.a" )

# Import target "rtplot::gui" for configuration "Release"
set_property(TARGET rtplot::gui APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(rtplot::gui PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "CXX"
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/librtplot_gui.a"
  )

list(APPEND _cmake_import_check_targets rtplot::gui )
list(APPEND _cmake_import_check_files_for_rtplot::gui "${_IMPORT_PREFIX}/lib/librtplot_gui.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
