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
// The hardware watchdog also covers hangs during initialization and polling.
[[noreturn]] static void reboot_device() {
#ifdef ENABLE_DEBUG
    stdio_flush();
    sleep_ms(50);
#endif
    watchdog_reboot(0, 0, 0);
    while (true) tight_loop_contents();
}

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
        DEBUG("BTstack up and running on %s.", bd_addr_to_str(address));
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

}

int main() {
    watchdog_enable(HARDWARE_WATCHDOG_MS, true);

    bt_cache.reserve(MAX_CACHED_DEVICES);

    stdio_init_all();
#ifdef ENABLE_DEBUG
    // Give USB serial a short window to reconnect after a reset.
    sleep_ms(1500);
    DEBUG("Boot: watchdog_reset=%d", watchdog_caused_reboot());
#endif
    watchdog_update();

    if (cyw43_arch_init() != 0) {
        DEBUG("cyw43_arch_init failed");
        reboot_device();
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
    if (cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK) != 0) {
        DEBUG("Failed to start Wi-Fi connection, rebooting");
        reboot_device();
    }

    // Poll while connecting so a normal DHCP wait can exceed the hardware
    // watchdog timeout, while a stuck driver still triggers a reset.
    uint32_t connect_started_ms = to_ms_since_boot(get_absolute_time());
    while (true) {
        cyw43_arch_poll();
        int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (status == CYW43_LINK_UP) break;
        if (status < 0 ||
            (uint32_t)(to_ms_since_boot(get_absolute_time()) - connect_started_ms) >= WIFI_CONNECT_TIMEOUT_MS) {
            DEBUG("Wi-Fi connection failed or timed out (status=%d), rebooting", status);
            reboot_device();
        }
        watchdog_update();
        sleep_ms(10);
    }
    DEBUG("Connected.");

    server.setConnectCallback(on_connect);
    server.setCloseCallback(on_disconnect);
    server.setMessageCallback(on_message);

    bool server_ok = server.startListening(80);
    if (!server_ok) {
        DEBUG("Failed to start WebSocket server");
        reboot_device();
    }
    DEBUG("WebSocket server started");

    uint32_t last_wifi_check_ms = to_ms_since_boot(get_absolute_time());
    while (true) {
        cyw43_arch_poll();
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if ((uint32_t)(now - last_wifi_check_ms) >= WIFI_STATUS_POLL_MS) {
            last_wifi_check_ms = now;
            // Association alone is insufficient: require an assigned IP too.
            int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
            if (status != CYW43_LINK_UP) {
                DEBUG("Wi-Fi/IP connection lost (status=%d, uptime_ms=%lu), rebooting",
                      status, (unsigned long)now);
                reboot_device();
            }
        }
        watchdog_update();
    }
}
