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

#ifndef IGNITE_HARDWARE_AND_CV_HPP
#define IGNITE_HARDWARE_AND_CV_HPP

#include "ignite_algorithmic_engines.hpp"
#include "ignite_sequencer_core.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ignite {

constexpr std::size_t kNumCVChannels = 8;
constexpr std::size_t kNumMacroKnobs = 8;     // Knobs A-H
constexpr std::size_t kNumCalibPoints10 = 10; // 0V..9V 10-point 1V/Oct calibration table
constexpr float kPi = 3.14159265358979323846f;

// ============================================================================
// Enhancement 6: Dual-Project Live Transition Engine (`DualProjectTransitionEngine`)
// ============================================================================
enum class TrackGroup : uint8_t {
    Drums = 0,
    Bass = 1,
    Leads = 2,
    ModularCV = 3
};

enum class TransitionBehavior : uint8_t {
    InstantBarSwitch = 0, // Cuts cleanly on the next bar boundary once morph >= 0.5
    VelocityAndCCFade,    // Crossfades outgoing/incoming velocities & 14-bit CV/CC
    PatternInterleave     // Probabilistically interleaves steps between Project A & B
};

struct IgniteProjectState {
    float bpm{120.0f};
    float swingPercent{50.0f};
    std::array<MatricealTrackEngine, kNumTracks> tracks{};
    std::array<TrackGroup, kNumTracks> trackGroups{};
    MasterHarmonizerBus harmonizer{};

    IgniteProjectState() noexcept {
        for (std::size_t i = 0; i < kNumTracks; ++i) {
            if (i < 16) trackGroups[i] = TrackGroup::Drums;
            else if (i < 32) trackGroups[i] = TrackGroup::Bass;
            else if (i < 48) trackGroups[i] = TrackGroup::Leads;
            else trackGroups[i] = TrackGroup::ModularCV;
        }
    }
};

struct TransitionStepResult {
    EvaluatedStepEvent primaryEvent{};
    EvaluatedStepEvent secondaryFadeEvent{}; // Populated in VelocityAndCCFade when both A & B sound
    bool hasSecondaryFadeEvent{false};
    bool fromProjectB{false};
};

class DualProjectTransitionEngine {
public:
    DualProjectTransitionEngine() noexcept { reset(); }

    void reset() noexcept {
        projectA_ = IgniteProjectState{};
        projectB_ = IgniteProjectState{};
        projectA_.bpm = 120.0f;
        projectB_.bpm = 128.0f;
        morphPosition_ = 0.0f;
        groupBehaviors_ = {
            TransitionBehavior::InstantBarSwitch,  // Drums
            TransitionBehavior::PatternInterleave, // Bass
            TransitionBehavior::VelocityAndCCFade, // Leads
            TransitionBehavior::VelocityAndCCFade  // ModularCV
        };
        barSwitchedToB_.fill(false);
    }

    IgniteProjectState& projectA() noexcept { return projectA_; }
    const IgniteProjectState& projectA() const noexcept { return projectA_; }
    IgniteProjectState& projectB() noexcept { return projectB_; }
    const IgniteProjectState& projectB() const noexcept { return projectB_; }

    void setTransitionMorphPosition(float pos0To1) noexcept {
        morphPosition_ = std::clamp(pos0To1, 0.0f, 1.0f);
    }
    float transitionMorphPosition() const noexcept { return morphPosition_; }

    void setGroupTransitionBehavior(TrackGroup group, TransitionBehavior behavior) noexcept {
        const std::size_t idx = static_cast<std::size_t>(group) & 0x03U;
        groupBehaviors_[idx] = behavior;
    }

    TransitionBehavior groupTransitionBehavior(TrackGroup group) const noexcept {
        const std::size_t idx = static_cast<std::size_t>(group) & 0x03U;
        return groupBehaviors_[idx];
    }

    // Smoothly interpolates master tempo between Project A and Project B
    float interpolatedBpm() const noexcept {
        return projectA_.bpm + morphPosition_ * (projectB_.bpm - projectA_.bpm);
    }

