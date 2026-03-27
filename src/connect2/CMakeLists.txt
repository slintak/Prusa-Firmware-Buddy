set(CONNECT2_TELEMETRY_PROTO ${CMAKE_SOURCE_DIR}/specs/connect2/telemetry.proto)
set(CONNECT2_TELEMETRY_OPTIONS ${CMAKE_SOURCE_DIR}/specs/connect2/telemetry.options)
set(CONNECT2_GCODE_PROTO ${CMAKE_SOURCE_DIR}/specs/connect2/gcode.proto)
set(CONNECT2_GCODE_OPTIONS ${CMAKE_SOURCE_DIR}/specs/connect2/gcode.options)
set(CONNECT2_COMMAND_PROTO ${CMAKE_SOURCE_DIR}/specs/connect2/command.proto)
set(CONNECT2_COMMAND_OPTIONS ${CMAKE_SOURCE_DIR}/specs/connect2/command.options)
set(CONNECT2_EVENT_PROTO ${CMAKE_SOURCE_DIR}/specs/connect2/event.proto)
set(CONNECT2_EVENT_OPTIONS ${CMAKE_SOURCE_DIR}/specs/connect2/event.options)
set(CONNECT2_NANOPB_GENERATOR ${CMAKE_SOURCE_DIR}/lib/Middlewares/Third_Party/nanopb/generator/nanopb_generator.py)
set(CONNECT2_NANOPB_PYTHON ${CMAKE_SOURCE_DIR}/utils/oauth-mqtt/.venv/bin/python)
set(CONNECT2_TELEMETRY_PB_C ${CMAKE_CURRENT_SOURCE_DIR}/telemetry.pb.c)
set(CONNECT2_TELEMETRY_PB_H ${CMAKE_CURRENT_SOURCE_DIR}/telemetry.pb.h)
set(CONNECT2_GCODE_PB_C ${CMAKE_CURRENT_SOURCE_DIR}/gcode.pb.c)
set(CONNECT2_GCODE_PB_H ${CMAKE_CURRENT_SOURCE_DIR}/gcode.pb.h)
set(CONNECT2_COMMAND_PB_C ${CMAKE_CURRENT_SOURCE_DIR}/command.pb.c)
set(CONNECT2_COMMAND_PB_H ${CMAKE_CURRENT_SOURCE_DIR}/command.pb.h)
set(CONNECT2_EVENT_PB_C ${CMAKE_CURRENT_SOURCE_DIR}/event.pb.c)
set(CONNECT2_EVENT_PB_H ${CMAKE_CURRENT_SOURCE_DIR}/event.pb.h)

if(EXISTS ${CONNECT2_NANOPB_PYTHON})
  add_custom_command(
    OUTPUT ${CONNECT2_TELEMETRY_PB_C} ${CONNECT2_TELEMETRY_PB_H}
    COMMAND ${CONNECT2_NANOPB_PYTHON} ${CONNECT2_NANOPB_GENERATOR}
            -I ${CMAKE_SOURCE_DIR}/specs/connect2
            -D ${CMAKE_CURRENT_SOURCE_DIR}
            ${CONNECT2_TELEMETRY_PROTO}
    DEPENDS ${CONNECT2_TELEMETRY_PROTO} ${CONNECT2_TELEMETRY_OPTIONS} ${CONNECT2_NANOPB_GENERATOR}
    COMMENT "Generating nanopb sources for telemetry.proto"
    VERBATIM
  )
  add_custom_command(
    OUTPUT ${CONNECT2_GCODE_PB_C} ${CONNECT2_GCODE_PB_H}
    COMMAND ${CONNECT2_NANOPB_PYTHON} ${CONNECT2_NANOPB_GENERATOR}
            -I ${CMAKE_SOURCE_DIR}/specs/connect2
            -D ${CMAKE_CURRENT_SOURCE_DIR}
            ${CONNECT2_GCODE_PROTO}
    DEPENDS ${CONNECT2_GCODE_PROTO} ${CONNECT2_GCODE_OPTIONS} ${CONNECT2_NANOPB_GENERATOR}
    COMMENT "Generating nanopb sources for gcode.proto"
    VERBATIM
  )
  add_custom_command(
    OUTPUT ${CONNECT2_COMMAND_PB_C} ${CONNECT2_COMMAND_PB_H}
    COMMAND ${CONNECT2_NANOPB_PYTHON} ${CONNECT2_NANOPB_GENERATOR}
            -I ${CMAKE_SOURCE_DIR}/specs/connect2
            -D ${CMAKE_CURRENT_SOURCE_DIR}
            ${CONNECT2_COMMAND_PROTO}
    DEPENDS ${CONNECT2_COMMAND_PROTO} ${CONNECT2_COMMAND_OPTIONS} ${CONNECT2_NANOPB_GENERATOR}
    COMMENT "Generating nanopb sources for command.proto"
    VERBATIM
  )
  add_custom_command(
    OUTPUT ${CONNECT2_EVENT_PB_C} ${CONNECT2_EVENT_PB_H}
    COMMAND ${CONNECT2_NANOPB_PYTHON} ${CONNECT2_NANOPB_GENERATOR}
            -I ${CMAKE_SOURCE_DIR}/specs/connect2
            -D ${CMAKE_CURRENT_SOURCE_DIR}
            ${CONNECT2_EVENT_PROTO}
    DEPENDS ${CONNECT2_EVENT_PROTO} ${CONNECT2_EVENT_OPTIONS} ${CONNECT2_NANOPB_GENERATOR} ${CONNECT2_COMMAND_PROTO}
    COMMENT "Generating nanopb sources for event.proto"
    VERBATIM
  )
  add_custom_target(connect2_proto
    DEPENDS
      ${CONNECT2_TELEMETRY_PB_C} ${CONNECT2_TELEMETRY_PB_H}
      ${CONNECT2_GCODE_PB_C} ${CONNECT2_GCODE_PB_H}
      ${CONNECT2_COMMAND_PB_C} ${CONNECT2_COMMAND_PB_H}
      ${CONNECT2_EVENT_PB_C} ${CONNECT2_EVENT_PB_H})
  add_dependencies(firmware connect2_proto)
endif()

target_sources(
  firmware
  PRIVATE client.cpp
          config.cpp
          hostname.cpp
          oauth_storage.cpp
          command.pb.c
          event.pb.c
          gcode.pb.c
          telemetry.cpp
          telemetry.pb.c
          run.cpp
          log_component.cpp
          ../../lib/Middlewares/Third_Party/nanopb/pb_encode.c
          ../../lib/Middlewares/Third_Party/nanopb/pb_decode.c
          ../../lib/Middlewares/Third_Party/nanopb/pb_common.c
  )

target_include_directories(
  firmware
  PRIVATE ../../lib/Middlewares/Third_Party/nanopb
)

# TEMP: Reuse selected Connect sources for telemetry/Marlin integration.
# TODO: Extract shared printer/config/host utilities to common/ and stop
#       depending on src/connect from CONNECT2.
target_sources(
  firmware
  PRIVATE ../connect/hostname.cpp
          ../connect/printer.cpp
          ../connect/printer_common.cpp
          ../connect/marlin_printer.cpp
  )
