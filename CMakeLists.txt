cmake_minimum_required(VERSION 3.24)
project(KeePassXcGnome VERSION 0.1.0 LANGUAGES NONE)

include(ExternalProject)
set(KEEPASSXC_SOURCE_DIR "${CMAKE_SOURCE_DIR}/upstream/keepassxc" CACHE PATH "Unmodified pinned KeePassXC checkout")
if(NOT EXISTS "${KEEPASSXC_SOURCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Fetch the pinned upstream source with tools/fetch-upstream.sh first.")
endif()
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${KEEPASSXC_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE upstream_commit OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT upstream_commit STREQUAL "1c2f23ef5914c0f442a00533def1689857afb5a7")
    message(FATAL_ERROR "This application baseline requires KeePassXC commit 1c2f23ef5914c0f442a00533def1689857afb5a7.")
endif()
ExternalProject_Add(upstream_reference_cli
    SOURCE_DIR "${KEEPASSXC_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/reference"
    DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
    CMAKE_ARGS
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo" "-DCMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}"
        -DCMAKE_PROJECT_TOP_LEVEL_INCLUDES= -DCMAKE_SKIP_RPATH=ON
        -DKPXC_MINIMAL=ON -DKPXC_FEATURE_NETWORK=OFF -DKPXC_FEATURE_DOCS=OFF
        -DWITH_X11=OFF -DWITH_TESTS=OFF -DWITH_GUI_TESTS=OFF
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target keepassxc-cli
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
        -DCMAKE_SKIP_RPATH=ON -DKPXC_MINIMAL=ON -DKPXC_FEATURE_NETWORK=OFF
        -DKPXC_FEATURE_DOCS=OFF -DWITH_X11=OFF -DWITH_TESTS=ON -DWITH_GUI_TESTS=OFF
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target keepassxc-gnome gnome_domain_tests gnome_compatibility_tests gnome_reader_finalization_tests
        gnome_upstream_cipher_tests gnome_upstream_hmac_tests gnome_upstream_hashed_tests
        gnome_upstream_kdbx3_tests gnome_upstream_kdbx4_tests gnome_xml_model_tests
    BUILD_ALWAYS TRUE
    INSTALL_COMMAND "")
enable_testing()
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
