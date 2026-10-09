# Imported Core V2 / Bench / DUT profile regression suite.
# Every test is a standalone executable that returns non-zero on failure.

function(ecu_add_test name source)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE ${ARGN})
  set_target_properties(${name} PROPERTIES CXX_EXTENSIONS OFF)
  if(MSVC)
    target_compile_options(${name} PRIVATE /W4 /WX)
  else()
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

file(GLOB ECU_PORTABLE_TESTS CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/core_v2_*_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/bench_*_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/dut_profile_*_tests.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/daf_sac_profile_tests.cpp)

foreach(source IN LISTS ECU_PORTABLE_TESTS)
  get_filename_component(stem ${source} NAME_WE)
  ecu_add_test(ecu_${stem} ${source} ECU::daf_sac_profile)
endforeach()

if(TARGET ECU::platform_linux_v2)
  ecu_add_test(ecu_linux_v2_platform_tests linux_v2_platform_tests.cpp
    ECU::platform_linux_v2)
endif()
