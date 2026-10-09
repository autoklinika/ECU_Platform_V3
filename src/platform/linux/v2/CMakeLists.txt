add_library(ecu_platform_linux_v2 STATIC
  src/boottime_clock.cpp
  src/socketcan_adapter.cpp
  src/socketcan_link_info.cpp
)

add_library(ECU::platform_linux_v2 ALIAS ecu_platform_linux_v2)

target_include_directories(ecu_platform_linux_v2
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(
  ecu_platform_linux_v2
  PUBLIC ECU::core_v2
)

target_compile_features(ecu_platform_linux_v2 PUBLIC cxx_std_17)
set_target_properties(ecu_platform_linux_v2 PROPERTIES CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(
    ecu_platform_linux_v2
    PRIVATE -Wall -Wextra -Wpedantic -Werror
  )
endif()
