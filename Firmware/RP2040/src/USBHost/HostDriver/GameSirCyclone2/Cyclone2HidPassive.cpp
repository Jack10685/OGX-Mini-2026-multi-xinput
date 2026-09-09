#include "tusb.h"
#include "host/usbh.h"
#include "class/hid/hid_host.h"

#include "USBHost/HostDriver/GameSirCyclone2/Cyclone2HidPassive.h"

#include <cstdio>
#include <cstring>

#include "Board/ogxm_log.h"
#include "USBHost/HostDriver/GameSirCyclone2/GameSirCyclone2Trace.h"

namespace {

void utf16_to_ascii(const uint8_t* buf, uint16_t buflen, char* out, size_t out_len) {
    if (!out || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (!buf || buflen < 2) {
        return;
    }
    const uint8_t str_len = buf[0];
    if (str_len < 2 || buf[1] != 0x03) {
        return;
    }
    size_t o = 0;
    for (uint8_t i = 2; i + 1 < str_len && i + 1 < buflen && o + 1 < out_len; i += 2) {
        const uint8_t lo = buf[i];
        const uint8_t hi = buf[i + 1];
        out[o++] = (hi == 0 && lo >= 0x20 && lo < 0x7F) ? static_cast<char>(lo) : '?';
    }
    out[o] = '\0';
}

} // namespace

void Cyclone2HidPassive::reset() {
    started_ = false;
    banner_printed_ = false;
    prev_raw_len_ = 0;
    std::memset(prev_raw_, 0, sizeof(prev_raw_));
}

void Cyclone2HidPassive::dump_hid_usage_hint(const uint8_t* report_desc, uint16_t desc_len) {
    if (!report_desc || desc_len < 6) {
        OGXM_LOG("HID usage hint: (no descriptor)\n");
        return;
    }
    /* Very light scan: Usage Page / Usage near start of common gamepad descriptors. */
    bool saw_generic_desktop = false;
    bool saw_gamepad = false;
    bool saw_keyboard = false;
    bool saw_mouse = false;
    bool saw_vendor = false;
    for (uint16_t i = 0; i + 1 < desc_len; ++i) {
        if (report_desc[i] == 0x05 && report_desc[i + 1] == 0x01) {
            saw_generic_desktop = true;
        }
        if (report_desc[i] == 0x05 && report_desc[i + 1] == 0x07) {
            saw_keyboard = true;
        }
        if (report_desc[i] == 0x05 && report_desc[i + 1] >= 0xFF) {
            saw_vendor = true;
        }
        if (report_desc[i] == 0x09 && report_desc[i + 1] == 0x05) {
            saw_gamepad = true;
        }
        if (report_desc[i] == 0x09 && report_desc[i + 1] == 0x06) {
            saw_keyboard = true;
        }
        if (report_desc[i] == 0x09 && report_desc[i + 1] == 0x02) {
            saw_mouse = true;
        }
    }
    OGXM_LOG("HID usage hint: generic_desktop=%s gamepad=%s keyboard=%s mouse=%s vendor=%s\n",
             saw_generic_desktop ? "YES" : "NO", saw_gamepad ? "YES" : "NO",
             saw_keyboard ? "YES" : "NO", saw_mouse ? "YES" : "NO",
             saw_vendor ? "YES" : "NO");
}

void Cyclone2HidPassive::dump_personality(uint8_t address, uint8_t instance,
                                           const uint8_t* report_desc, uint16_t desc_len) {
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(address, &vid, &pid);
    tusb_desc_device_t desc{};
    char manufacturer[48]{};
    char product[48]{};
    char serial[48]{};
    uint16_t bcd = 0;
    if (tuh_descriptor_get_device_sync(address, &desc, sizeof(desc)) == XFER_RESULT_SUCCESS) {
        bcd = tu_le16toh(desc.bcdDevice);
        uint8_t str_buf[64]{};
        if (desc.iManufacturer &&
            tuh_descriptor_get_string_sync(address, desc.iManufacturer, 0x0409, str_buf,
                                           sizeof(str_buf)) == XFER_RESULT_SUCCESS) {
            utf16_to_ascii(str_buf, sizeof(str_buf), manufacturer, sizeof(manufacturer));
        }
        if (desc.iProduct &&
            tuh_descriptor_get_string_sync(address, desc.iProduct, 0x0409, str_buf,
                                           sizeof(str_buf)) == XFER_RESULT_SUCCESS) {
            utf16_to_ascii(str_buf, sizeof(str_buf), product, sizeof(product));
        }
        if (desc.iSerialNumber &&
            tuh_descriptor_get_string_sync(address, desc.iSerialNumber, 0x0409, str_buf,
                                           sizeof(str_buf)) == XFER_RESULT_SUCCESS) {
            utf16_to_ascii(str_buf, sizeof(str_buf), serial, sizeof(serial));
        }
    }

    OGXM_LOG("\n================================================\n");
    OGXM_LOG("CYCLONE 2 HID PERSONALITY\n");
    OGXM_LOG("================================================\n");
    OGXM_LOG("VID:\n%04X\n", vid);
    OGXM_LOG("PID:\n%04X\n", pid);
    OGXM_LOG("bcdDevice:\n%04X\n", bcd);
    OGXM_LOG("Manufacturer:\n%s\n", manufacturer);
    OGXM_LOG("Product:\n%s\n", product);
    OGXM_LOG("Serial:\n%s\n", serial);
    OGXM_LOG("\nCYCLONE2 HID SAFETY MODE ACTIVE\n");
    OGXM_LOG("PASSIVE: enumerate + listen only — NO application OUT/feature/vendor TX\n");
    OGXM_LOG("Do NOT send: 0F F2, Switch 0x80/0x01, DS4, rumble, LED\n");

    tuh_itf_info_t info{};
    if (tuh_hid_itf_get_info(address, instance, &info)) {
        OGXM_LOG("\nThis HID mount instance=%u\n", static_cast<unsigned>(instance));
        OGXM_LOG("Interface %u:\n", info.desc.bInterfaceNumber);
        OGXM_LOG("  class: 0x%02X\n", info.desc.bInterfaceClass);
        OGXM_LOG("  subclass: 0x%02X\n", info.desc.bInterfaceSubClass);
        OGXM_LOG("  protocol: 0x%02X\n", info.desc.bInterfaceProtocol);
    }
    OGXM_LOG("report descriptor length: %u\n", static_cast<unsigned>(desc_len));
    dump_hid_usage_hint(report_desc, desc_len);
    if (report_desc && desc_len > 0) {
        OGXM_LOG("report descriptor (first 64):\n");
        OGXM_LOG_HEX(report_desc, desc_len > 64 ? 64 : desc_len);
    }

    /* Full config tree for all interfaces. */
    uint8_t cfg[512]{};
    if (tuh_descriptor_get_configuration_sync(address, 0, cfg, sizeof(cfg)) == XFER_RESULT_SUCCESS) {
        const auto* conf = reinterpret_cast<const tusb_desc_configuration_t*>(cfg);
        const uint16_t total = tu_le16toh(conf->wTotalLength);
        const uint16_t len = TU_MIN(total, static_cast<uint16_t>(sizeof(cfg)));
        OGXM_LOG("\nInterface count: %u\n", conf->bNumInterfaces);
        const uint8_t* p = tu_desc_next(cfg);
        const uint8_t* end = cfg + len;
        while (p < end && tu_desc_type(p) != 0) {
            const uint8_t dlen = tu_desc_len(p);
            if (dlen < 2 || p + dlen > end) {
                break;
            }
            if (tu_desc_type(p) == TUSB_DESC_INTERFACE) {
                const auto* itf = reinterpret_cast<const tusb_desc_interface_t*>(p);
                OGXM_LOG("\nInterface %u:\n", itf->bInterfaceNumber);
                OGXM_LOG("  class: 0x%02X subclass: 0x%02X protocol: 0x%02X\n",
                         itf->bInterfaceClass, itf->bInterfaceSubClass, itf->bInterfaceProtocol);
            } else if (tu_desc_type(p) == TUSB_DESC_ENDPOINT) {
                const auto* ep = reinterpret_cast<const tusb_desc_endpoint_t*>(p);
                const uint8_t addr = ep->bEndpointAddress;
                OGXM_LOG("  endpoint: 0x%02X %s type=%u max=%u interval=%u\n",
                         addr, (addr & TUSB_DIR_IN_MASK) ? "IN" : "OUT",
                         static_cast<unsigned>(ep->bmAttributes.xfer),
                         static_cast<unsigned>(tu_edpt_packet_size(ep)),
                         static_cast<unsigned>(ep->bInterval));
            }
            p = tu_desc_next(p);
        }
    }

    OGXM_LOG("\n================================================\n");
    OGXM_LOG("INPUT DRIVER OWNERSHIP\n");
    OGXM_LOG("Physical driver: GAMESIR_CYCLONE2\n");
    OGXM_LOG("Mode: HID\n");
    OGXM_LOG("Protocol engine: PASSIVE_PROBE (no input translation yet)\n");
    OGXM_LOG("================================================\n");
}

void Cyclone2HidPassive::start(uint8_t address, uint8_t instance,
                                const uint8_t* report_desc, uint16_t desc_len) {
    reset();
    started_ = true;
    address_ = address;
    instance_ = instance;
    if (!banner_printed_) {
        banner_printed_ = true;
        dump_personality(address, instance, report_desc, desc_len);
    } else {
        OGXM_LOG("[CYCLONE2 HID] additional HID instance=%u (still PASSIVE)\n",
                 static_cast<unsigned>(instance));
        dump_hid_usage_hint(report_desc, desc_len);
    }
    /* Arm IN only — never OUT. */
    (void)tuh_hid_receive_report(address, instance);
}

void Cyclone2HidPassive::process_report(uint8_t address, uint8_t instance,
                                         const uint8_t* report, uint16_t len) {
    if (!started_ || !report || len == 0) {
        tuh_hid_receive_report(address, instance);
        return;
    }

    const uint16_t n = (len < sizeof(prev_raw_)) ? len : static_cast<uint16_t>(sizeof(prev_raw_));
    const bool changed = (n != prev_raw_len_) || (std::memcmp(prev_raw_, report, n) != 0);
    if (changed) {
        prev_raw_len_ = n;
        std::memcpy(prev_raw_, report, n);
        /* Changing IN traffic suggests a live controller on this identity — still no PadIn. */
        GameSirCyclone2Trace::set_receiver_controller_online(true);
        OGXM_LOG("\n[CYCLONE2 HID RAW]\ninterface_instance=%u\nreport_id=0x%02X\nlen=%u\nDATA:\n",
                 static_cast<unsigned>(instance), report[0], static_cast<unsigned>(len));
        OGXM_LOG_HEX(report, n > 32 ? 32 : n);
        OGXM_LOG("(passive — not mapped to PadIn; idle receiver vs HID TBD from capture)\n");
    }

    tuh_hid_receive_report(address, instance);
}

bool Cyclone2HidPassive::send_feedback(uint8_t address, uint8_t instance) {
    (void)address;
    (void)instance;
    /* Explicit safety: never rumble/LED/vendor in HID passive mode. */
    return true;
}

void Cyclone2HidPassive::disconnect() {
    reset();
}
