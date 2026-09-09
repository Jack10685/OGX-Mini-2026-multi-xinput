#include "USBHost/HostDriver/GameSirCyclone2/Cyclone2BtProbe.h"

#include <cstring>

#include "Board/Config.h"
#include "Board/ogxm_log.h"
#include "USBHost/HostDriver/GameSirCyclone2/GameSirCyclone2Transport.h"

namespace {

bool name_has(const char* name, const char* needle) {
    if (!name || !needle) {
        return false;
    }
    return std::strstr(name, needle) != nullptr;
}

bool is_gamesir_cyclone_name(const char* name) {
    if (!name || !name[0]) {
        return false;
    }
    /* Official HID/Android BT name; do not treat bare "Wireless Controller" as Cyclone. */
    return name_has(name, "GameSir-Cyclone") || name_has(name, "GameSir Cyclone") ||
           name_has(name, "Gamesir-Cyclone") || name_has(name, "Cyclone 2");
}

void log_bt_banner(const char* phase, const uni_hid_device_t* device, const char* name) {
#if defined(CONFIG_OGXM_DEBUG)
    using namespace GameSirCyclone2Transport;
    OGXM_LOG("\n====================================================\n");
    OGXM_LOG("GAMESIR CYCLONE 2\n");
    OGXM_LOG("====================================================\n");
    OGXM_LOG("Physical driver:\nGAMESIR_CYCLONE2\n");
    OGXM_LOG("Transport:\n%s\n", transport_name(Transport::Bluetooth));
    OGXM_LOG("BT phase:\n%s\n", phase ? phase : "?");
    OGXM_LOG("Bluetooth name:\n%s\n", name && name[0] ? name : "(none)");
    if (device) {
        OGXM_LOG("Identity:\nVID=%04X\nPID=%04X\n", device->vendor_id, device->product_id);
        OGXM_LOG("controller_type:\n%u\n", static_cast<unsigned>(device->controller_type));
    }
    OGXM_LOG("Receiver present:\nNO\n");
    OGXM_LOG("Controller connected:\n%s\n",
             (device != nullptr) ? "YES (BT link)" : "UNKNOWN");
    OGXM_LOG("Note:\nBT probe only — no dedicated Cyclone parser installed yet\n");
    OGXM_LOG("Note:\nBluetooth XInput is N/A (GameSir unsupported)\n");
    OGXM_LOG("====================================================\n");
#else
    (void)phase;
    (void)device;
    (void)name;
#endif
}

} // namespace

extern "C" int gamesir_cyclone2_bt_is_strong_candidate(const uni_hid_device_t* device) {
    if (!device) {
        return 0;
    }
    return is_gamesir_cyclone_name(device->name) ? 1 : 0;
}

extern "C" void gamesir_cyclone2_bt_on_discovered(const uint8_t* bd_addr, const char* name,
                                                  uint16_t cod, uint8_t rssi) {
    (void)bd_addr;
    (void)cod;
    (void)rssi;
#if defined(CONFIG_OGXM_DEBUG)
    if (is_gamesir_cyclone_name(name)) {
        OGXM_LOG("[CYCLONE2 BT] discovered strong name=\"%s\" COD=0x%04X RSSI=%d\n",
                 name ? name : "?", cod, static_cast<int>(rssi));
        log_bt_banner("DISCOVERED", nullptr, name);
    } else if (name && std::strstr(name, "Wireless Controller")) {
        /* Shared DS4 identity — observe only; do not claim as Cyclone. */
        OGXM_LOG("[CYCLONE2 BT] note shared DS4-style name=\"Wireless Controller\" "
                 "(not claimed as Cyclone without stronger fingerprint)\n");
    }
#else
    (void)name;
#endif
}

extern "C" void gamesir_cyclone2_bt_on_connected(uni_hid_device_t* device) {
    if (!gamesir_cyclone2_bt_is_strong_candidate(device)) {
        return;
    }
    log_bt_banner("CONNECTED", device, device ? device->name : nullptr);
}

extern "C" void gamesir_cyclone2_bt_on_ready(uni_hid_device_t* device) {
    if (!gamesir_cyclone2_bt_is_strong_candidate(device)) {
        return;
    }
    log_bt_banner("READY", device, device ? device->name : nullptr);
    OGXM_LOG("[CYCLONE2 BT] using generic Bluepad32 parser until dedicated mapping lands\n");
}

extern "C" void gamesir_cyclone2_bt_on_disconnected(uni_hid_device_t* device) {
    if (!gamesir_cyclone2_bt_is_strong_candidate(device)) {
        return;
    }
    log_bt_banner("DISCONNECTED", device, device ? device->name : nullptr);
}
