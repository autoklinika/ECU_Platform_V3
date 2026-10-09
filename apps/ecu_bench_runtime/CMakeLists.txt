add_executable(ecu_bench_runtime main.cpp)
target_link_libraries(ecu_bench_runtime PRIVATE ECU::platform_linux_runtime)
set_target_properties(ecu_bench_runtime PROPERTIES CXX_EXTENSIONS OFF)
target_compile_options(ecu_bench_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
