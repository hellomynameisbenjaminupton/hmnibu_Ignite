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

## 2. Repository Bootstrap (Clone Upstream or Start Standalone)

Run either Option A (fork/clone official upstream) or Option B (clean standalone repo that compiles on Linux/WSL2/macOS immediately) in Antigravity's terminal:

```bash
# OPTION A: Clone the official Behringer IGNITE open-source repository and add custom folders
git clone https://github.com/MusicTribeOpenSource/0716-AAF.git behringer-ignite-custom-fw
cd behringer-ignite-custom-fw
mkdir -p Source/Application/CustomSequencer AudIO/CustomEngines tests docs

# OPTION B: Initialize a clean standalone repository from scratch
mkdir -p behringer-ignite-custom-fw/{Source/Application/CustomSequencer,AudIO/CustomEngines,ComProtocol,tests,docs}
cd behringer-ignite-custom-fw
git init
```

## 3. Hardware Control Surface & I/O Invariants

Every feature implemented by Gemini in Antigravity must map directly to the physical **Behringer IGNITE (`0716-AAF`)** controls with zero menu diving:

1. **Bottom 16-Step Matrix**:
   - **16 RGB Step Buttons (`Steps 1–16`)**: Trigger/mute steps, select tracks, or punch in manual Euclidean overrides.
   - **16 Dedicated Per-Step Rotary Encoders (`StepEnc 1–16`)**: Located directly above each step button. Directly dial per-step Pitch, Velocity, Gate Length, Ratchets, Probability, Accumulator delta, or Stochastic weight depending on the active `ROW`.
2. **Top-Right 8 Assignable LED-Ring Macro Knobs (`Knobs A–H`)**:
   - Continuous encoders with LED collar rings (BCR32 style) for real-time algorithmic macros (Euclidean Pulses/Steps/Rotation/Accent, Turing Lock, Swing, Harmonizer Voicing, CV LFO Rate/Depth).
3. **Dedicated Navigation Encoders**:
   - **`ROW` Encoder**: Scrolls vertically between parameter lanes (Matriceal lanes: Gate, Pitch, Velocity, Octave, Glide, Ratchet, Probability, Accumulator, Aux Event) or tracks.
   - **`BAR` Encoder**: Jumps across bars `1–16` within a pattern or scenes `1–200` within a song.
   - **Main Jog Wheel + Touchscreen**: High-resolution piano-roll editing, Dual-Project crossfader view, and CV/MIDI routing matrix.
4. **Optical AI Camera (VL53L5CX Time-of-Flight)**:
   - Hand Z-distance and X-tilt mapped to real-time MIDI CC, pitch/velo "Sculpt" depth, or dedicated modular CV outputs (`CV 1–8`).
5. **Rear & Front I/O Matrix**:
   - **5 × 5-pin DIN MIDI IN & OUT** (Ports `DIN 1–5` = 80 independent DIN channels).
   - **USB-A Host MIDI** (up to 5 external USB MIDI synths/controllers) + **USB-C Device MIDI**.
   - **8 × Calibrated CV Outputs (`CV 1–8`) & 8 × Gate Outputs (`Gate 1–8`)** + **Analog Sync In/Out**.
   - **Stereo Line / Headphone Output** driven by `AudIO/`.

## 4. Technical Specification for the 9 Flagship Enhancements

