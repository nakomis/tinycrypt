# WHOLE_ARCHIVE so the port's strong device hooks always win over solo1's weak defaults.
idf_component_register(
  SRCS main.c
  PRIV_REQUIRES solo_core nvs_flash esp_driver_gpio esp_timer bootloader_support espcoredump
  WHOLE_ARCHIVE
)
