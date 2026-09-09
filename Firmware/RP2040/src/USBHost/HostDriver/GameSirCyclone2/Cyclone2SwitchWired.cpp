#include "USBHost/HostDriver/GameSirCyclone2/Cyclone2SwitchWired.h"

#include "Board/ogxm_log.h"
#include "Descriptors/SwitchPro.h"
#include "USBHost/HostDriver/GameSirCyclone2/GameSirCyclone2Trace.h"

namespace {

/** Same payload offset rules as SwitchProHost — face bits only. */
const SwitchPro::InReport* cyclone2_switch_in_report(const uint8_t* report, uint16_t len) {
    if (!report) {
        return nullptr;
    }
    if (len == sizeof(SwitchPro::InReport)) {
        return reinterpret_cast<const SwitchPro::InReport*>(report);
    }
    if (len >= 2U + sizeof(SwitchPro::InReport)) {
        const uint8_t rid = report[0];
        if (rid == SwitchPro::REPORT_ID_STANDARD || rid == SwitchPro::REPORT_ID_FULL_ALT ||
            rid == SwitchPro::REPORT_ID_SWITCH2_FULL || rid == SwitchPro::REPORT_ID_SUBCMD) {
            return reinterpret_cast<const SwitchPro::InReport*>(report + 2);
        }
    }
    return nullptr;
}

/**
 * Cyclone 2 uses Xbox-style physical face labels even in Switch mode.
 * SwitchProHost applies Nintendo layout conversion (A↔B, X↔Y) — wrong here.
 *
 * Snapshot Switch logical bits from the raw report, clear face bits, write 1:1:
 *   Switch A -> Xbox A, B -> B, X -> X, Y -> Y
 *
 * Uses BUTTON_* (XInput device reads these). Peek/overwrite latest PadIn — never
 * get_pad_in() (consumes queue) and never re-read bits written this frame.
 */
void apply_cyclone2_xbox_face_layout(Gamepad& gamepad, const SwitchPro::InReport* in) {
    Gamepad::PadIn gp = gamepad.peek_latest_pad_in();

    const uint8_t original_face = in->buttons[0];
    const bool switch_a = (original_face & SwitchPro::Buttons0::A) != 0;
    const bool switch_b = (original_face & SwitchPro::Buttons0::B) != 0;
    const bool switch_x = (original_face & SwitchPro::Buttons0::X) != 0;
    const bool switch_y = (original_face & SwitchPro::Buttons0::Y) != 0;

    constexpr uint16_t kFaceMask =
        Gamepad::BUTTON_A | Gamepad::BUTTON_B | Gamepad::BUTTON_X | Gamepad::BUTTON_Y;
    /* Also clear MAP_* in case SwitchPro wrote remapped face bits. */
    const uint16_t map_face =
        static_cast<uint16_t>(gamepad.MAP_BUTTON_A | gamepad.MAP_BUTTON_B |
                              gamepad.MAP_BUTTON_X | gamepad.MAP_BUTTON_Y);
    gp.buttons = static_cast<uint16_t>(gp.buttons & ~(kFaceMask | map_face));

    if (switch_a) {
        gp.buttons = static_cast<uint16_t>(gp.buttons | Gamepad::BUTTON_A);
    }
    if (switch_b) {
        gp.buttons = static_cast<uint16_t>(gp.buttons | Gamepad::BUTTON_B);
    }
    if (switch_x) {
        gp.buttons = static_cast<uint16_t>(gp.buttons | Gamepad::BUTTON_X);
    }
    if (switch_y) {
        gp.buttons = static_cast<uint16_t>(gp.buttons | Gamepad::BUTTON_Y);
    }

    gamepad.overwrite_latest_pad_in(gp);

#if defined(CONFIG_OGXM_DEBUG)
    static uint16_t s_prev_face = 0xFFFF;
    static int16_t s_prev_lx = 0x7FFF;
    static int16_t s_prev_ly = 0x7FFF;
    const uint16_t face = static_cast<uint16_t>(gp.buttons & kFaceMask);
    if (face != s_prev_face || gp.joystick_lx != s_prev_lx || gp.joystick_ly != s_prev_ly) {
        s_prev_face = face;
        s_prev_lx = gp.joystick_lx;
        s_prev_ly = gp.joystick_ly;
        OGXM_LOG("[1 RAW] report=0x30 face_raw=0x%02X\n", original_face);
        OGXM_LOG("[2 DRIVER] physical_driver=GAMESIR_CYCLONE2 mode=SWITCH\n");
        OGXM_LOG("[3 DECODED] A=%u B=%u X=%u Y=%u LX=%d LY=%d\n",
                 switch_a ? 1u : 0u, switch_b ? 1u : 0u, switch_x ? 1u : 0u, switch_y ? 1u : 0u,
                 static_cast<int>(gp.joystick_lx), static_cast<int>(gp.joystick_ly));
        OGXM_LOG("[4 OGX INPUT] buttons=0x%04X lx=%d ly=%d\n",
                 gp.buttons, static_cast<int>(gp.joystick_lx), static_cast<int>(gp.joystick_ly));
    }
#endif
}

} // namespace

void Cyclone2SwitchWired::reset() {
    started_ = false;
}

void Cyclone2SwitchWired::start(Gamepad& gamepad, uint8_t address, uint8_t instance,
                                 const uint8_t* report_desc, uint16_t desc_len) {
    started_ = true;

    OGXM_LOG("\n================================================\n");
    OGXM_LOG("INPUT DRIVER OWNERSHIP\n");
    OGXM_LOG("================================================\n");
    OGXM_LOG("Physical device:\nGAMESIR CYCLONE 2\n");
    OGXM_LOG("Session affinity:\n%s\n",
             GameSirCyclone2Trace::cyclone_session_active() ? "YES" : "NO");
    OGXM_LOG("Transport:\nUSB\n");
    OGXM_LOG("Mode:\nSWITCH\n");
    OGXM_LOG("Physical driver:\nGAMESIR_CYCLONE2\n");
    OGXM_LOG("Protocol engine:\n%s\n", protocol_engine_name());
    OGXM_LOG("Parser:\n%s\n", parser_name());
    OGXM_LOG("Face layout:\nXbox-style 1:1 (A→A B→B X→X Y→Y)\n");
    OGXM_LOG("Face map:\nbypass SwitchPro Nintendo A↔B/X↔Y for Cyclone\n");
    OGXM_LOG("================================================\n");

    engine_.set_init_profile(SwitchProHost::InitProfile::MinimalReportMode);
    engine_.initialize(gamepad, address, instance, report_desc, desc_len);
}

void Cyclone2SwitchWired::process_report(Gamepad& gamepad, uint8_t address, uint8_t instance,
                                          const uint8_t* report, uint16_t len) {
    if (!started_) {
        return;
    }
    engine_.process_report(gamepad, address, instance, report, len);

    const SwitchPro::InReport* in = cyclone2_switch_in_report(report, len);
    if (in) {
        apply_cyclone2_xbox_face_layout(gamepad, in);
    }
}

bool Cyclone2SwitchWired::send_feedback(Gamepad& gamepad, uint8_t address, uint8_t instance) {
    if (!started_) {
        return false;
    }
    return engine_.send_feedback(gamepad, address, instance);
}

void Cyclone2SwitchWired::disconnect(Gamepad& gamepad, uint8_t address, uint8_t instance) {
    if (started_) {
        engine_.disconnect_cb(gamepad, address, instance);
    }
    started_ = false;
}