    float interpolatedSwing() const noexcept {
        return projectA_.swingPercent + morphPosition_ * (projectB_.swingPercent - projectA_.swingPercent);
    }

    // Called at each bar boundary to commit pending InstantBarSwitch transitions
    void onBarBoundary() noexcept {
        const bool targetB = (morphPosition_ >= 0.5f);
        barSwitchedToB_.fill(targetB);
    }

    // Evaluates a single track across Project A and Project B according to its group's TransitionBehavior
    TransitionStepResult evaluateTrackStep(std::size_t trackIdx, XorShift32& rng) noexcept {
        TransitionStepResult res{};
        if (trackIdx >= kNumTracks) return res;

        const uint8_t rollA = static_cast<uint8_t>(rng.nextBounded(100));
        const uint8_t rollB = static_cast<uint8_t>(rng.nextBounded(100));
        EvaluatedStepEvent evA = projectA_.tracks[trackIdx].evaluateAndAdvance(rollA);
        EvaluatedStepEvent evB = projectB_.tracks[trackIdx].evaluateAndAdvance(rollB);

        const TrackGroup group = projectA_.trackGroups[trackIdx];
        const TransitionBehavior behavior = groupBehaviors_[static_cast<std::size_t>(group) & 0x03U];

        switch (behavior) {
            case TransitionBehavior::InstantBarSwitch: {
                const bool useB = barSwitchedToB_[static_cast<std::size_t>(group) & 0x03U];
                res.primaryEvent = useB ? evB : evA;
                res.fromProjectB = useB;
                break;
            }

            case TransitionBehavior::VelocityAndCCFade: {
                const float gainA = 1.0f - morphPosition_;
                const float gainB = morphPosition_;

                evA.velocity = static_cast<uint8_t>(std::clamp<int>(
                    static_cast<int>(std::lround(static_cast<float>(evA.velocity) * gainA)), 0, 127));
                evB.velocity = static_cast<uint8_t>(std::clamp<int>(
                    static_cast<int>(std::lround(static_cast<float>(evB.velocity) * gainB)), 0, 127));

                if (evA.velocity == 0) evA.triggered = false;
                if (evB.velocity == 0) evB.triggered = false;

                // Interpolate 14-bit CV/CC seamlessly
                const float blendedCV = static_cast<float>(evA.modCV14Bit) * gainA +
                                        static_cast<float>(evB.modCV14Bit) * gainB;
                const uint16_t cv14 = static_cast<uint16_t>(std::clamp<int>(static_cast<int>(std::lround(blendedCV)), 0, 16383));
                evA.modCV14Bit = cv14;
                evA.modCC7Bit = static_cast<uint8_t>(cv14 >> 7);
                evB.modCV14Bit = cv14;
                evB.modCC7Bit = static_cast<uint8_t>(cv14 >> 7);

                if (morphPosition_ < 0.5f) {
                    res.primaryEvent = evA;
                    res.secondaryFadeEvent = evB;
                    res.hasSecondaryFadeEvent = evB.triggered;
                    res.fromProjectB = false;
                } else {
                    res.primaryEvent = evB;
                    res.secondaryFadeEvent = evA;
                    res.hasSecondaryFadeEvent = evA.triggered;
                    res.fromProjectB = true;
                }
                break;
            }

            case TransitionBehavior::PatternInterleave: {
                const bool chooseB = (morphPosition_ >= 1.0f) ||
                                     (morphPosition_ > 0.0f && rng.nextFloat01() < morphPosition_);
                res.primaryEvent = chooseB ? evB : evA;
                res.fromProjectB = chooseB;
                break;
            }
        }
        return res;
    }

private:
    IgniteProjectState projectA_{};
    IgniteProjectState projectB_{};
    float morphPosition_{0.0f};
    std::array<TransitionBehavior, 4> groupBehaviors_{};
    std::array<bool, 4> barSwitchedToB_{};
};

