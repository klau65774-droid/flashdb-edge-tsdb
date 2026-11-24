if(NOT DEFINED CLI OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "CLI and TEST_DIR are required")
endif()

file(MAKE_DIRECTORY "${TEST_DIR}")

function(run_valid_cli)
    execute_process(COMMAND "${CLI}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT "${status}" STREQUAL "0")
        message(FATAL_ERROR "CLI failed (${status}) for ${ARGN}\n${error}\n${output}")
    endif()
    set(cli_output "${output}" PARENT_SCOPE)
endfunction()

function(expect_output snippet)
    string(FIND "${cli_output}" "${snippet}" offset)
    if(offset EQUAL -1)
        message(FATAL_ERROR "Missing '${snippet}' in CLI output\n${cli_output}")
    endif()
endfunction()

function(expect_rejection diagnostic)
    execute_process(COMMAND "${CLI}" ${ARGN}
        RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT "${status}" MATCHES "^[1-9][0-9]*$" OR NOT "${error}" MATCHES "${diagnostic}")
        message(FATAL_ERROR "Unexpected rejection result (${status}) for ${ARGN}\n${error}\n${output}")
    endif()
endfunction()

set(csv "${TEST_DIR}/fixture.csv")
file(WRITE "${csv}" "timestamp,value\n2024-02-29 00:00:00,-0.0\n2024-02-29T00:00:01,4.25")
run_valid_cli(bench-csv --csv "${csv}" --mode nab --rollback-at 1 --batch 0)
expect_output("\"rows\":2")
expect_output("\"queried_matched\":2,\"mismatch\":0")
expect_output("\"rollbacks\":1")
expect_output("\"bit_exact\":true")

expect_rejection("geometry" bench-csv --csv "${csv}" --sectors 2 --sector-size 2147483648)
expect_rejection("geometry" bench-csv --csv "${csv}" --sectors 4 --sector-size 1073741824)
expect_rejection("geometry" bench-csv --csv "${csv}" --sector-size 768)
expect_rejection("option" bench-csv --csv "${csv}" --batch 65536)
expect_rejection("option" bench-csv --csv "${csv}" --sectors -2)
expect_rejection("option" bench-csv --csv "${csv}" --sectors 3abc)
expect_rejection("option" bench-csv --csv "${csv}" --rollback-at)
expect_rejection("mode" bench-csv --csv "${csv}" --mode other)

foreach(bad_nab IN ITEMS
        "2023-02-29 00:00:00,1"
        "2024-10-01 00:00:00junk,1"
        "2024-10-01 00:00:00,1junk"
        "2024-10-01 00:00:00,")
    file(WRITE "${csv}" "timestamp,value\n${bad_nab}\n")
    expect_rejection("Invalid or empty" bench-csv --csv "${csv}" --mode nab)
endforeach()
file(WRITE "${csv}" "timestamp,value\n")
expect_rejection("Invalid or empty" bench-csv --csv "${csv}" --mode nab)
string(REPEAT "0" 2048 overlong_value)
file(WRITE "${csv}" "timestamp,value\n2024-10-01 00:00:00,${overlong_value}\n")
expect_rejection("Invalid or empty" bench-csv --csv "${csv}" --mode nab)

set(imu_header "seconds,a,b,c,d,e,f,g,h,i\n")
file(WRITE "${csv}" "${imu_header}0,1,2,3,4,5,6,7,8,9\n0.001,9,8,7,6,5,4,3,2,1\n")
run_valid_cli(bench-csv --csv "${csv}" --mode imu --batch 2)
expect_output("\"rows\":2,\"channels\":9")
expect_output("\"queried_matched\":2,\"mismatch\":0")
expect_output("\"bit_exact\":true")

foreach(bad_imu IN ITEMS
        "NaN,1,2,3,4,5,6,7,8,9"
        "1e300,1,2,3,4,5,6,7,8,9"
        "9223372036854776,1,2,3,4,5,6,7,8,9"
        "9223372000000000,1,2,3,4,5,6,7,8,9"
        "0,1,2,3,4,5,6,7,8"
        "0,1,2,3,4,5,6,7,8,9,10"
        "0,1,2,3,4,5,6,7,8,1e3000")
    file(WRITE "${csv}" "${imu_header}${bad_imu}\n")
    expect_rejection("Invalid or empty" bench-csv --csv "${csv}" --mode imu)
endforeach()

file(WRITE "${csv}" "timestamp,value\n")
foreach(index RANGE 0 7)
    file(APPEND "${csv}" "2024-02-29 00:00:0${index},${index}\n")
endforeach()
run_valid_cli(bench-csv --csv "${csv}" --mode nab --sectors 2 --sector-size 256 --batch 1)
expect_output("\"rows\":8")
expect_output("\"bit_exact\":false")

run_valid_cli(fault-inject --trials 16 --seed 42)
expect_output("\"trials\":16,\"passed\":16,\"committed_loss_count\":0")
expect_output("\"other_failure_count\":0,\"power_cuts_triggered\":16")
expect_output("\"nor_bit_violations\":0")
expect_rejection("option" fault-inject --trials 0)

run_valid_cli(sync-demo --samples 96 --corrupt-pct 100 --seed 7)
expect_output("\"samples\":96,\"attempts\":2,\"injected_corruptions\":2")
expect_output("\"src_flushed\":96,\"dst_flushed\":96,\"synced_exact\":true")
expect_rejection("option" sync-demo --samples 0)
expect_rejection("option" sync-demo --corrupt-pct 101)
