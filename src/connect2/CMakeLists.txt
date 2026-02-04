target_sources(
  firmware
  PRIVATE client.cpp
          config.cpp
          hostname.cpp
          run.cpp
          log_component.cpp
          tls/hardware_rng.cpp
          tls/net_sockets.cpp
          tls/tls.cpp
  )
