# Usage: cmake -DBRANCH_TRUCE_SOURCE_DIR=<root> -P NativeVersionTests.cmake
#
# Pins which release versions packaging/scripts/native-version.sh accepts and
# what it maps them to, then checks that the package versions it prints sort in
# SemVer order with dpkg.
cmake_minimum_required(VERSION 3.28)

set(script "${BRANCH_TRUCE_SOURCE_DIR}/packaging/scripts/native-version.sh")

function(native_version out version)
    execute_process(COMMAND "${script}" "${version}"
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "\"${version}\": expected success, got exit ${result}:\n${stderr}")
    endif()
    set(${out} "${stdout}" PARENT_SCOPE)
endfunction()

function(expect_mapped version expected)
    native_version(actual "${version}")
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR "\"${version}\": expected \"${expected}\", got \"${actual}\"")
    endif()
endfunction()

function(expect_refused)
    execute_process(COMMAND "${script}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT result EQUAL 2)
        message(FATAL_ERROR "\"${ARGN}\": expected exit 2, got ${result}")
    endif()
    if(NOT stdout STREQUAL "")
        message(FATAL_ERROR "\"${ARGN}\": refused, but printed \"${stdout}\"")
    endif()
    if(stderr STREQUAL "")
        message(FATAL_ERROR "\"${ARGN}\": refused without a message")
    endif()
endfunction()

expect_mapped(1.0.1 1.0.1)
expect_mapped(0.0.0 0.0.0)
expect_mapped(1.2.0-rc 1.2.0~rc)
expect_mapped(1.2.0-rc.1 1.2.0~rc1)
expect_mapped(1.2.0-RC.10 1.2.0~RC10)

foreach(version
        1.2.0-alpha-beta 1.2.0-alpha-1 1.2.0-rc1 1.2.0-rc.0 1.2.0-rc.01
        1.2.0-1 1.2.0-rc.1.2 1.2.0+build.5 1.2.0-rc.1+build.5
        1.0 v1.0.1 01.0.0 1.0.0-)
    expect_refused("${version}")
endforeach()
expect_refused()
expect_refused(1.0.0 1.0.1)

set(ordered
    1.0.0 1.0.1 1.2.0-RC.1 1.2.0-alpha 1.2.0-alpha.1 1.2.0-alpha.2
    1.2.0-alpha.10 1.2.0-alphab 1.2.0-alphab.1 1.2.0-beta 1.2.0-beta.1
    1.2.0-rc.1 1.2.0-rc.9 1.2.0-rcx 1.2.0 2.0.0-rc.1 10.0.0)
set(native "")
foreach(version IN LISTS ordered)
    native_version(mapped "${version}")
    list(APPEND native "${mapped}")
endforeach()
list(LENGTH native count)
math(EXPR last "${count} - 1")

find_program(DPKG dpkg)
if(NOT DPKG)
    message(STATUS "dpkg not found; skipping the dpkg order check")
endif()

set(pairs 0)
foreach(i RANGE ${last})
    list(GET native ${i} lower)
    math(EXPR next "${i} + 1")
    if(next GREATER last)
        break()
    endif()
    foreach(j RANGE ${next} ${last})
        list(GET native ${j} higher)
        math(EXPR pairs "${pairs} + 1")
        if(DPKG)
            execute_process(COMMAND "${DPKG}" --compare-versions "${lower}" lt "${higher}"
                RESULT_VARIABLE result)
            if(NOT result EQUAL 0)
                message(FATAL_ERROR "dpkg does not sort ${lower} before ${higher}")
            endif()
        endif()
    endforeach()
endforeach()

message(STATUS "native version tests passed; checked ${pairs} pairs")
