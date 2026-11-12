# Reuse upstream test sources with the core library and QCoreApplication.
# Upstream's normal test targets link keepassxc_gui; these equivalents avoid
# building that Qt Widgets frontend while exercising the same test bodies.
find_package(Qt6 REQUIRED COMPONENTS Test)
function(gnome_core_test name)
    set(sources)
    foreach(source IN LISTS ARGN)
        if(EXISTS "${gnome_override_dir}/tests/${source}")
            list(APPEND sources "${gnome_override_dir}/tests/${source}")
        else()
            list(APPEND sources "${CMAKE_SOURCE_DIR}/tests/${source}")
        endif()
        string(REGEX REPLACE "\\.cpp$" ".h" header "${source}")
        if(EXISTS "${CMAKE_SOURCE_DIR}/tests/${header}")
            list(APPEND sources "${CMAKE_SOURCE_DIR}/tests/${header}")
        endif()
    endforeach()
    add_executable("${name}" ${sources})
    target_include_directories("${name}" BEFORE PRIVATE "${gnome_override_dir}/src")
    target_include_directories("${name}" SYSTEM PRIVATE
        "${CMAKE_SOURCE_DIR}/src" "${CMAKE_BINARY_DIR}/src"
        "${CMAKE_SOURCE_DIR}/tests" "${CMAKE_BINARY_DIR}/tests")
    target_link_libraries("${name}" PRIVATE keepassxc_core text_stream Qt6::Test)
    target_compile_definitions("${name}" PRIVATE QT_NO_USE_NODISCARD_FILE_OPEN)
    set_target_properties("${name}" PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${KPXC_GNOME_OUTPUT}/bin")
endfunction()
gnome_core_test(gnome_upstream_cipher_tests TestSymmetricCipher.cpp)
gnome_core_test(gnome_upstream_hmac_tests TestHmacBlockStream.cpp)
gnome_core_test(gnome_upstream_hashed_tests TestHashedBlockStream.cpp FailDevice.cpp)
gnome_core_test(gnome_upstream_kdbx3_tests TestKdbx3.cpp TestKeePass2Format.cpp FailDevice.cpp
    mock/MockClock.cpp mock/MockChallengeResponseKey.cpp)
gnome_core_test(gnome_upstream_kdbx4_tests TestKdbx4.cpp TestKeePass2Format.cpp FailDevice.cpp
    mock/MockClock.cpp mock/MockChallengeResponseKey.cpp)
