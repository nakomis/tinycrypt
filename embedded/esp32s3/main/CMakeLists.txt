include(${COMPONENT_DIR}/../../cmake/solo_sources.cmake)

# The provisioner (CRYPT-22) is its own image: it configures and locks the ATECC608B and prints the
# IoT key's CSR. It never ships as an update, and holds only what that needs.
if(CONFIG_TINYCRYPT_PROVISIONER)
  set(srcs provisioner.c ${TINYCRYPT_ROOT}/port/x509_min.c)
else()
  set(srcs main.c ota.c led_status.c se_tls.c ${TINYCRYPT_PORT_SOURCES})
endif()
# The secure element backend.
if(TINYCRYPT_KEYSTORE_ATECC)
  list(APPEND srcs se_atecc.c)
else()
  list(APPEND srcs ${TINYCRYPT_SE_SOFT_SOURCES} se_soft_nvs.c)
endif()
set(embed "")
if(CONFIG_TINYCRYPT_WATCH_BLE AND NOT CONFIG_TINYCRYPT_PROVISIONER)
  list(APPEND srcs ble_presence.c)
endif()
# Amazon Root CA 1 (public) for IoT Core and the OTA download. The key's own certificate, key and
# settings are not in the image: they're in the prov partition (prov.h, provision-key.sh).
list(APPEND embed ${COMPONENT_DIR}/../AmazonRootCA1.pem)
if(CONFIG_TINYCRYPT_NOTIFIER AND NOT CONFIG_TINYCRYPT_PROVISIONER)
  list(APPEND srcs notifier.c prov.c)
  if(CONFIG_TINYCRYPT_METRICS)
    list(APPEND srcs metrics.c)
  endif()
endif()
# The Secure Enclave vector self-test (CRYPT-10) is test firmware only.
if(TINYCRYPT_INSECURE_SOFT_KEY AND NOT CONFIG_TINYCRYPT_PROVISIONER)
  list(APPEND srcs se_selftest.c)
endif()

# WHOLE_ARCHIVE so the port's strong device hooks always win over solo1's weak defaults.
idf_component_register(
  SRCS ${srcs}
  PRIV_REQUIRES solo_core nvs_flash esp_driver_gpio esp_timer bootloader_support espcoredump mbedtls
                esp_wifi esp_netif esp_event mqtt json bt app_update esp_http_client esp_app_format
                espressif__esp-cryptoauthlib espressif__libsodium esp_driver_uart esp_driver_usb_serial_jtag esp_vfs_console
  EMBED_TXTFILES ${embed}
  WHOLE_ARCHIVE
)

if(TINYCRYPT_INSECURE_SOFT_KEY AND NOT CONFIG_TINYCRYPT_PROVISIONER)
  include(${COMPONENT_DIR}/../../cmake/se_vector.cmake)
  tinycrypt_se_vector_header(${CMAKE_CURRENT_BINARY_DIR}/tinycrypt_se_vector.h)
  target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
  target_compile_definitions(${COMPONENT_LIB} PRIVATE TINYCRYPT_SE_SELFTEST)
endif()
