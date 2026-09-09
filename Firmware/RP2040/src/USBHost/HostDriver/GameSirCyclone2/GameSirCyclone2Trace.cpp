#include "USBHost/HostDriver/GameSirCyclone2/GameSirCyclone2Trace.h"

#include <cstdio>
#include <cstring>

#include "host/usbh.h"
#include "pico/time.h"

#include "Board/Config.h"
#include "Board/ogxm_log.h"

#if defined(CONFIG_OGXM_DEBUG)

namespace GameSirCyclone2Trace {
namespace {

constexpr uint16_t kGameSirVid = 0x3537;
constexpr uint16_t kNintendoVid = 0x057E;
constexpr uint16_t kSonyVid = 0x054C;

uint32_t cable_attach_ms_ = 0;
uint32_t enum_seq_ = 0;

uint16_t prev_vid_ = 0;
uint16_t prev_pid_ = 0;
Mode prev_mode_ = Mode::Unknown;
bool saw_cyclone_xinput_ = false;
uint32_t cyclone_xinput_seen_ms_ = 0;
uint32_t last_detach_ms_ = 0;
uint8_t hid_passive_safety_addr_ = 0;
bool saw_0575_ = false;
uint32_t saw_0575_ms_ = 0;
GameSirCyclone2Transport::ReceiverState receiver_state_ =
    GameSirCyclone2Transport::ReceiverState::None;
GameSirCyclone2Transport::Transport last_transport_ =
    GameSirCyclone2Transport::Transport::Unknown;

void utf16le_to_ascii(const uint8_t* buf, uint16_t buflen, char* out, size_t out_len)
{
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    if (!buf || buflen < 2)
        return;
    const uint8_t str_len = buf[0];
    if (str_len < 2 || buf[1] != 0x03)
        return;
    size_t o = 0;
    for (uint8_t i = 2; i + 1 < str_len && i + 1 < buflen && o + 1 < out_len; i += 2) {
        const uint8_t lo = buf[i];
        const uint8_t hi = buf[i + 1];
        out[o++] = (hi == 0 && lo >= 0x20 && lo < 0x7F) ? static_cast<char>(lo) : '?';
    }
    out[o] = '\0';
}

void fetch_string(uint8_t daddr, uint8_t index, char* out, size_t out_len)
{
    out[0] = '\0';
    if (!index)
        return;
    uint8_t buf[64]{};
    if (tuh_descriptor_get_string_sync(daddr, index, 0x0409, buf, sizeof(buf)) != XFER_RESULT_SUCCESS)
        return;
    utf16le_to_ascii(buf, sizeof(buf), out, out_len);
}

const char* xfer_type_name(uint8_t attr)
{
    switch (attr & 0x03u) {
        case 0: return "CTRL";
        case 1: return "ISO";
        case 2: return "BULK";
        case 3: return "INT";
        default: return "?";
    }
}

void dump_interfaces_brief(uint8_t daddr)
{
    uint8_t cfg[512]{};
    if (tuh_descriptor_get_configuration_sync(daddr, 0, cfg, sizeof(cfg)) != XFER_RESULT_SUCCESS) {
        OGXM_LOG("configuration descriptor fetch failed\n");
        return;
    }

    auto const* conf = reinterpret_cast<tusb_desc_configuration_t const*>(cfg);
    const uint16_t total = tu_le16toh(conf->wTotalLength);
    const uint16_t len = TU_MIN(total, static_cast<uint16_t>(sizeof(cfg)));

    OGXM_LOG("Configuration: %u\n", conf->bConfigurationValue);
    OGXM_LOG("Interface count: %u\n", conf->bNumInterfaces);
    OGXM_LOG("wTotalLength: %u\n", total);

    uint8_t const* p = tu_desc_next(cfg);
    uint8_t const* end = cfg + len;
    while (p < end && tu_desc_type(p) != 0) {
        const uint8_t dtype = tu_desc_type(p);
        const uint8_t dlen = tu_desc_len(p);
        if (dlen < 2 || p + dlen > end)
            break;

        if (dtype == TUSB_DESC_INTERFACE) {
            auto const* itf = reinterpret_cast<tusb_desc_interface_t const*>(p);
            OGXM_LOG("\nInterface %u: class=0x%02X sub=0x%02X proto=0x%02X eps=%u\n",
                     itf->bInterfaceNumber, itf->bInterfaceClass, itf->bInterfaceSubClass,
                     itf->bInterfaceProtocol, itf->bNumEndpoints);
        } else if (dtype == TUSB_DESC_ENDPOINT) {
            auto const* ep = reinterpret_cast<tusb_desc_endpoint_t const*>(p);
            const uint8_t addr = ep->bEndpointAddress;
            OGXM_LOG("  endpoint 0x%02X %s type=%s max=%u interval=%u\n",
                     addr, (addr & TUSB_DIR_IN_MASK) ? "IN" : "OUT",
                     xfer_type_name(static_cast<uint8_t>(ep->bmAttributes.xfer)),
                     static_cast<unsigned>(tu_edpt_packet_size(ep)),
                     static_cast<unsigned>(ep->bInterval));
        }
        p = tu_desc_next(p);
    }
}

} // namespace

bool is_xinput_candidate(uint16_t vid, uint16_t pid)
{
    return vid == kGameSirVid && (pid == 0x100B || pid == 0x1053);
}

Mode infer_mode(uint16_t vid, uint16_t pid)
{
    if (is_xinput_candidate(vid, pid))
        return Mode::XInput;
    if (vid == kNintendoVid && pid == 0x2009)
        return Mode::Switch;
    if (vid == kSonyVid && pid == 0x09CC)
        return Mode::Ds4;
    if (vid == kGameSirVid && pid == 0x0575)
        return Mode::Hid;
    if (vid == kGameSirVid)
        return Mode::OtherGameSir;
    return Mode::Unknown;
}

const char* mode_name(Mode m)
{
    switch (m) {
        case Mode::XInput: return "XINPUT";
        case Mode::Switch: return "SWITCH";
        case Mode::Ds4: return "DS4";
        case Mode::Hid: return "HID";
        case Mode::OtherGameSir: return "GAMESIR_OTHER";
        default: return "UNKNOWN";
    }
}

const char* led_expected(Mode m)
{
    switch (m) {
        case Mode::XInput: return "GREEN";
        case Mode::Switch: return "RED";
        case Mode::Ds4: return "BLUE";
        case Mode::Hid: return "YELLOW";
        default: return "?";
    }
}

uint32_t cable_attach_ms()
{
    return cable_attach_ms_;
}

bool cyclone_session_active()
{
    return saw_cyclone_xinput_;
}

void note_cyclone_xinput_seen()
{
    if (!saw_cyclone_xinput_) {
        saw_cyclone_xinput_ = true;
        cyclone_xinput_seen_ms_ = to_ms_since_boot(get_absolute_time());
        OGXM_LOG("[CYCLONE2] session affinity: XInput personality armed (sticky for NS/DS4)\n");
    }
}

bool should_own_switch_ns(uint16_t vid, uint16_t pid)
{
    return saw_cyclone_xinput_ && vid == kNintendoVid && pid == 0x2009;
}

bool looks_like_cyclone_switch_ns(uint8_t address, uint16_t vid, uint16_t pid)
{
    if (vid != kNintendoVid || pid != 0x2009) {
        return false;
    }
    tusb_desc_device_t desc{};
    if (tuh_descriptor_get_device_sync(address, &desc, sizeof(desc)) != XFER_RESULT_SUCCESS) {
        OGXM_LOG("[CYCLONE2] NS fingerprint: device descriptor sync FAILED addr=%u "
                 "(session=%s)\n",
                 static_cast<unsigned>(address), saw_cyclone_xinput_ ? "YES" : "NO");
        return false;
    }
    const uint16_t bcd = tu_le16toh(desc.bcdDevice);
    char product[48]{};
    fetch_string(address, desc.iProduct, product, sizeof(product));

    /* Genuine Pro Controller product is typically "Pro Controller"; Cyclone 2 NS uses "Gamepad". */
    const bool product_cyclone = (std::strcmp(product, "Gamepad") == 0);
    const bool bcd_cyclone = (bcd == 0x0326);
    if (product_cyclone || bcd_cyclone) {
        OGXM_LOG("[CYCLONE2] NS fingerprint match addr=%u product=\"%s\" bcd=%04X "
                 "(claim dedicated driver; not SwitchProHost)\n",
                 static_cast<unsigned>(address), product, bcd);
        /* Sticky session so DS4/Switch remounts stay Cyclone-owned. */
        note_cyclone_xinput_seen();
        return true;
    }
    OGXM_LOG("[CYCLONE2] NS fingerprint miss addr=%u product=\"%s\" bcd=%04X "
             "session=%s → leave SWITCH_PRO\n",
             static_cast<unsigned>(address), product, bcd,
             saw_cyclone_xinput_ ? "YES" : "NO");
    return false;
}

bool should_own_ds4(uint16_t vid, uint16_t pid)
{
    return saw_cyclone_xinput_ && vid == kSonyVid && pid == 0x09CC;
}

bool should_own_hid_passive(uint16_t vid, uint16_t pid)
{
    return vid == kGameSirVid && pid == 0x0575;
}

void set_hid_passive_safety(uint8_t address, bool active)
{
    if (active) {
        hid_passive_safety_addr_ = address;
        OGXM_LOG("\nCYCLONE2 HID SAFETY MODE ACTIVE\n");
        OGXM_LOG("addr=%u — application OUT/feature/vendor TX suppressed\n",
                 static_cast<unsigned>(address));
    } else if (hid_passive_safety_addr_ == address) {
        hid_passive_safety_addr_ = 0;
        OGXM_LOG("[CYCLONE2 HID SAFETY] cleared for addr=%u\n", static_cast<unsigned>(address));
    }
}

bool hid_passive_safety_active(uint8_t address)
{
    return hid_passive_safety_addr_ != 0 && hid_passive_safety_addr_ == address;
}

void log_blocked_hid_out(uint8_t address, uint8_t instance, const char* caller,
                         const uint8_t* data, uint16_t len)
{
    OGXM_LOG("\n[CYCLONE2 HID SAFETY]\nBLOCKED OUT REPORT\n");
    OGXM_LOG("caller:\n%s\n", caller ? caller : "?");
    OGXM_LOG("addr=%u instance=%u\n", static_cast<unsigned>(address),
             static_cast<unsigned>(instance));
    OGXM_LOG("Reason:\nHID-mode initialization not yet verified\n");
    if (data && len > 0) {
        OGXM_LOG("report:\n");
        OGXM_LOG_HEX(data, len > 32 ? 32 : len);
    }
}

bool receiver_session_active()
{
    return saw_0575_;
}

void note_0575_mounted()
{
    saw_0575_ = true;
    saw_0575_ms_ = to_ms_since_boot(get_absolute_time());
    receiver_state_ = GameSirCyclone2Transport::ReceiverState::ControllerOffline;
    last_transport_ = GameSirCyclone2Transport::Transport::UsbReceiver24Ghz;
    OGXM_LOG("\n================================================\n");
    OGXM_LOG("CYCLONE 2 2.4GHZ RECEIVER / HID 3537:0575\n");
    OGXM_LOG("================================================\n");
    OGXM_LOG("Receiver detected (or wired HID personality)\n");
    OGXM_LOG("Controller radio link:\nOFFLINE / UNKNOWN\n");
    OGXM_LOG("Note:\n3537:0575 alone does NOT prove an active gamepad\n");
    OGXM_LOG("Note:\nNo PadIn mapping until controller-online personality\n");
    OGXM_LOG("================================================\n");
}

void note_controller_personality_mounted(uint16_t vid, uint16_t pid)
{
    (void)vid;
    (void)pid;
    if (saw_0575_) {
        receiver_state_ = GameSirCyclone2Transport::ReceiverState::ControllerOnline;
        last_transport_ = GameSirCyclone2Transport::Transport::UsbReceiver24Ghz;
        OGXM_LOG("[CYCLONE2 2.4GHZ] controller personality online after 0575 session "
                 "(reuse wired protocol engine)\n");
    } else {
        last_transport_ = GameSirCyclone2Transport::Transport::Wired;
        receiver_state_ = GameSirCyclone2Transport::ReceiverState::None;
    }
}

GameSirCyclone2Transport::Transport infer_usb_transport(uint16_t vid, uint16_t pid)
{
    using namespace GameSirCyclone2Transport;
    if (vid == kGameSirVid && pid == 0x0575) {
        return Transport::UsbReceiver24Ghz;
    }
    if (saw_0575_) {
        return Transport::UsbReceiver24Ghz;
    }
    if (is_xinput_candidate(vid, pid) || (vid == kNintendoVid && pid == 0x2009) ||
        (vid == kSonyVid && pid == 0x09CC)) {
        return Transport::Wired;
    }
    return last_transport_;
}

GameSirCyclone2Transport::ReceiverState receiver_state()
{
    return receiver_state_;
}

void set_receiver_controller_online(bool online)
{
    if (!saw_0575_) {
        return;
    }
    receiver_state_ = online ? GameSirCyclone2Transport::ReceiverState::ControllerOnline
                             : GameSirCyclone2Transport::ReceiverState::ControllerOffline;
}

void log_unified_banner(uint16_t vid, uint16_t pid, const char* bt_name,
                        bool input_active, const char* protocol_engine)
{
    using namespace GameSirCyclone2Transport;
    const GameSirCyclone2Transport::Mode m = mode_from_vid_pid(vid, pid);
    const Transport t = infer_usb_transport(vid, pid);
    OGXM_LOG("\n====================================================\n");
    OGXM_LOG("GAMESIR CYCLONE 2\n");
    OGXM_LOG("====================================================\n");
    OGXM_LOG("Physical driver:\nGAMESIR_CYCLONE2\n");
    OGXM_LOG("Transport:\n%s\n", transport_name(t));
    OGXM_LOG("Mode:\n%s\n", GameSirCyclone2Transport::mode_name(m));
    OGXM_LOG("Identity:\nVID=%04X\nPID=%04X\n", vid, pid);
    OGXM_LOG("Bluetooth name:\n%s\n", bt_name && bt_name[0] ? bt_name : "(n/a)");
    OGXM_LOG("Receiver present:\n%s\n",
             (t == Transport::UsbReceiver24Ghz || saw_0575_) ? "YES" : "NO");
    OGXM_LOG("Controller connected:\n%s\n",
             input_active ? "YES" : "NO / WAITING");
    OGXM_LOG("Receiver state:\n%s\n", receiver_state_name(receiver_state_));
    OGXM_LOG("Protocol engine:\n%s\n",
             protocol_engine ? protocol_engine : protocol_engine_name(m));
    OGXM_LOG("Input:\n%s\n", input_active ? "ACTIVE" : "WAITING");
    OGXM_LOG("====================================================\n");
}

void on_bus_attach(uint8_t rhport)
{
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    /* Physical unplug gap: mode-switch reattach is typically <1s; clear session after longer gap. */
    constexpr uint32_t kSessionClearGapMs = 2000;
    if ((saw_cyclone_xinput_ || saw_0575_) && last_detach_ms_ != 0 &&
        (now - last_detach_ms_) >= kSessionClearGapMs) {
        saw_cyclone_xinput_ = false;
        cyclone_xinput_seen_ms_ = 0;
        saw_0575_ = false;
        saw_0575_ms_ = 0;
        receiver_state_ = GameSirCyclone2Transport::ReceiverState::None;
        last_transport_ = GameSirCyclone2Transport::Transport::Unknown;
        OGXM_LOG("[CYCLONE2] session cleared after %lums cable gap\n",
                 static_cast<unsigned long>(now - last_detach_ms_));
    }
    if (cable_attach_ms_ == 0)
        cable_attach_ms_ = now;
    OGXM_LOG("\n[USB ATTACH] t=%lums rhport=%u\n",
             static_cast<unsigned long>(now), static_cast<unsigned>(rhport));
    OGXM_LOG("[CYCLONE2 MODE TRACE] Cable attach (relative 0 ms if first)\n");
}

void on_bus_remove(uint8_t rhport)
{
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    last_detach_ms_ = now;
    OGXM_LOG("\n[USB DETACH] t=%lums rhport=%u\n",
             static_cast<unsigned long>(now), static_cast<unsigned>(rhport));
    if (prev_vid_ || prev_pid_) {
        OGXM_LOG("previous VID/PID=%04X:%04X mode=%s\n",
                 prev_vid_, prev_pid_, mode_name(prev_mode_));
    }
    if (saw_cyclone_xinput_ && cyclone_xinput_seen_ms_) {
        OGXM_LOG("[CYCLONE2 MODE TRACE] USB detach %lums after first 3537 XInput sighting\n",
                 static_cast<unsigned long>(now - cyclone_xinput_seen_ms_));
    }
}

void on_device_configured(uint8_t address)
{
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    ++enum_seq_;

    tusb_desc_device_t desc{};
    if (tuh_descriptor_get_device_sync(address, &desc, sizeof(desc)) != XFER_RESULT_SUCCESS) {
        OGXM_LOG("\n[USB ENUM #%lu] t=%lums addr=%u device descriptor FAILED\n",
                 static_cast<unsigned long>(enum_seq_), static_cast<unsigned long>(now),
                 static_cast<unsigned>(address));
        return;
    }

    const uint16_t vid = tu_le16toh(desc.idVendor);
    const uint16_t pid = tu_le16toh(desc.idProduct);
    const uint16_t bcd = tu_le16toh(desc.bcdDevice);
    const Mode mode = infer_mode(vid, pid);

    char mfg[48]{};
    char product[48]{};
    char serial[48]{};
    fetch_string(address, desc.iManufacturer, mfg, sizeof(mfg));
    fetch_string(address, desc.iProduct, product, sizeof(product));
    fetch_string(address, desc.iSerialNumber, serial, sizeof(serial));

    const uint32_t rel = cable_attach_ms_ ? (now - cable_attach_ms_) : 0;

    OGXM_LOG("\n================================================\n");
    OGXM_LOG("[USB ENUM #%lu]\n", static_cast<unsigned long>(enum_seq_));
    OGXM_LOG("================================================\n");
    OGXM_LOG("t=%lums (cable+%lums)\n", static_cast<unsigned long>(now),
             static_cast<unsigned long>(rel));
    OGXM_LOG("addr=%u\n", static_cast<unsigned>(address));
    OGXM_LOG("\n");
    OGXM_LOG("VID=%04X\n", vid);
    OGXM_LOG("PID=%04X\n", pid);
    OGXM_LOG("bcdDevice=%04X\n", bcd);
    OGXM_LOG("\n");
    OGXM_LOG("Manufacturer=\"%s\"\n", mfg);
    OGXM_LOG("Product=\"%s\"\n", product);
    OGXM_LOG("Serial=\"%s\"\n", serial);
    OGXM_LOG("\n");
    OGXM_LOG("Device class=0x%02X subclass=0x%02X protocol=0x%02X\n",
             desc.bDeviceClass, desc.bDeviceSubClass, desc.bDeviceProtocol);
    OGXM_LOG("Configuration count=%u\n", desc.bNumConfigurations);
    OGXM_LOG("\n");
    OGXM_LOG("Cyclone2 XInput candidate: %s\n", is_xinput_candidate(vid, pid) ? "YES" : "NO");
    OGXM_LOG("Mode inferred: %s\n", mode_name(mode));
    OGXM_LOG("Home LED expected: %s\n", led_expected(mode));

    if (is_xinput_candidate(vid, pid)) {
        if (!saw_cyclone_xinput_) {
            saw_cyclone_xinput_ = true;
            cyclone_xinput_seen_ms_ = now;
            OGXM_LOG("\n[CYCLONE2 MODE TRACE] 3537 XInput detected at cable+%lums\n",
                     static_cast<unsigned long>(rel));
        }
    }

    if (saw_cyclone_xinput_ && mode == Mode::Switch) {
        OGXM_LOG("\n================================================\n");
        OGXM_LOG("CYCLONE 2 RESET / RE-ENUMERATION TRACE\n");
        OGXM_LOG("================================================\n");
        OGXM_LOG("OLD:\n");
        OGXM_LOG("VID:\n%04X\n", prev_vid_ ? prev_vid_ : 0x3537);
        OGXM_LOG("PID:\n%04X\n", prev_pid_ ? prev_pid_ : 0x100B);
        OGXM_LOG("mode:\n%s\n", mode_name(prev_mode_ != Mode::Unknown ? prev_mode_ : Mode::XInput));
        OGXM_LOG("session affinity:\nYES\n");
        OGXM_LOG("\n--- re-enumeration / personality change ---\n\n");
        OGXM_LOG("NEW:\n");
        OGXM_LOG("VID:\n%04X\n", vid);
        OGXM_LOG("PID:\n%04X\n", pid);
        OGXM_LOG("mode:\nSWITCH\n");
        OGXM_LOG("session affinity:\nYES (Cyclone will own NS; protocol=SWITCH_PRO_STANDARD)\n");
        OGXM_LOG("Expected physical driver:\nGAMESIR_CYCLONE2\n");
        OGXM_LOG("Expected protocol engine:\nSWITCH_PRO_STANDARD\n");
        OGXM_LOG("================================================\n");
        OGXM_LOG("\n*** CYCLONE 2 CHANGED USB PERSONALITY ***\n");
        OGXM_LOG("[CYCLONE2 MODE CHANGE]\n");
        OGXM_LOG("OLD: %04X:%04X %s %s\n",
                 prev_vid_ ? prev_vid_ : 0x3537, prev_pid_ ? prev_pid_ : 0x100B,
                 mode_name(prev_mode_ != Mode::Unknown ? prev_mode_ : Mode::XInput),
                 led_expected(prev_mode_ != Mode::Unknown ? prev_mode_ : Mode::XInput));
        OGXM_LOG("NEW: %04X:%04X %s %s\n", vid, pid, mode_name(mode), led_expected(mode));
        if (cyclone_xinput_seen_ms_) {
            OGXM_LOG("Fallback delay: ~%lums after first XInput sighting\n",
                     static_cast<unsigned long>(now - cyclone_xinput_seen_ms_));
        }
        if (last_detach_ms_) {
            OGXM_LOG("Re-attach gap after detach: %lums\n",
                     static_cast<unsigned long>(now - last_detach_ms_));
        }
        OGXM_LOG("Session affinity: Cyclone physical ownership; shared SwitchProHost protocol.\n");
    } else if (mode == Mode::Switch && !saw_cyclone_xinput_) {
        OGXM_LOG("\n================================================\n");
        OGXM_LOG("CYCLONE 2 RESET / RE-ENUMERATION TRACE\n");
        OGXM_LOG("================================================\n");
        OGXM_LOG("OLD:\n(session affinity clear or cold Switch plug)\n");
        OGXM_LOG("session affinity:\nNO\n");
        OGXM_LOG("\n--- mount ---\n\n");
        OGXM_LOG("NEW:\n");
        OGXM_LOG("VID:\n%04X\n", vid);
        OGXM_LOG("PID:\n%04X\n", pid);
        OGXM_LOG("session affinity:\nNO\n");
        OGXM_LOG("Expected path:\nSWITCH_PRO (or Cyclone if NS fingerprint matches)\n");
        OGXM_LOG("================================================\n");
    }

    /* Non-Cyclone personality after a long gap clears sticky if somehow still set. */
    if (saw_cyclone_xinput_ && mode == Mode::Unknown) {
        saw_cyclone_xinput_ = false;
        cyclone_xinput_seen_ms_ = 0;
        OGXM_LOG("[CYCLONE2] session cleared — non-Cyclone device enumerated\n");
    }

    dump_interfaces_brief(address);
    OGXM_LOG("================================================\n\n");

    prev_vid_ = vid;
    prev_pid_ = pid;
    prev_mode_ = mode;
}

void on_device_unmounted(uint8_t address)
{
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    OGXM_LOG("\n[USB UNMOUNT] t=%lums addr=%u prev=%04X:%04X (%s)\n",
             static_cast<unsigned long>(now), static_cast<unsigned>(address),
             prev_vid_, prev_pid_, mode_name(prev_mode_));
    last_detach_ms_ = now;
    if (hid_passive_safety_addr_ == address) {
        set_hid_passive_safety(address, false);
    }
}

void on_xinput_claimed(uint8_t address, uint8_t instance, uint8_t itf_num,
                       uint8_t ep_in, uint8_t ep_out)
{
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(address, &vid, &pid);
    const uint32_t now = to_ms_since_boot(get_absolute_time());
    const uint32_t rel = cable_attach_ms_ ? (now - cable_attach_ms_) : 0;

    OGXM_LOG("\n[CYCLONE2 MODE TRACE] XInput interface claimed\n");
    OGXM_LOG("t=%lums (cable+%lums) VID=%04X PID=%04X\n",
             static_cast<unsigned long>(now), static_cast<unsigned long>(rel), vid, pid);
    OGXM_LOG("instance=%u itf=%u ep_in=0x%02X ep_out=0x%02X\n",
             static_cast<unsigned>(instance), static_cast<unsigned>(itf_num), ep_in, ep_out);
}

void log_host_tx(uint8_t address, const char* kind, uint8_t ep,
                 const uint8_t* data, uint16_t len)
{
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(address, &vid, &pid);
    if (!is_xinput_candidate(vid, pid) && !(saw_cyclone_xinput_ && prev_mode_ == Mode::XInput))
        return;

    const uint32_t now = to_ms_since_boot(get_absolute_time());
    OGXM_LOG("[CYCLONE2 USB TX] t=%lums type=%s ep=0x%02X len=%u\n",
             static_cast<unsigned long>(now), kind ? kind : "?", ep,
             static_cast<unsigned>(len));
    if (data && len) {
        char line[96]{};
        size_t o = 0;
        const uint16_t n = (len > 16) ? 16 : len;
        for (uint16_t i = 0; i < n && o + 4 < sizeof(line); ++i)
            o += static_cast<size_t>(snprintf(line + o, sizeof(line) - o, "%02X ", data[i]));
        OGXM_LOG("  payload: %s%s\n", line, (len > 16) ? "..." : "");
    }
}

void log_host_ctrl(uint8_t address, uint8_t bm_req, uint8_t b_req,
                   uint16_t w_value, uint16_t w_index, uint16_t w_len)
{
    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(address, &vid, &pid);
    if (!is_xinput_candidate(vid, pid))
        return;

    const uint32_t now = to_ms_since_boot(get_absolute_time());
    OGXM_LOG("[CYCLONE2 USB TX] t=%lums type=control bm=0x%02X req=0x%02X "
             "wValue=0x%04X wIndex=0x%04X wLen=%u\n",
             static_cast<unsigned long>(now), bm_req, b_req, w_value, w_index,
             static_cast<unsigned>(w_len));
}

} // namespace GameSirCyclone2Trace

