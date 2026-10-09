cmake_minimum_required(VERSION 3.25)

project(
  ECUPlatformV3
  VERSION 3.0.0
  DESCRIPTION "ECU Platform V3 - single bench runtime on the portable Core V2"
  LANGUAGES CXX
)

option(ECU_BUILD_TESTS "Build ECU Platform V3 tests" ON)

set(ECU_DEFAULT_LINUX_PLATFORM OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(ECU_DEFAULT_LINUX_PLATFORM ON)
endif()
option(ECU_BUILD_LINUX_PLATFORM
  "Build Linux platform adapters (SocketCAN, boottime clock). Never a Core dependency."
  ${ECU_DEFAULT_LINUX_PLATFORM})

if(ECU_BUILD_LINUX_PLATFORM AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "ECU_BUILD_LINUX_PLATFORM=ON requires a Linux target")
endif()

# Portable layers (imported unchanged from ECU Platform V2, see docs/SOURCE.md).
add_subdirectory(src/core_v2)
add_subdirectory(src/bench)
add_subdirectory(src/dut_profile)
add_subdirectory(src/dut_profiles/daf_sac)
add_subdirectory(src/runtime)

# Platform layer.
if(ECU_BUILD_LINUX_PLATFORM)
  add_subdirectory(src/platform/linux/v2)
  add_subdirectory(src/platform/linux/runtime)
  add_subdirectory(apps/ecu_bench_runtime)
endif()

if(ECU_BUILD_TESTS)
  include(CTest)
  enable_testing()
  add_subdirectory(tests)
endif()
