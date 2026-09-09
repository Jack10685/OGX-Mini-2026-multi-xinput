#include "Input/InputSlot.h"

#include <cstdio>

#include "Board/Config.h"
#include "Board/ogxm_log.h"
#include "pico/time.h"

namespace InputSlot {
namespace {

State s_slots[MAX_GAMEPADS]{};
uint32_t s_last_rx_log_ms{0};

} // namespace

const char* transport_name(InputTransport t) {
    switch (t) {
        case InputTransport::USB: return "USB";
        case InputTransport::BLUETOOTH_CLASSIC: return "BT_CLASSIC";
        case InputTransport::BLUETOOTH_LE: return "BT_LE";
        default: return "NONE";
    }
}

const char* driver_name(HostDriverType t) {
    switch (t) {
        case HostDriverType::GAMESIR_CYCLONE2: return "GAMESIR_CYCLONE2";
        case HostDriverType::SWITCH_PRO: return "SWITCH_PRO";
        case HostDriverType::SWITCH_PRO_2: return "SWITCH_PRO_2";
        case HostDriverType::SWITCH: return "SWITCH";
        case HostDriverType::PS4: return "PS4";
        case HostDriverType::PS5: return "PS5";
        case HostDriverType::PS3: return "PS3";
        case HostDriverType::XBOX360: return "XBOX360";
        case HostDriverType::XBOX360W: return "XBOX360W";
        case HostDriverType::XBOXONE: return "XBOXONE";
        case HostDriverType::XBOXOG: return "XBOXOG";
        case HostDriverType::DINPUT: return "DINPUT";
        case HostDriverType::HID_GENERIC: return "HID_GENERIC";
        case HostDriverType::N64: return "N64";
        case HostDriverType::PSCLASSIC: return "PSCLASSIC";
        case HostDriverType::FLYDIGI_APEX4_WUKONG: return "FLYDIGI_APEX4_WUKONG";
        default: return "UNKNOWN";
    }
}

const char* protocol_for_ids(HostDriverType physical, uint16_t vid, uint16_t pid) {
    if (physical == HostDriverType::GAMESIR_CYCLONE2) {
        if (vid == 0x057E && pid == 0x2009) {
            return "SWITCH_PRO_STANDARD";
        }
        if (vid == 0x054C && pid == 0x09CC) {
            return "DS4";
        }
        if (vid == 0x3537 && (pid == 0x100B || pid == 0x1053)) {
            return "XINPUT";
        }
        if (vid == 0x3537 && pid == 0x0575) {
            return "RECEIVER_IDLE_OR_HID_PASSIVE";
        }
        return "CYCLONE2";
    }
    if (physical == HostDriverType::SWITCH_PRO || physical == HostDriverType::SWITCH_PRO_2 ||
        physical == HostDriverType::SWITCH) {
        return "SWITCH";
    }
    if (physical == HostDriverType::PS4 || physical == HostDriverType::PS5) {
        return "DS4";
    }
    if (physical == HostDriverType::XBOX360 || physical == HostDriverType::XBOX360W ||
        physical == HostDriverType::XBOXONE || physical == HostDriverType::XBOXOG) {
        return "XINPUT";
    }
    return driver_name(physical);
}

State get(uint8_t slot) {
    if (slot >= MAX_GAMEPADS) {
        return {};
    }
    return s_slots[slot];
}

void clear(uint8_t slot, const char* reason) {
    if (slot >= MAX_GAMEPADS) {
        return;
    }
    State& st = s_slots[slot];
    if (st.transport == InputTransport::NONE) {
        return;
    }
#if defined(CONFIG_OGXM_DEBUG)
    printf("\n[SLOT STATE CHANGE]\nslot=%u\nold=%s\nnew=NONE\ncaller=%s\nreason=%s\n",
           static_cast<unsigned>(slot), transport_name(st.transport),
           reason ? reason : "?", reason ? reason : "?");
#endif
    st = {};
}

void bind_usb(uint8_t slot, uint8_t usb_addr, uint8_t usb_instance,
              HostDriverType physical, uint16_t vid, uint16_t pid,
              const char* caller) {
    if (slot >= MAX_GAMEPADS) {
#if defined(CONFIG_OGXM_DEBUG)
        printf("\n[USB SLOT BIND]\nFAILED: slot=%u out of range (MAX_GAMEPADS=%u)\n",
               static_cast<unsigned>(slot), static_cast<unsigned>(MAX_GAMEPADS));
#endif
        return;
    }

    State& st = s_slots[slot];
    const InputTransport old = st.transport;
    const char* protocol = protocol_for_ids(physical, vid, pid);

#if defined(CONFIG_OGXM_DEBUG)
    printf("\n[USB SLOT BIND]\n");
    printf("USB address=%u\ninstance=%u\n", static_cast<unsigned>(usb_addr),
           static_cast<unsigned>(usb_instance));
    printf("VID=%04X\nPID=%04X\n", vid, pid);
    printf("detected_type=%s\n", driver_name(physical));
    printf("mount_callback=YES\n");
    printf("driver_object=%s\n", driver_name(physical));
    printf("searching for free OGX input slot...\n");
    for (uint8_t i = 0; i < MAX_GAMEPADS; ++i) {
        printf("slot %u:\ntransport=%s\navailable=%s\n",
               static_cast<unsigned>(i), transport_name(s_slots[i].transport),
               (s_slots[i].transport == InputTransport::NONE || i == slot) ? "YES" : "NO");
    }
    printf("selected slot=%u\n", static_cast<unsigned>(slot));
    printf("assigning:\ntransport=USB\nusb_addr=%u\nusb_instance=%u\n",
           static_cast<unsigned>(usb_addr), static_cast<unsigned>(usb_instance));
    printf("physical controller:\n%s\n", driver_name(physical));
    printf("protocol:\n%s\n", protocol);
#endif

    st.transport = InputTransport::USB;
    st.usb_addr = usb_addr;
    st.usb_instance = usb_instance;
    st.physical_driver = physical;
    st.protocol = protocol;

#if defined(CONFIG_OGXM_DEBUG)
    if (old != InputTransport::USB) {
        printf("\n[SLOT STATE CHANGE]\nslot=%u\nold=%s\nnew=USB\ncaller=%s\n",
               static_cast<unsigned>(slot), transport_name(old),
               caller ? caller : "HostManager::setup_driver");
    }
    printf("binding SUCCESS\n");
    printf("\n[INPUT SLOT %u]\ntransport=USB\ndriver=%s\nmode=%s\nprotocol_engine=%s\naddr=%u\ninstance=%u\n",
           static_cast<unsigned>(slot), driver_name(physical), protocol,
           protocol, static_cast<unsigned>(usb_addr), static_cast<unsigned>(usb_instance));
    for (uint8_t i = 0; i < MAX_GAMEPADS; ++i) {
        if (i == slot) {
            continue;
        }
        printf("[INPUT SLOT %u]\ntransport=%s\n", static_cast<unsigned>(i),
               transport_name(s_slots[i].transport));
    }
#else
    (void)old;
    (void)caller;
#endif
}

void log_all(const char* context) {
#if defined(CONFIG_OGXM_DEBUG)
    printf("\n[INPUT SLOTS]%s%s\n", context && context[0] ? " " : "", context ? context : "");
    for (uint8_t i = 0; i < MAX_GAMEPADS; ++i) {
        const State& st = s_slots[i];
        if (st.transport == InputTransport::USB) {
            printf("[INPUT SLOT %u] transport=USB driver=%s mode=%s addr=%u instance=%u\n",
                   static_cast<unsigned>(i), driver_name(st.physical_driver), st.protocol,
                   static_cast<unsigned>(st.usb_addr), static_cast<unsigned>(st.usb_instance));
        } else {
            printf("[INPUT SLOT %u] transport=%s\n", static_cast<unsigned>(i),
                   transport_name(st.transport));
        }
    }
#else
    (void)context;
#endif
}

void log_hid_rx_route(uint8_t usb_addr, uint8_t usb_instance, uint8_t report_id, uint16_t len,
                      uint8_t owning_slot, HostDriverType physical) {
#if defined(CONFIG_OGXM_DEBUG)
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    if ((now - s_last_rx_log_ms) < 200) {
        return;
    }
    s_last_rx_log_ms = now;

    printf("\n[USB HID RX ROUTE]\naddr=%u\ninstance=%u\nreport_id=0x%02X\nlen=%u\n",
           static_cast<unsigned>(usb_addr), static_cast<unsigned>(usb_instance),
           report_id, static_cast<unsigned>(len));
    printf("lookup owning slot...\n");
    if (owning_slot >= MAX_GAMEPADS) {
        printf("owner slot=NONE\n");
        return;
    }
    const State& st = s_slots[owning_slot];
    printf("owner slot=%u\ntransport=%s\ndriver=%s\nmode=%s\nrouting report...\n",
           static_cast<unsigned>(owning_slot), transport_name(st.transport),
           driver_name(physical != HostDriverType::UNKNOWN ? physical : st.physical_driver),
           st.protocol ? st.protocol : "?");
#else
    (void)usb_addr;
    (void)usb_instance;
    (void)report_id;
    (void)len;
    (void)owning_slot;
    (void)physical;
#endif
}

} // namespace InputSlot
