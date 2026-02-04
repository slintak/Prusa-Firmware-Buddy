find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(NANOPB_DIR ${CMAKE_SOURCE_DIR}/lib/Middlewares/Third_Party/nanopb)
set(NANOPB_GENERATOR ${NANOPB_DIR}/generator/nanopb_generator.py)
set(INFO_PROTO ${CMAKE_SOURCE_DIR}/specs/connect2/info_event.proto)
set(INFO_PB_C ${CMAKE_CURRENT_SOURCE_DIR}/info_event.pb.c)
set(INFO_PB_H ${CMAKE_CURRENT_SOURCE_DIR}/info_event.pb.h)
set(INFO_OPTIONS ${CMAKE_SOURCE_DIR}/specs/connect2/info_event.options)

add_custom_command(
  OUTPUT ${INFO_PB_C} ${INFO_PB_H}
  COMMAND ${Python3_EXECUTABLE} ${NANOPB_GENERATOR}
          -I ${CMAKE_SOURCE_DIR}/specs/connect2
          -D ${CMAKE_CURRENT_SOURCE_DIR}
          ${INFO_PROTO}
  DEPENDS ${INFO_PROTO} ${INFO_OPTIONS}
  COMMENT "Generating nanopb sources for info_event.proto"
  VERBATIM
)

add_custom_target(connect2_info_proto DEPENDS ${INFO_PB_C} ${INFO_PB_H})

target_sources(
  firmware
  PRIVATE client.cpp
          config.cpp
          hostname.cpp
          info_event.cpp
          telemetry.cpp
          run.cpp
          log_component.cpp
          ${INFO_PB_C}
          ${NANOPB_DIR}/pb_encode.c
          ${NANOPB_DIR}/pb_decode.c
          ${NANOPB_DIR}/pb_common.c
  )

add_dependencies(firmware connect2_info_proto)
target_include_directories(firmware PRIVATE ${NANOPB_DIR})

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
