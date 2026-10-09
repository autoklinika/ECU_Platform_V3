add_library(ecu_bench STATIC
  src/session.cpp
  src/event_bus_publisher.cpp
  src/host_service.cpp
)

add_library(ECU::bench ALIAS ecu_bench)

target_include_directories(ecu_bench
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ecu_bench PUBLIC ECU::core_v2)
target_compile_features(ecu_bench PUBLIC cxx_std_17)
set_target_properties(ecu_bench PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
  target_compile_options(ecu_bench PRIVATE /W4 /WX)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(ecu_bench PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
