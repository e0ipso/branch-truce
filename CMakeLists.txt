cmake_minimum_required(VERSION 3.28)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/BranchTruceVersion.cmake")

# Empty by default: the version comes from the root package.json. Release
# builds pass the resolved version explicitly, e.g. -DBRANCH_TRUCE_VERSION=1.2.0.
set(BRANCH_TRUCE_VERSION "" CACHE STRING "Branch Truce release version; empty reads the root package.json")
set(BRANCH_TRUCE_PACKAGE_JSON "${CMAKE_CURRENT_LIST_DIR}/package.json"
    CACHE FILEPATH "package.json that provides the default version")

branch_truce_resolve_version(BRANCH_TRUCE_RESOLVED_VERSION
    PACKAGE_JSON "${BRANCH_TRUCE_PACKAGE_JSON}"
    OVERRIDE "${BRANCH_TRUCE_VERSION}")
branch_truce_version_core(_branch_truce_version_core "${BRANCH_TRUCE_RESOLVED_VERSION}")
if(BRANCH_TRUCE_VERSION STREQUAL "")
    # Re-run configure when the root version changes.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${BRANCH_TRUCE_PACKAGE_JSON}")
endif()

project(BranchTruce VERSION ${_branch_truce_version_core} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(BRANCH_TRUCE_BUILD_DESKTOP "Build Qt/Kirigami desktop targets" ON)
option(BRANCH_TRUCE_WARNINGS_AS_ERRORS "Treat project warnings as errors" OFF)
option(BRANCH_TRUCE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

include(CTest)
include(cmake/CompilerOptions.cmake)
add_subdirectory(src/core)
if(BRANCH_TRUCE_BUILD_DESKTOP)
    find_package(ECM 6.0 REQUIRED NO_MODULE)
    list(APPEND CMAKE_MODULE_PATH "${ECM_MODULE_PATH}")
    include(KDEInstallDirs6)
    find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Widgets Qml Quick QuickControls2 Network)
    find_package(KF6Kirigami 6.0 REQUIRED)
    qt_standard_project_setup(REQUIRES 6.6)
    message(STATUS "Desktop dependencies: Qt ${Qt6_VERSION}, ECM ${ECM_VERSION}, Kirigami ${KF6Kirigami_VERSION}")
    # Reverse-DNS form of the organization domain branch-truce.local. It names
    # the desktop entry, metainfo and icon, and the Wayland app id.
    set(BRANCH_TRUCE_APP_ID "local.branch_truce.BranchTruce")
    add_subdirectory(src/git)
    add_subdirectory(src/storage)
    add_subdirectory(src/app)
    add_subdirectory(src/ui)
    add_subdirectory(packaging)
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
