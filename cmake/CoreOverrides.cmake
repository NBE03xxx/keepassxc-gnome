find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(KPXC_PATCH_EXECUTABLE patch REQUIRED)
set(gnome_override_dir "${CMAKE_BINARY_DIR}/gnome-core")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${KPXC_GNOME_SOURCE}/tools/prepare-core-overrides.py"
    --upstream "${CMAKE_SOURCE_DIR}" --output "${gnome_override_dir}" --patch-tool "${KPXC_PATCH_EXECUTABLE}"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${KPXC_GNOME_SOURCE}/tools/prepare-core-overrides.py"
    "${KPXC_GNOME_SOURCE}/patches/0001-reader-finalization.patch"
    "${KPXC_GNOME_SOURCE}/patches/0002-tests-truncate-reused-buffer.patch"
    "${KPXC_GNOME_SOURCE}/patches/0003-read-resource-policy.patch"
    "${KPXC_GNOME_SOURCE}/patches/0004-xml-model-preservation.patch"
    "${KPXC_GNOME_SOURCE}/patches/reader-finalization.json")
get_target_property(core_sources keepassxc_core SOURCES)
set(replaced_sources)
set(replacement_count 0)
foreach(source IN LISTS core_sources)
    get_filename_component(absolute "${source}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}/src")
    file(RELATIVE_PATH relative "${CMAKE_SOURCE_DIR}" "${absolute}")
    if(EXISTS "${gnome_override_dir}/${relative}")
        list(APPEND replaced_sources "${gnome_override_dir}/${relative}")
        get_filename_component(original_directory "${absolute}" DIRECTORY)
        set_source_files_properties("${gnome_override_dir}/${relative}" TARGET_DIRECTORY keepassxc_core
            PROPERTIES INCLUDE_DIRECTORIES "${original_directory}")
        math(EXPR replacement_count "${replacement_count}+1")
    else()
        list(APPEND replaced_sources "${source}")
    endif()
endforeach()
if(NOT replacement_count EQUAL 8)
    message(FATAL_ERROR "Expected eight core source replacements, found ${replacement_count}.")
endif()
set_property(TARGET keepassxc_core PROPERTY SOURCES "${replaced_sources}")
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" override_cmake_hash)
file(SHA256 "${KPXC_GNOME_SOURCE}/patches/reader-finalization.json" override_manifest_hash)
file(SHA256 "${KPXC_GNOME_SOURCE}/patches/0001-reader-finalization.patch" override_patch_hash)
file(SHA256 "${KPXC_GNOME_SOURCE}/patches/0003-read-resource-policy.patch" override_policy_hash)
file(SHA256 "${KPXC_GNOME_SOURCE}/patches/0004-xml-model-preservation.patch" override_xml_hash)
string(SHA256 override_signature "${override_cmake_hash}${override_manifest_hash}${override_patch_hash}${override_policy_hash}${override_xml_hash}")
string(SUBSTRING "${override_signature}" 0 16 override_signature)
set_property(TARGET keepassxc_core PROPERTY AUTOGEN_BUILD_DIR
    "${CMAKE_BINARY_DIR}/gnome-core-autogen/${override_signature}")
# AUTOMOC can discover the original headers through other core includes or its
# parse cache. Make the patched Q_OBJECT headers explicit and exclude originals.
foreach(header IN ITEMS streams/SymmetricCipherStream.h streams/qtiocompressor.h streams/HashedBlockStream.h streams/HmacBlockStream.h)
    set_source_files_properties("${CMAKE_SOURCE_DIR}/src/${header}" TARGET_DIRECTORY keepassxc_core
        PROPERTIES SKIP_AUTOMOC ON)
    target_sources(keepassxc_core PRIVATE "${gnome_override_dir}/src/${header}")
    set_property(TARGET keepassxc_core APPEND PROPERTY AUTOGEN_TARGET_DEPENDS
        "${gnome_override_dir}/src/${header}")
endforeach()
set_property(TARGET keepassxc_core APPEND PROPERTY AUTOGEN_TARGET_DEPENDS
    "${KPXC_GNOME_SOURCE}/cmake/CoreOverrides.cmake")
# The stream header has an additional state field. All consumers must see the
# matching header, including AUTOMOC and upstream test/CLI targets.
target_include_directories(keepassxc_core BEFORE PUBLIC "${gnome_override_dir}/src")
target_include_directories(keepassxc_core PUBLIC "${KPXC_GNOME_SOURCE}/src" "${KPXC_GNOME_SOURCE}/prototype")

# Upstream tests add src/ as a directory include before transitive usage
# requirements. Put the matching copied headers first for those targets too.
function(gnome_prepend_core_headers directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(kind "${target}" TYPE)
        if(NOT kind STREQUAL "UTILITY" AND NOT kind STREQUAL "INTERFACE_LIBRARY")
            target_include_directories("${target}" BEFORE PRIVATE "${gnome_override_dir}/src")
        endif()
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        gnome_prepend_core_headers("${child}")
    endforeach()
endfunction()
gnome_prepend_core_headers("${CMAKE_SOURCE_DIR}")