// ============================================================================
// Enhancement 7: 8-Channel 48 kHz Modular CV Modulation Matrix (`ModularCVMatrix8`)
// ============================================================================
enum class CVChannelMode : uint8_t {
    Pitch1VPerOct = 0, // Calibrated 10-point 1V/Oct lookup table
    HzPerVolt,         // Vintage Korg/Yamaha Hz/V exponential pitch scaling
    SyncLFOWave,       // Phase-locked to 960 PPQN clock
    LoopingADEnvelope, // Triggered by any of the 64 tracks
    SmoothRandomWalk,  // Slewed sample-and-hold noise
    AICameraOpticalCV  // VL53L5CX Time-of-Flight Z-distance & X-tilt gesture CV
};

enum class LFOWaveShape : uint8_t {
    Sine = 0,
    Triangle,
    Saw,
    Ramp,
    Square,
    Exponential
};

struct CVChannelConfig {
    CVChannelMode mode{CVChannelMode::Pitch1VPerOct};
    LFOWaveShape lfoShape{LFOWaveShape::Sine};
    uint32_t lfoPeriodTicks960{960}; // Default 1 beat (quarter note = 960 ticks)
    float attackMs{15.0f};
    float decayMs{250.0f};
    bool loopEnvelope{false};
    uint8_t triggerSourceTrack{0};   // 0..63
    bool bipolarOutput{false};       // false = 0..10V, true = -5V..+5V
    bool cameraUseXTilt{false};      // false = Z-distance, true = X-tilt
    // 10-point hardware calibration table (DAC 16-bit code for 0V, 1V, 2V, ..., 9V)
    std::array<uint16_t, kNumCalibPoints10> calibration10Point{};
};

struct VL53L5CXSensorFrame {
    uint16_t zDistanceMm{250}; // Valid range 20mm .. 1000mm
    float xTiltDegrees{0.0f};  // Valid range -45.0 .. +45.0 deg
    bool targetValid{true};
};

class ModularCVMatrix8 {
public:
    ModularCVMatrix8() noexcept { reset(); }

    void reset() noexcept {
        sampleRate_ = 48000.0f;
        sensor_ = VL53L5CXSensorFrame{};
        for (std::size_t ch = 0; ch < kNumCVChannels; ++ch) {
            auto& cfg = configs_[ch];
            cfg = CVChannelConfig{};
            // Populate default linear 10-point calibration table (0V..9V mapped across 0..58981 DAC codes,
            // where full scale 10.0V = 65535 -> 6553.5 DAC LSBs per Volt)
            for (std::size_t pt = 0; pt < kNumCalibPoints10; ++pt) {
                cfg.calibration10Point[pt] = static_cast<uint16_t>(std::lround(static_cast<double>(pt) * 6553.5));
            }
            currentNotes_[ch] = 60;
            gateOutputs_[ch] = false;
            envStage_[ch] = 0; // 0=idle, 1=attack, 2=decay
            envLevel_[ch] = 0.0f;
            randomTarget_[ch] = 0.5f;
            randomSlewed_[ch] = 0.5f;
            randomCounter_[ch] = 0;
            cvVolts_[ch] = 0.0f;
            dacCodes_[ch] = 0;
        }
    }

    void setSampleRate(float sr) noexcept {
        sampleRate_ = std::clamp(sr, 8000.0f, 192000.0f);
    }

    CVChannelConfig& channelConfig(std::size_t chIdx) noexcept {
        return configs_[chIdx % kNumCVChannels];
    }
    const CVChannelConfig& channelConfig(std::size_t chIdx) const noexcept {
        return configs_[chIdx % kNumCVChannels];
    }

    void setCalibrationPoint(std::size_t chIdx, std::size_t octave0To9, uint16_t dacCode) noexcept {
        if (chIdx < kNumCVChannels && octave0To9 < kNumCalibPoints10) {
            configs_[chIdx].calibration10Point[octave0To9] = dacCode;
        }
    }

    void setChannelNoteAndGate(std::size_t chIdx, uint8_t midiNote, bool gateHigh) noexcept {
        if (chIdx >= kNumCVChannels) return;
        currentNotes_[chIdx] = std::min<uint8_t>(midiNote, 120);
        gateOutputs_[chIdx] = gateHigh;
        if (gateHigh) {
            envStage_[chIdx] = 1; // Trigger Attack stage
        }
    }

