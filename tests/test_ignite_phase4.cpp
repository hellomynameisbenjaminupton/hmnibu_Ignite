/*
 * Copyright (c) 2026 Behringer IGNITE (BE 0716-AAF) Custom Firmware Contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 4 Unit Tests:
 *  - IgniteInternalSynthEngine (4-Op 8-Algorithm FM4Voice, GranularVoice, AnalogDrumVoice stereo DSP)
 *  - UnifiedMIDICVRouter (5x MIDI DIN, 5x USB Host, USB-C, 8x CV/Gate polyphonic voice allocation, Wi-Fi Ableton Link)
 *  - IgniteFirmwareApp (Full 64-track integrated custom firmware step & audio rendering pipeline)
 */

#include "../Source/Application/CustomSequencer/ignite_firmware_app.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>

using namespace ignite;

namespace {

void testInternalSynthEngine() {
    IgniteInternalSynthEngine synth;
    synth.setSampleRate(48000.0f);

    // Test all 8 FM algorithms produce non-zero, bounded audio output (-1.0 .. +1.0)
    for (uint8_t alg = 0; alg < 8; ++alg) {
        FM4Voice voice;
        voice.noteOn(60, 110, alg);
        float energy = 0.0f;
        for (int s = 0; s < 128; ++s) {
            const float sample = voice.renderSample(48000.0f);
            assert(sample >= -1.0f && sample <= 1.0f);
            energy += std::abs(sample);
        }
        assert(energy > 0.1f);
    }

    // Test GranularVoice and full stereo block rendering
    synth.triggerPatch(InternalSynthPatchType::FM4Op, 64, 100, 3);
    synth.triggerPatch(InternalSynthPatchType::GranularCloud, 67, 110, 0);
    synth.triggerPatch(InternalSynthPatchType::KickAnalogDrum, 36, 127, 0);

    std::vector<float> left(256, 0.0f);
    std::vector<float> right(256, 0.0f);
    synth.renderStereoBlock(left.data(), right.data(), 256);

    float stereoEnergy = 0.0f;
    for (std::size_t i = 0; i < 256; ++i) {
        assert(left[i] >= -1.0f && left[i] <= 1.0f);
        assert(right[i] >= -1.0f && right[i] <= 1.0f);
        stereoEnergy += std::abs(left[i]) + std::abs(right[i]);
    }
    assert(stereoEnergy > 1.0f);

    std::cout << "[PASS] Phase 4: IgniteInternalSynthEngine (4-Op 8-Alg FM + Granular + KICK Drum stereo DSP)\n";
}

void testUnifiedMIDICVRouterAndPolyCV() {
    UnifiedMIDICVRouter router;
    ModularCVMatrix8 cvMatrix;
    IgniteInternalSynthEngine synth;
    HighResClock960 clock;

    // Route Track 0 to DIN Ports 1 & 3 (mask 0b00101 = 0x05), USB Host Port 2 (mask 0x02),
    // Wi-Fi RTP-MIDI, and a 4-voice polyphonic Eurorack CV/Gate pool on CV/Gate 1-4 (mask 0x0F)
    auto& route0 = router.trackRoute(0);
    route0.midiDinPortMask = 0x05; // DIN 1 & DIN 3
    route0.midiDinChannel = 1;
    route0.usbHostPortMask = 0x02; // USB Host 2
    route0.usbHostChannel = 2;
    route0.sendToUsbCDevice = true;
    route0.sendToWiFiRtpMidi = true;
    route0.internalPatch = InternalSynthPatchType::FM4Op;
    router.setTrackPolyphonicCVPool(0, 0x0F); // CV/Gate 1..4 polyphonic round-robin pool

    // Trigger 4 chord notes sequentially on Track 0 and verify polyphonic CV/Gate 1..4 allocation
    const std::array<uint8_t, 4> chordNotes = {60, 64, 67, 71};
    for (std::size_t i = 0; i < 4; ++i) {
        EvaluatedStepEvent ev{};
        ev.triggered = true;
        ev.midiNote = chordNotes[i];
        ev.velocity = 100;
        router.routeTrackEvent(0, ev, cvMatrix, synth);
    }

    for (std::size_t ch = 0; ch < 4; ++ch) {
        assert(router.cvChannelLastAllocatedNote(ch) == chordNotes[ch]);
        assert(cvMatrix.channelGate(ch));
    }

    // Each note emitted: 2 DIN packets + 1 USB Host packet + 1 USB-C packet + 1 Wi-Fi packet = 5 packets * 4 notes = 20 packets
    assert(router.midiOutbox().size() == 20);

    // Test Wireless Ableton Link tempo & phase sync
    router.configureAbletonLink(true, 4.0, 3);
    router.receiveAbletonLinkNetworkBeat(132.5, 2.75, clock);
    assert(std::abs(clock.bpm() - 132.5f) < 1e-4f);
    assert(std::abs(router.abletonLinkState().phaseBeats - 2.75) < 1e-6);

    std::cout << "[PASS] Phase 4: UnifiedMIDICVRouter (5x DIN, 5x USB Host, 8x Poly CV/Gate, Ableton Link sync)\n";
}

void testFull64TrackIgniteFirmwareApp() {
    IgniteFirmwareApp app;

    // Configure Track 0 as Euclidean FM4Op synth track and Track 1 as Follower ChordToneLock track
    app.setTrackGeneratorModes(0, true, false);
    app.trackEuclidean(0).setEuclideanParams(16, 16, 0, 4, 0);
    app.router().trackRoute(0).internalPatch = InternalSynthPatchType::FM4Op;

    app.dualProjects().projectA().tracks[1].pressStepButton(0);
    app.dualProjects().projectA().tracks[1].setBaseMidiNote(65); // F4
    app.dualProjects().projectA().harmonizer.setTrackFollowMode(1, HarmonizerFollowMode::ChordToneLock);

    app.stepAll64TracksOnce();
    assert(!app.router().midiOutbox().empty());

    std::vector<float> outL(128, 0.0f);
    std::vector<float> outR(128, 0.0f);
    app.internalSynth().renderStereoBlock(outL.data(), outR.data(), 128);

    float totalOut = 0.0f;
    for (float s : outL) totalOut += std::abs(s);
    assert(totalOut > 0.05f);

    std::cout << "[PASS] Phase 4: IgniteFirmwareApp (64-Track integrated sequencer + harmonizer + synth pipeline)\n";
}

} // namespace

int main() {
    std::cout << "=== Behringer IGNITE (0716-AAF) Custom Firmware - Phase 4 Tests ===\n";
    testInternalSynthEngine();
    testUnifiedMIDICVRouterAndPolyCV();
    testFull64TrackIgniteFirmwareApp();
    std::cout << "=== All Phase 4 Tests Passed Successfully ===\n";
    return 0;
}
