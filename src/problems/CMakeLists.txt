# Problems sit below every layer that reports one, so git, storage and the mergetool share them without
# depending on each other.
add_library(branch_truce_problems STATIC Problem.cpp Problem.h ProblemText.cpp ProblemText.h)
target_include_directories(branch_truce_problems PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(branch_truce_problems PUBLIC Qt6::Core PRIVATE branch_truce_build_options)
