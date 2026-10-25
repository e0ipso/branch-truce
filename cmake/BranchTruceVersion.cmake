# Version resolution for Branch Truce.
#
# Works in project mode and in script mode (cmake -P), so the tests can call
# these functions without configuring a project.

if(POLICY CMP0174)
    cmake_policy(SET CMP0174 NEW)
endif()

# Same shape semantic-release produces: MAJOR.MINOR.PATCH with optional
# pre-release and build parts.
set(BRANCH_TRUCE_SEMVER_REGEX
    "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?(\\+[0-9A-Za-z.-]+)?$")

# branch_truce_resolve_version(<out-var> PACKAGE_JSON <path> [OVERRIDE <version>])
#
# Sets <out-var> to OVERRIDE when it is non-empty, otherwise to the "version"
# field of PACKAGE_JSON. Fails on a missing file, a missing field or a value
# that is not a semantic version.
function(branch_truce_resolve_version out_var)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "PACKAGE_JSON;OVERRIDE" "")

    if(NOT "${ARG_OVERRIDE}" STREQUAL "")
        set(version "${ARG_OVERRIDE}")
        set(origin "the explicit BRANCH_TRUCE_VERSION")
    else()
        if(NOT EXISTS "${ARG_PACKAGE_JSON}")
            message(FATAL_ERROR
                "Cannot read the Branch Truce version: ${ARG_PACKAGE_JSON} does not exist. "
                "Pass -DBRANCH_TRUCE_VERSION=<version> when building outside the Branch Truce checkout.")
        endif()
        file(READ "${ARG_PACKAGE_JSON}" package_json)
        string(JSON version ERROR_VARIABLE json_error GET "${package_json}" version)
        if(json_error)
            message(FATAL_ERROR "Cannot read \"version\" from ${ARG_PACKAGE_JSON}: ${json_error}")
        endif()
        set(origin "${ARG_PACKAGE_JSON}")
    endif()

    if(NOT version MATCHES "${BRANCH_TRUCE_SEMVER_REGEX}")
        message(FATAL_ERROR "\"${version}\" from ${origin} is not a semantic version (MAJOR.MINOR.PATCH).")
    endif()

    set(${out_var} "${version}" PARENT_SCOPE)
endfunction()

# branch_truce_version_core(<out-var> <version>)
#
# Sets <out-var> to MAJOR.MINOR.PATCH, the part CMake's project(VERSION) accepts.
function(branch_truce_version_core out_var version)
    if(NOT version MATCHES "${BRANCH_TRUCE_SEMVER_REGEX}")
        message(FATAL_ERROR "\"${version}\" is not a semantic version.")
    endif()
    set(${out_var} "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}" PARENT_SCOPE)
endfunction()
