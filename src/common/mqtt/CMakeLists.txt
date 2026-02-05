target_sources(
  firmware
  PRIVATE connection.cpp
          socket.cpp
          mqtt_client.cpp
          mqtt_transport.cpp
          mqtt_pal.cpp
          log_component.cpp
          ../tls/hardware_rng.cpp
          ../tls/net_sockets.cpp
          ../tls/tls.cpp
          ../../../lib/Middlewares/Third_Party/MQTT-C/src/mqtt.c
  )

target_include_directories(
  firmware
  PRIVATE ../../../lib/Middlewares/Third_Party/MQTT-C/include
)

target_compile_definitions(
  firmware
  PRIVATE MQTTC_PAL_FILE=common/mqtt/mqtt_pal.h
)
