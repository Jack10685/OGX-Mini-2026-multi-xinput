# GameSir Cyclone 2

Physically tested / implementation status for the **GameSir Cyclone 2** on OGX-Mini (this fork).

All personalities are owned by the dedicated **`GameSirCyclone2Host`**. Shared protocol helpers (e.g. `SwitchPro::*` / `SwitchProHost`) may be reused; **do not patch Xbox / PS4 / Switch drivers for Cyclone-only quirks** when a dedicated personality path exists.

## Mode USB IDs (wired / 2.4 GHz receiver)

| Home LED | Mode | Typical USB ID | Status |
|----------|------|----------------|--------|
| Green | PC / XInput | `3537:100B` (or `3537:1053`) | **WORKING** wired — reuse on receiver when same ID |
| Blue | DS4 | `054C:09CC` | **WORKING** wired (session-affined) |
| Red | NS / Switch | `057E:2009` | **WORKING** wired (1:1 Xbox face layout) |
| Yellow / idle dongle | HID or idle receiver | `3537:0575` | **PASSIVE** — no PadIn (may be idle 2.4 GHz receiver) |

Mode combos: `Home+X` XInput, `Home+Y` Switch, `Home+A` HID, `Home+B` DS4. Wired / 2.4 GHz also cycle with **View + Menu**.

## Transport model

```text
physical_driver = GAMESIR_CYCLONE2
transport       = WIRED | 2.4GHZ_RECEIVER | BLUETOOTH
mode            = XINPUT | DS4 | SWITCH | HID
```

- **`3537:0575` alone does not mean a live gamepad.** Research: idle 2.4 GHz receiver can enumerate as this ID with the pad powered off. OGX claims it as Cyclone, listens passively, does **not** map PadIn.
- After `0575` in-session, remount as `100B` / Switch / DS4 is tagged `2.4GHZ_RECEIVER` and **reuses** the same wired protocol engines.
- Cold-plug green XInput without prior `0575` stays `WIRED`.

## Wired XInput (green) — WORKING

Do not regress. Optional vendor `0F F2` / report `0x12` for L4/R4/M (XInput only).

## Wired Switch / NS (red) — WORKING

`Cyclone2SwitchWired` → `SwitchProHost` MinimalReportMode. Face buttons: **A=A B=B X=X Y=Y** (bypass Nintendo A↔B).

## Wired DS4 (blue) — WORKING (session-affined)

`Cyclone2Ds4Wired`. No `0F F2`.

## 2.4 GHz receiver — IN PROGRESS

| Test | Expectation |
|------|-------------|
| Receiver in, pad OFF | `3537:0575` (or capture actual ID), `Input: WAITING`, no phantom controls |
| Pad ON XInput | Remount / link → reuse XInput engine, `Input: ACTIVE` |
| Pad OFF again | Slot waits; receiver stays monitored |
| Mode cycle on dongle | Re-detect personality; shared parser |

Confirm VID/PID / product / bcd / report activity on **physical** hardware before marking PASS.

## Bluetooth — SCAFFOLDING

| Mode | Official | Status |
|------|----------|--------|
| DS4 (blue) | name often `Wireless Controller` | Probe logs shared name; **do not** claim all DS4 pads |
| Switch (red) | GameSir BT Switch | Not installed yet — no USB `0x80` handshake on BT |
| HID (yellow) | `GameSir-Cyclone 2` | Strong-name probe only |
| XInput | **N/A** | GameSir does not support BT XInput |

`Cyclone2BtProbe` observes discovery/ready for strong `GameSir-Cyclone*` names. Dedicated BT parsers come after capture.

## Related

- [Wired Controllers](Wired_Controllers.md)
- [Adding supported controllers](Adding_Supported_Controllers.md)
