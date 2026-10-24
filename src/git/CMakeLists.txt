add_library(branch_truce_git STATIC GitAdapter.cpp GitAdapter.h GitParsing.h
    RepositoryDiscovery.cpp RepositoryDiscovery.h TextConversion.cpp TextConversion.h
    ConflictClassifier.cpp ConflictClassifier.h)
target_include_directories(branch_truce_git PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(branch_truce_git PUBLIC Qt6::Core branch_truce_storage PRIVATE branch_truce_build_options)
