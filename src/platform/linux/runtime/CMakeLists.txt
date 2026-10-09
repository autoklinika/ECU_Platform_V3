# Linux implementations of Bench Runtime contracts (lock, link control).
add_library(ecu_platform_linux_runtime STATIC
  src/flock_exclusive_lock.cpp
  src/ip_can_link_control.cpp
)
add_library(ECU::platform_linux_runtime ALIAS ecu_platform_linux_runtime)

target_include_directories(ecu_platform_linux_runtime
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(ecu_platform_linux_runtime
  PUBLIC ECU::runtime ECU::platform_linux_v2
)
target_compile_features(ecu_platform_linux_runtime PUBLIC cxx_std_17)
set_target_properties(ecu_platform_linux_runtime PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(ecu_platform_linux_runtime
  PRIVATE -Wall -Wextra -Wpedantic -Werror
)
