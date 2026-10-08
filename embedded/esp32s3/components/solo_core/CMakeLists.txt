include(${COMPONENT_DIR}/../../../cmake/solo_sources.cmake)

idf_component_register(
  SRCS ${SOLO_CORE_SOURCES}
  INCLUDE_DIRS ${SOLO_CORE_INCLUDES}
)
target_compile_definitions(${COMPONENT_LIB} PUBLIC ${SOLO_CORE_DEFINES})
# Upstream code: keep its warnings out of our build output.
target_compile_options(${COMPONENT_LIB} PRIVATE -w -fcommon)
