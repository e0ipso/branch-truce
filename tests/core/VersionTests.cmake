# Usage: cmake -DBRANCH_TRUCE_SOURCE_DIR=<root> -DWORK_DIR=<dir> -P VersionTests.cmake
cmake_minimum_required(VERSION 3.28)

include("${BRANCH_TRUCE_SOURCE_DIR}/cmake/BranchTruceVersion.cmake")

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

function(expect_equal actual expected what)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "${what}: expected \"${expected}\", got \"${actual}\"")
    endif()
endfunction()

function(expect_failure pattern what snippet)
    set(script "${WORK_DIR}/failure.cmake")
    file(WRITE "${script}"
        "cmake_minimum_required(VERSION 3.28)\n"
        "include(\"${BRANCH_TRUCE_SOURCE_DIR}/cmake/BranchTruceVersion.cmake\")\n"
        "${snippet}\n")
    execute_process(COMMAND ${CMAKE_COMMAND} -P "${script}"
        RESULT_VARIABLE result ERROR_VARIABLE stderr OUTPUT_QUIET)
    if(result EQUAL 0)
        message(FATAL_ERROR "${what}: expected a failure, but it succeeded")
    endif()
    string(REGEX REPLACE "[ \n]+" " " stderr "${stderr}")
    if(NOT stderr MATCHES "${pattern}")
        message(FATAL_ERROR "${what}: failure did not match \"${pattern}\":\n${stderr}")
    endif()
endfunction()

set(package_json "${WORK_DIR}/package.json")
file(WRITE "${package_json}" "{ \"name\": \"branch-truce\", \"version\": \"3.4.5\" }\n")

branch_truce_resolve_version(version PACKAGE_JSON "${package_json}")
expect_equal("${version}" "3.4.5" "default version")
branch_truce_resolve_version(version PACKAGE_JSON "${package_json}" OVERRIDE "")
expect_equal("${version}" "3.4.5" "empty override")

branch_truce_resolve_version(version PACKAGE_JSON "${package_json}" OVERRIDE "4.0.0")
expect_equal("${version}" "4.0.0" "explicit version")
branch_truce_resolve_version(version PACKAGE_JSON "${WORK_DIR}/missing.json" OVERRIDE "4.1.0-rc.1+build.7")
expect_equal("${version}" "4.1.0-rc.1+build.7" "explicit version without package.json")
branch_truce_version_core(core "4.1.0-rc.1+build.7")
expect_equal("${core}" "4.1.0" "version core")

set(root_package_json "${BRANCH_TRUCE_SOURCE_DIR}/package.json")
if(EXISTS "${root_package_json}")
    branch_truce_resolve_version(version PACKAGE_JSON "${root_package_json}")
    if(NOT version MATCHES "${BRANCH_TRUCE_SEMVER_REGEX}")
        message(FATAL_ERROR "root package.json version \"${version}\" is not a semantic version")
    endif()
else()
    message(FATAL_ERROR "root package.json ${root_package_json} does not exist")
endif()

expect_failure("is not a semantic version" "malformed override"
    "branch_truce_resolve_version(v PACKAGE_JSON \"${package_json}\" OVERRIDE \"v1.2\")")
expect_failure("missing.json does not exist" "missing package.json"
    "branch_truce_resolve_version(v PACKAGE_JSON \"${WORK_DIR}/missing.json\")")
file(WRITE "${WORK_DIR}/noversion.json" "{ \"name\": \"branch-truce\" }\n")
expect_failure("Cannot read \"version\"" "package.json without a version"
    "branch_truce_resolve_version(v PACKAGE_JSON \"${WORK_DIR}/noversion.json\")")

message(STATUS "version tests passed")