    void notifyTrackTrigger(std::size_t trackIdx) noexcept {
        for (std::size_t ch = 0; ch < kNumCVChannels; ++ch) {
            if (configs_[ch].mode == CVChannelMode::LoopingADEnvelope &&
                configs_[ch].triggerSourceTrack == trackIdx) {
                envStage_[ch] = 1;
            }
        }
    }

    void updateAICameraSensor(uint16_t zDistanceMm, float xTiltDegrees, bool valid = true) noexcept {
        sensor_.zDistanceMm = std::clamp<uint16_t>(zDistanceMm, 20, 1000);
        sensor_.xTiltDegrees = std::clamp(xTiltDegrees, -45.0f, 45.0f);
        sensor_.targetValid = valid;
    }

    const VL53L5CXSensorFrame& aiCameraSensor() const noexcept { return sensor_; }

    // Renders one 48 kHz audio-rate modulation sample across all 8 CV DACs
    void processSampleAt48kHz(uint64_t currentTick960, XorShift32& rng) noexcept {
        for (std::size_t ch = 0; ch < kNumCVChannels; ++ch) {
            const auto& cfg = configs_[ch];
            switch (cfg.mode) {
                case CVChannelMode::Pitch1VPerOct: {
                    const float volts = std::clamp(static_cast<float>(currentNotes_[ch]) / 12.0f, 0.0f, 9.0f);
                    cvVolts_[ch] = volts;
                    dacCodes_[ch] = lookupCalibrated1VPerOctDAC(ch, volts);
                    break;
                }

                case CVChannelMode::HzPerVolt: {
                    // Korg/Yamaha Hz/V: 1.0V at C2 (MIDI 36), doubles every octave (+12 semitones)
                    const float octavesFromC2 = (static_cast<float>(currentNotes_[ch]) - 36.0f) / 12.0f;
                    const float volts = std::clamp(std::pow(2.0f, octavesFromC2), 0.0f, 10.0f);
                    cvVolts_[ch] = volts;
                    dacCodes_[ch] = voltsToDAC16(volts, false);
                    break;
                }

                case CVChannelMode::SyncLFOWave: {
                    const uint32_t period = std::max<uint32_t>(1, cfg.lfoPeriodTicks960);
                    const float phase01 = static_cast<float>(currentTick960 % period) / static_cast<float>(period);
                    const float norm01 = evaluateLFOWave01(cfg.lfoShape, phase01);
                    cvVolts_[ch] = cfg.bipolarOutput ? (norm01 * 10.0f - 5.0f) : (norm01 * 10.0f);
                    dacCodes_[ch] = voltsToDAC16(cvVolts_[ch], cfg.bipolarOutput);
                    break;
                }

                case CVChannelMode::LoopingADEnvelope: {
                    const float norm01 = advanceEnvelopeSample(ch);
                    cvVolts_[ch] = cfg.bipolarOutput ? (norm01 * 10.0f - 5.0f) : (norm01 * 10.0f);
                    dacCodes_[ch] = voltsToDAC16(cvVolts_[ch], cfg.bipolarOutput);
                    break;
                }

                case CVChannelMode::SmoothRandomWalk: {
                    ++randomCounter_[ch];
                    if (randomCounter_[ch] >= 480) { // New random target every 10ms at 48kHz
                        randomCounter_[ch] = 0;
                        randomTarget_[ch] = rng.nextFloat01();
                    }
                    // One-pole exponential slew limiter
                    randomSlewed_[ch] += 0.005f * (randomTarget_[ch] - randomSlewed_[ch]);
                    const float norm01 = std::clamp(randomSlewed_[ch], 0.0f, 1.0f);
                    cvVolts_[ch] = cfg.bipolarOutput ? (norm01 * 10.0f - 5.0f) : (norm01 * 10.0f);
                    dacCodes_[ch] = voltsToDAC16(cvVolts_[ch], cfg.bipolarOutput);
                    break;
                }

                case CVChannelMode::AICameraOpticalCV: {
                    float norm01 = 0.0f;
                    if (sensor_.targetValid) {
                        if (cfg.cameraUseXTilt) {
                            norm01 = (sensor_.xTiltDegrees + 45.0f) / 90.0f;
                        } else {
                            // Closer hand (20mm) = 1.0 (max modulation), 1000mm = 0.0
                            norm01 = 1.0f - (static_cast<float>(sensor_.zDistanceMm - 20) / 980.0f);
                        }
                    }
                    norm01 = std::clamp(norm01, 0.0f, 1.0f);
                    cvVolts_[ch] = cfg.bipolarOutput ? (norm01 * 10.0f - 5.0f) : (norm01 * 10.0f);
                    dacCodes_[ch] = voltsToDAC16(cvVolts_[ch], cfg.bipolarOutput);
                    break;
                }
            }
        }
    }

