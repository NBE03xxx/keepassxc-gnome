if(NOT TARGET keepassxc_core OR NOT TARGET text_stream)
    message(FATAL_ERROR "The Phase 2 probe requires KeePassXC's keepassxc_core and text_stream targets.")
endif()

add_library(phase2_session STATIC "${CMAKE_CURRENT_LIST_DIR}/CoreSession.cpp")
target_include_directories(phase2_session SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
target_link_libraries(phase2_session PUBLIC keepassxc_core PRIVATE text_stream)

add_executable(phase2_core_probe "${CMAKE_CURRENT_LIST_DIR}/core_probe.cpp")
target_include_directories(phase2_core_probe SYSTEM PRIVATE
    "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
target_link_libraries(phase2_core_probe PRIVATE phase2_session)
add_dependencies(phase2_core_probe keepassxc-cli)
add_test(NAME phase2_core_probe COMMAND phase2_core_probe "$<TARGET_FILE:keepassxc-cli>")

option(KPXC_PHASE2_GTK "Build the GTK fixture probe" OFF)
if(KPXC_PHASE2_GTK)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1)
    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
    add_executable(phase2_file_picker "${CMAKE_CURRENT_LIST_DIR}/file_picker.cpp")
    target_link_libraries(phase2_file_picker PRIVATE PkgConfig::GIO)
    add_executable(phase2_gtk_probe "${CMAKE_CURRENT_LIST_DIR}/gtk_probe.cpp")
    target_link_libraries(phase2_gtk_probe PRIVATE phase2_session PkgConfig::ADWAITA)
    add_dependencies(phase2_gtk_probe phase2_file_picker)
endif()
