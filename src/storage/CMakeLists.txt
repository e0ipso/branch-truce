add_library(branch_truce_storage STATIC RecoveryStore.cpp TextFormat.cpp TextFormat.h)
target_include_directories(branch_truce_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(branch_truce_storage PUBLIC Qt6::Core PRIVATE branch_truce_build_options)