#else // !CONFIG_OGXM_DEBUG

namespace GameSirCyclone2Trace {
void on_bus_attach(uint8_t) {}
void on_bus_remove(uint8_t) {}
void on_device_configured(uint8_t) {}
void on_device_unmounted(uint8_t) {}
void on_xinput_claimed(uint8_t, uint8_t, uint8_t, uint8_t, uint8_t) {}
void log_host_tx(uint8_t, const char*, uint8_t, const uint8_t*, uint16_t) {}
void log_host_ctrl(uint8_t, uint8_t, uint8_t, uint16_t, uint16_t, uint16_t) {}
bool is_xinput_candidate(uint16_t, uint16_t) { return false; }
Mode infer_mode(uint16_t, uint16_t) { return Mode::Unknown; }
const char* mode_name(Mode) { return "UNKNOWN"; }
const char* led_expected(Mode) { return "?"; }
uint32_t cable_attach_ms() { return 0; }
bool cyclone_session_active() { return false; }
void note_cyclone_xinput_seen() {}
bool should_own_switch_ns(uint16_t, uint16_t) { return false; }
bool looks_like_cyclone_switch_ns(uint8_t, uint16_t, uint16_t) { return false; }
bool should_own_ds4(uint16_t, uint16_t) { return false; }
bool should_own_hid_passive(uint16_t, uint16_t) { return false; }
void set_hid_passive_safety(uint8_t, bool) {}
bool hid_passive_safety_active(uint8_t) { return false; }
void log_blocked_hid_out(uint8_t, uint8_t, const char*, const uint8_t*, uint16_t) {}
bool receiver_session_active() { return false; }
GameSirCyclone2Transport::Transport infer_usb_transport(uint16_t, uint16_t) {
    return GameSirCyclone2Transport::Transport::Unknown;
}
GameSirCyclone2Transport::ReceiverState receiver_state() {
    return GameSirCyclone2Transport::ReceiverState::None;
}
void set_receiver_controller_online(bool) {}
void note_0575_mounted() {}
void note_controller_personality_mounted(uint16_t, uint16_t) {}
void log_unified_banner(uint16_t, uint16_t, const char*, bool, const char*) {}
} // namespace GameSirCyclone2Trace

#endif // CONFIG_OGXM_DEBUG
