#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "btstack.h"
#include "debug.h"
#include "hardware/watchdog.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico_ws_server/web_socket_server.h"

WebSocketServer server(MAX_WS_CONNECTIONS);
uint32_t sw_timer;

// Updated only when a valid matching BTHome advertisement is seen.
volatile uint32_t last_valid_bthome_ms = 0;

static btstack_packet_callback_registration_t hci_event_callback_registration;

// Each entry is stored as:
// [0..5]   = BLE address
// [6..end] = cached payload bytes
static std::vector<std::vector<uint8_t>> bt_cache;

static int find_device_index(const bd_addr_t address) {
    for (size_t i = 0; i < bt_cache.size(); ++i) {
        if (bt_cache[i].size() >= 6 && memcmp(bt_cache[i].data(), address, 6) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

static std::string to_hex_string(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(len * 2);

    for (size_t i = 0; i < len; ++i) {
        uint8_t byte = data[i];
        out += char_for_nibble((byte >> 4) & 0x0F);
        out += char_for_nibble(byte & 0x0F);
    }

    return out;
}

static std::vector<uint8_t>* cache_device(const bd_addr_t address, const uint8_t* payload, size_t payload_len) {
    int idx = find_device_index(address);

    if (idx >= 0) {
        std::vector<uint8_t>& entry = bt_cache[idx];
        entry.resize(6 + payload_len);
        memcpy(entry.data(), address, 6);
        memcpy(entry.data() + 6, payload, payload_len);
        return &entry;
    }

    if (bt_cache.size() >= MAX_CACHED_DEVICES) {
        DEBUG("Device cache full, dropping new device");
        return nullptr;
    }

    std::vector<uint8_t> entry(6 + payload_len);
    memcpy(entry.data(), address, 6);
    memcpy(entry.data() + 6, payload, payload_len);
    bt_cache.push_back(std::move(entry));
    return &bt_cache.back();
}


static void broadcast_entry(const std::vector<uint8_t>* entry) {
    if (!entry || entry->size() < 6) return;
    std::string message = to_hex_string(entry->data(), entry->size());
    server.broadcastMessage(message.c_str());
}

void on_connect(WebSocketServer& server, uint32_t conn_id) {
    UNUSED(server);
    UNUSED(conn_id);

    DEBUG("WebSocket opened");

    for (const auto& entry : bt_cache) {
        broadcast_entry(&entry);
    }
}

void on_disconnect(WebSocketServer& server, uint32_t conn_id) {
    UNUSED(server);
    UNUSED(conn_id);
    DEBUG("WebSocket closed");
}

void on_message(WebSocketServer& server, uint32_t conn_id, const void* data, size_t len) {
  return;
}


static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(size);
    UNUSED(channel);

    bd_addr_t address;
    const uint8_t* data;
    uint8_t length;

    if (packet_type != HCI_EVENT_PACKET) return;

    uint8_t event_type = hci_event_packet_get_type(packet);

    if (event_type == BTSTACK_EVENT_STATE) {
        if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) return;
        gap_local_bd_addr(address);
        printf("BTstack up and running on %s.\n", bd_addr_to_str(address));
        return;
    }

    if (event_type != GAP_EVENT_ADVERTISING_REPORT) return;

    static const uint8_t bthome_magic[] = {0x02, 0x01, 0x06, 0x11, 0x16, 0xD2, 0xFC};

    data = gap_event_advertising_report_get_data(packet);
    length = gap_event_advertising_report_get_data_length(packet);

    if (!data || length < sizeof(bthome_magic) || memcmp(data, bthome_magic, sizeof(bthome_magic)) != 0) {
        return;
    }

    gap_event_advertising_report_get_address(packet, address);

    // Preserving your original payload slice.
    const uint8_t* payload = &data[5];
    size_t payload_len = static_cast<size_t>(length - 5);

    std::vector<uint8_t> *entry = cache_device(address, payload, payload_len);
    if (entry) {
        broadcast_entry(entry);  
    }

    // Liveness marker for watchdog.
    last_valid_bthome_ms = to_ms_since_boot(get_absolute_time());
}

int main() {
    sw_timer = 0;
    last_valid_bthome_ms = to_ms_since_boot(get_absolute_time());

    bt_cache.reserve(MAX_CACHED_DEVICES);

    stdio_init_all();

    if (cyw43_arch_init() != 0) {
        DEBUG("cyw43_arch_init failed");
        while (1) tight_loop_contents();
    }

    // Active scanning, 100% (scan interval = scan window)
    gap_set_scan_parameters(1, 48, 48);
    gap_start_scan();

    // Setup HCI callback for btstack and turn on
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
    hci_power_control(HCI_POWER_ON);
    DEBUG("Listening for BLE advertisements");

    cyw43_arch_enable_sta_mode();

    DEBUG("Connecting to Wi-Fi...");
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, WIFI_STATUS_POLL_MS)) {
        DEBUG("Failed to connect to Wi-Fi");
        watchdog_reboot(0, 0, 0);
        while (1) tight_loop_contents();
    }
    DEBUG("Connected.");

    server.setConnectCallback(on_connect);
    server.setCloseCallback(on_disconnect);

    bool server_ok = server.startListening(80);
    if (!server_ok) {
        DEBUG("Failed to start WebSocket server");
        while (1) tight_loop_contents();
    }
    DEBUG("WebSocket server started");

    watchdog_enable(WATCHDOG_MS, 1);

    while (1) {
        uint32_t now = to_ms_since_boot(get_absolute_time());

        if ((now - sw_timer) > WIFI_STATUS_POLL_MS) {
            sw_timer = now;
            if (cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA) <= 0) {
                DEBUG("Wi-Fi link lost, rebooting");
                watchdog_reboot(0, 0, 0);
                while (1) tight_loop_contents();
            }
        }

        cyw43_arch_poll();

        // Only pet the watchdog when valid matching BTHome traffic has been seen recently enough.
        if ((now - last_valid_bthome_ms) < WATCHDOG_MS) {
            watchdog_update();
        }
    }
}