add_library(ecu_core_v2 STATIC
  src/can_types.cpp
  src/can_bus_runtime.cpp
  src/deadline_watchdog.cpp
  src/cyclic_can_actuator_runtime.cpp
  src/resource_manager.cpp
  src/command_dispatcher.cpp
  src/state_registry.cpp
  src/event_bus.cpp
  src/module_registry.cpp
  src/dut_registry.cpp
  src/j1939_identifier.cpp
  src/j1939_name.cpp
  src/address_claim.cpp
  src/network_manager.cpp
  src/request.cpp
  src/acknowledgment.cpp
  src/fd_contained_pg.cpp
  src/fd_transport_protocol.cpp
  src/isotp_endpoint.cpp
  src/uds_types.cpp
  src/uds_services.cpp
  src/uds_client.cpp
  src/diagnostics.cpp
  src/transport_protocol.cpp
  src/extended_transport.cpp
  src/working_set.cpp
  src/isobus_network_management.cpp
  src/working_set_lifecycle.cpp
)

add_library(ECU::core_v2 ALIAS ecu_core_v2)

target_include_directories(ecu_core_v2
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(ecu_core_v2 PUBLIC cxx_std_17)

set_target_properties(ecu_core_v2 PROPERTIES
  CXX_EXTENSIONS OFF
)

if(MSVC)
  target_compile_options(
    ecu_core_v2
    PRIVATE /W4 /WX /permissive- /Zc:__cplusplus /GR-
  )
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(
    ecu_core_v2
    PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Werror
      -fno-exceptions
      -fno-rtti
  )
endif()
