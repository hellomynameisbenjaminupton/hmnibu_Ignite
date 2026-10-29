/*
 * Copyright (c) 2026 Behringer IGNITE (BE 0716-AAF) Custom Firmware Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 3 Unit Tests:
 *  - DualProjectTransitionEngine (Project A/B crossfading, InstantBarSwitch, VelocityAndCCFade, PatternInterleave, BPM ramp)
 *  - ModularCVMatrix8 (10-point 1V/Oct calibration, Hz/V, 960 PPQN Sync LFO, Looping AD Envelope, VL53L5CX AI Camera CV)
 *  - IgniteHardwareBridge (StepEnc 1-16, 16 RGB Pads, Knobs A-H with LED rings, ROW/BAR, ComProtocol CRC-8 framing)
 */

#include "../Source/Application/CustomSequencer/ignite_hardware_and_cv.hpp"
#include "../ComProtocol/PktMgr/PktMgr.hpp"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace ignite;

namespace {

void testDualProjectTransitionEngine() {
    DualProjectTransitionEngine engine;
    XorShift32 rng(42U);

    engine.projectA().bpm = 120.0f;
    engine.projectB().bpm = 140.0f;

    // Configure Track 0 (Drums -> InstantBarSwitch) and Track 32 (Leads -> VelocityAndCCFade)
    engine.projectA().tracks[0].pressStepButton(0);
    engine.projectA().tracks[0].setBaseMidiNote(36); // Kick A = 36
    engine.projectB().tracks[0].pressStepButton(0);
    engine.projectB().tracks[0].setBaseMidiNote(38); // Snare B = 38

    engine.projectA().tracks[32].pressStepButton(0);
    engine.projectA().tracks[32].velocityLane().values[0] = 100;
    engine.projectA().tracks[32].modCCCVLane().values[0] = 4000;

    engine.projectB().tracks[32].pressStepButton(0);
    engine.projectB().tracks[32].velocityLane().values[0] = 100;
    engine.projectB().tracks[32].modCCCVLane().values[0] = 12000;

    // Morph at 0.25 -> BPM = 125.0
    engine.setTransitionMorphPosition(0.25f);
    assert(std::abs(engine.interpolatedBpm() - 125.0f) < 1e-4f);

    // Move morph to 0.75: Drums (InstantBarSwitch) should STILL play Project A until onBarBoundary() is called!
    engine.setTransitionMorphPosition(0.75f);
    assert(std::abs(engine.interpolatedBpm() - 135.0f) < 1e-4f);

    TransitionStepResult drumBeforeBar = engine.evaluateTrackStep(0, rng);
    assert(!drumBeforeBar.fromProjectB);
    assert(drumBeforeBar.primaryEvent.midiNote == 36);

    // Trigger bar boundary -> Drums now cleanly switch to Project B!
    engine.onBarBoundary();
    engine.projectA().tracks[0].resetAllPlayheads();
    engine.projectB().tracks[0].resetAllPlayheads();
    TransitionStepResult drumAfterBar = engine.evaluateTrackStep(0, rng);
    assert(drumAfterBar.fromProjectB);
    assert(drumAfterBar.primaryEvent.midiNote == 38);

    // Verify VelocityAndCCFade on Track 32 (Leads) at morph = 0.75:
    // Project B velocity = 100 * 0.75 = 75, Project A fade velocity = 100 * 0.25 = 25
    // Blended 14-bit CV = 4000 * 0.25 + 12000 * 0.75 = 1000 + 9000 = 10000
    TransitionStepResult leadFade = engine.evaluateTrackStep(32, rng);
    assert(leadFade.fromProjectB);
    assert(leadFade.primaryEvent.velocity == 75);
    assert(leadFade.secondaryFadeEvent.velocity == 25);
    assert(leadFade.primaryEvent.modCV14Bit == 10000);

    std::cout << "[PASS] Phase 3: DualProjectTransitionEngine (Project A/B morphing, BarSwitch, Vel/CV Fade, BPM ramp)\n";
}

void testModularCVMatrix8() {
    ModularCVMatrix8 matrix;
    XorShift32 rng(999U);

    // CV 0: 1V/Oct with custom 10-point hardware calibration table
    matrix.setCalibrationPoint(0, 3, 20000); // 3.0V (C3 = MIDI 36)
    matrix.setCalibrationPoint(0, 4, 26600); // 4.0V (C4 = MIDI 48)
    // MIDI note 42 (F#3 = 3.5V) should interpolate halfway between 20000 and 26600 -> 23300
    matrix.setChannelNoteAndGate(0, 42, true);

    // CV 1: Hz/V mode at C3 (MIDI 48 -> 1 octave above C2=36 -> 2.0V)
    matrix.channelConfig(1).mode = CVChannelMode::HzPerVolt;
    matrix.setChannelNoteAndGate(1, 48, true);

    // CV 2: Sync LFO (Ramp wave, period = 960 ticks)
    matrix.channelConfig(2).mode = CVChannelMode::SyncLFOWave;
    matrix.channelConfig(2).lfoShape = LFOWaveShape::Ramp;
    matrix.channelConfig(2).lfoPeriodTicks960 = 960;

    // CV 3: VL53L5CX AI Camera Optical CV (Z-distance at 20mm = max 10.0V)
    matrix.channelConfig(3).mode = CVChannelMode::AICameraOpticalCV;
    matrix.updateAICameraSensor(20, 0.0f, true);

    // Process at tick 480 (50% phase of 960-tick LFO)
    matrix.processSampleAt48kHz(480, rng);

    assert(matrix.channelDACCode(0) == 23300);
    assert(std::abs(matrix.channelVoltage(1) - 2.0f) < 1e-4f);
    assert(std::abs(matrix.channelVoltage(2) - 5.0f) < 1e-3f); // 50% ramp of 0..10V = 5.0V
    assert(std::abs(matrix.channelVoltage(3) - 10.0f) < 1e-3f);
    assert(matrix.channelDACCode(3) == 65535);

    std::cout << "[PASS] Phase 3: ModularCVMatrix8 (10-pt 1V/Oct calibration, Hz/V, 960-PPQN LFO, VL53L5CX Optical CV)\n";
}

void testIgniteHardwareBridgeAndComProtocol() {
    IgniteHardwareBridge bridge;
    MatricealTrackEngine track;
    EuclideanOverrideGenerator euclidean;
    StochasticTuringGenerator stochastic;
    StepAccumulatorAndAuxEngine accumulator;
    MasterHarmonizerBus harmonizer;
    DualProjectTransitionEngine dualProj;

    // Test Hold BAR + Step Pad 7 -> sets active lane loop length to 7
    bridge.setBarButtonHeld(true);
    bridge.onStepPadPressed(6, track, euclidean); // pad index 6 = Step Button 7
    bridge.setBarButtonHeld(false);
    assert(track.laneLength(MatricealLaneType::Gate) == 7);

    // Test Euclidean mode pad override & Cyan LED feedback
    bridge.setControlMode(IgniteControlMode::EuclideanMode);
    bridge.onStepPadPressed(3, track, euclidean);
    const RGBColor led3 = bridge.padLedColor(3);
    assert(led3.r == 0 && led3.g == 255 && led3.b == 255);

    // Test Macro Knob H (index 7) controlling Dual-Project Transition Crossfader
    bridge.onMacroKnobTurned(7, 63, harmonizer, euclidean, stochastic, dualProj); // 64 + 63 = 127
    assert(bridge.macroKnobValue(7) == 127);
    assert(bridge.macroKnobLedRing(7) == 0x7FFFU); // All 15 collar LEDs lit
    assert(std::abs(dualProj.transitionMorphPosition() - 1.0f) < 1e-5f);

    // Verify ComProtocol (`CPktMgr`) CRC-8 compatibility
    const std::vector<uint8_t> frame = IgniteHardwareBridge::buildComProtocolFrame(
        IgnitePktType::Pad, 0, 0x05, {0x03, 0x00, 0xFF, 0xFF});
    assert(IgniteHardwareBridge::verifyComProtocolFrame(frame));

    // Cross-check CRC-8 against upstream CPktMgr::CrcGen
    std::vector<char> upstreamVec(frame.begin(), frame.end() - 1);
    const uint8_t upstreamCrc = CPktMgr::CrcGen(upstreamVec);
    assert(upstreamCrc == frame.back());

    std::cout << "[PASS] Phase 3: IgniteHardwareBridge & upstream ComProtocol/CPktMgr CRC-8 interoperability\n";
}

} // namespace

int main() {
    std::cout << "=== Behringer IGNITE (0716-AAF) Custom Firmware - Phase 3 Tests ===\n";
    testDualProjectTransitionEngine();
    testModularCVMatrix8();
    testIgniteHardwareBridgeAndComProtocol();
    std::cout << "=== All Phase 3 Tests Passed Successfully ===\n";
    return 0;
}

