# Behringer IGNITE (`BE 0716-AAF`) Custom Firmware — Antigravity Blueprint & `GEMINI.md`

Save this file as **`GEMINI.md`** (or `docs/IGNITE_BLUEPRINT.md`) in the root of your **Behringer IGNITE (`0716-AAF`)** repository. Gemini in Antigravity automatically reads `GEMINI.md` at the start of every session, giving it the complete architectural specification, hardware control map, and 4-phase implementation plan for all 9 flagship sequencer enhancements.

## 1. Project Overview & Upstream Architecture

- **Target Hardware**: **Behringer IGNITE (`BE 0716-AAF`)** — 64-Channel Multi-Track Hardware Sequencer with Touchscreen, 16 Step Encoders, 8 Macro Knobs (`A–H`), 8 CV/Gate Outputs, 5 MIDI DIN In/Out Pairs, USB Host MIDI, Wi-Fi, AI Camera, and Internal Sound Engine ([`MusicTribeOpenSource/0716-AAF`](https://github.com/MusicTribeOpenSource/0716-AAF), [Synth Anatomy](https://synthanatomy.com/2026/10/behringer-ignite-64-channel-multi-track-sequencer-with-open-firmware.html)).
- **Companion Sister Project**: **Behringer KICK (`BE 0700-AAM`)** ([`MusicTribeOpenSource/0700-AAM`](https://github.com/MusicTribeOpenSource/0700-AAM)). Both devices share Music Tribe's open-source `AudIO/`, `ComProtocol/`, and `Linux/` (Buildroot) architecture.
- **License Requirement**: Upstream `Source/Application/` (built on C++ **JUCE**), `AudIO/`, and `ComProtocol/` are licensed under **AGPL-3.0-or-later** (`Linux/` Buildroot is **GPL-2.0**). All custom C++17 modules and web simulators in this project must maintain AGPL-3.0-or-later compatibility.

### Upstream Directory Map (`MusicTribeOpenSource/0716-AAF`)

| Directory | Upstream Role | License | Custom Firmware Extension Target |
| :--- | :--- | :--- | :--- |
| `Source/Application/` | Main 64-track sequencer application & touchscreen UI (built on JUCE) | AGPL-3.0-or-later | `Source/Application/CustomSequencer/` (9-enhancement C++17 sequencing engine) |
| `AudIO/` | Internal audio & General MIDI synth engine | AGPL-3.0-or-later | `AudIO/CustomEngines/` (Ported 4-Op FM, Granular, & 48 kHz CV/Audio DSP) |
| `ComProtocol/` | Hardware MCU communication protocol (pads, 16 step encoders, knobs A–H, CV DACs) | AGPL-3.0-or-later | `Source/Application/CustomSequencer/ignite_hardware_bridge.hpp` |
| `Linux/` | Embedded Buildroot Linux image & Wi-Fi stack | GPL-2.0 | Ableton Link / RTP-MIDI daemon & cross-compilation (`aarch64` / `armv7`) |

