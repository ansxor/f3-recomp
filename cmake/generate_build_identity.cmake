# Generate build_identity.hpp with a deterministic SHA-256 build hash
# Variables:
#   SOURCE_DIR
#   BINARY_DIR
#   GAME_ID
#   INPUT_FILES_LIST
#   TEMPLATE_FILE
#   OUTPUT_FILE

if(NOT EXISTS "${INPUT_FILES_LIST}")
    message(FATAL_ERROR "INPUT_FILES_LIST not found: ${INPUT_FILES_LIST}")
endif()

file(STRINGS "${INPUT_FILES_LIST}" _files)
set(_hash_data "")
list(SORT _files)

foreach(_file IN LISTS _files)
    if(NOT EXISTS "${_file}")
        continue()
    endif()
    # Compute relative path
    file(RELATIVE_PATH _rel "${SOURCE_DIR}" "${_file}")
    if(_rel MATCHES "^\\.\\./")
        file(RELATIVE_PATH _rel_bin "${BINARY_DIR}" "${_file}")
        if(NOT _rel_bin MATCHES "^\\.\\./")
            set(_rel "BUILD/${_rel_bin}")
        endif()
    endif()
    file(SHA256 "${_file}" _fhash)
    string(APPEND _hash_data "${_rel}:${_fhash};")
endforeach()

string(SHA256 F3_BUILD_HASH_HEX "${_hash_data}")

set(F3_BUILD_HASH_BYTES "")
string(LENGTH "${F3_BUILD_HASH_HEX}" _hex_len)
math(EXPR _byte_count "${_hex_len} / 2")
math(EXPR _last_idx "${_byte_count} - 1")
foreach(_i RANGE 0 ${_last_idx})
    math(EXPR _start "${_i} * 2")
    string(SUBSTRING "${F3_BUILD_HASH_HEX}" ${_start} 2 _hex_byte)
    if(_i EQUAL 0)
        string(APPEND F3_BUILD_HASH_BYTES "0x${_hex_byte}")
    else()
        string(APPEND F3_BUILD_HASH_BYTES ", 0x${_hex_byte}")
    endif()
endforeach()

set(F3_GAME "${GAME_ID}")
configure_file("${TEMPLATE_FILE}" "${OUTPUT_FILE}.tmp" @ONLY)

execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${OUTPUT_FILE}.tmp" "${OUTPUT_FILE}"
)
file(REMOVE "${OUTPUT_FILE}.tmp")
