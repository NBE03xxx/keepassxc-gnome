cmake_minimum_required(VERSION 3.24)
project(KeePassXcGnome VERSION 0.5.0 LANGUAGES NONE)

option(KPXC_GNOME_DEVELOPMENT_UI "Show development qualification in the frontend" OFF)

include(ExternalProject)
set(KEEPASSXC_SOURCE_DIR "${CMAKE_SOURCE_DIR}/upstream/keepassxc" CACHE PATH "Unmodified pinned KeePassXC checkout")
if(NOT EXISTS "${KEEPASSXC_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Fetch the pinned upstream source with tools/fetch-upstream.sh first.")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/verify-upstream.py" "${KEEPASSXC_SOURCE_DIR}"
    OUTPUT_VARIABLE upstream_commit OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
ExternalProject_Add(upstream_reference_cli
    SOURCE_DIR "${KEEPASSXC_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/reference"
    DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
    CMAKE_ARGS
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo" "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
        "-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=${CMAKE_SOURCE_DIR}/cmake/AttachReference.cmake"
        "-DKPXC_GNOME_SOURCE=${CMAKE_SOURCE_DIR}" -DCMAKE_SKIP_RPATH=ON
        -DKPXC_MINIMAL=ON -DKPXC_FEATURE_NETWORK=OFF -DKPXC_FEATURE_DOCS=OFF
        -DWITH_X11=OFF -DWITH_TESTS=OFF -DWITH_GUI_TESTS=OFF
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target keepassxc-cli gnome_reference_fixture gnome_reference_stream_tests gnome_reference_keyfile_fixture
    BUILD_ALWAYS TRUE
    INSTALL_COMMAND "")
ExternalProject_Add(gnome_native
    DEPENDS upstream_reference_cli
    SOURCE_DIR "${KEEPASSXC_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/native"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CMAKE_ARGS
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
        "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
        "-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=${CMAKE_SOURCE_DIR}/cmake/AttachFrontend.cmake"
        "-DKPXC_GNOME_SOURCE=${CMAKE_SOURCE_DIR}"
        "-DKPXC_GNOME_OUTPUT=${CMAKE_BINARY_DIR}"
        "-DKPXC_GNOME_VERSION=${PROJECT_VERSION}"
        "-DKPXC_GNOME_DEVELOPMENT_UI=${KPXC_GNOME_DEVELOPMENT_UI}"
        -DCMAKE_SKIP_RPATH=ON -DKPXC_MINIMAL=ON -DKPXC_FEATURE_NETWORK=OFF
        -DKPXC_FEATURE_DOCS=OFF -DWITH_X11=OFF -DWITH_TESTS=ON -DWITH_GUI_TESTS=OFF
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target keepassxc-gnome keepassxc-gnome-auto-type gnome_domain_tests gnome_compatibility_tests gnome_reader_finalization_tests
        gnome_upstream_cipher_tests gnome_upstream_hmac_tests gnome_upstream_hashed_tests gnome_browser_pairing_tests gnome_auto_type_tests gnome_totp_tests
        gnome_upstream_kdbx3_tests gnome_upstream_kdbx4_tests gnome_xml_model_tests gnome_known_data_tests
        gnome_process_domain_tests gnome_process_known_data_tests gnome_process_tests gnome_save_tests gnome_save_ui_probe gnome_lifetime_tests gnome_lifetime_ui_probe gnome_picker_tests gnome_polish_ui_probe gnome_stream_finalization_tests gnome_font_cache_tests gnome_recent_tests gnome_welcome_ui_probe gnome_public_welcome_ui_probe gnome_close_ui_probe gnome_default_keys_ui_probe gnome_keyfile_tests gnome_keyfile_ui_probe
    BUILD_ALWAYS TRUE
    INSTALL_COMMAND "")
enable_testing()
add_test(NAME gnome_browser_pairing_tests
    COMMAND "${CMAKE_BINARY_DIR}/bin/gnome_browser_pairing_tests" "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome-worker")
