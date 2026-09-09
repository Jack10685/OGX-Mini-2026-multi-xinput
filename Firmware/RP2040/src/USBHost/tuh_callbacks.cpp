#include <cstdint>

#include "tusb.h"
#include "host/usbh.h"
#include "host/hcd.h"
#include "class/hid/hid_host.h"

#include "USBHost/HostDriver/XInput/tuh_xinput/tuh_xinput.h"
#include "USBHost/HostManager.h"
#include "USBHost/UsbHostLifecycle.h"
#include "OGXMini/OGXMini.h"
#include "Board/ogxm_log.h"
#include "Input/HidRxDebug.h"

#if defined(CONFIG_OGXM_DEBUG)
#include "USBHost/HostDriver/GameSirCyclone2/GameSirCyclone2.h"
#include "USBHost/HostDriver/GameSirCyclone2/GameSirCyclone2Trace.h"
#endif

namespace {

#if defined(CONFIG_OGXM_DEBUG)
const char* host_type_name(HostDriverType t)
{
    switch (t) {
        case HostDriverType::UNKNOWN: return "UNKNOWN";
        case HostDriverType::SWITCH_PRO: return "SWITCH_PRO";
        case HostDriverType::SWITCH_PRO_2: return "SWITCH_PRO_2";
        case HostDriverType::SWITCH: return "SWITCH";
        case HostDriverType::PSCLASSIC: return "PSCLASSIC";
        case HostDriverType::DINPUT: return "DINPUT";
        case HostDriverType::PS3: return "PS3";
        case HostDriverType::PS4: return "PS4";
        case HostDriverType::PS5: return "PS5";
        case HostDriverType::N64: return "N64";
        case HostDriverType::FLYDIGI_APEX4_WUKONG: return "FLYDIGI_APEX4_WUKONG";
        case HostDriverType::GAMESIR_CYCLONE2: return "GAMESIR_CYCLONE2";
        case HostDriverType::XBOXOG: return "XBOXOG";
        case HostDriverType::XBOXONE: return "XBOXONE";
        case HostDriverType::XBOX360W: return "XBOX360W";
        case HostDriverType::XBOX360: return "XBOX360";
        case HostDriverType::XBOX360_CHATPAD: return "XBOX360_CHATPAD";
        case HostDriverType::HID_GENERIC: return "HID_GENERIC";
        default: return "?";
    }
}

void log_usb_driver_select(uint8_t address, uint8_t instance, HostManager::DriverClass dclass,
                           HostDriverType selected, uint16_t vid, uint16_t pid)
{
    const bool cyclone_xinput = GameSirCyclone2Host::is_known_id(vid, pid);
    const bool nintendo_switch_pro = (vid == 0x057E && pid == 0x2009);

    OGXM_LOG("\n[USB DRIVER SELECT]\n");
    OGXM_LOG("instance=%u path=%s VID=%04X PID=%04X\n",
             static_cast<unsigned>(instance),
             dclass == HostManager::DriverClass::XINPUT ? "XINPUT" : "HID",
             vid, pid);
    OGXM_LOG("Cyclone2 XInput candidate: %s\n", cyclone_xinput ? "YES" : "NO");
    OGXM_LOG("selected=%s\n", host_type_name(selected));

    if (nintendo_switch_pro && selected == HostDriverType::SWITCH_PRO) {
        OGXM_LOG("NOTE: 057E:2009 → SWITCH_PRO is correct IF the pad already fell back to red NS mode.\n");
        OGXM_LOG("Goal remains: keep green XInput 3537:100B — do not patch SwitchProHost.\n");
    }
    if (cyclone_xinput && selected != HostDriverType::GAMESIR_CYCLONE2) {
        OGXM_LOG("*** WRONG DRIVER SELECTION *** Cyclone XInput ID but selected=%s\n",
                 host_type_name(selected));
    }
    OGXM_LOG("\n");
}
#endif

} // namespace

