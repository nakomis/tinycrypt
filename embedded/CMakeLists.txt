cmake_minimum_required(VERSION 3.20)
project(tinycrypt C)

option(TINYCRYPT_INSECURE_SOFT_KEY "INSECURE software keystore - TEST ONLY" OFF)
option(TINYCRYPT_RELEASE "Release configuration (refuses the software keystore)" OFF)

if(TINYCRYPT_RELEASE AND TINYCRYPT_INSECURE_SOFT_KEY)
  message(FATAL_ERROR "TINYCRYPT_INSECURE_SOFT_KEY must never be built in a release configuration")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/solo_core.cmake)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
target_link_libraries(solo_core PUBLIC PkgConfig::SODIUM)

add_executable(tinycrypt-sim sim/sim_main.c)
target_link_libraries(tinycrypt-sim PRIVATE solo_core)

# Our own portable code: built with warnings on, unlike the upstream core.
add_library(tinycrypt_port STATIC ${TINYCRYPT_PORT_SOURCES})
target_link_libraries(tinycrypt_port PUBLIC solo_core)
target_compile_options(tinycrypt_port PRIVATE -Wall -Wextra -Werror)

add_executable(tinycrypt-sigcheck sim/sigcheck_main.c)
target_link_libraries(tinycrypt-sigcheck PRIVATE tinycrypt_port)
target_compile_options(tinycrypt-sigcheck PRIVATE -Wall -Wextra -Werror)