### Enhancement 1: Matriceal Polymetric Lanes (`MatricealTrackEngine`)
- **Inspiration**: [OXI ONE MKII](https://oxiinstruments.com/oxi-one-mkii) ([User Manual](https://imagescdn.juno.co.uk/manual/948545-01U.pdf)).
- **Architecture**: Instead of tying all parameters of a step to a single shared loop length, decouple each track into **8 independent polymetric parameter lanes**:
  1. `GateLane` (length `1–64`, step division `1/4` to `1/64T`)
  2. `PitchLane` (length `1–64`, semitone offsets or scale degrees)
  3. `VelocityLane` (length `1–64`, `1–127`)
  4. `OctaveLane` (length `1–64`, `-3` to `+3` octaves)
  5. `GlideGateLenLane` (length `1–64`, `5%` to `100%` tie/portamento)
  6. `RatchetLane` (length `1–64`, `1x` to `8x` sub-triggers)
  7. `ProbabilityLane` (length `1–64`, `0%` to `100%` + `A:B` trig conditions)
  8. `ModCCCVLane` (length `1–64`, 14-bit CV or MIDI CC value)
- **Hardware Mapping**: Turn the **`ROW` encoder** to select the active lane; hold **`BAR` + Step Button `N`** to set that individual lane's loop length; turn **`StepEnc 1–16`** to edit all 16 step values for that lane simultaneously.

### Enhancement 2: Master Harmonizer & 63-Track Follower Bus (`MasterHarmonizerBus`)
- **Inspiration**: [OXI ONE MKII](https://oxiinstruments.com/oxi-one-mkii) Chord & Harmonizer Mode.
- **Architecture**:
  - Track 1 (or any designated track) acts as the **Master Chord & Scale Track**, broadcasting a live `HarmonicContext` struct (`rootNote`, `scaleMask12`, `activeChordNotes[6]`, `bassNote`, `tensionLevel`).
  - Any of the remaining 63 tracks can set its `HarmonizerFollowMode` to `Off`, `ScaleQuantize`, `ChordToneLock` (snaps arpeggios/melodies to the active chord tones), `BassRootFollow` (locks basslines to root/fifth), or `VoiceLedPad` (minimizes semitone movement between chord changes).
- **Hardware Mapping**: **Knobs `A–D`** on the Master Chord Track control Chord Voicing Spread, Inversion, Borrowed/Modal Interchange, and Strum Timing.

### Enhancement 3: Per-Step Accumulators & Inter-Track Aux Events (`StepAccumulatorAndAuxEngine`)
- **Inspiration**: [Sequentix Cirklon](https://www.reddit.com/r/synthesizers/comments/1gzfqwp/is_the_sequentix_cirklon_still_considered_the/) ([Accumulator Overview](https://www.youtube.com/watch?v=OVbotlbX8bs)).
- **Architecture**:
  - Every step includes a `StepAccumulator` struct: `deltaSemitones` (`-12` to `+12`), `deltaVelocity`, `minBound`, `maxBound`, `wrapMode` (`Wrap`, `Pendulum`, `Clamp`, `ResetOnBar`). Each time the step is evaluated, its accumulated offset advances by `delta` before quantizing to the active scale.
  - **Inter-Track Aux Events**: Any step can emit an internal command targeting another track (`TargetTrack 1–64`): `TransposeTrack(semitones)`, `RotateTrackSteps(n)`, `MuteTrackToggle()`, `ResetTrackPlayhead()`, or `RandomizeTrackVelocity(depth)`.
- **Hardware Mapping**: Select the Accumulator row via **`ROW`** and dial per-step pitch/velocity increments directly on **`StepEnc 1–16`**.

### Enhancement 4: Stochastic & Turing-Loop Generator (`StochasticTuringGenerator`)
- **Inspiration**: [OXI ONE MKII](https://oxiinstruments.com/oxi-one-mkii) Stochastic Mode & [Torso T-1](https://www.perfectcircuit.com/torso-t-1.html) Random/Turing Cycles.
- **Architecture**:
  - In **Stochastic Mode**, **`StepEnc 1–12`** set the probability weights (`0–100%`) of the 12 chromatic notes (or 7 diatonic degrees + 5 passing tones), while **`StepEnc 13–16`** set probability weights for Octave Jump, Rest/Tie, Ratchet Burst, and Glide.
  - A 16-bit **Turing Shift Register** sits downstream of the stochastic generator.
- **Hardware Mapping**: **Knob `E` (Turing Lock)** morphs continuously from `0%` (purely generative Markov/Stochastic stream) to `50%` (subtle bit-flipping mutations) to `100%` (locked repeating loop of `N` steps set by **Knob `F`**).

### Enhancement 5: Euclidean Generator with Non-Destructive Manual Overrides (`EuclideanOverrideGenerator`)
- **Inspiration**: [Torso T-1](https://www.perfectcircuit.com/torso-t-1.html).
- **Architecture**:
  - Computes Bjorklund's Euclidean rhythm algorithm `E(k, n, rotation)` alongside an independent Euclidean accent pattern `E_acc(k_acc, n, rot_acc)`.
  - Maintains two non-destructive bitmasks per bar: `uint16_t manualForceOnMask` and `uint16_t manualForceMuteMask`. Final step trigger = `(euclideanBit & ~manualForceMuteMask) | manualForceOnMask`.
- **Hardware Mapping**: **Knobs `A–D`** adjust Euclidean Steps (`n`), Pulses (`k`), Rotation, and Accent Division in real time; pressing any of the **16 RGB Step Pads** toggles a manual override step (lit in cyan for forced-on, dim red for forced-mute) without resetting the live Euclidean knobs.

### Enhancement 6: Dual-Project Live Transition Engine (`DualProjectTransitionEngine`)
- **Inspiration**: [Squarp Hapax](https://squarp.net/hapax/) ([Product Specs](https://www.perfectcircuit.com/squarp-hapax.html)).
- **Architecture**:
  - Instantiates two complete 64-track `IgniteProjectState` objects in pre-allocated RAM: `projectA` and `projectB`.
  - Provides a master **`TransitionMorpher`** (`0.0f` = 100% Project A, `1.0f` = 100% Project B) with three selectable transition behaviors per track group (Drums, Bass, Leads, Modular CV):
    1. `InstantBarSwitch` (cuts over cleanly on the next bar boundary)
    2. `VelocityAndCCFade` (scales outgoing MIDI velocities/CVs down while scaling incoming track velocities up)
    3. `PatternInterleave` (probabilistically swaps steps between Project A and Project B as the crossfader moves)
  - Smoothly interpolates master tempo between `projectA.bpm` and `projectB.bpm`.

### Enhancement 7: 8-Channel 48 kHz Modular CV Modulation Matrix (`ModularCVMatrix8`)
- **Inspiration**: [Squarp Hapax](https://squarp.net/hapax/) & [OXI ONE MKII](https://oxiinstruments.com/oxi-one-mkii) CV routing.
- **Architecture**:
  - Drives the rear **8 × CV (`CV 1–8`) and 8 × Gate (`Gate 1–8`)** hardware DACs at the audio block rate so unused CV jacks become high-resolution modular modulation sources:
    - `Pitch1VPerOct` (with 10-point hardware calibration lookup table matching IGNITE's calibration spec)
    - `HzPerVolt` (for vintage Korg/Yamaha synths)
    - `SyncLFOWave` (Sine, Triangle, Saw, Ramp, Square, Exponential, Phase-locked to 960 PPQN)
    - `LoopingADEnvelope` (triggered by any of the 64 tracks)
    - `SmoothRandomWalk` (slewed S&H noise)
    - `AICameraOpticalCV` (maps the VL53L5CX Time-of-Flight sensor's Z-height and X-tilt directly to calibrated `-5V..+5V` or `0..10V` CV outputs)

### Enhancement 8: 960 PPQN Clock Engine & Ported KICK Synth Core (`HighResClock960AndSynthEngine`)
- **Inspiration**: Upgrading stock [`MusicTribeOpenSource/0716-AAF`](https://github.com/MusicTribeOpenSource/0716-AAF) specs + cross-pollination from [`MusicTribeOpenSource/0700-AAM`](https://github.com/MusicTribeOpenSource/0700-AAM).
- **Architecture**:
  - Replaces IGNITE's stock **96 PPQN** clock with a **960 PPQN** (10× higher resolution) sub-tick scheduler supporting per-track micro-timing (`-239` to `+239` ticks), non-destructive groove templates, and polymetric phase reset.
  - Replaces or augments the basic stock General MIDI sound engine in `AudIO/` by importing `FM4Voice` (4-Operator, 8-Algorithm FM synth) and `GranularVoice` from the Behringer KICK custom firmware (`kick_custom_firmware_core.hpp`), giving IGNITE 8-voice internal FM + Drum synthesis for standalone sketching on headphones.

### Enhancement 9: Unified 64-Track I/O Router & Wireless Ableton Link (`UnifiedMIDICVRouter`)
- **Inspiration**: [Synth Anatomy IGNITE Overview](https://synthanatomy.com/2026/10/behringer-ignite-64-channel-multi-track-sequencer-with-open-firmware.html).
- **Architecture**:
  - Routes each of the 64 tracks to any combination of:
    - `MIDI DIN Ports 1–5` (Channels `1–16` per port)
    - `USB Host Ports 1–5` (Channels `1–16` per device)
    - `USB-C Device Port`
    - `CV/Gate Pairs 1–8` (with polyphonic voice allocation across multiple CV/Gate jacks, e.g., 4-voice polyphonic Eurorack routing on `CV/Gate 1–4`)
    - `Internal Synth Engine` (`FM4Voice` / `GranularVoice` / General MIDI)
    - `Wi-Fi RTP-MIDI & Ableton Link` (wireless tempo/phase sync with Behringer KICK, DAWs, and iOS devices).

## 5. Phased Implementation Plan for Gemini in Antigravity

All C++17 code must be written with **zero external library dependencies** for core headers (so host unit tests compile with standard `g++ -std=c++17 -Wall -Wextra -O2`), keeping JUCE UI bindings cleanly separated in `Source/Application/`.

| Phase | Target Files to Create | Milestone Scope & Verification |
| :--- | :--- | :--- |
| **Phase 1: 960 PPQN Core, Matriceal Lanes & Harmonizer** | `Source/Application/CustomSequencer/ignite_sequencer_core.hpp`, `tests/test_ignite_phase1.cpp`, `Makefile` | Implement the 960 PPQN clock, `MatricealTrackEngine` (8 independent polymetric lanes per track), and `MasterHarmonizerBus` (Master Chord track + 63 Follower tracks). Verify with `make test`. |
| **Phase 2: Algorithmic Generators (Accumulators, Stochastic/Turing, Euclidean)** | `Source/Application/CustomSequencer/ignite_algorithmic_engines.hpp`, `tests/test_ignite_phase2.cpp` | Implement Cirklon-style `StepAccumulatorAndAuxEngine`, `StochasticTuringGenerator` (16-encoder weight mixer + Turing shift register), and `EuclideanOverrideGenerator` (Bjorklund + non-destructive 16-pad override masks). Verify with `make test`. |
| **Phase 3: Dual-Project Live Engine, 8-CV Matrix & Hardware Bridge** | `Source/Application/CustomSequencer/ignite_hardware_and_cv.hpp`, `tests/test_ignite_phase3.cpp` | Implement `DualProjectTransitionEngine` (Project A/B RAM crossfading & BPM ramp), `ModularCVMatrix8` (calibrated 1V/Oct, Hz/V, LFOs, Envelopes, AI Camera CV), and `IgniteHardwareBridge` (16 Step Encoders, 16 RGB Pads, Knobs A–H, ROW/BAR encoders). Verify with `make test`. |
| **Phase 4: Unified 64-Track App, KICK FM/Drum Synth Port & WebMIDI Simulator** | `AudIO/CustomEngines/ignite_internal_synth.hpp`, `Source/Application/CustomSequencer/ignite_firmware_app.hpp`, `behringer_ignite_virtual_sequencer.html`, `tests/test_ignite_phase4.cpp` | Integrate all 64 tracks with the `UnifiedMIDICVRouter` (5× DIN, 5× USB Host, 8× CV/Gate, Wi-Fi Link) and internal 4-Op FM/Drum synth; build a single-file WebMIDI/WebAudio browser simulator matching the physical Behringer IGNITE front panel (16 Step Encoders + 16 RGB buttons + 8 LED-ring knobs `A–H` + Touchscreen). |

