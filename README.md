<p align="center">
  <img src="docs/images/social-preview.png" alt="Behringer IGNITE (BE 0716-AAF)" width="100%">
</p>

# Behringer IGNITE (BE 0716-AAF) — Custom Firmware & 9-Enhancement Sequencer Engine

> There is a Creator in every human being. Our mission is to empower those who have not and cannot.

Open-source repository and **9-Enhancement Custom Firmware** for the **Behringer IGNITE (`BE 0716-AAF`)** 64-channel multi-track hardware sequencer.

<p align="center">
  <img src="docs/images/ignite-front.jpg" alt="Behringer IGNITE" width="720">
</p>

## Custom Firmware Flagship Enhancements (`Source/Application/CustomSequencer/` & `AudIO/CustomEngines/`)

This repository extends the upstream `MusicTribeOpenSource/0716-AAF` architecture with 9 hardware-mapped sequencing, synthesis, and modular CV enhancements (all licensed under **AGPL-3.0-or-later**):

1. **Enhancement 1 — Matriceal Polymetric Lanes (`MatricealTrackEngine`)**: Decouples each of the 64 tracks into 8 independent polymetric parameter lanes (`Gate`, `Pitch`, `Velocity`, `Octave`, `GlideGateLen`, `Ratchet`, `Probability` with `A:B` trig conditions, and `ModCCCV` 14-bit CV/CC), each with independent loop length (`1–64`) and clock division (`1/4` to `1/64T`), mapped directly to `ROW`, `BAR`, and `StepEnc 1–16`.
2. **Enhancement 2 — Master Harmonizer & 63-Track Follower Bus (`MasterHarmonizerBus`)**: Track 1 broadcasts a live `HarmonicContext` (`rootNote`, `scaleMask12`, `activeChordNotes`, `bassNote`, `tensionLevel`) controlled by Macro Knobs `A–D` (Voicing Spread, Inversion, Modal Interchange, Strum Timing) to up to 63 follower tracks (`Off`, `ScaleQuantize`, `ChordToneLock`, `BassRootFollow`, `VoiceLedPad`).
3. **Enhancement 3 — Per-Step Accumulators & Inter-Track Aux Events (`StepAccumulatorAndAuxEngine`)**: Cirklon-inspired per-step pitch/velocity accumulators (`Wrap`, `Pendulum`, `Clamp`, `ResetOnBar`) and inter-track auxiliary commands (`TransposeTrack`, `RotateTrackSteps`, `MuteTrackToggle`, `ResetTrackPlayhead`, `RandomizeTrackVelocity`).
4. **Enhancement 4 — Stochastic & Turing-Loop Generator (`StochasticTuringGenerator`)**: `StepEnc 1–12` dial chromatic/diatonic note probability weights and `StepEnc 13–16` dial Octave Jump, Rest/Tie, Ratchet Burst, and Glide weights feeding a 16-bit Turing shift register controlled by Knob `E` (Turing Lock `0%–100%`) and Knob `F` (Loop Length `1–16`).
5. **Enhancement 5 — Euclidean Generator with Non-Destructive Manual Overrides (`EuclideanOverrideGenerator`)**: Bjorklund's `E(k, n, rotation)` + independent accent pattern `E_acc(k_acc, n, rot_acc)` with non-destructive `manualForceOnMask` (Cyan LED) and `manualForceMuteMask` (Dim Red LED) toggled on the 16 RGB step pads.
6. **Enhancement 6 — Dual-Project Live Transition Engine (`DualProjectTransitionEngine`)**: Pre-allocated RAM state for `projectA` and `projectB` (64 tracks each) with master `TransitionMorpher` (`0.0..1.0`), per-group transition behaviors (`InstantBarSwitch`, `VelocityAndCCFade`, `PatternInterleave`), and smooth BPM interpolation.
7. **Enhancement 7 — 8-Channel 48 kHz Modular CV Modulation Matrix (`ModularCVMatrix8`)**: Drives the rear `CV 1–8` and `Gate 1–8` DACs at 48 kHz with 10-point calibrated `Pitch1VPerOct`, `HzPerVolt`, 960-PPQN phase-locked `SyncLFOWave`, `LoopingADEnvelope`, `SmoothRandomWalk`, and `AICameraOpticalCV` (`VL53L5CX` Time-of-Flight Z-distance and X-tilt mapping).
8. **Enhancement 8 — 960 PPQN Clock Engine & Ported KICK Synth Core (`HighResClock960` & `IgniteInternalSynthEngine`)**: Upgrades stock 96 PPQN timing to 960 PPQN (10× resolution) with per-track micro-timing (`-239..+239` ticks), swing, and groove templates, plus an 8-voice internal synthesizer featuring a 4-Operator, 8-Algorithm FM synth (`FM4Voice`), `GranularVoice`, and analog drum synth ported from the companion Behringer KICK (`0700-AAM`) architecture.
9. **Enhancement 9 — Unified 64-Track I/O Router & Wireless Ableton Link (`UnifiedMIDICVRouter` & `IgniteFirmwareApp`)**: Routes all 64 tracks across `5 × MIDI DIN` ports (80 channels), `5 × USB Host` ports (80 channels), `USB-C Device MIDI`, `8 × CV/Gate` jacks (with polyphonic voice allocation), the internal FM/Granular/Drum synth engine, and wireless Wi-Fi RTP-MIDI / Ableton Link sync.

