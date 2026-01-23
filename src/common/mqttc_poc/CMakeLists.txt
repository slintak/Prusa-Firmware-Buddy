target_sources(firmware PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/mqttc_poc.cpp
  ${CMAKE_CURRENT_LIST_DIR}/mqttc_pal.c
  ${PROJECT_ROOT_DIR}/lib/Middlewares/Third_Party/MQTT-C/src/mqtt.c
)

target_include_directories(firmware PRIVATE
  ${PROJECT_ROOT_DIR}/lib/Middlewares/Third_Party/MQTT-C/include
  ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_definitions(firmware PRIVATE
  MQTTC_PAL_FILE=mqttc_pal.h
)