    float channelVoltage(std::size_t chIdx) const noexcept {
        return (chIdx < kNumCVChannels) ? cvVolts_[chIdx] : 0.0f;
    }

    uint16_t channelDACCode(std::size_t chIdx) const noexcept {
        return (chIdx < kNumCVChannels) ? dacCodes_[chIdx] : 0;
    }

    bool channelGate(std::size_t chIdx) const noexcept {
        return (chIdx < kNumCVChannels) ? gateOutputs_[chIdx] : false;
    }

    // Interpolates the 10-point hardware calibration table for a given 1V/Oct voltage (0.0V .. 9.0V)
    uint16_t lookupCalibrated1VPerOctDAC(std::size_t chIdx, float volts0To9) const noexcept {
        if (chIdx >= kNumCVChannels) return 0;
        const auto& table = configs_[chIdx].calibration10Point;
        const float clampedV = std::clamp(volts0To9, 0.0f, 9.0f);
        const std::size_t lowOct = static_cast<std::size_t>(std::floor(clampedV));
        const std::size_t highOct = std::min<std::size_t>(lowOct + 1, kNumCalibPoints10 - 1);
        const float frac = clampedV - static_cast<float>(lowOct);
        const float code = static_cast<float>(table[lowOct]) +
                           frac * (static_cast<float>(table[highOct]) - static_cast<float>(table[lowOct]));
        return static_cast<uint16_t>(std::clamp<int>(static_cast<int>(std::lround(code)), 0, 65535));
    }

private:
    static float evaluateLFOWave01(LFOWaveShape shape, float phase01) noexcept {
        const float p = std::clamp(phase01, 0.0f, 1.0f);
        switch (shape) {
            case LFOWaveShape::Sine:
                return 0.5f + 0.5f * std::sin(2.0f * kPi * p);
            case LFOWaveShape::Triangle:
                return (p < 0.5f) ? (p * 2.0f) : (2.0f - p * 2.0f);
            case LFOWaveShape::Saw:
                return 1.0f - p;
            case LFOWaveShape::Ramp:
                return p;
            case LFOWaveShape::Square:
                return (p < 0.5f) ? 1.0f : 0.0f;
            case LFOWaveShape::Exponential:
                return p * p;
        }
        return 0.0f;
    }

    float advanceEnvelopeSample(std::size_t ch) noexcept {
        const auto& cfg = configs_[ch];
        if (envStage_[ch] == 0) {
            if (cfg.loopEnvelope) envStage_[ch] = 1;
            else return 0.0f;
        }

        if (envStage_[ch] == 1) {
            const float attackSamples = std::max(1.0f, (cfg.attackMs * 0.001f) * sampleRate_);
            envLevel_[ch] += 1.0f / attackSamples;
            if (envLevel_[ch] >= 1.0f) {
                envLevel_[ch] = 1.0f;
                envStage_[ch] = 2;
            }
        } else if (envStage_[ch] == 2) {
            const float decaySamples = std::max(1.0f, (cfg.decayMs * 0.001f) * sampleRate_);
            envLevel_[ch] -= 1.0f / decaySamples;
            if (envLevel_[ch] <= 0.0f) {
                envLevel_[ch] = 0.0f;
                envStage_[ch] = cfg.loopEnvelope ? 1 : 0;
            }
        }
        return envLevel_[ch];
    }

