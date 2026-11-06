idf_component_register(
    SRCS "main.c" "bridge_transport.c" "bridge_protocol.c" "ekf_service.c" "app_ekf.c"
         "../ThirdParty/mul_core/mul_telemetry.c"
         "../ThirdParty/mul_core/mul_ekf.c" "../ThirdParty/mul_core/mul_ekf_stream.c"
         "../ThirdParty/mul_core/mul_pose.c" "../ThirdParty/mul_core/mul_schedule.c"
         "../ThirdParty/mul_core/mul_common.c"
    INCLUDE_DIRS "." "../ThirdParty/mul_core")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/mul_core/verify.py RESULT_VARIABLE verify_result)
if(NOT verify_result EQUAL 0)
 message(FATAL_ERROR "MUL core verification failed")
endif()
add_custom_target(verify_mul_core ALL COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/mul_core/verify.py)
add_dependencies(${COMPONENT_LIB} verify_mul_core)

set(BRIDGE_UART_BAUD "921600" CACHE STRING "ESP UART1 baud; match STM32 APP_ESP_BAUD")
if(NOT BRIDGE_UART_BAUD MATCHES "^(115200|921600)$")
 message(FATAL_ERROR "Use115200 or921600 baud")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE BRIDGE_UART_BAUD=${BRIDGE_UART_BAUD}U)
