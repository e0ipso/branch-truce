add_library(branch_truce_core STATIC BuildInfo.cpp SessionState.cpp ConflictMarkers.cpp Correspondence.cpp EditingCommands.cpp)
target_include_directories(branch_truce_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(branch_truce_core PUBLIC cxx_std_20)
target_compile_definitions(branch_truce_core PRIVATE BRANCH_TRUCE_VERSION_STRING="${BRANCH_TRUCE_RESOLVED_VERSION}")
target_link_libraries(branch_truce_core PRIVATE branch_truce_build_options)
# Consumers of a sanitized static library also need its sanitizer runtime.
if(BRANCH_TRUCE_SANITIZERS)
    target_link_libraries(branch_truce_core PUBLIC branch_truce_build_options)
endif()
