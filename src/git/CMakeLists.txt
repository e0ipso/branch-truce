add_library(branch_truce_git STATIC GitAdapter.cpp)
target_link_libraries(branch_truce_git PRIVATE Qt6::Core branch_truce_build_options)
