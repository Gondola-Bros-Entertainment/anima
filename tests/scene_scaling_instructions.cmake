# Counts with Callgrind the instructions each scene scaling scenario's measured operation runs at
# OBJECTS objects and at twice as many, prints each ratio, and fails when doubling the objects
# multiplies them by more than the scenario's limit.
#
#   cmake -DVALGRIND=<valgrind> -DSCENARIO=<anima_scene_scaling_scenario> -DOBJECTS=<count>
#         -DOUTPUT_DIR=<directory> -P scene_scaling_instructions.cmake
#
# Callgrind collects only inside measured_operation, which the scenario program calls once around
# the operation, so setup, teardown and process startup add nothing. Linear work doubles its
# instructions and n log n work multiplies them by 2 log(2n) / log(n), 2.14 at 16,384 objects, but
# quadratic work quadruples them, so a scenario may at most triple them. A component query should not
# grow with objects of other types, and a scan of every object would double it, so it may grow by
# half.
foreach(required VALGRIND SCENARIO OBJECTS OUTPUT_DIR)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing ${required}")
    endif()
endforeach()
if(NOT OBJECTS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "OBJECTS must be a positive integer: ${OBJECTS}")
endif()

# Each scenario with its limit in thousandths.
set(scenarios chain=3000 destroy_children=3000 destroy_renderers=3000 destroy_wide_subtree=3000
              destroy_deep_subtree=3000 reuse_slots=3000 query=1500)

# Sets @p result to the instructions that @p scenario's measured operation runs at @p objects.
function(count_instructions scenario objects result)
    set(profile "${OUTPUT_DIR}/${scenario}-${objects}.callgrind")
    file(REMOVE "${profile}")
    execute_process(
        COMMAND "${VALGRIND}" --tool=callgrind --collect-atstart=no --toggle-collect=measured_operation
                "--callgrind-out-file=${profile}" "${SCENARIO}" "${scenario}" "${objects}"
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(NOT status STREQUAL "0")
        message(FATAL_ERROR "${scenario} at ${objects} objects exited with ${status}:\n${output}")
    endif()
    file(STRINGS "${profile}" totals REGEX "^totals: [0-9]+$")
    if(NOT totals)
        file(STRINGS "${profile}" totals REGEX "^summary: [0-9]+$")
    endif()
    list(LENGTH totals found)
    if(NOT found EQUAL 1)
        message(FATAL_ERROR "Expected one instruction total in ${profile}, found ${found}")
    endif()
    string(REGEX REPLACE "^[a-z]+: " "" count "${totals}")
    # Nothing collected means Callgrind never entered measured_operation, so the check would pass
    # without measuring.
    if(count EQUAL 0)
        message(FATAL_ERROR "Callgrind collected no instructions in measured_operation for ${scenario}")
    endif()
    set(${result} "${count}" PARENT_SCOPE)
endfunction()

# Sets @p result to @p thousandths written as a decimal with three places.
function(decimal thousandths result)
    math(EXPR whole "${thousandths} / 1000")
    math(EXPR fraction "${thousandths} % 1000 + 1000")
    string(SUBSTRING "${fraction}" 1 3 fraction)
    set(${result} "${whole}.${fraction}" PARENT_SCOPE)
endfunction()

file(MAKE_DIRECTORY "${OUTPUT_DIR}")
math(EXPR doubled "${OBJECTS} * 2")
set(failed)
foreach(entry IN LISTS scenarios)
    string(REPLACE "=" ";" entry "${entry}")
    list(GET entry 0 scenario)
    list(GET entry 1 limit)
    count_instructions(${scenario} ${OBJECTS} small)
    count_instructions(${scenario} ${doubled} large)
    math(EXPR ratio "${large} * 1000 / ${small}")
    math(EXPR excess "${large} * 1000 - ${limit} * ${small}")
    decimal(${ratio} shown_ratio)
    decimal(${limit} shown_limit)
    set(verdict "within")
    if(excess GREATER 0)
        set(verdict "OVER")
        list(APPEND failed ${scenario})
    endif()
    message(STATUS "${scenario}: ${small} instructions at ${OBJECTS} objects, ${large} at ${doubled}: "
                   "${shown_ratio} times, ${verdict} the limit of ${shown_limit}")
endforeach()
if(failed)
    string(JOIN ", " failed ${failed})
    message(FATAL_ERROR "Doubling the objects multiplied the instructions beyond the limit: ${failed}")
endif()
