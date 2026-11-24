file(MAKE_DIRECTORY "${WORK_DIR}")
foreach(file IN ITEMS input.wal incomplete.wal collector.bin alias.bin ingest.ack replay.ack invalid.bin short.bin)
    file(REMOVE "${WORK_DIR}/${file}")
endforeach()

function(require_command expected check_pattern)
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${WORK_DIR}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE stderr
    )
    if(NOT result STREQUAL "${expected}" OR NOT "${output}${stderr}" MATCHES "${check_pattern}")
        message(FATAL_ERROR "Expected exit ${expected} with '${check_pattern}', got ${result}: ${output}${stderr}")
    endif()
endfunction()

set(options --wal-in input.wal --db-img collector.bin --device-id 42 --sectors 4 --sector-size 4096 --channels 1)
require_command(0 "" "${FIXTURE}" prepare)
require_command(0 "\"accepted_frames\":80,\"corrupt_frames\":90,\"duplicate_frames\":0"
    "${COLLECTOR}" ${options} --ack-out ingest.ack)
require_command(0 "" "${FIXTURE}" verify_ingest)
file(SHA256 "${WORK_DIR}/collector.bin" image_hash)
file(SHA256 "${WORK_DIR}/input.wal" wal_hash)
file(WRITE "${WORK_DIR}/replay.ack" "previous ACK output")
require_command(0 "\"accepted_frames\":0,\"corrupt_frames\":90,\"duplicate_frames\":80"
    "${COLLECTOR}" ${options} --ack-out replay.ack)
require_command(0 "" "${FIXTURE}" verify_replay)

set(invalid_options --wal-in input.wal --db-img invalid.bin)
require_command(2 "missing value" "${COLLECTOR}" ${invalid_options} --channels)
require_command(2 "missing value" "${COLLECTOR}" ${invalid_options} --ack-out --channels 1)
require_command(2 "invalid option" "${COLLECTOR}" ${invalid_options} --unknown value)
require_command(2 "invalid option" "${COLLECTOR}" ${invalid_options} --device-id 4294967296)
require_command(2 "invalid option" "${COLLECTOR}" ${invalid_options} --device-id 42suffix)
require_command(2 "invalid option" "${COLLECTOR}" ${invalid_options} --sectors -1)
require_command(2 "invalid option" "${COLLECTOR}" ${invalid_options} --sectors 1025)
require_command(2 "invalid option" "${COLLECTOR}" ${invalid_options} --channels 257)
require_command(2 "invalid flash geometry" "${COLLECTOR}" ${invalid_options} --channels 0)
require_command(2 "invalid flash geometry" "${COLLECTOR}" ${invalid_options} --sectors 1)
require_command(2 "invalid flash geometry" "${COLLECTOR}" ${invalid_options} --sector-size 300)
require_command(2 "invalid flash geometry"
    "${COLLECTOR}" ${invalid_options} --sector-size 1073741824 --sectors 4)
require_command(5 "cannot open WAL input" "${COLLECTOR}" --wal-in absent.wal --db-img invalid.bin)
if(EXISTS "${WORK_DIR}/invalid.bin")
    message(FATAL_ERROR "Invalid arguments or inaccessible WAL created an image")
endif()

file(WRITE "${WORK_DIR}/short.bin" "short image")
require_command(3 "cannot use backing image" "${COLLECTOR}" ${options} --db-img short.bin)
file(READ "${WORK_DIR}/short.bin" short_image)
if(NOT short_image STREQUAL "short image")
    message(FATAL_ERROR "Mismatched backing file was changed")
endif()
require_command(3 "cannot use backing image" "${COLLECTOR}" ${options} --db-img input.wal)
require_command(6 "cannot use ACK output" "${COLLECTOR}" ${options} --ack-out input.wal)
require_command(6 "cannot use ACK output" "${COLLECTOR}" ${options} --ack-out collector.bin)
file(CREATE_LINK "${WORK_DIR}/collector.bin" "${WORK_DIR}/alias.bin" RESULT link_result)
if(link_result STREQUAL "0")
    require_command(6 "cannot use ACK output" "${COLLECTOR}" ${options} --ack-out alias.bin)
endif()
require_command(6 "cannot use ACK output" "${COLLECTOR}" ${options} --ack-out .)
require_command(7 "incomplete WAL stream" "${COLLECTOR}" ${options} --wal-in incomplete.wal)
file(SHA256 "${WORK_DIR}/collector.bin" final_image_hash)
file(SHA256 "${WORK_DIR}/input.wal" final_wal_hash)
if(NOT final_image_hash STREQUAL image_hash OR NOT final_wal_hash STREQUAL wal_hash)
    message(FATAL_ERROR "Image or source WAL changed during replay or rejected operations")
endif()
