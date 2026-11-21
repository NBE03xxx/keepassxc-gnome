if(NOT TARGET keepassxc_core OR NOT TARGET text_stream)
    message(FATAL_ERROR "KeePassXC core targets are required.")
endif()
include("${KPXC_GNOME_SOURCE}/cmake/CoreOverrides.cmake")
find_package(PkgConfig REQUIRED)
pkg_check_modules(ADWAITA REQUIRED IMPORTED_TARGET libadwaita-1>=1.6)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
pkg_check_modules(GIO_UNIX REQUIRED IMPORTED_TARGET gio-unix-2.0)
pkg_check_modules(PANGOFT2 REQUIRED IMPORTED_TARGET pangoft2)
find_program(MSGFMT_EXECUTABLE msgfmt REQUIRED)
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)
# Keep KeePass IconID 0..68, reusing the pinned upstream artwork unchanged.
set(database_icons_xml "<gresources><gresource prefix=\"/org/keepassxc/Gnome/database-icons\">")
set(database_icons_files)
foreach(number RANGE 0 68)
    if(number LESS 10)
        set(icon_prefix "0${number}")
    else()
        set(icon_prefix "${number}")
    endif()
    file(GLOB candidates "${CMAKE_SOURCE_DIR}/share/icons/database/C${icon_prefix}_*.svg")
    list(SORT candidates)
    list(GET candidates 0 icon_file)
    list(APPEND database_icons_files "${icon_file}")
    string(APPEND database_icons_xml "<file alias=\"${number}.svg\">${icon_file}</file>")
endforeach()
string(APPEND database_icons_xml "</gresource></gresources>")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/database-icons.xml" "${database_icons_xml}")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/database-icons.cpp"
    COMMAND "${GLIB_COMPILE_RESOURCES}" --generate-source --c-name=database_icons
        "--target=${CMAKE_CURRENT_BINARY_DIR}/database-icons.cpp" "${CMAKE_CURRENT_BINARY_DIR}/database-icons.xml"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/database-icons.xml" ${database_icons_files} VERBATIM)


add_library(gnome_safe_save STATIC "${KPXC_GNOME_SOURCE}/src/gnome/SafeSave.cpp")
target_include_directories(gnome_safe_save PUBLIC "${KPXC_GNOME_SOURCE}/src" "${KPXC_GNOME_SOURCE}/prototype")
target_link_libraries(gnome_safe_save PUBLIC Qt6::Core)
add_library(gnome_session STATIC
    "${KPXC_GNOME_SOURCE}/prototype/CoreSession.cpp"
    "${KPXC_GNOME_SOURCE}/prototype/BrowserAccess.cpp")