    static uint16_t voltsToDAC16(float volts, bool bipolar) noexcept {
        const float norm01 = bipolar ? ((volts + 5.0f) / 10.0f) : (volts / 10.0f);
        const float clamped = std::clamp(norm01, 0.0f, 1.0f);
        return static_cast<uint16_t>(std::lround(clamped * 65535.0f));
    }

    float sampleRate_{48000.0f};
    VL53L5CXSensorFrame sensor_{};
    std::array<CVChannelConfig, kNumCVChannels> configs_{};
    std::array<uint8_t, kNumCVChannels> currentNotes_{};
    std::array<bool, kNumCVChannels> gateOutputs_{};
    std::array<uint8_t, kNumCVChannels> envStage_{};
    std::array<float, kNumCVChannels> envLevel_{};
    std::array<float, kNumCVChannels> randomTarget_{};
    std::array<float, kNumCVChannels> randomSlewed_{};
    std::array<uint32_t, kNumCVChannels> randomCounter_{};
    std::array<float, kNumCVChannels> cvVolts_{};
    std::array<uint16_t, kNumCVChannels> dacCodes_{};
};

// ============================================================================
// Hardware Bridge (`IgniteHardwareBridge`) — ComProtocol PktMgr Framing & Surface
// ============================================================================
enum class IgnitePktType : uint8_t {
    System = 0,
    Button = 1,
    Led    = 2,
    Pad    = 3,
    Knobs  = 4,
    Midi   = 5,
    CV     = 6,
    Slider = 7
};

enum class IgniteControlMode : uint8_t {
    MatricealLanes = 0, // StepEnc 1-16 edit active ROW lane; Knobs A-D edit Master Chord
    EuclideanMode,      // Knobs A-D edit Euclidean N/K/Rot/Acc; Pads 1-16 toggle ForcedOn/ForcedMute
    StochasticTuring,   // StepEnc 1-12 edit Note Weights, 13-16 Articulations, Knob E/F Turing Lock/Len
    AccumulatorMode     // StepEnc 1-16 edit per-step pitch/velocity accumulators
};

class IgniteHardwareBridge {
public:
    IgniteHardwareBridge() noexcept { reset(); }

    void reset() noexcept {
        controlMode_ = IgniteControlMode::MatricealLanes;
        activeTrack_ = 0;
        barButtonHeld_ = false;
        macroKnobs_.fill(64);
        knobLedRings_.fill(0x007FU);
        padColors_.fill(RGBColor{0, 0, 0});
    }

    void setControlMode(IgniteControlMode mode) noexcept { controlMode_ = mode; }
    IgniteControlMode controlMode() const noexcept { return controlMode_; }

    void setActiveTrack(std::size_t trackIdx0To63) noexcept {
        activeTrack_ = std::min<std::size_t>(trackIdx0To63, kNumTracks - 1);
    }
    std::size_t activeTrack() const noexcept { return activeTrack_; }

    void setBarButtonHeld(bool held) noexcept { barButtonHeld_ = held; }
    bool isBarButtonHeld() const noexcept { return barButtonHeld_; }

    // --- Physical Control Handlers (Zero Menu Diving) ---

    // 1. 16 Dedicated Per-Step Rotary Encoders (StepEnc 1-16, index 0..15)
    void onStepEncoderTurned(uint8_t encIdx0To15, int delta,
                             MatricealTrackEngine& track,
                             StochasticTuringGenerator& stochastic,
                             StepAccumulatorAndAuxEngine& accumulator) noexcept {
        if (encIdx0To15 >= kStepsPerBar) return;
        switch (controlMode_) {
            case IgniteControlMode::MatricealLanes:
            case IgniteControlMode::EuclideanMode:
                track.turnStepEncoder(encIdx0To15, delta);
                break;
            case IgniteControlMode::StochasticTuring:
                stochastic.turnStepEncoderWeight(encIdx0To15, delta);
                break;
            case IgniteControlMode::AccumulatorMode:
                accumulator.turnStepEncoderPitchDelta(track.activeBarPage(), encIdx0To15, delta);
                break;
        }
    }

