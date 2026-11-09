add_library(branch_truce_storage STATIC
    FileSystem.cpp FileSystem.h RecoveryStore.cpp RecoveryStore.h TextFormat.cpp TextFormat.h DestinationWriter.cpp DestinationWriter.h)
target_include_directories(branch_truce_storage PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(branch_truce_storage PUBLIC Qt6::Core branch_truce_core PRIVATE branch_truce_build_options)
