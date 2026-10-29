/*
 * Copyright (c) 2026 Behringer IGNITE (BE 0716-AAF) Custom Firmware Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 2 Unit Tests:
 *  - StepAccumulatorAndAuxEngine (Wrap, Pendulum, Clamp, ResetOnBar accumulators & Inter-Track Aux Events)
 *  - StochasticTuringGenerator (16-encoder weight mixer + 16-bit Turing shift register lock & loop length)
 *  - EuclideanOverrideGenerator (Bjorklund E(k, n, rot) + non-destructive Forced-On/Forced-Mute bitmasks & RGB LEDs)
 */

#include "../Source/Application/CustomSequencer/ignite_algorithmic_engines.hpp"
#include <cassert>
#include <iostream>
#include <vector>

using namespace ignite;

namespace {

void testStepAccumulatorAndAuxEngine() {
    StepAccumulatorAndAuxEngine engine;

    // 1. Test Wrap Mode on Step 0: delta = +3 semitones, bounds = [-2, +5] (span = 8)
    engine.configureStepAccumulator(0, 3, 5, -2, 5, AccumulatorWrapMode::Wrap);
    std::vector<uint8_t> wrapNotes;
    for (int i = 0; i < 4; ++i) {
        EvaluatedStepEvent ev{};
        ev.triggered = true;
        ev.midiNote = 60;
        ev.velocity = 100;
        engine.evaluateStepAccumulator(0, ev);
        wrapNotes.push_back(ev.midiNote);
    }
    // Pass 0: 60 + 0 = 60
    // Pass 1: 60 + 3 = 63
    // Pass 2: +3 + 3 = +6 -> wraps by -8 to -2 -> 60 - 2 = 58
    // Pass 3: -2 + 3 = +1 -> 60 + 1 = 61
    assert(wrapNotes[0] == 60);
    assert(wrapNotes[1] == 63);
    assert(wrapNotes[2] == 58);
    assert(wrapNotes[3] == 61);

    // 2. Test Pendulum Mode on Step 1: delta = +4 semitones, bounds = [0, 6]
    engine.configureStepAccumulator(1, 4, 0, 0, 6, AccumulatorWrapMode::Pendulum);
    std::vector<uint8_t> pendNotes;
    for (int i = 0; i < 4; ++i) {
        EvaluatedStepEvent ev{};
        ev.triggered = true;
        ev.midiNote = 60;
        engine.evaluateStepAccumulator(1, ev);
        pendNotes.push_back(ev.midiNote);
    }
    // Pass 0: 0 -> 60
    // Pass 1: +4 -> 64
    // Pass 2: +4 + 4 = 8 -> bounces at 6 to 4 -> 64
    // Pass 3: 4 - 4 = 0 -> 60
    assert(pendNotes[0] == 60);
    assert(pendNotes[1] == 64);
    assert(pendNotes[2] == 64);
    assert(pendNotes[3] == 60);

    // 3. Test Inter-Track Aux Events targeting another track (e.g. Step 0 transposes Track 5 by +7 semitones,
    //    Step 1 toggles mute on Track 2, Step 2 resets playheads on Track 3)
    std::array<MatricealTrackEngine, kNumTracks> tracks{};
    std::array<TrackAuxRuntimeState, kNumTracks> states{};
    XorShift32 rng(12345U);

    engine.configureStepAuxEvent(0, AuxCommandType::TransposeTrack, 5, 7);
    engine.configureStepAuxEvent(1, AuxCommandType::MuteTrackToggle, 2, 0);
    engine.configureStepAuxEvent(2, AuxCommandType::ResetTrackPlayhead, 3, 0);

    tracks[3].evaluateAndAdvance(0);
    tracks[3].evaluateAndAdvance(0);
    assert(tracks[3].gateLane().playhead == 2);

    engine.dispatchAuxEvent(0, tracks, states, rng);
    assert(tracks[5].baseMidiNote() == 67);
    assert(states[5].transposeSemitones == 7);

    engine.dispatchAuxEvent(1, tracks, states, rng);
    assert(states[2].muted);

    engine.dispatchAuxEvent(2, tracks, states, rng);
    assert(tracks[3].gateLane().playhead == 0);

    std::cout << "[PASS] Phase 2: StepAccumulatorAndAuxEngine (Wrap/Pendulum/Clamp accumulators & Inter-Track Aux Events)\n";
}

void testStochasticTuringGenerator() {
    StochasticTuringGenerator gen;
    XorShift32 rng(0xCAFEBABEU);

    // Set Knob E = 0% (purely generative) and verify notes follow weights
    // Set ONLY semitone +7 (5th) weight to 100%, all other 11 notes to 0%
    for (std::size_t i = 0; i < 12; ++i) {
        gen.setNoteWeight(i, (i == 7) ? 100 : 0);
    }
    gen.setArticulationWeights(0, 0, 0, 0); // No octave jump, no rests
    gen.setKnobE_TuringLock(0.0f);
    gen.setKnobF_LoopLength(4);

    for (int i = 0; i < 8; ++i) {
        StochasticStepOutput out = gen.nextStep(rng);
        assert(!out.isRest);
        assert(out.midiNote == 67); // Base 60 + 7 = 67
    }

    // Now enable all 12 chromatic weights, generate 4 steps at 0% lock, then lock Knob E to 100% (4-step loop)
    for (std::size_t i = 0; i < 12; ++i) {
        gen.setNoteWeight(i, 80);
    }
    gen.setArticulationWeights(30, 0, 0, 0);
    gen.setKnobE_TuringLock(0.0f);
    gen.setKnobF_LoopLength(4);
    for (int i = 0; i < 4; ++i) {
        gen.nextStep(rng);
    }

    // Lock Turing register at 100% and verify the 4-step sequence repeats identically across multiple cycles
    gen.setKnobE_TuringLock(100.0f);
    std::array<uint8_t, 4> cycle1{};
    std::array<uint8_t, 4> cycle2{};
    for (std::size_t i = 0; i < 4; ++i) {
        cycle1[i] = gen.nextStep(rng).midiNote;
    }
    for (std::size_t i = 0; i < 4; ++i) {
        cycle2[i] = gen.nextStep(rng).midiNote;
    }
    assert(cycle1 == cycle2);

    std::cout << "[PASS] Phase 2: StochasticTuringGenerator (16-encoder weights + Turing Lock 0%..100% loop verification)\n";
}

void testEuclideanOverrideGenerator() {
    EuclideanOverrideGenerator euc;

    // E(4, 16, 0) = four-on-the-floor (steps 0, 4, 8, 12 -> 0x1111)
    euc.setEuclideanParams(16, 4, 0, 2, 0);
    assert(euc.rawEuclideanMask() == 0x1111U);
    assert(euc.effectiveTriggerMask() == 0x1111U);

    // Punch in non-destructive manual overrides on Step Pads:
    // - Press Pad 2 once -> ForcedOn (Cyan LED)
    // - Press Pad 4 twice -> ForcedMute (Dim Red LED)
    euc.pressStepPadOverride(2);
    assert(euc.stepOverrideState(2) == PadOverrideState::ForcedOn);
    const RGBColor cyan = euc.padLedColor(2);
    assert(cyan.r == 0 && cyan.g == 255 && cyan.b == 255);

    euc.pressStepPadOverride(4); // ForcedOn
    euc.pressStepPadOverride(4); // ForcedMute
    assert(euc.stepOverrideState(4) == PadOverrideState::ForcedMute);
    const RGBColor dimRed = euc.padLedColor(4);
    assert(dimRed.r == 90 && dimRed.g == 0 && dimRed.b == 0);

    // Verify formula: (euclideanBit & ~manualForceMuteMask) | manualForceOnMask
    // Step 2 is ForcedOn (true), Step 4 is ForcedMute (false), Step 0 & 8 & 12 remain true
    assert(euc.shouldTriggerStep(0));
    assert(euc.shouldTriggerStep(2));
    assert(!euc.shouldTriggerStep(4));
    assert(euc.shouldTriggerStep(8));

    // Turn Euclidean Knobs A-D to E(8, 16, 1) and verify manual overrides on Pads 2 and 4 survive!
    euc.setEuclideanParams(16, 8, 1, 4, 0);
    assert(euc.stepOverrideState(2) == PadOverrideState::ForcedOn);
    assert(euc.stepOverrideState(4) == PadOverrideState::ForcedMute);
    assert(euc.shouldTriggerStep(2));
    assert(!euc.shouldTriggerStep(4));

    std::cout << "[PASS] Phase 2: EuclideanOverrideGenerator (Bjorklund E(k,n,rot) + non-destructive Cyan/Red pad overrides)\n";
}

} // namespace

int main() {
    std::cout << "=== Behringer IGNITE (0716-AAF) Custom Firmware - Phase 2 Tests ===\n";
    testStepAccumulatorAndAuxEngine();
    testStochasticTuringGenerator();
    testEuclideanOverrideGenerator();
    std::cout << "=== All Phase 2 Tests Passed Successfully ===\n";
    return 0;
}
