cmake_minimum_required(VERSION 3.13)
file(READ static.html PICO_WS_SERVER_STATIC_HTML_HEX HEX)

project(bthome-websocket-bridge C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

add_library(lwipopts_provider INTERFACE)
target_include_directories(lwipopts_provider INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_definitions(lwipopts_provider INTERFACE
  CYW43_HOST_NAME=\"BTHOME_REPEATER\"
  PICO_WS_SERVER_STATIC_HTML_HEX=\"${PICO_WS_SERVER_STATIC_HTML_HEX}\"
  ENABLE_DEBUG=0      # pico-ws-server enable debug logging
  NDEBUG=1          # LWIP disable debug logging
)

add_executable(bthome-websocket-bridge
  src/bthome-websocket-bridge.cpp
)

target_link_libraries(bthome-websocket-bridge
  pico_stdlib
  pico_cyw43_arch_lwip_poll
  pico_btstack_ble
  pico_btstack_cyw43
  pico_ws_server
)

target_compile_definitions(bthome-websocket-bridge PRIVATE
  WIFI_SSID=\"${WIFI_SSID}\"
  WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
  MAX_WS_CONNECTIONS=2
  BT_DEVICE_COUNT=4           # How many 
  WIFI_STATUS_POLL_MS=10000   # How often to check wifi is still connected
  ENABLE_DEBUG=0              # bthome-websocket-bridge enable debug logging
  )

pico_add_extra_outputs(bthome-websocket-bridge)
