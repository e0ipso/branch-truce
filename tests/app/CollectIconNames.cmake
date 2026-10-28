# Collects the icon names the QML files name literally, one per line as
# "<name><TAB><file>:<line>", for the icon check in ThemeTests.cpp.
#
#   cmake -DOUTPUT=icons.txt -DSOURCE_DIR=<root> "-DQML_FILES=a.qml|b.qml" -P CollectIconNames.cmake
#
# QML_FILES takes "|" or ";" between files.
#
# A name counts when it is a quoted string in the value of icon.name, an
# object literal's icon key or an iconName property, or a quoted string a
# function whose name ends in Icon returns. File paths in the output are
# relative to SOURCE_DIR.
if(NOT OUTPUT OR NOT QML_FILES)
    message(FATAL_ERROR "CollectIconNames.cmake needs OUTPUT and QML_FILES")
endif()

string(REPLACE "|" ";" QML_FILES "${QML_FILES}")
set(collected "")
foreach(qml IN LISTS QML_FILES)
    file(READ "${qml}" content)
    # Semicolons and brackets would split or join CMake list elements, so swap
    # them for characters that cannot start or end an icon name.
    string(REPLACE ";" "," content "${content}")
    string(REPLACE "[" "(" content "${content}")
    string(REPLACE "]" ")" content "${content}")
    string(REPLACE "\\" "/" content "${content}")
    string(REPLACE "\n" ";" lines "${content}")
    file(RELATIVE_PATH shown "${SOURCE_DIR}" "${qml}")
    set(number 0)
    set(function_indent "")
    foreach(line IN LISTS lines)
        math(EXPR number "${number} + 1")
        set(values "")
        if(NOT function_indent STREQUAL "")
            if(line MATCHES "^${function_indent}}")
                set(function_indent "")
            else()
                string(REGEX MATCHALL "return .*" returns "${line}")
                list(APPEND values ${returns})
            endif()
        elseif(line MATCHES "^( *)function [A-Za-z0-9_]*Icon *\\(")
            set(function_indent "${CMAKE_MATCH_1}")
        endif()
        string(REGEX MATCHALL "(^|[^A-Za-z0-9_])icon(\\.name|Name)? *[:=][^,}]*" assignments "${line}")
        list(APPEND values ${assignments})
        foreach(value IN LISTS values)
            string(REGEX MATCHALL "\"[^\"]+\"" names "${value}")
            foreach(name IN LISTS names)
                string(REPLACE "\"" "" name "${name}")
                list(APPEND collected "${name}\t${shown}:${number}")
            endforeach()
        endforeach()
    endforeach()
endforeach()

list(REMOVE_DUPLICATES collected)
list(LENGTH collected count)
if(count EQUAL 0)
    message(FATAL_ERROR "CollectIconNames.cmake found no icon names in ${QML_FILES}")
endif()
string(REPLACE ";" "\n" text "${collected}")
file(WRITE "${OUTPUT}.tmp" "${text}\n")
file(COPY_FILE "${OUTPUT}.tmp" "${OUTPUT}" ONLY_IF_DIFFERENT)
file(REMOVE "${OUTPUT}.tmp")
