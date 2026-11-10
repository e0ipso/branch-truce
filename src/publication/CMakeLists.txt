add_library(branch_truce_publication OBJECT ResolutionPublisher.cpp ResolutionPublisher.h ParentStaging.cpp ParentStaging.h)
target_include_directories(branch_truce_publication PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(branch_truce_publication PUBLIC Qt6::Core branch_truce_core branch_truce_git branch_truce_storage
    branch_truce_session PRIVATE branch_truce_build_options)