set_tests_properties(gnome_browser_pairing_tests PROPERTIES TIMEOUT 45)
add_test(NAME gnome_auto_type_tests COMMAND "${CMAKE_COMMAND}" -E env
    "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/auto-type-profile/config"
    "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/auto-type-profile/cache"
    "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/auto-type-profile/state"
    "${CMAKE_BINARY_DIR}/bin/gnome_auto_type_tests" "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome-worker")
set_tests_properties(gnome_auto_type_tests PROPERTIES TIMEOUT 45)
add_test(NAME gnome_totp_tests
    COMMAND "${CMAKE_BINARY_DIR}/bin/gnome_totp_tests" "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome-worker")
set_tests_properties(gnome_totp_tests PROPERTIES TIMEOUT 45)
add_test(NAME gnome_keyfile_tests COMMAND "${CMAKE_COMMAND}" -E env
    "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/keyfile-profile/config"
    "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/keyfile-profile/cache"
    "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/keyfile-profile/state"
    "${CMAKE_BINARY_DIR}/bin/gnome_keyfile_tests"
    "${CMAKE_BINARY_DIR}/reference/gnome_reference_keyfile_fixture"
    "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome-worker")
set_tests_properties(gnome_keyfile_tests PROPERTIES TIMEOUT 120)
add_test(NAME gnome_domain_tests COMMAND "${CMAKE_COMMAND}" -E env
    "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-profile/config"
    "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/test-profile/cache"
    "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-profile/state"
    "${CMAKE_BINARY_DIR}/bin/gnome_domain_tests")
add_test(NAME gnome_compatibility_tests COMMAND "${CMAKE_COMMAND}" -E env
    "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-profile/config"
    "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/test-profile/cache"
    "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-profile/state"
    "${CMAKE_BINARY_DIR}/bin/gnome_compatibility_tests"
    "${CMAKE_BINARY_DIR}/reference/src/cli/keepassxc-cli"
    "${KEEPASSXC_SOURCE_DIR}/tests/data")
add_test(NAME gnome_known_data_tests COMMAND "${CMAKE_COMMAND}" -E env
    "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-profile/config"
    "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/test-profile/cache"
    "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-profile/state"
    "${CMAKE_BINARY_DIR}/bin/gnome_known_data_tests"
    "${CMAKE_BINARY_DIR}/reference/gnome_reference_fixture")
set_tests_properties(gnome_known_data_tests PROPERTIES TIMEOUT 120)
foreach(suite IN ITEMS domain known_data)
    add_test(NAME gnome_process_${suite}_tests COMMAND "${CMAKE_COMMAND}" -E env
        "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-profile/config"
        "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/test-profile/cache"
        "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-profile/state"
        "${CMAKE_BINARY_DIR}/bin/gnome_process_${suite}_tests"
        "${CMAKE_BINARY_DIR}/reference/gnome_reference_fixture"
        "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome-worker")
    set_tests_properties(gnome_process_${suite}_tests PROPERTIES TIMEOUT 120)
endforeach()
add_test(NAME gnome_process_tests COMMAND "${CMAKE_COMMAND}" -E env
    "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-profile/config"
    "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/test-profile/cache"
    "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-profile/state"
    "${CMAKE_BINARY_DIR}/bin/gnome_process_tests"
    "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome-worker"
    "${CMAKE_BINARY_DIR}/bin/gnome_process_fault_helper")
set_tests_properties(gnome_process_tests PROPERTIES TIMEOUT 45)
add_test(NAME gnome_locale_ja COMMAND "${CMAKE_COMMAND}" -E env
    LC_ALL=ja_JP.UTF-8 LANGUAGE=ja "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome" --language-check)
set_tests_properties(gnome_locale_ja PROPERTIES PASS_REGULAR_EXPRESSION "準備完了")
add_test(NAME gnome_locale_fallback COMMAND "${CMAKE_COMMAND}" -E env
    LC_ALL=C.UTF-8 LANGUAGE=de "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome" --language-check)
