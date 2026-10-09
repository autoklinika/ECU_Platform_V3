add_library(ecu_dut_profile STATIC
  src/profile.cpp
  src/registry.cpp
  src/runtime.cpp
)

add_library(ECU::dut_profile ALIAS ecu_dut_profile)

target_include_directories(ecu_dut_profile
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(ecu_dut_profile PUBLIC ECU::bench)
target_compile_features(ecu_dut_profile PUBLIC cxx_std_17)
set_target_properties(ecu_dut_profile PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
  target_compile_options(ecu_dut_profile PRIVATE /W4 /WX)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(
    ecu_dut_profile
    PRIVATE -Wall -Wextra -Wpedantic -Werror
  )
endif()
