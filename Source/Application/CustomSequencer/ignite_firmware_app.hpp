/*
 * Copyright (c) 2026 Behringer IGNITE (BE 0716-AAF) Custom Firmware Contributors
 * Based on the open-source Behringer IGNITE (0716-AAF) architecture.
 *
 * This file is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef IGNITE_FIRMWARE_APP_HPP
#define IGNITE_FIRMWARE_APP_HPP

#include "../../AudIO/CustomEngines/ignite_internal_synth.hpp"
#include "ignite_algorithmic_engines.hpp"
#include "ignite_hardware_and_cv.hpp"
#include "ignite_sequencer_core.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace ignite {

constexpr std::size_t kNumMidiDinPorts = 5;    // DIN 1-5 (80 channels total)
constexpr std::size_t kNumUsbHostPorts = 5;    // USB Host 1-5 (80 channels total)

// ============================================================================
// Enhancement 9: Unified 64-Track I/O Router & Wireless Ableton Link
// (`UnifiedMIDICVRouter`)
// ============================================================================
struct TrackRoutingConfig {
    uint8_t midiDinPortMask{0x01};      // Bit 0..4 -> DIN Ports 1..5
    uint8_t midiDinChannel{1};          // 1..16
    uint8_t usbHostPortMask{0x00};      // Bit 0..4 -> USB Host Ports 1..5
    uint8_t usbHostChannel{1};          // 1..16
    bool sendToUsbCDevice{true};        // USB-C Device MIDI output
    uint8_t cvGatePairMask{0x00};       // Bit 0..7 -> CV/Gate Pairs 1..8 (supports polyphonic voice pool!)
    InternalSynthPatchType internalPatch{InternalSynthPatchType::Off};
    uint8_t fmAlgorithm{0};             // 0..7 when internalPatch == FM4Op
    bool sendToWiFiRtpMidi{false};      // Wireless RTP-MIDI over embedded Linux Wi-Fi
};

struct RoutedMidiPacket {
    enum class BusType : uint8_t {
        MidiDin = 0,
        UsbHost,
        UsbCDevice,
        WiFiRtpMidi
    };
    BusType bus{BusType::MidiDin};
    uint8_t portIndex{0};  // 0..4 for DIN/USB Host, 0 for USB-C / Wi-Fi
    uint8_t statusByte{0x90};
    uint8_t data1{60};
    uint8_t data2{100};
};

struct AbletonLinkSyncState {
    bool enabled{false};
    uint8_t connectedPeers{0};
    double networkBpm{120.0};
    double beatQuantum{4.0};
    double phaseBeats{0.0};
};

class UnifiedMIDICVRouter {
public:
    UnifiedMIDICVRouter() noexcept { reset(); }

    void reset() noexcept {
        for (std::size_t t = 0; t < kNumTracks; ++t) {
            auto& cfg = routes_[t];
            cfg = TrackRoutingConfig{};
            cfg.midiDinPortMask = static_cast<uint8_t>(1U << ((t / 16) % kNumMidiDinPorts));
            cfg.midiDinChannel = static_cast<uint8_t>((t % 16) + 1);
            cfg.usbHostChannel = static_cast<uint8_t>((t % 16) + 1);
        }
        cvPolyAllocatorCursor_ = 0;
        cvVoiceLastNote_.fill(0);
        outbox_.clear();
        linkState_ = AbletonLinkSyncState{};
    }

    TrackRoutingConfig& trackRoute(std::size_t trackIdx) noexcept {
        return routes_[trackIdx % kNumTracks];
    }
    const TrackRoutingConfig& trackRoute(std::size_t trackIdx) const noexcept {
        return routes_[trackIdx % kNumTracks];
    }

    // Configures a track for polyphonic CV/Gate allocation across a bitmask of CV/Gate jacks (e.g., 0x0F = CV/Gate 1-4)
    void setTrackPolyphonicCVPool(std::size_t trackIdx, uint8_t cvGateMask8Bit) noexcept {
        if (trackIdx < kNumTracks) {
            routes_[trackIdx].cvGatePairMask = cvGateMask8Bit;
        }
    }

    // Wireless Ableton Link tempo & phase synchronization
    void configureAbletonLink(bool enabled, double quantumBeats = 4.0, uint8_t peers = 1) noexcept {
        linkState_.enabled = enabled;
        linkState_.beatQuantum = std::clamp(quantumBeats, 1.0, 16.0);
        linkState_.connectedPeers = peers;
    }

    void receiveAbletonLinkNetworkBeat(double networkBpm, double networkPhaseBeats, HighResClock960& clock) noexcept {
        if (!linkState_.enabled) return;
        linkState_.networkBpm = std::clamp(networkBpm, 20.0, 300.0);
        linkState_.phaseBeats = std::fmod(std::max(0.0, networkPhaseBeats), linkState_.beatQuantum);
        clock.setBpm(static_cast<float>(linkState_.networkBpm));
    }

    const AbletonLinkSyncState& abletonLinkState() const noexcept { return linkState_; }

    // Routes an evaluated track step event to all assigned hardware outputs (DIN 1-5, USB Host 1-5, USB-C, CV/Gate 1-8, Internal Synth, Wi-Fi)
    void routeTrackEvent(std::size_t trackIdx,
                         const EvaluatedStepEvent& ev,
                         ModularCVMatrix8& cvMatrix,
                         IgniteInternalSynthEngine& internalSynth) {
        if (trackIdx >= kNumTracks || !ev.triggered) return;
        const auto& cfg = routes_[trackIdx];

        const uint8_t dinChan0 = static_cast<uint8_t>(std::clamp<uint8_t>(cfg.midiDinChannel, 1, 16) - 1);
        const uint8_t usbChan0 = static_cast<uint8_t>(std::clamp<uint8_t>(cfg.usbHostChannel, 1, 16) - 1);

        // 1. 5 x MIDI DIN Ports (DIN 1-5)
        for (uint8_t p = 0; p < kNumMidiDinPorts; ++p) {
            if ((cfg.midiDinPortMask & (1U << p)) != 0) {
                outbox_.push_back(RoutedMidiPacket{
                    RoutedMidiPacket::BusType::MidiDin,
                    p,
                    static_cast<uint8_t>(0x90U | dinChan0),
                    ev.midiNote,
                    ev.velocity
                });
            }
        }

        // 2. 5 x USB Host MIDI Ports (USB Host 1-5)
        for (uint8_t p = 0; p < kNumUsbHostPorts; ++p) {
            if ((cfg.usbHostPortMask & (1U << p)) != 0) {
                outbox_.push_back(RoutedMidiPacket{
                    RoutedMidiPacket::BusType::UsbHost,
                    p,
                    static_cast<uint8_t>(0x90U | usbChan0),
                    ev.midiNote,
                    ev.velocity
                });
            }
        }

        // 3. USB-C Device MIDI Port
        if (cfg.sendToUsbCDevice) {
            outbox_.push_back(RoutedMidiPacket{
                RoutedMidiPacket::BusType::UsbCDevice,
                0,
                static_cast<uint8_t>(0x90U | dinChan0),
                ev.midiNote,
                ev.velocity
            });
        }

        // 4. Wi-Fi RTP-MIDI
        if (cfg.sendToWiFiRtpMidi) {
            outbox_.push_back(RoutedMidiPacket{
                RoutedMidiPacket::BusType::WiFiRtpMidi,
                0,
                static_cast<uint8_t>(0x90U | dinChan0),
                ev.midiNote,
                ev.velocity
            });
        }

        // 5. 8 x CV/Gate Jacks (with polyphonic round-robin voice allocation across masked CV/Gate pairs)
        if (cfg.cvGatePairMask != 0) {
            const std::size_t allocatedCvCh = allocatePolyphonicCVChannel(cfg.cvGatePairMask);
            cvVoiceLastNote_[allocatedCvCh] = ev.midiNote;
            cvMatrix.setChannelNoteAndGate(allocatedCvCh, ev.midiNote, true);
        }

        // Notify CV Matrix envelopes watching this track
        cvMatrix.notifyTrackTrigger(trackIdx);

        // 6. Internal Synth Engine (4-Op FM / Granular / KICK Drum)
        if (cfg.internalPatch != InternalSynthPatchType::Off) {
            internalSynth.triggerPatch(cfg.internalPatch, ev.midiNote, ev.velocity, cfg.fmAlgorithm);
        }
    }

    const std::vector<RoutedMidiPacket>& midiOutbox() const noexcept { return outbox_; }
    void clearMidiOutbox() noexcept { outbox_.clear(); }

    uint8_t cvChannelLastAllocatedNote(std::size_t cvChIdx) const noexcept {
        return (cvChIdx < kNumCVChannels) ? cvVoiceLastNote_[cvChIdx] : 0;
    }

private:
    std::size_t allocatePolyphonicCVChannel(uint8_t mask) noexcept {
        for (std::size_t attempt = 0; attempt < kNumCVChannels; ++attempt) {
            const std::size_t candidate = (cvPolyAllocatorCursor_ + attempt) % kNumCVChannels;
            if ((mask & (1U << candidate)) != 0) {
                cvPolyAllocatorCursor_ = (candidate + 1) % kNumCVChannels;
                return candidate;
            }
        }
        return 0;
    }

    std::array<TrackRoutingConfig, kNumTracks> routes_{};
    std::size_t cvPolyAllocatorCursor_{0};
    std::array<uint8_t, kNumCVChannels> cvVoiceLastNote_{};
    std::vector<RoutedMidiPacket> outbox_{};
    AbletonLinkSyncState linkState_{};
};

// ============================================================================
// Top-Level Integrated 64-Track Behringer IGNITE Custom Firmware Application
// ============================================================================
class IgniteFirmwareApp {
public:
    IgniteFirmwareApp() noexcept { reset(); }

    void reset() noexcept {
        clock_.reset();
        dualProjects_.reset();
        cvMatrix_.reset();
        hardwareBridge_.reset();
        router_.reset();
        internalSynth_.reset();
        for (std::size_t t = 0; t < kNumTracks; ++t) {
            accumulators_[t].reset();
            stochastic_[t].reset();
            euclidean_[t].reset();
            auxRuntime_[t] = TrackAuxRuntimeState{};
            useEuclideanMode_[t] = false;
            useStochasticMode_[t] = false;
        }
        clock_.start();
    }

    void setTrackGeneratorModes(std::size_t trackIdx, bool euclideanOn, bool stochasticOn) noexcept {
        if (trackIdx < kNumTracks) {
            useEuclideanMode_[trackIdx] = euclideanOn;
            useStochasticMode_[trackIdx] = stochasticOn;
        }
    }

    // Executes one 16th-note sequencer step across all 64 tracks and routes MIDI, CV/Gate, and Internal Synth events
    void stepAll64TracksOnce() {
        auto& activeHarmonizer = (dualProjects_.transitionMorphPosition() < 0.5f)
                                     ? dualProjects_.projectA().harmonizer
                                     : dualProjects_.projectB().harmonizer;
        auto& activeTracks = (dualProjects_.transitionMorphPosition() < 0.5f)
                                 ? dualProjects_.projectA().tracks
                                 : dualProjects_.projectB().tracks;

        for (std::size_t t = 0; t < kNumTracks; ++t) {
            const std::size_t stepCursor = activeTracks[t].gateLane().playhead;
            TransitionStepResult trans = dualProjects_.evaluateTrackStep(t, rng_);
            EvaluatedStepEvent ev = trans.primaryEvent;

            // Optional Euclidean Generator override
            if (useEuclideanMode_[t]) {
                ev.triggered = euclidean_[t].shouldTriggerStep(static_cast<uint8_t>(stepCursor));
                if (euclidean_[t].isStepAccented(static_cast<uint8_t>(stepCursor))) {
                    ev.velocity = static_cast<uint8_t>(std::min<int>(127, static_cast<int>(ev.velocity) + 24));
                }
            }

            // Optional Stochastic & Turing-Loop Generator override
            if (useStochasticMode_[t]) {
                StochasticStepOutput stOut = stochastic_[t].nextStep(rng_);
                if (stOut.isRest) {
                    ev.triggered = false;
                } else {
                    ev.midiNote = stOut.midiNote;
                    ev.ratchetCount = stOut.ratchetCount;
                    ev.glideEnabled = stOut.glideEnabled;
                }
            }

            // Per-Step Accumulator & Inter-Track Aux Events
            accumulators_[t].evaluateStepAccumulator(stepCursor, ev);
            if (ev.triggered) {
                accumulators_[t].dispatchAuxEvent(stepCursor, activeTracks, auxRuntime_, rng_);
            }

            // Apply Master Harmonizer & Follower Bus quantization / voice-leading
            if (ev.triggered) {
                ev.midiNote = activeHarmonizer.harmonizeNote(t, ev.midiNote);
            }

            // Respect track mute state
            if (auxRuntime_[t].muted) {
                ev.triggered = false;
            }

            if (ev.triggered) {
                router_.routeTrackEvent(t, ev, cvMatrix_, internalSynth_);
            }
        }

        clock_.advanceTicks(kTicksPerSixteenth);
        cvMatrix_.processSampleAt48kHz(clock_.currentTick(), rng_);
    }

    HighResClock960& clock() noexcept { return clock_; }
    DualProjectTransitionEngine& dualProjects() noexcept { return dualProjects_; }
    ModularCVMatrix8& cvMatrix() noexcept { return cvMatrix_; }
    IgniteHardwareBridge& hardwareBridge() noexcept { return hardwareBridge_; }
    UnifiedMIDICVRouter& router() noexcept { return router_; }
    IgniteInternalSynthEngine& internalSynth() noexcept { return internalSynth_; }
    StepAccumulatorAndAuxEngine& trackAccumulator(std::size_t t) noexcept { return accumulators_[t % kNumTracks]; }
    StochasticTuringGenerator& trackStochastic(std::size_t t) noexcept { return stochastic_[t % kNumTracks]; }
    EuclideanOverrideGenerator& trackEuclidean(std::size_t t) noexcept { return euclidean_[t % kNumTracks]; }

private:
    XorShift32 rng_{0x16A17E01U};
    HighResClock960 clock_{};
    DualProjectTransitionEngine dualProjects_{};
    ModularCVMatrix8 cvMatrix_{};
    IgniteHardwareBridge hardwareBridge_{};
    UnifiedMIDICVRouter router_{};
    IgniteInternalSynthEngine internalSynth_{};
    std::array<StepAccumulatorAndAuxEngine, kNumTracks> accumulators_{};
    std::array<StochasticTuringGenerator, kNumTracks> stochastic_{};
    std::array<EuclideanOverrideGenerator, kNumTracks> euclidean_{};
    std::array<TrackAuxRuntimeState, kNumTracks> auxRuntime_{};
    std::array<bool, kNumTracks> useEuclideanMode_{};
    std::array<bool, kNumTracks> useStochasticMode_{};
};

} // namespace ignite

#endif // IGNITE_FIRMWARE_APP_HPP

