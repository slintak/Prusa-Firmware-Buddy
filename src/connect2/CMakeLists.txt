target_sources(
  firmware
  PRIVATE client.cpp
          config.cpp
          hostname.cpp
          oauth_storage.cpp
          telemetry.cpp
          run.cpp
          log_component.cpp
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
