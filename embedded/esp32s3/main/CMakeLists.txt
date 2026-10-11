include(${COMPONENT_DIR}/../../cmake/solo_sources.cmake)

set(srcs main.c ${TINYCRYPT_PORT_SOURCES})
set(embed "")
# The notifier (CRYPT-15) needs the key's certificate from infra/scripts/provision-key.sh.
if(CONFIG_TINYCRYPT_NOTIFIER)
  list(APPEND srcs notifier.c)
  set(certs ${COMPONENT_DIR}/../certs)
  foreach(f AmazonRootCA1.pem device.pem.crt device.pem.key)
    if(NOT EXISTS ${certs}/${f})
      message(FATAL_ERROR "certs/${f} is missing: run infra/scripts/provision-key.sh")
    endif()
    list(APPEND embed ${certs}/${f})
  endforeach()
endif()
# The Secure Enclave vector self-test (CRYPT-10) is test firmware only.
if(TINYCRYPT_INSECURE_SOFT_KEY)
  list(APPEND srcs se_selftest.c)
endif()

# WHOLE_ARCHIVE so the port's strong device hooks always win over solo1's weak defaults.
idf_component_register(
  SRCS ${srcs}
  PRIV_REQUIRES solo_core nvs_flash esp_driver_gpio esp_timer bootloader_support espcoredump mbedtls
                esp_wifi esp_netif esp_event mqtt json
  EMBED_TXTFILES ${embed}
  WHOLE_ARCHIVE
)

if(TINYCRYPT_INSECURE_SOFT_KEY)
  include(${COMPONENT_DIR}/../../cmake/se_vector.cmake)
  tinycrypt_se_vector_header(${CMAKE_CURRENT_BINARY_DIR}/tinycrypt_se_vector.h)
  target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  target_compile_definitions(${COMPONENT_LIB} PRIVATE TINYCRYPT_SE_SELFTEST)
endif()