    // 2. 16 RGB Step Buttons (Steps 1-16, index 0..15)
    void onStepPadPressed(uint8_t padIdx0To15,
                          MatricealTrackEngine& track,
                          EuclideanOverrideGenerator& euclidean) noexcept {
        if (padIdx0To15 >= kStepsPerBar) return;
        if (barButtonHeld_) {
            // Hold BAR + Step Button N sets the active lane's independent loop length!
            track.setLaneLoopLengthViaBarAndStep(static_cast<uint8_t>(padIdx0To15 + 1));
            refreshPadLeds(track, euclidean);
            return;
        }

        if (controlMode_ == IgniteControlMode::EuclideanMode) {
            euclidean.pressStepPadOverride(padIdx0To15);
        } else {
            track.pressStepButton(padIdx0To15);
        }
        refreshPadLeds(track, euclidean);
    }

    // 3. Top-Right 8 Assignable LED-Ring Macro Knobs (Knobs A-H, index 0..7)
    void onMacroKnobTurned(uint8_t knobIdx0To7, int delta,
                           MasterHarmonizerBus& harmonizer,
                           EuclideanOverrideGenerator& euclidean,
                           StochasticTuringGenerator& stochastic,
                           DualProjectTransitionEngine& dualProj) noexcept {
        if (knobIdx0To7 >= kNumMacroKnobs) return;
        const int nextVal = std::clamp(static_cast<int>(macroKnobs_[knobIdx0To7]) + delta, 0, 127);
        macroKnobs_[knobIdx0To7] = static_cast<uint8_t>(nextVal);

        // Update 15-LED collar ring bitmask (proportional fill 0..15 LEDs)
        const uint8_t numLit = static_cast<uint8_t>((nextVal * 15) / 127);
        knobLedRings_[knobIdx0To7] = (numLit == 0) ? 0U : static_cast<uint16_t>((1U << numLit) - 1U);

        if (controlMode_ == IgniteControlMode::EuclideanMode && knobIdx0To7 < 4) {
            const uint8_t stepsN = static_cast<uint8_t>(1 + (macroKnobs_[0] * 15) / 127);
            const uint8_t pulsesK = static_cast<uint8_t>((macroKnobs_[1] * stepsN) / 127);
            const uint8_t rot = static_cast<uint8_t>((macroKnobs_[2] * (stepsN - 1)) / 127);
            const uint8_t accK = static_cast<uint8_t>((macroKnobs_[3] * stepsN) / 127);
            euclidean.setEuclideanParams(stepsN, pulsesK, rot, accK, 0);
        } else if (knobIdx0To7 < 4) {
            harmonizer.setMasterKnobsAD(macroKnobs_[0], macroKnobs_[1], macroKnobs_[2], macroKnobs_[3]);
        }

        if (knobIdx0To7 == 4) { // Knob E: Turing Lock (0..100%)
            stochastic.setKnobE_TuringLock((static_cast<float>(macroKnobs_[4]) / 127.0f) * 100.0f);
        } else if (knobIdx0To7 == 5) { // Knob F: Turing Loop Length (1..16)
            const uint8_t loopLen = static_cast<uint8_t>(1 + (macroKnobs_[5] * 15) / 127);
            stochastic.setKnobF_LoopLength(loopLen);
        } else if (knobIdx0To7 == 7) { // Knob H: Dual-Project Live Transition Crossfader (0.0 .. 1.0)
            dualProj.setTransitionMorphPosition(static_cast<float>(macroKnobs_[7]) / 127.0f);
        }
    }

    // 4. Dedicated Navigation Encoders (ROW and BAR)
    void onRowEncoderTurned(int delta, MatricealTrackEngine& track) noexcept {
        track.turnRowEncoder(delta);
    }

    void onBarEncoderTurned(int delta, MatricealTrackEngine& track) noexcept {
        track.turnBarEncoder(delta);
    }

