# Every test is a standalone executable that returns non-zero on failure.

function(ecu_add_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE ${ARGN})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  set_target_properties(${name} PROPERTIES CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(${name} PRIVATE /W4 /WX)
  else()
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

# Core V2 regression suite (taken with Core V2, unchanged).
file(GLOB ECU_CORE_V2_TESTS CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/core_v2_*_tests.cpp)
foreach(source IN LISTS ECU_CORE_V2_TESTS)
  get_filename_component(stem ${source} NAME_WE)
  ecu_add_test(ecu_${stem} ${source} ECU::core_v2)
endforeach()

# V3 runtime tests.
file(GLOB ECU_RUNTIME_TESTS CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/runtime_*_tests.cpp)
foreach(source IN LISTS ECU_RUNTIME_TESTS)
  get_filename_component(stem ${source} NAME_WE)
  ecu_add_test(ecu_${stem} ${source} ECU::runtime)
endforeach()

if(TARGET ECU::platform_linux_runtime)
  ecu_add_test(ecu_linux_runtime_platform_tests linux_runtime_platform_tests.cpp
    ECU::platform_linux_runtime)
  add_test(NAME ecu_runtime_app_e2e
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/runtime_app_e2e.sh
            $<TARGET_FILE:ecu_bench_runtime>)
endif()

# Bench check scripts: syntax and behaviour on a simulated bench (no hardware).
find_program(ECU_BASH bash)
if(ECU_BASH AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME ecu_bench_krok1_sim
    COMMAND ${ECU_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/bench_krok1_sim.sh)
endif()