usbh_class_driver_t const* usbh_app_driver_get_cb(uint8_t* driver_count) {
    *driver_count = 1;
    return tuh_xinput::class_driver();
}

#if defined(CONFIG_OGXM_DEBUG)
void tuh_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
    (void)in_isr;
    if (eventid == HCD_EVENT_DEVICE_ATTACH) {
        UsbHostLifecycle::on_bus_attach(rhport);
        GameSirCyclone2Trace::on_bus_attach(rhport);
    } else if (eventid == HCD_EVENT_DEVICE_REMOVE) {
        UsbHostLifecycle::on_bus_remove(rhport);
        GameSirCyclone2Trace::on_bus_remove(rhport);
    }
}

void tuh_mount_cb(uint8_t daddr) {
    UsbHostLifecycle::on_device_configured(daddr);
    /* After all interfaces configured — identity dump / mode-change detect. */
    GameSirCyclone2Trace::on_device_configured(daddr);
}

void tuh_umount_cb(uint8_t daddr) {
    UsbHostLifecycle::on_device_unmounted(daddr);
    GameSirCyclone2Trace::on_device_unmounted(daddr);
}
#else
void tuh_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
    (void)in_isr;
    if (eventid == HCD_EVENT_DEVICE_ATTACH)
        UsbHostLifecycle::on_bus_attach(rhport);
    else if (eventid == HCD_EVENT_DEVICE_REMOVE)
        UsbHostLifecycle::on_bus_remove(rhport);
}

void tuh_mount_cb(uint8_t daddr) {
    UsbHostLifecycle::on_device_configured(daddr);
}

void tuh_umount_cb(uint8_t daddr) {
    UsbHostLifecycle::on_device_unmounted(daddr);
}
#endif

//HID

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* desc_report, uint16_t desc_len) {
    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

    HostManager& host_manager = HostManager::get_instance();
    HostDriverType host_type = HostManager::get_type({ vid, pid });

#if defined(CONFIG_OGXM_DEBUG)
    /* HID mount runs before tuh_mount_cb — claim must use address fingerprint + session, not enum log. */
    if (GameSirCyclone2Host::should_claim(dev_addr, vid, pid))
        host_type = HostDriverType::GAMESIR_CYCLONE2;
    log_usb_driver_select(dev_addr, instance, HostManager::DriverClass::HID, host_type, vid, pid);
#endif

    if (host_manager.setup_driver(host_type, HostManager::DriverClass::HID,
            dev_addr, instance, desc_report, desc_len)) {
        UsbHostLifecycle::on_interface_opened("HID_OPEN");
        HidRxDebug::ensure_armed_after_mount(dev_addr, instance);
        OGXMini::host_mounted(true);
    }
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
    HostManager& host_manager = HostManager::get_instance();
    host_manager.deinit_driver(HostManager::DriverClass::HID, dev_addr, instance);

    if (!host_manager.any_mounted()) {
        OGXMini::host_mounted(false);
    }
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, uint8_t const* report, uint16_t len) {
    HidRxDebug::log_raw_rx_if_changed(dev_addr, instance, report, len);
    HostManager::get_instance().process_report(HostManager::DriverClass::HID, dev_addr, instance, report, len);
    HidRxDebug::ensure_rearmed_after_rx(dev_addr, instance);
}