    void refreshPadLeds(const MatricealTrackEngine& track,
                        const EuclideanOverrideGenerator& euclidean) noexcept {
        for (uint8_t i = 0; i < kStepsPerBar; ++i) {
            if (controlMode_ == IgniteControlMode::EuclideanMode) {
                padColors_[i] = euclidean.padLedColor(i);
            } else {
                const std::size_t stepIdx = static_cast<std::size_t>(track.activeBarPage()) * kStepsPerBar + i;
                const bool gateOn = (track.gateLane().values[stepIdx] != 0);
                padColors_[i] = gateOn ? RGBColor{0, 220, 120} : RGBColor{10, 10, 15};
            }
        }
    }

    uint8_t macroKnobValue(std::size_t knobIdx0To7) const noexcept {
        return (knobIdx0To7 < kNumMacroKnobs) ? macroKnobs_[knobIdx0To7] : 0;
    }

    uint16_t macroKnobLedRing(std::size_t knobIdx0To7) const noexcept {
        return (knobIdx0To7 < kNumMacroKnobs) ? knobLedRings_[knobIdx0To7] : 0;
    }

    RGBColor padLedColor(std::size_t padIdx0To15) const noexcept {
        return (padIdx0To15 < kStepsPerBar) ? padColors_[padIdx0To15] : RGBColor{0, 0, 0};
    }

    // --- ComProtocol (`CPktMgr`) Compatible Packet Framing & CRC-8 (Poly 0x31, Init 0xFF) ---
    static uint8_t computeComProtocolCrc8(const std::vector<uint8_t>& frameWithoutCrc) noexcept {
        uint8_t crc = 0xFFU;
        for (uint8_t b : frameWithoutCrc) {
            crc ^= b;
            for (int bit = 0; bit < 8; ++bit) {
                if ((crc & 0x80U) != 0) {
                    crc = static_cast<uint8_t>((crc << 1U) ^ 0x31U);
                } else {
                    crc = static_cast<uint8_t>(crc << 1U);
                }
            }
        }
        return crc;
    }

    static std::vector<uint8_t> buildComProtocolFrame(IgnitePktType type, uint8_t dir, uint8_t cmdId,
                                                      const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> frame;
        // Bound payload size to prevent frame overflow (max 32 bytes payload)
        const std::size_t boundedSize = std::min<std::size_t>(payload.size(), 32);
        frame.reserve(6 + boundedSize + 1);
        frame.push_back(static_cast<uint8_t>('M'));
        frame.push_back(static_cast<uint8_t>('T'));
        frame.push_back(static_cast<uint8_t>(type));
        frame.push_back(dir);
        frame.push_back(cmdId);
        frame.push_back(static_cast<uint8_t>(boundedSize + 1)); // matches CPktMgr sizeValue + PKTMGR_CRC_BYTE
        for (std::size_t i = 0; i < boundedSize; ++i) {
            frame.push_back(payload[i]);
        }
        const uint8_t crc = computeComProtocolCrc8(frame);
        frame.push_back(crc);
        return frame;
    }

    static bool verifyComProtocolFrame(const std::vector<uint8_t>& frame) noexcept {
        if (frame.size() < 7) return false;
        if (frame[0] != static_cast<uint8_t>('M') || frame[1] != static_cast<uint8_t>('T')) return false;
        const std::size_t declaredPayloadPlusCrc = frame[5];
        if (frame.size() != 6 + declaredPayloadPlusCrc) return false;
        std::vector<uint8_t> headerAndPayload(frame.begin(), frame.end() - 1);
        return computeComProtocolCrc8(headerAndPayload) == frame.back();
    }

private:
    IgniteControlMode controlMode_{IgniteControlMode::MatricealLanes};
    std::size_t activeTrack_{0};
    bool barButtonHeld_{false};
    std::array<uint8_t, kNumMacroKnobs> macroKnobs_{};
    std::array<uint16_t, kNumMacroKnobs> knobLedRings_{};
    std::array<RGBColor, kStepsPerBar> padColors_{};
};

} // namespace ignite

#endif // IGNITE_HARDWARE_AND_CV_HPP

