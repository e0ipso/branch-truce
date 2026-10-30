# Only compiler entry points with a successfully probed cache include this file.
# Keep hash_dir enabled. Maps normalize both __FILE__ and debug paths rather
# than accepting cached objects with another disposable directory's identity.
# This project uses headers and has no C++ module interfaces. Automatic C++20
# scanning on recent GCC adds -fmodules-ts and per-build module maps, which
# ccache rejects. Keep ordinary header dependency tracking and all warnings.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
find_program(_bt_ccache ccache REQUIRED)
set(CMAKE_C_COMPILER_LAUNCHER "${_bt_ccache}")
set(CMAKE_CXX_COMPILER_LAUNCHER "${_bt_ccache}")
file(RELATIVE_PATH _bt_relative_source "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}")
add_compile_options(
    "-ffile-prefix-map=${_bt_relative_source}=/bt/source"
    "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=/bt/source"
    "-ffile-prefix-map=${CMAKE_BINARY_DIR}=/bt/build"
    "-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=/bt/source"
    "-fdebug-prefix-map=${CMAKE_BINARY_DIR}=/bt/build")

# Fedora's -Wp hardening options require preprocessor mode. GCC's directory
# linemarker otherwise embeds a disposable CWD in that input. Keep real DWARF
# directory mapping above; this changes only the preprocessor's extra marker.
if("$ENV{BRANCH_TRUCE_CACHE_PROFILE}" STREQUAL "native-rpm-distro")
    add_compile_options("$<$<CXX_COMPILER_ID:GNU>:-fno-working-directory>")
endif()
