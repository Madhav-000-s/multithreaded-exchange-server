# Phase 3 sweep, machine-checked: no raw `new` or `delete` in the library.
#
# ARCHITECTURE section 5 states there is no new or delete anywhere outside a
# factory function that immediately wraps the pointer. In this codebase there
# are no such factories -- every allocation goes through std::make_unique -- so
# the rule reduces to "none at all", which is checkable.
#
# `= delete` on a special member is not an allocation and is excluded: the
# pattern below requires an identifier or a parenthesis after the keyword,
# which `= delete;` does not have.
#
# Run with:
#   cmake -D SOURCE_DIRS=<dir>;<dir> -P check_no_raw_memory.cmake

if(NOT DEFINED SOURCE_DIRS)
    message(FATAL_ERROR "SOURCE_DIRS must be set")
endif()

set(sources "")
foreach(directory IN LISTS SOURCE_DIRS)
    file(GLOB_RECURSE found "${directory}/*.hpp" "${directory}/*.cpp")
    list(APPEND sources ${found})
endforeach()

if(sources STREQUAL "")
    message(FATAL_ERROR "no sources found under ${SOURCE_DIRS} -- check the paths")
endif()

set(offenders "")

foreach(source IN LISTS sources)
    # `new Type`, `new(...)`, `delete ptr`, `delete[] ptr`.
    file(STRINGS "${source}" hits
        REGEX "(^|[^A-Za-z0-9_>.])(new[ \t]+[A-Za-z_(]|new\(|delete[ \t]*\[\]|delete[ \t]+[A-Za-z_(*])")

    foreach(line IN LISTS hits)
        # Comments and documentation are prose about allocation, not allocation.
        if(NOT line MATCHES "^[ \t]*(//|\*|/\*)")
            get_filename_component(name "${source}" NAME)
            string(STRIP "${line}" line)
            list(APPEND offenders "  ${name}: ${line}")
        endif()
    endforeach()
endforeach()

list(LENGTH sources sourceCount)

if(offenders)
    string(REPLACE ";" "\n" report "${offenders}")
    message(FATAL_ERROR
        "raw new/delete found; ownership must go through a smart pointer:\n${report}")
endif()

message(STATUS "no raw new/delete in ${sourceCount} library source(s)")
