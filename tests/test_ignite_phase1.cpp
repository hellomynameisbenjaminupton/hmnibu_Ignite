/*
 * Copyright (c) 2026 Behringer IGNITE (BE 0716-AAF) Custom Firmware Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 1 Unit Tests:
 *  - HighResClock960 (960 PPQN sub-tick scheduler, micro-timing, swing, groove, polymetric reset)
 *  - MatricealTrackEngine (8 independent polymetric lanes, ROW/BAR/StepEnc hardware mapping, A:B trig conditions)
 *  - MasterHarmonizerBus (Master Chord Track + 63 Follower Tracks, Knobs A-D voicing/inversion/modal/strum)
 */

#include "../Source/Application/CustomSequencer/ignite_sequencer_core.hpp"
#include <cassert>
#include <cmath>
#include <iostream>

using namespace ignite;

namespace {

void testHighResClock960() {
    HighResClock960 clock;
    assert(kPPQN960 == 960);
    assert(kPPQN960 == 10 * kStockPPQN96);

    clock.setBpm(120.0f);
    clock.setSampleRate(48000.0f);
    clock.start();

    // At 120 BPM (= 2 beats/sec), 960 PPQN = 1920 ticks/sec.
    // At 48000 Hz, 48000 / 1920 = 25.0 audio samples per tick.
    assert(std::abs(clock.samplesPerTick() - 25.0) < 1e-6);

    // Advance 2500 samples -> exactly 100 ticks
    const uint32_t elapsed = clock.advanceAudioSamples(2500);
    assert(elapsed == 100);
    assert(clock.currentTick() == 100);

    // Test micro-timing clamping (-239 to +239 ticks)
    clock.setTrackMicroTiming(3, -300);
    assert(clock.trackMicroTiming(3) == -239);
    clock.setTrackMicroTiming(3, 300);
    assert(clock.trackMicroTiming(3) == 239);
    clock.setTrackMicroTiming(3, -45);
    assert(clock.trackMicroTiming(3) == -45);

    // Step 2 (index 2, even step) at 1/16 division = 2 * 240 = 480 ticks; with -45 micro-timing = 435 ticks
    assert(clock.scheduledTickForStep(3, 2, StepDivision::Div_1_16) == 435);

    // Test Swing on odd step (Step 1): 62.5% swing -> (12.5 / 50.0) * 240 = +60 ticks -> 240 + 60 = 300
    clock.setSwingPercent(62.5f);
    assert(clock.scheduledTickForStep(0, 1, StepDivision::Div_1_16) == 300);

    // Test Polymetric Bar Reset every 4 bars (4 * 3840 = 15360 ticks)
    clock.setPolymetricResetBars(4);
    assert(!clock.shouldTriggerPolymetricReset(3840));
    assert(clock.shouldTriggerPolymetricReset(15360));

    std::cout << "[PASS] Phase 1: HighResClock960 (960 PPQN, micro-timing, swing, polymetric reset)\n";
}

void testMatricealTrackEngine() {
    MatricealTrackEngine track;

    // Configure polymetric lane lengths using hardware mapping (ROW + BAR + Step Button N)
    // Gate lane length = 5, Pitch lane length = 3, Velocity lane length = 4, Octave lane length = 7
    track.setActiveRow(MatricealLaneType::Gate);
    track.setActiveBarPage(0);
    track.setLaneLoopLengthViaBarAndStep(5);
    assert(track.laneLength(MatricealLaneType::Gate) == 5);

    // Turn ROW encoder +1 -> Pitch lane
    track.turnRowEncoder(1);
    assert(track.activeRow() == MatricealLaneType::Pitch);
    track.setLaneLoopLengthViaBarAndStep(3);
    assert(track.laneLength(MatricealLaneType::Pitch) == 3);

    // Turn StepEnc 0..2 on Pitch lane to +0, +7, +12 semitones
    track.turnStepEncoder(0, 0);
    track.turnStepEncoder(1, 7);
    track.turnStepEncoder(2, 12);

    // Turn ROW encoder +1 -> Velocity lane (length 4)
    track.turnRowEncoder(1);
    assert(track.activeRow() == MatricealLaneType::Velocity);
    track.setLaneLoopLengthViaBarAndStep(4);
    track.turnStepEncoder(0, -20); // 100 - 20 = 80
    track.turnStepEncoder(1, 10);  // 100 + 10 = 110
    track.turnStepEncoder(2, -40); // 100 - 40 = 60
    track.turnStepEncoder(3, 27);  // 100 + 27 = 127

    // Enable all 5 gates on Gate lane
    for (uint8_t s = 0; s < 5; ++s) {
        track.pressStepButton(s);
    }

    // Verify independent polymetric cycling over 6 steps:
    // Pitch (len 3) cycles: 60, 67, 72, 60, 67, 72
    // Velocity (len 4) cycles: 80, 110, 60, 127, 80, 110
    // Gate (len 5) playhead cycles: 0, 1, 2, 3, 4, 0
    const std::array<uint8_t, 6> expectedNotes = {60, 67, 72, 60, 67, 72};
    const std::array<uint8_t, 6> expectedVel   = {80, 110, 60, 127, 80, 110};
    const std::array<uint8_t, 6> expectedGateHead = {0, 1, 2, 3, 4, 0};

    for (std::size_t i = 0; i < 6; ++i) {
        EvaluatedStepEvent ev = track.evaluateAndAdvance(0);
        assert(ev.triggered);
        assert(ev.midiNote == expectedNotes[i]);
        assert(ev.velocity == expectedVel[i]);
        assert(ev.lanePlayheads[static_cast<std::size_t>(MatricealLaneType::Gate)] == expectedGateHead[i]);
    }

    // Verify A:B Trig Condition (e.g. 2:2 -> triggers only on 2nd pass of Gate lane)
    track.resetToDefaults();
    track.setLaneLength(MatricealLaneType::Gate, 2);
    track.pressStepButton(0);
    track.pressStepButton(1);
    track.setTrigCondition(0, 2, 2); // Step 0 fires only on pass 2 of 2

    // Pass 1: Step 0 should NOT trigger, Step 1 should trigger
    EvaluatedStepEvent p1s0 = track.evaluateAndAdvance(0);
    EvaluatedStepEvent p1s1 = track.evaluateAndAdvance(0);
    assert(!p1s0.triggered);
    assert(p1s1.triggered);

    // Pass 2: Step 0 SHOULD trigger, Step 1 should trigger
    EvaluatedStepEvent p2s0 = track.evaluateAndAdvance(0);
    EvaluatedStepEvent p2s1 = track.evaluateAndAdvance(0);
    assert(p2s0.triggered);
    assert(p2s1.triggered);

    std::cout << "[PASS] Phase 1: MatricealTrackEngine (8 polymetric lanes, ROW/BAR/StepEnc, A:B trig conditions)\n";
}

void testMasterHarmonizerBus() {
    MasterHarmonizerBus bus;

    // Configure Track 0 as Master Chord Track in C Major (C4-E4-G4-B4)
    HarmonicContext ctx{};
    ctx.rootNote = 0; // C
    ctx.scaleMask12 = ScaleMasks::kIonianMajor;
    ctx.activeChordNotes = {60, 64, 67, 71, 0, 0};
    ctx.activeChordSize = 4;
    ctx.bassNote = 36; // C2
    bus.setHarmonicContext(ctx);

    // Track 1: ScaleQuantize (C#4 = 61 -> quantized to C4 = 60)
    bus.setTrackFollowMode(1, HarmonizerFollowMode::ScaleQuantize);
    assert(bus.harmonizeNote(1, 61) == 60);
    assert(bus.harmonizeNote(1, 62) == 62); // D4 is in C Major

    // Track 2: ChordToneLock (D4 = 62 -> snaps to C4=60 or E4=64; F4 = 65 -> snaps to E4=64)
    bus.setTrackFollowMode(2, HarmonizerFollowMode::ChordToneLock);
    assert(bus.harmonizeNote(2, 65) == 64);
    assert(bus.harmonizeNote(2, 68) == 67);

    // Track 3: BassRootFollow (locks bass note to root C or dominant G)
    bus.setTrackFollowMode(3, HarmonizerFollowMode::BassRootFollow);
    assert(bus.harmonizeNote(3, 38) == 36); // D2 -> locks to C2 (36)
    assert(bus.harmonizeNote(3, 43) == 43); // G2 (5th) -> preserves dominant 5th (43)

    // Track 4: VoiceLedPad (minimizes semitone movement when chord changes to F Major: F-A-C-E)
    bus.setTrackFollowMode(4, HarmonizerFollowMode::VoiceLedPad);
    bus.setTrackLastVoiceLedNote(4, 64); // Previously playing E4 (64)
    ctx.activeChordNotes = {65, 69, 72, 76, 0, 0}; // Fmaj7 (F, A, C, E)
    bus.setHarmonicContext(ctx);
    // Requesting chord voice 0 (F) from previous E4 (64) should move by just +1 semitone to F4 (65)
    const uint8_t voiceLed = bus.harmonizeNote(4, 60);
    assert(voiceLed == 65);

    // Test Master Chord Knobs A-D (Spread, Inversion, Modal Interchange, Strum Timing)
    bus.setMasterChord(60, {0, 4, 7, 11});
    bus.setMasterKnobsAD(96, 64, 110, 64);
    assert(bus.harmonicContext().voicingSpread == 3);
    assert(bus.harmonicContext().inversion == 2);
    assert(bus.harmonicContext().strumTimingTicks > 100);
    assert(bus.chordVoiceStrumDelayTicks(2) == 2U * bus.harmonicContext().strumTimingTicks);

    std::cout << "[PASS] Phase 1: MasterHarmonizerBus (Master Chord Track + 63 Follower Tracks + Knobs A-D)\n";
}

} // namespace

int main() {
    std::cout << "=== Behringer IGNITE (0716-AAF) Custom Firmware - Phase 1 Tests ===\n";
    testHighResClock960();
    testMatricealTrackEngine();
    testMasterHarmonizerBus();
    std::cout << "=== All Phase 1 Tests Passed Successfully ===\n";
    return 0;
}
