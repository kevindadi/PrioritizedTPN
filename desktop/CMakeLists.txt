find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

set(PTPN_DESKTOP_SOURCES
    src/main.cpp
    src/main_window.cpp
    src/main_window.h
    src/analysis_service.cpp
    src/analysis_service.h
    src/dot_parser.cpp
    src/dot_parser.h
    src/petri_view.cpp
    src/petri_view.h
    src/state_graph_view.cpp
    src/state_graph_view.h
    src/metrics_panel.cpp
    src/metrics_panel.h
    src/app_model.h
    src/theme.h
)

qt_add_executable(ptpn_desktop MANUAL_FINALIZATION ${PTPN_DESKTOP_SOURCES})
set_target_properties(ptpn_desktop PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE FALSE
)
target_link_libraries(ptpn_desktop PRIVATE Qt6::Widgets)

if(WIN32)
  target_link_libraries(ptpn_desktop PRIVATE dwmapi)
  target_compile_definitions(ptpn_desktop PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(TARGET ptpn)
  target_compile_definitions(ptpn_desktop PRIVATE PTPN_CLI_PATH="$<TARGET_FILE:ptpn>")
  target_compile_definitions(ptpn_desktop PRIVATE PTPN_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  add_dependencies(ptpn_desktop ptpn)
else()
  target_compile_definitions(ptpn_desktop PRIVATE PTPN_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()

# Keep the build tree directly runnable: copy the Qt runtime, platform plugin and
# the MinGW runtime next to the executable. This avoids requiring PATH tweaks
# when launching from CLion or Explorer.
if(WIN32)
  get_target_property(_qt_core_location Qt6::Core IMPORTED_LOCATION_DEBUG)
  if(NOT _qt_core_location)
    get_target_property(_qt_core_location Qt6::Core IMPORTED_LOCATION_RELEASE)
  endif()
  if(_qt_core_location)
    get_filename_component(_qt_bin_dir "${_qt_core_location}" DIRECTORY)
    get_filename_component(_qt_prefix_dir "${_qt_bin_dir}" DIRECTORY)
    file(GLOB _qt_runtime_dlls "${_qt_bin_dir}/*.dll")

    if(_qt_runtime_dlls)
      add_custom_command(TARGET ptpn_desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_qt_runtime_dlls}
                "$<TARGET_FILE_DIR:ptpn_desktop>"
        COMMENT "Deploying Qt runtime next to ptpn_desktop"
        VERBATIM)
    endif()

    set(_qt_plugins_dir "${_qt_prefix_dir}/Qt6/plugins")
    if(NOT EXISTS "${_qt_plugins_dir}")
      set(_qt_plugins_dir "${_qt_prefix_dir}/plugins")
    endif()
    foreach(_plugin_dir platforms styles imageformats)
      if(EXISTS "${_qt_plugins_dir}/${_plugin_dir}")
        add_custom_command(TARGET ptpn_desktop POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_directory
                  "${_qt_plugins_dir}/${_plugin_dir}"
                  "$<TARGET_FILE_DIR:ptpn_desktop>/${_plugin_dir}"
          COMMENT "Deploying Qt ${_plugin_dir} plugins"
          VERBATIM)
      endif()
    endforeach()

    get_filename_component(_mingw_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_runtime_destinations "$<TARGET_FILE_DIR:ptpn_desktop>")
    if(TARGET ptpn)
      list(APPEND _runtime_destinations "$<TARGET_FILE_DIR:ptpn>")
    endif()
    foreach(_runtime libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll libssp-0.dll)
      if(EXISTS "${_mingw_bin_dir}/${_runtime}")
        foreach(_destination IN LISTS _runtime_destinations)
          add_custom_command(TARGET ptpn_desktop POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_mingw_bin_dir}/${_runtime}" "${_destination}"
            COMMENT "Deploying MinGW runtime ${_runtime}"
            VERBATIM)
        endforeach()
      endif()
    endforeach()
  endif()
endif()

qt_finalize_executable(ptpn_desktop)
