#pragma once

#include <cstdint>

/**
 * Bluetooth discovery/lifecycle probe for GameSir Cyclone 2.
 *
 * Phase 1: observe and log only.
 * - Do NOT claim all "Wireless Controller" DS4 devices as Cyclone.
 * - Do NOT invent Bluetooth XInput (unsupported by GameSir).
 * - Dedicated parser install comes after physical name/descriptor capture.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "uni_hid_device.h"

/** True for strong Cyclone BT name fingerprints (not shared Sony DS4 alone). */
int gamesir_cyclone2_bt_is_strong_candidate(const uni_hid_device_t* device);

/** Name/COD discovery note (may fire for "Wireless Controller" without claiming). */
void gamesir_cyclone2_bt_on_discovered(const uint8_t* bd_addr, const char* name, uint16_t cod,
                                       uint8_t rssi);

void gamesir_cyclone2_bt_on_connected(uni_hid_device_t* device);
void gamesir_cyclone2_bt_on_ready(uni_hid_device_t* device);
void gamesir_cyclone2_bt_on_disconnected(uni_hid_device_t* device);

#ifdef __cplusplus
}
#endif
