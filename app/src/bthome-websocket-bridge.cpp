#include <stdio.h>
#include <map>
#include <string>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico_ws_server/web_socket_server.h"
#include "debug.h"
#include "btstack.h"

WebSocketServer server(MAX_WS_CONNECTIONS);
uint32_t sw_timer;
std::map<std::string, std::string> bt_device_map;
static btstack_packet_callback_registration_t hci_event_callback_registration;

void on_connect(WebSocketServer& server, uint32_t conn_id) {
  DEBUG("WebSocket opened");
  for (const auto& [key, value] : bt_device_map) {
    std::string message = key + value;
    server.broadcastMessage(message.c_str());
  }
}

void on_disconnect(WebSocketServer& server, uint32_t conn_id) {
  DEBUG("WebSocket closed");
}

void on_message(WebSocketServer& server, uint32_t conn_id, const void* data, size_t len) {
  return;
}

std::string to_hex_string(const uint8_t* array, size_t length) {
    std::string hex_string;
    hex_string.reserve(length * 2); // Reserve space for performance

    for (size_t i = 0; i < length; ++i) {
        uint8_t byte = array[i];
        // Convert high nibble (4 most significant bits) to a hex character
        hex_string += char_for_nibble((byte >> 4) & 0x0F);
        // Convert low nibble (4 least significant bits) to a hex character
        hex_string += char_for_nibble(byte & 0x0F);
    }

    return hex_string;
}


static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(size);
    UNUSED(channel);
    bd_addr_t address;
    uint8_t length;
    const uint8_t * data;
    if (packet_type != HCI_EVENT_PACKET) return;
    if(hci_event_packet_get_type(packet) == BTSTACK_EVENT_STATE){
        if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) return;
        gap_local_bd_addr(address);
        printf("BTstack up and running on %s.\n", bd_addr_to_str(address));
    }
    else if(hci_event_packet_get_type(packet) == GAP_EVENT_ADVERTISING_REPORT) {
        uint8_t bthome_magic[] = {0x02, 0x01, 0x06, 0x11, 0x16, 0xD2, 0xFC};
        data = gap_event_advertising_report_get_data(packet);

        if (memcmp(data, bthome_magic, count_of(bthome_magic))) {
            return;
        }

        gap_event_advertising_report_get_address(packet, address);
        length = gap_event_advertising_report_get_data_length(packet);

        std::string hex_address = to_hex_string(address, count_of(address));
        bt_device_map[hex_address] = to_hex_string(&(data[5]), length-5);
        std::string message = hex_address + bt_device_map[hex_address];
        server.broadcastMessage(message.c_str());
    }
}


int main() {
  sw_timer = 0;

  stdio_init_all();

  if (cyw43_arch_init() != 0) {
    DEBUG("cyw43_arch_init failed");
    while (1) tight_loop_contents();
  }

  // Active scanning, 100% (scan interval = scan window)
  gap_set_scan_parameters(1,48,48);
  gap_start_scan(); 
  // Setup HCI callback for btstack and turn on
  hci_event_callback_registration.callback = &packet_handler;
  hci_add_event_handler(&hci_event_callback_registration);
  hci_power_control(HCI_POWER_ON);
  DEBUG("Listening for BLE advertisements");

  cyw43_arch_enable_sta_mode();

  DEBUG("Connecting to Wi-Fi...");
  do {} while(cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000));
  DEBUG("Connected.");


  server.setConnectCallback(on_connect);
  server.setCloseCallback(on_disconnect);
  server.setMessageCallback(on_message);

  bool server_ok = server.startListening(80);
  if (!server_ok) {
    DEBUG("Failed to start WebSocket server");
    while (1) tight_loop_contents();
  }
  DEBUG("WebSocket server started");
  
  
  while (1) {
    // Check WiFI status periodically
    if(cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA) <= 0 && (to_ms_since_boot(get_absolute_time()) - sw_timer) > WIFI_STATUS_POLL_MS) {
      cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
      sw_timer = to_ms_since_boot(get_absolute_time());
    }
    cyw43_arch_poll();
  }
}