### Quick Start: Build & Run Unit Tests + Web Simulator

```sh
# Compile and run all 4 phases of C++17 unit tests (zero external dependencies required)
make test

# Launch the interactive single-file WebAudio/WebMIDI hardware simulator in any browser:
xdg-open behringer_ignite_virtual_sequencer.html
```

---

## Stock Hardware Key Features

- 64-channel multi-track sequencer with a song, scene, track and pattern structure
- Patterns of up to 16 bars with 16 steps per bar, scenes of up to 200 bars, 96 PPQN timing (upgraded to 960 PPQN in Custom Firmware)
- Real-time recording, undo, fill, solo/skip, gang and sculpt functions
- Transpose control and up to 10 user-defined scales
- Internal General MIDI sound engine (augmented with 8-voice 4-Op FM, Granular & Drum synth in Custom Firmware)
- 5 MIDI DIN in/out pairs, USB MIDI, and USB host MIDI for up to 5 devices
- MIDI clock sync, MIDI Machine Control and remote control
- 8 CV and 8 Gate jacks with calibration, plus sync in/out
- Wi-Fi connectivity

## Front panel

- Touchscreen
- 16 RGB step pads, each with its own encoder
- 8 assignable knobs (A–H) and a jog wheel
- ROW and BAR encoders
- Transport, song/scene/track/pattern, and edit buttons
- USB-A port (5 V, 1 A) with a USB STOP button

## Connections

<p align="center">
  <img src="docs/images/ignite-rear.jpg" alt="Behringer IGNITE rear panel" width="100%">
</p>

| Rear panel |
|---|
| Power switch, 12 V DC input (2000 mA) |
| USB-C |
| 5 × MIDI IN / OUT (5-pin DIN) |
| 8 × CV, 8 × Gate |
| Sync in and out |
| Line / headphone output |

## Repository

| Folder / File | Contents | License |
|---|---|---|
| [`Source/Application/CustomSequencer/`](Source/Application/CustomSequencer) | 9-enhancement C++17 custom sequencer core, algorithmic engines, CV matrix & hardware bridge | AGPL-3.0-or-later |
| [`AudIO/CustomEngines/`](AudIO/CustomEngines) | Ported 4-Op 8-Algorithm FM, Granular & KICK Drum synthesis engine | AGPL-3.0-or-later |
| [`tests/`](tests) | Phase 1–4 unit & integration test suites (`make test`) | AGPL-3.0-or-later |
| [`behringer_ignite_virtual_sequencer.html`](behringer_ignite_virtual_sequencer.html) | Standalone interactive WebAudio/WebMIDI hardware front-panel simulator | AGPL-3.0-or-later |
| [`GEMINI.md`](GEMINI.md) | Architectural blueprint & hardware control mapping specification | AGPL-3.0-or-later |
| [`Source/Application/`](Source/Application) | Upstream Application (built on JUCE) | AGPL-3.0-or-later |
| [`AudIO/`](AudIO) | Upstream Audio engine library | AGPL-3.0-or-later |
| [`ComProtocol/`](ComProtocol) | Upstream Hardware communication protocol (`CPktMgr`) | AGPL-3.0-or-later |
| [`Linux/`](Linux) | Embedded Buildroot Linux image | GPL-2.0 (Buildroot) |

The root [`LICENSE`](LICENSE) is the AGPL-3.0 text that covers the Music Tribe code (Application, AudIO,
ComProtocol) and all custom firmware extensions. Third-party components keep their original licenses. See the README in each folder.

---

Behringer is a brand of MUSIC Tribe. The Behringer name and logo, and the IGNITE name and logo, are
trademarks and are not covered by the software licenses in this repository. Neither are the product
images in `docs/images/`. See the
[trademark policy](https://github.com/MusicTribeOpenSource/.github/blob/main/TRADEMARKS.md).

Copyright (c) MUSIC Tribe & Custom Firmware Contributors.
