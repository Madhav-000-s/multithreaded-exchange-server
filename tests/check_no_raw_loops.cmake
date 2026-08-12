# Phase 2 exit criterion, machine-checked: analytics/ contains no hand-written
# loops. Every statistic is expressed with a standard algorithm.
#
# A CTest case rather than a code-review convention, for the same reason the
# exception guarantee gets a fault-injection harness in Phase 3 -- a property
# nothing verifies is a property that decays on the next commit.
#
# Run with:
#   cmake -D ANALYTICS_DIR=<dir> -P check_no_raw_loops.cmake
#
# The irony of iterating to prove an absence of iteration is noted. CMake is
# not the language under review.

if(NOT DEFINED ANALYTICS_DIR)
    message(FATAL_ERROR "ANALYTICS_DIR must be set")
endif()

file(GLOB_RECURSE sources "${ANALYTICS_DIR}/*.hpp" "${ANALYTICS_DIR}/*.cpp")

if(sources STREQUAL "")
    message(FATAL_ERROR "no sources found under ${ANALYTICS_DIR} -- check the path")
endif()

set(offenders "")

foreach(source IN LISTS sources)
    # `for (`, `while (`, and `goto`, but not `for_each` or a member named
    # `...for(`; the leading boundary keeps identifiers ending in "for" out.
    file(STRINGS "${source}" hits REGEX "(^|[^A-Za-z0-9_])(for|while|goto)[ \t]*\\(")

    foreach(line IN LISTS hits)
        # Skip comment lines. Prose about loops is not a loop.
        if(NOT line MATCHES "^[ \t]*(//|\\*|/\\*)")
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
        "analytics/ must contain no hand-written loops, found:\n${report}\n"
        "Express the traversal with a standard algorithm instead.")
endif()

message(STATUS "no raw loops in ${sourceCount} analytics source(s)")