set_tests_properties(gnome_locale_fallback PROPERTIES PASS_REGULAR_EXPRESSION "Ready")
add_test(NAME gnome_reader_finalization_tests COMMAND "${CMAKE_COMMAND}" -E env
    "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-profile/config"
    "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/test-profile/cache"
    "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-profile/state"
    "${CMAKE_BINARY_DIR}/bin/gnome_reader_finalization_tests")
set_tests_properties(gnome_reader_finalization_tests PROPERTIES TIMEOUT 60)
add_test(NAME gnome_reader_resource_tests COMMAND "${CMAKE_COMMAND}" -E env
    "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-profile/config"
    "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/test-profile/cache"
    "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-profile/state"
    "${CMAKE_BINARY_DIR}/bin/gnome_reader_finalization_tests" --policy)
set_tests_properties(gnome_reader_resource_tests PROPERTIES TIMEOUT 30)
add_test(NAME gnome_xml_model_tests COMMAND "${CMAKE_COMMAND}" -E env
    "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-profile/config"
    "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/test-profile/cache"
    "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-profile/state"
    "${CMAKE_BINARY_DIR}/bin/gnome_xml_model_tests")
set_tests_properties(gnome_xml_model_tests PROPERTIES TIMEOUT 45)
foreach(suite IN ITEMS cipher hmac hashed kdbx3 kdbx4)
    add_test(NAME "gnome_upstream_${suite}_tests" COMMAND "${CMAKE_COMMAND}" -E env
        "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-profile/config"
        "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/test-profile/cache"
        "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-profile/state"
        LANG=en_US.UTF-8 "${CMAKE_BINARY_DIR}/bin/gnome_upstream_${suite}_tests")
    set_tests_properties("gnome_upstream_${suite}_tests" PROPERTIES TIMEOUT 180)
endforeach()

add_test(NAME gnome_save_tests COMMAND "${CMAKE_COMMAND}" -E env
    "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-profile/config"
    "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/test-profile/cache"
    "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-profile/state"
    "${CMAKE_BINARY_DIR}/bin/gnome_save_tests" "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome-worker")
set_tests_properties(gnome_save_tests PROPERTIES TIMEOUT 120)

add_test(NAME gnome_lifetime_tests COMMAND "${CMAKE_COMMAND}" -E env
    "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-profile/config"
    "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/test-profile/cache"
    "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-profile/state"
    "${CMAKE_BINARY_DIR}/bin/gnome_lifetime_tests" "${CMAKE_BINARY_DIR}/bin/keepassxc-gnome-worker"
    "${CMAKE_BINARY_DIR}/reference/src/cli/keepassxc-cli")
set_tests_properties(gnome_lifetime_tests PROPERTIES TIMEOUT 45)

add_test(NAME gnome_picker_tests COMMAND "${CMAKE_BINARY_DIR}/bin/gnome_picker_tests")
set_tests_properties(gnome_picker_tests PROPERTIES TIMEOUT 45)

add_test(NAME gnome_stream_finalization_tests COMMAND "${CMAKE_BINARY_DIR}/bin/gnome_stream_finalization_tests")
add_test(NAME gnome_reference_stream_reproduction COMMAND "${CMAKE_BINARY_DIR}/reference/gnome_reference_stream_tests" --reference)
set_tests_properties(gnome_stream_finalization_tests gnome_reference_stream_reproduction PROPERTIES TIMEOUT 30)

add_test(NAME gnome_font_cache_tests COMMAND "${CMAKE_BINARY_DIR}/bin/gnome_font_cache_tests")
set_tests_properties(gnome_font_cache_tests PROPERTIES TIMEOUT 30)

include("${CMAKE_SOURCE_DIR}/packaging/Install.cmake")

add_test(NAME gnome_recent_tests COMMAND "${CMAKE_BINARY_DIR}/bin/gnome_recent_tests")
set_tests_properties(gnome_recent_tests PROPERTIES TIMEOUT 20)