#if defined(CONFIG_OGXM_DEBUG)
void tuh_hid_report_sent_cb(uint8_t dev_addr, uint8_t idx, uint8_t const* report, uint16_t len) {
    if (!report || len == 0) {
        return;
    }
    if (GameSirCyclone2Trace::hid_passive_safety_active(dev_addr)) {
        GameSirCyclone2Trace::log_blocked_hid_out(dev_addr, idx, "tuh_hid_report_sent_cb (UNEXPECTED)",
                                                    report, len);
        OGXM_LOG("*** WARNING *** HID OUT reached wire during PASSIVE safety — find caller\n");
        return;
    }
    /* Focus on Switch-family OUT reports (0x80 handshake / 0x01 subcmd / 0x10 rumble). */
    if (report[0] != 0x01 && report[0] != 0x80 && report[0] != 0x10) {
        return;
    }
    OGXM_LOG("\n[CYCLONE2 SWITCH ACTUAL USB TX]\n");
    OGXM_LOG("addr=%u\ninstance=%u\nendpoint=0x02 (HID OUT)\n", static_cast<unsigned>(dev_addr),
             static_cast<unsigned>(idx));
    OGXM_LOG("len=%u\nDATA:\n", static_cast<unsigned>(len));
    OGXM_LOG_HEX(report, len > 32 ? 32 : len);
    if (len >= 2 && report[0] == 0x01 && report[1] == 0x01) {
        OGXM_LOG("*** WARNING *** possible duplicated report ID 01 01 …\n");
    }
    OGXM_LOG("[SWITCH TX] USB transfer=SUCCESS\n");
}
#endif

//XINPUT

void tuh_xinput::mount_cb(uint8_t dev_addr, uint8_t instance, const tuh_xinput::Interface* interface) {
    HostManager& host_manager = HostManager::get_instance();
    HostDriverType host_type = HostManager::get_type(interface->dev_type);

    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(dev_addr, &vid, &pid);

#if defined(CONFIG_OGXM_DEBUG)
    GameSirCyclone2Trace::on_xinput_claimed(dev_addr, instance, interface->itf_num,
                                            interface->ep_in, interface->ep_out);

    if (GameSirCyclone2Host::is_known_id(vid, pid))
        host_type = HostDriverType::GAMESIR_CYCLONE2;

    log_usb_driver_select(dev_addr, instance, HostManager::DriverClass::XINPUT, host_type, vid, pid);
#else
    (void)vid;
    (void)pid;
#endif

    if (host_manager.setup_driver(host_type, HostManager::DriverClass::XINPUT, dev_addr, instance)) {
        UsbHostLifecycle::on_interface_opened("XINPUT_OPEN");
        OGXMini::host_mounted(true, host_type);
    }
}

void tuh_xinput::unmount_cb(uint8_t dev_addr, uint8_t instance, const tuh_xinput::Interface* interface) {
    (void)interface;
    HostManager& host_manager = HostManager::get_instance();
    host_manager.deinit_driver(HostManager::DriverClass::XINPUT, dev_addr, instance);

    if (!host_manager.any_mounted()) {
        OGXMini::host_mounted(false);
    }
}

void tuh_xinput::report_received_cb(uint8_t dev_addr, uint8_t instance, const uint8_t* report, uint16_t len) {
    HostManager::get_instance().process_report(HostManager::DriverClass::XINPUT, dev_addr, instance, report, len);
}

void tuh_xinput::xbox360w_connect_cb(uint8_t dev_addr, uint8_t instance) {
    uint8_t idx = HostManager::get_instance().get_gamepad_idx(  HostManager::DriverClass::XINPUT, 
                                                                dev_addr, instance);
    OGXMini::wireless_connected(true, idx);
    HostManager::get_instance().connect_cb(HostManager::DriverClass::XINPUT, dev_addr, instance);
}

void tuh_xinput::xbox360w_disconnect_cb(uint8_t dev_addr, uint8_t instance) {
    uint8_t idx = HostManager::get_instance().get_gamepad_idx(  HostManager::DriverClass::XINPUT, 
                                                                dev_addr, instance);
    OGXMini::wireless_connected(false, idx);
    HostManager::get_instance().disconnect_cb(HostManager::DriverClass::XINPUT, dev_addr, instance);
}

void tuh_xinput::host_activity_cb(uint8_t dev_addr, uint8_t instance) {
    (void)dev_addr;
    (void)instance;
    HostManager::get_instance().record_usb_host_input_activity();
}
