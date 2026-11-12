if(NOT TARGET keepassxc_core OR NOT TARGET text_stream)
    message(FATAL_ERROR "KeePassXC core targets are required.")
endif()
include("${KPXC_GNOME_SOURCE}/cmake/CoreOverrides.cmake")
find_package(PkgConfig REQUIRED)
pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1>=1.5)
find_program(MSGFMT_EXECUTABLE msgfmt REQUIRED)

add_library(gnome_session STATIC "${KPXC_GNOME_SOURCE}/prototype/CoreSession.cpp")
target_include_directories(gnome_session PUBLIC "${KPXC_GNOME_SOURCE}/prototype")
target_include_directories(gnome_session SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
target_link_libraries(gnome_session PUBLIC keepassxc_core PRIVATE text_stream)

set(locale_file "${KPXC_GNOME_OUTPUT}/share/locale/ja/LC_MESSAGES/keepassxc-gnome.mo")
add_custom_command(OUTPUT "${locale_file}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${KPXC_GNOME_OUTPUT}/share/locale/ja/LC_MESSAGES"
    COMMAND "${MSGFMT_EXECUTABLE}" --check -o "${locale_file}" "${KPXC_GNOME_SOURCE}/po/ja.po"
    DEPENDS "${KPXC_GNOME_SOURCE}/po/ja.po" VERBATIM)
add_custom_target(gnome_translations DEPENDS "${locale_file}")

add_executable(keepassxc-gnome "${KPXC_GNOME_SOURCE}/src/main.cpp")
target_link_libraries(keepassxc-gnome PRIVATE gnome_session PkgConfig::ADWAITA)
set_target_properties(keepassxc-gnome PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_dependencies(keepassxc-gnome gnome_translations)

add_executable(gnome_domain_tests "${KPXC_GNOME_SOURCE}/tests/domain_tests.cpp")
target_include_directories(gnome_domain_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
target_link_libraries(gnome_domain_tests PRIVATE gnome_session)
set_target_properties(gnome_domain_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")

add_executable(gnome_compatibility_tests "${KPXC_GNOME_SOURCE}/prototype/core_probe.cpp")
target_include_directories(gnome_compatibility_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
target_link_libraries(gnome_compatibility_tests PRIVATE gnome_session)
set_target_properties(gnome_compatibility_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")

add_executable(gnome_reader_finalization_tests "${KPXC_GNOME_SOURCE}/tests/reader_finalization.cpp")
target_include_directories(gnome_reader_finalization_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
target_link_libraries(gnome_reader_finalization_tests PRIVATE gnome_session)
set_target_properties(gnome_reader_finalization_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
include("${KPXC_GNOME_SOURCE}/cmake/CoreTests.cmake")

add_executable(gnome_xml_model_tests "${KPXC_GNOME_SOURCE}/tests/xml_model_policy.cpp")
target_include_directories(gnome_xml_model_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
target_link_libraries(gnome_xml_model_tests PRIVATE gnome_session)
set_target_properties(gnome_xml_model_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
