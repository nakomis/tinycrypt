# Native (Mac sim) build of the solo1 core as a static library.
include(${CMAKE_CURRENT_LIST_DIR}/solo_sources.cmake)

add_library(solo_core STATIC ${SOLO_CORE_SOURCES})
target_include_directories(solo_core PUBLIC ${SOLO_CORE_INCLUDES})
target_compile_definitions(solo_core PUBLIC ${SOLO_CORE_DEFINES})
# Upstream code: keep its warnings out of our build output.
target_compile_options(solo_core PRIVATE -w -fcommon)
