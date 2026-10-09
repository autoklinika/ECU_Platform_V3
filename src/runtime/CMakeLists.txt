# Bench Runtime: portable single-owner runtime built on Core V2.
add_library(ecu_runtime STATIC
  src/can_owner.cpp
)
add_library(ECU::runtime ALIAS ecu_runtime)

target_include_directories(ecu_runtime
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(ecu_runtime PUBLIC ECU::daf_sac_profile)
target_compile_features(ecu_runtime PUBLIC cxx_std_17)
set_target_properties(ecu_runtime PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
  target_compile_options(ecu_runtime PRIVATE /W4 /WX)
else()
  target_compile_options(ecu_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
