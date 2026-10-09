add_library(ecu_daf_sac_profile STATIC
  src/profile.cpp
  src/identification_program.cpp
  src/service_program.cpp
)

add_library(ECU::daf_sac_profile ALIAS ecu_daf_sac_profile)

target_include_directories(ecu_daf_sac_profile
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(
  ecu_daf_sac_profile
  PUBLIC
    ECU::dut_profile
    ECU::core_v2
)

target_compile_features(ecu_daf_sac_profile PUBLIC cxx_std_17)
set_target_properties(ecu_daf_sac_profile PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
  target_compile_options(ecu_daf_sac_profile PRIVATE /W4 /WX)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(
    ecu_daf_sac_profile
    PRIVATE -Wall -Wextra -Wpedantic -Werror
  )
endif()
