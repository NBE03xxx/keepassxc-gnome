# Load the probe after the unmodified upstream project has defined its targets.
set(KPXC_PHASE2_SOURCE "${CMAKE_CURRENT_LIST_DIR}")
cmake_language(DEFER CALL include "${KPXC_PHASE2_SOURCE}/targets.cmake")