target_include_directories(gnome_session PUBLIC "${KPXC_GNOME_SOURCE}/prototype")
target_include_directories(gnome_session SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
target_link_libraries(gnome_session PUBLIC keepassxc_core gnome_safe_save PRIVATE text_stream)

add_library(gnome_remote STATIC
    "${KPXC_GNOME_SOURCE}/src/gnome/RemoteSession.cpp"
    "${KPXC_GNOME_SOURCE}/src/gnome/BrowserIntegration.cpp")
target_include_directories(gnome_remote PUBLIC "${KPXC_GNOME_SOURCE}/src" "${KPXC_GNOME_SOURCE}/prototype")
target_link_libraries(gnome_remote PUBLIC Qt6::Core PRIVATE PkgConfig::GIO_UNIX)
add_executable(keepassxc-gnome-browser-host "${KPXC_GNOME_SOURCE}/src/gnome/browser_native_host.cpp")
target_include_directories(keepassxc-gnome-browser-host PRIVATE "${KPXC_GNOME_SOURCE}/src")
target_link_libraries(keepassxc-gnome-browser-host PRIVATE Qt6::Core)
set_target_properties(keepassxc-gnome-browser-host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_library(gnome_browser_messages STATIC "${CMAKE_SOURCE_DIR}/src/browser/BrowserMessageBuilder.cpp")
target_include_directories(gnome_browser_messages SYSTEM PUBLIC
    "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src" "${BOTAN_INCLUDE_DIR}")
target_link_libraries(gnome_browser_messages PUBLIC Qt6::Core ${BOTAN_LIBRARIES})
add_executable(keepassxc-gnome-worker
    "${KPXC_GNOME_SOURCE}/src/gnome/session_worker.cpp"
    "${KPXC_GNOME_SOURCE}/src/gnome/BrowserSession.cpp")
target_link_libraries(keepassxc-gnome-worker PRIVATE gnome_session gnome_browser_messages)
target_include_directories(keepassxc-gnome-worker SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
set_target_properties(keepassxc-gnome-worker PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")

set(locale_file "${KPXC_GNOME_OUTPUT}/share/locale/ja/LC_MESSAGES/keepassxc-gnome.mo")
add_custom_command(OUTPUT "${locale_file}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${KPXC_GNOME_OUTPUT}/share/locale/ja/LC_MESSAGES"
    COMMAND "${MSGFMT_EXECUTABLE}" --check -o "${locale_file}" "${KPXC_GNOME_SOURCE}/po/ja.po"
    DEPENDS "${KPXC_GNOME_SOURCE}/po/ja.po" VERBATIM)
add_custom_target(gnome_translations DEPENDS "${locale_file}")

add_library(gnome_lifetime STATIC "${CMAKE_CURRENT_BINARY_DIR}/database-icons.cpp" "${KPXC_GNOME_SOURCE}/src/gnome/SessionGuard.cpp" "${KPXC_GNOME_SOURCE}/src/gnome/SecretClipboard.cpp" "${KPXC_GNOME_SOURCE}/src/gnome/NativePicker.cpp")
target_include_directories(gnome_lifetime PUBLIC "${KPXC_GNOME_SOURCE}/src" "${KPXC_GNOME_SOURCE}/prototype")
target_link_libraries(gnome_lifetime PUBLIC PkgConfig::ADWAITA PkgConfig::PANGOFT2)
add_executable(keepassxc-gnome-picker "${KPXC_GNOME_SOURCE}/src/gnome/native_picker_worker.cpp")
target_link_libraries(keepassxc-gnome-picker PRIVATE PkgConfig::GIO)
set_target_properties(keepassxc-gnome-picker PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_executable(keepassxc-gnome "${KPXC_GNOME_SOURCE}/src/main.cpp")
target_link_libraries(keepassxc-gnome PRIVATE gnome_session gnome_remote gnome_lifetime PkgConfig::ADWAITA)
set_target_properties(keepassxc-gnome PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_dependencies(keepassxc-gnome gnome_translations keepassxc-gnome-worker keepassxc-gnome-picker keepassxc-gnome-browser-host)

add_executable(gnome_domain_tests "${KPXC_GNOME_SOURCE}/tests/domain_tests.cpp")
add_executable(gnome_keyfile_tests "${KPXC_GNOME_SOURCE}/tests/keyfile_tests.cpp")
add_executable(gnome_keyfile_ui_probe "${KPXC_GNOME_SOURCE}/tests/keyfile_ui_probe.cpp")
target_include_directories(gnome_keyfile_ui_probe SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
target_link_libraries(gnome_keyfile_ui_probe PRIVATE gnome_session gnome_remote gnome_lifetime PkgConfig::ADWAITA)
set_target_properties(gnome_keyfile_ui_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_dependencies(gnome_keyfile_ui_probe gnome_translations keepassxc-gnome-worker keepassxc-gnome-picker)
target_include_directories(gnome_keyfile_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
target_link_libraries(gnome_keyfile_tests PRIVATE gnome_session gnome_remote)
set_target_properties(gnome_keyfile_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_dependencies(gnome_keyfile_tests keepassxc-gnome-worker)

# Synthetic responder for the isolated browser compatibility test.
add_executable(gnome_browser_connection_probe EXCLUDE_FROM_ALL
    "${KPXC_GNOME_SOURCE}/tests/browser_connection_probe.cpp")
target_link_libraries(gnome_browser_connection_probe PRIVATE gnome_browser_messages)
set_target_properties(gnome_browser_connection_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_executable(gnome_browser_pairing_tests "${KPXC_GNOME_SOURCE}/tests/browser_pairing_tests.cpp")
target_link_libraries(gnome_browser_pairing_tests PRIVATE gnome_session gnome_remote gnome_browser_messages)
set_target_properties(gnome_browser_pairing_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_dependencies(gnome_browser_pairing_tests keepassxc-gnome-worker)

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

add_executable(gnome_known_data_tests "${KPXC_GNOME_SOURCE}/tests/known_data.cpp")
target_include_directories(gnome_known_data_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
target_link_libraries(gnome_known_data_tests PRIVATE gnome_session)
set_target_properties(gnome_known_data_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")

foreach(suite IN ITEMS domain known_data)
    if(suite STREQUAL "domain")
        set(process_test_source "tests/domain_tests.cpp")
    else()
        set(process_test_source "tests/known_data.cpp")
    endif()
    add_executable(gnome_process_${suite}_tests "${KPXC_GNOME_SOURCE}/${process_test_source}")
    target_compile_definitions(gnome_process_${suite}_tests PRIVATE GNOME_PROCESS_TEST)
    target_include_directories(gnome_process_${suite}_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
    target_link_libraries(gnome_process_${suite}_tests PRIVATE gnome_session gnome_remote)
    set_target_properties(gnome_process_${suite}_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
    add_dependencies(gnome_process_${suite}_tests keepassxc-gnome-worker)
endforeach()

add_executable(gnome_process_fault_helper "${KPXC_GNOME_SOURCE}/tests/process_fault_helper.cpp")
target_link_libraries(gnome_process_fault_helper PRIVATE gnome_remote)
set_target_properties(gnome_process_fault_helper PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_executable(gnome_process_tests "${KPXC_GNOME_SOURCE}/tests/process_tests.cpp")
target_include_directories(gnome_process_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
target_link_libraries(gnome_process_tests PRIVATE gnome_session gnome_remote)
set_target_properties(gnome_process_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_dependencies(gnome_process_tests keepassxc-gnome-worker gnome_process_fault_helper)

add_executable(gnome_save_tests "${KPXC_GNOME_SOURCE}/tests/save_tests.cpp")
target_include_directories(gnome_save_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
target_link_libraries(gnome_save_tests PRIVATE gnome_session gnome_remote)
set_target_properties(gnome_save_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_dependencies(gnome_save_tests keepassxc-gnome-worker)

add_executable(gnome_save_ui_probe "${KPXC_GNOME_SOURCE}/tests/save_ui_probe.cpp")
target_link_libraries(gnome_save_ui_probe PRIVATE gnome_session gnome_remote gnome_lifetime PkgConfig::ADWAITA)
set_target_properties(gnome_save_ui_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_dependencies(gnome_save_ui_probe gnome_translations keepassxc-gnome-worker keepassxc-gnome-picker)

add_executable(gnome_lifetime_tests "${KPXC_GNOME_SOURCE}/tests/lifetime_tests.cpp")
target_include_directories(gnome_lifetime_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
target_link_libraries(gnome_lifetime_tests PRIVATE gnome_session gnome_remote gnome_lifetime PkgConfig::GIO_UNIX)
set_target_properties(gnome_lifetime_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_dependencies(gnome_lifetime_tests keepassxc-gnome-worker)

add_executable(gnome_lifetime_ui_probe "${KPXC_GNOME_SOURCE}/tests/lifetime_ui_probe.cpp")
target_link_libraries(gnome_lifetime_ui_probe PRIVATE gnome_session gnome_remote gnome_lifetime PkgConfig::ADWAITA)
if(CMAKE_CXX_FLAGS MATCHES "fsanitize=address")
    # Test-only offset graph audit; Fontconfig is already a GTK dependency.
    target_link_libraries(gnome_lifetime_ui_probe PRIVATE fontconfig)
endif()
set_target_properties(gnome_lifetime_ui_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_dependencies(gnome_lifetime_ui_probe gnome_translations keepassxc-gnome-worker keepassxc-gnome-picker)

add_executable(gnome_gtk_memory_control "${KPXC_GNOME_SOURCE}/tests/gtk_memory_control.cpp")
target_link_libraries(gnome_gtk_memory_control PRIVATE PkgConfig::ADWAITA)
set_target_properties(gnome_gtk_memory_control PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")

add_executable(gnome_font_cache_tests "${KPXC_GNOME_SOURCE}/tests/font_cache_tests.cpp")
target_include_directories(gnome_font_cache_tests PRIVATE "${KPXC_GNOME_SOURCE}/src")
target_link_libraries(gnome_font_cache_tests PRIVATE PkgConfig::ADWAITA PkgConfig::PANGOFT2)
set_target_properties(gnome_font_cache_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")

add_executable(gnome_picker_tests "${KPXC_GNOME_SOURCE}/tests/picker_tests.cpp")
target_link_libraries(gnome_picker_tests PRIVATE gnome_lifetime)
set_target_properties(gnome_picker_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_dependencies(gnome_picker_tests keepassxc-gnome-picker)

add_executable(gnome_polish_ui_probe "${KPXC_GNOME_SOURCE}/tests/polish_ui_probe.cpp")
target_link_libraries(gnome_polish_ui_probe PRIVATE gnome_session gnome_remote gnome_lifetime PkgConfig::ADWAITA)
set_target_properties(gnome_polish_ui_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_dependencies(gnome_polish_ui_probe gnome_translations keepassxc-gnome-worker keepassxc-gnome-picker)

add_executable(gnome_stream_finalization_tests "${KPXC_GNOME_SOURCE}/tests/stream_finalization.cpp")
target_include_directories(gnome_stream_finalization_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src")
target_link_libraries(gnome_stream_finalization_tests PRIVATE keepassxc_core text_stream)
set_target_properties(gnome_stream_finalization_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")

# Development-only notices can be enabled explicitly for validation builds.
option(KPXC_GNOME_DEVELOPMENT_UI "Show development qualification in the frontend" OFF)
add_compile_definitions(KPXC_GNOME_VERSION="${KPXC_GNOME_VERSION}")
foreach(ui IN ITEMS keepassxc-gnome gnome_save_ui_probe gnome_lifetime_ui_probe gnome_polish_ui_probe)
    target_compile_definitions(${ui} PRIVATE KPXC_GNOME_DEVELOPMENT_UI=$<BOOL:${KPXC_GNOME_DEVELOPMENT_UI}>)
endforeach()
add_executable(gnome_recent_tests "${KPXC_GNOME_SOURCE}/tests/recent_tests.cpp")
target_include_directories(gnome_recent_tests PRIVATE "${KPXC_GNOME_SOURCE}/src")
target_link_libraries(gnome_recent_tests PRIVATE PkgConfig::GIO)
set_target_properties(gnome_recent_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
foreach(ui IN ITEMS gnome_welcome_ui_probe gnome_public_welcome_ui_probe)
    add_executable(${ui} "${KPXC_GNOME_SOURCE}/tests/welcome_ui_probe.cpp")
    target_link_libraries(${ui} PRIVATE gnome_session gnome_remote gnome_lifetime PkgConfig::ADWAITA)
    set_target_properties(${ui} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
    add_dependencies(${ui} gnome_translations keepassxc-gnome-worker keepassxc-gnome-picker)
endforeach()
target_compile_definitions(gnome_welcome_ui_probe PRIVATE KPXC_GNOME_DEVELOPMENT_UI=1)
target_compile_definitions(gnome_public_welcome_ui_probe PRIVATE KPXC_GNOME_DEVELOPMENT_UI=0)

add_executable(gnome_close_ui_probe "${KPXC_GNOME_SOURCE}/tests/close_ui_probe.cpp")
target_link_libraries(gnome_close_ui_probe PRIVATE gnome_session gnome_remote gnome_lifetime PkgConfig::ADWAITA)
target_compile_definitions(gnome_close_ui_probe PRIVATE KPXC_GNOME_DEVELOPMENT_UI=$<BOOL:${KPXC_GNOME_DEVELOPMENT_UI}>)
set_target_properties(gnome_close_ui_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_dependencies(gnome_close_ui_probe gnome_translations keepassxc-gnome-worker keepassxc-gnome-picker)

add_executable(gnome_default_keys_ui_probe "${KPXC_GNOME_SOURCE}/tests/default_keys_ui_probe.cpp")
target_link_libraries(gnome_default_keys_ui_probe PRIVATE gnome_session gnome_remote gnome_lifetime PkgConfig::ADWAITA)
target_compile_definitions(gnome_default_keys_ui_probe PRIVATE KPXC_GNOME_DEVELOPMENT_UI=$<BOOL:${KPXC_GNOME_DEVELOPMENT_UI}>)
set_target_properties(gnome_default_keys_ui_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
add_dependencies(gnome_default_keys_ui_probe gnome_translations keepassxc-gnome-worker keepassxc-gnome-picker)
