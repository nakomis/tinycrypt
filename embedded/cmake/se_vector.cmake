# Turns the first Secure Enclave test vector (tests/vectors/se_p256.json) into a
# C header, so firmware self-tests check the very bytes the pytest suite checks.
#   tinycrypt_se_vector_header(<output header path>)
set(TINYCRYPT_SE_VECTOR_JSON ${CMAKE_CURRENT_LIST_DIR}/../tests/vectors/se_p256.json)

function(_tinycrypt_hex_to_c hex out_var)
  string(REGEX REPLACE "([0-9a-fA-F][0-9a-fA-F])" "0x\\1," bytes "${hex}")
  set(${out_var} "${bytes}" PARENT_SCOPE)
endfunction()

function(tinycrypt_se_vector_header out)
  file(READ ${TINYCRYPT_SE_VECTOR_JSON} json)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${TINYCRYPT_SE_VECTOR_JSON})
  string(JSON pub GET "${json}" pub)
  string(JSON msg GET "${json}" cases 0 message)
  string(JSON sig GET "${json}" cases 0 sig)
  string(JSON name GET "${json}" cases 0 name)
  string(LENGTH "${pub}" pub_len)
  string(LENGTH "${sig}" sig_len)
  string(LENGTH "${msg}" msg_len)
  if(NOT pub_len EQUAL 128 OR NOT sig_len EQUAL 128 OR msg_len EQUAL 0)
    message(FATAL_ERROR "${TINYCRYPT_SE_VECTOR_JSON}: pub and sig must be 64 bytes, message non-empty")
  endif()
  _tinycrypt_hex_to_c(${pub} pub_c)
  _tinycrypt_hex_to_c(${msg} msg_c)
  _tinycrypt_hex_to_c(${sig} sig_c)
  file(CONFIGURE OUTPUT ${out} CONTENT
"// Generated from tests/vectors/se_p256.json by cmake/se_vector.cmake. Do not edit.
#pragma once
#include <stdint.h>
#define TINYCRYPT_SE_VECTOR_NAME \"@name@\"
static const uint8_t tinycrypt_se_vector_pub[64] = {@pub_c@};
static const uint8_t tinycrypt_se_vector_msg[] = {@msg_c@};
static const uint8_t tinycrypt_se_vector_sig[64] = {@sig_c@};
" @ONLY)
endfunction()
