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

#ifndef IGNITE_ALGORITHMIC_ENGINES_HPP
#define IGNITE_ALGORITHMIC_ENGINES_HPP

#include "ignite_sequencer_core.hpp"
#include <algorithm>
#include <array>
#include <cstdint>

namespace ignite {

// ============================================================================
// Deterministic PRNG for Real-Time Embedded Audio/Sequencer Thread (Zero-Alloc)
// ============================================================================
class XorShift32 {
public:
    explicit constexpr XorShift32(uint32_t seed = 0x9E3779B9U) noexcept
        : state_(seed == 0 ? 0x9E3779B9U : seed) {}

    constexpr uint32_t nextU32() noexcept {
        uint32_t x = state_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state_ = x;
        return x;
    }

    constexpr uint32_t nextBounded(uint32_t bound) noexcept {
        if (bound == 0) return 0;
        return nextU32() % bound;
    }

    constexpr float nextFloat01() noexcept {
        return static_cast<float>(nextU32() & 0x00FFFFFFU) / static_cast<float>(0x01000000U);
    }

private:
    uint32_t state_;
};

// ============================================================================
// Enhancement 3: Per-Step Accumulators & Inter-Track Aux Events
// (`StepAccumulatorAndAuxEngine`)
// ============================================================================
enum class AccumulatorWrapMode : uint8_t {
    Wrap = 0,       // Wraps around from maxBound back to minBound (and vice versa)
    Pendulum,       // Bounces direction at minBound / maxBound
    Clamp,          // Stops and saturates at minBound / maxBound
    ResetOnBar      // Wraps during bar and resets to 0 on bar boundary
};

struct StepAccumulator {
    int8_t deltaSemitones{0};     // -12 to +12 semitones per trigger
    int8_t deltaVelocity{0};      // -64 to +64 velocity per trigger
    int8_t minBound{-12};         // -36 to 0 semitones
    int8_t maxBound{12};          // 0 to +36 semitones
    AccumulatorWrapMode wrapMode{AccumulatorWrapMode::Wrap};

    // Runtime state
    int16_t currentSemitoneOffset{0};
    int16_t currentVelocityOffset{0};
    int8_t direction{1};          // +1 or -1 (used by Pendulum mode)
    bool initialized{false};

    void resetRuntime() noexcept {
        currentSemitoneOffset = 0;
        currentVelocityOffset = 0;
        direction = 1;
        initialized = false;
    }
};

enum class AuxCommandType : uint8_t {
    None = 0,
    TransposeTrack,         // param = semitones (-24..+24)
    RotateTrackSteps,       // param = rotation steps (-15..+15)
    MuteTrackToggle,        // toggles track mute state
    ResetTrackPlayhead,     // resets target track's polymetric lane playheads to 0
    RandomizeTrackVelocity  // param = randomization depth (0..127)
};

struct InterTrackAuxEvent {
    AuxCommandType command{AuxCommandType::None};
    uint8_t targetTrack{0}; // 0..63 (corresponds to Track 1..64)
    int8_t param{0};        // Semitones, step rotation, or depth
};

struct TrackAuxRuntimeState {
    bool muted{false};
    int8_t transposeSemitones{0};
};

class StepAccumulatorAndAuxEngine {
public:
    StepAccumulatorAndAuxEngine() noexcept { reset(); }

    void reset() noexcept {
        for (auto& acc : accumulators_) {
            acc = StepAccumulator{};
        }
        for (auto& aux : auxEvents_) {
            aux = InterTrackAuxEvent{};
        }
    }

    void configureStepAccumulator(std::size_t stepIdx, int8_t deltaSemi, int8_t deltaVel,
                                  int8_t minB, int8_t maxB, AccumulatorWrapMode mode) noexcept {
        if (stepIdx >= kMaxStepsPerLane) return;
        auto& acc = accumulators_[stepIdx];
        acc.deltaSemitones = std::clamp<int8_t>(deltaSemi, -12, 12);
        acc.deltaVelocity = std::clamp<int8_t>(deltaVel, -64, 64);
        acc.minBound = std::clamp<int8_t>( std::min(minB, maxB), -36, 0 );
        acc.maxBound = std::clamp<int8_t>( std::max(minB, maxB), 0, 36 );
        acc.wrapMode = mode;
        acc.resetRuntime();
    }

    // Hardware Mapping: Dial per-step pitch/velocity accumulator delta via StepEnc 1-16
    void turnStepEncoderPitchDelta(uint8_t barPage, uint8_t encIdx0To15, int delta) noexcept {
        if (encIdx0To15 >= kStepsPerBar) return;
        const std::size_t idx = (static_cast<std::size_t>(barPage) % kMaxBarsPerPattern) * kStepsPerBar + encIdx0To15;
        int next = static_cast<int>(accumulators_[idx].deltaSemitones) + delta;
        accumulators_[idx].deltaSemitones = static_cast<int8_t>(std::clamp(next, -12, 12));
    }

    void turnStepEncoderVelocityDelta(uint8_t barPage, uint8_t encIdx0To15, int delta) noexcept {
        if (encIdx0To15 >= kStepsPerBar) return;
        const std::size_t idx = (static_cast<std::size_t>(barPage) % kMaxBarsPerPattern) * kStepsPerBar + encIdx0To15;
        int next = static_cast<int>(accumulators_[idx].deltaVelocity) + delta;
        accumulators_[idx].deltaVelocity = static_cast<int8_t>(std::clamp(next, -64, 64));
    }

    void configureStepAuxEvent(std::size_t stepIdx, AuxCommandType cmd, uint8_t targetTrack0To63, int8_t param = 0) noexcept {
        if (stepIdx >= kMaxStepsPerLane) return;
        auxEvents_[stepIdx].command = cmd;
        auxEvents_[stepIdx].targetTrack = std::min<uint8_t>(targetTrack0To63, static_cast<uint8_t>(kNumTracks - 1));
        auxEvents_[stepIdx].param = std::clamp<int8_t>(param, -64, 127);
    }

    const StepAccumulator& stepAccumulator(std::size_t stepIdx) const noexcept {
        return accumulators_[stepIdx % kMaxStepsPerLane];
    }

    const InterTrackAuxEvent& stepAuxEvent(std::size_t stepIdx) const noexcept {
        return auxEvents_[stepIdx % kMaxStepsPerLane];
    }

    void onBarBoundary() noexcept {
        for (auto& acc : accumulators_) {
            if (acc.wrapMode == AccumulatorWrapMode::ResetOnBar) {
                acc.resetRuntime();
            }
        }
    }

    // Evaluates the step's accumulator, updates its accumulated pitch/velocity offset,
    // and applies it to `event` (quantizing can then be performed by MasterHarmonizerBus).
    void evaluateStepAccumulator(std::size_t stepIdx, EvaluatedStepEvent& event) noexcept {
        if (stepIdx >= kMaxStepsPerLane || !event.triggered) return;
        auto& acc = accumulators_[stepIdx];

        if (!acc.initialized) {
            acc.initialized = true;
            acc.currentSemitoneOffset = 0;
            acc.currentVelocityOffset = 0;
        } else {
            // Advance semitone accumulator
            int nextSemi = static_cast<int>(acc.currentSemitoneOffset) +
                           static_cast<int>(acc.deltaSemitones) * static_cast<int>(acc.direction);
            const int minB = static_cast<int>(acc.minBound);
            const int maxB = static_cast<int>(acc.maxBound);
            const int span = (maxB - minB) + 1;

            switch (acc.wrapMode) {
                case AccumulatorWrapMode::Wrap:
                case AccumulatorWrapMode::ResetOnBar: {
                    if (span > 0) {
                        while (nextSemi > maxB) nextSemi -= span;
                        while (nextSemi < minB) nextSemi += span;
                    }
                    break;
                }
                case AccumulatorWrapMode::Pendulum: {
                    if (nextSemi > maxB) {
                        nextSemi = maxB - (nextSemi - maxB);
                        acc.direction = static_cast<int8_t>(-acc.direction);
                    } else if (nextSemi < minB) {
                        nextSemi = minB + (minB - nextSemi);
                        acc.direction = static_cast<int8_t>(-acc.direction);
                    }
                    nextSemi = std::clamp(nextSemi, minB, maxB);
                    break;
                }
                case AccumulatorWrapMode::Clamp: {
                    nextSemi = std::clamp(nextSemi, minB, maxB);
                    break;
                }
            }
            acc.currentSemitoneOffset = static_cast<int16_t>(nextSemi);
            acc.currentVelocityOffset = static_cast<int16_t>(
                std::clamp<int>(acc.currentVelocityOffset + acc.deltaVelocity, -126, 126));
        }

        const int modifiedNote = static_cast<int>(event.midiNote) + static_cast<int>(acc.currentSemitoneOffset);
        event.midiNote = static_cast<uint8_t>(std::clamp(modifiedNote, 0, 127));

        const int modifiedVel = static_cast<int>(event.velocity) + static_cast<int>(acc.currentVelocityOffset);
        event.velocity = static_cast<uint8_t>(std::clamp(modifiedVel, 1, 127));
    }

    // Dispatches a step's Inter-Track Aux Event onto the 64-track array
    void dispatchAuxEvent(std::size_t stepIdx,
                          std::array<MatricealTrackEngine, kNumTracks>& tracks,
                          std::array<TrackAuxRuntimeState, kNumTracks>& runtimeStates,
                          XorShift32& rng) const noexcept {
        if (stepIdx >= kMaxStepsPerLane) return;
        const auto& aux = auxEvents_[stepIdx];
        if (aux.command == AuxCommandType::None || aux.targetTrack >= kNumTracks) return;

        auto& targetTrack = tracks[aux.targetTrack];
        auto& targetState = runtimeStates[aux.targetTrack];

        switch (aux.command) {
            case AuxCommandType::None:
                break;

            case AuxCommandType::TransposeTrack: {
                const int nextSemi = static_cast<int>(targetState.transposeSemitones) + static_cast<int>(aux.param);
                targetState.transposeSemitones = static_cast<int8_t>(std::clamp(nextSemi, -36, 36));
                const int newBase = static_cast<int>(targetTrack.baseMidiNote()) + static_cast<int>(aux.param);
                targetTrack.setBaseMidiNote(static_cast<uint8_t>(std::clamp(newBase, 12, 108)));
                break;
            }

            case AuxCommandType::RotateTrackSteps: {
                auto& gateValues = targetTrack.gateLane().values;
                const uint8_t len = targetTrack.gateLane().length;
                if (len > 1) {
                    const int rot = ((static_cast<int>(aux.param) % static_cast<int>(len)) + static_cast<int>(len)) % static_cast<int>(len);
                    std::rotate(gateValues.begin(), gateValues.begin() + rot, gateValues.begin() + len);
                }
                break;
            }

            case AuxCommandType::MuteTrackToggle: {
                targetState.muted = !targetState.muted;
                break;
            }

            case AuxCommandType::ResetTrackPlayhead: {
                targetTrack.resetAllPlayheads();
                break;
            }

            case AuxCommandType::RandomizeTrackVelocity: {
                const int depth = std::clamp<int>(aux.param, 0, 64);
                auto& velLane = targetTrack.velocityLane();
                for (uint8_t i = 0; i < velLane.length; ++i) {
                    const int delta = (depth > 0) ? (static_cast<int>(rng.nextBounded(static_cast<uint32_t>(depth * 2 + 1))) - depth) : 0;
                    velLane.values[i] = static_cast<uint8_t>(std::clamp(static_cast<int>(velLane.values[i]) + delta, 1, 127));
                }
                break;
            }
        }
    }

private:
    std::array<StepAccumulator, kMaxStepsPerLane> accumulators_{};
    std::array<InterTrackAuxEvent, kMaxStepsPerLane> auxEvents_{};
};

// ============================================================================
// Enhancement 4: Stochastic & Turing-Loop Generator (`StochasticTuringGenerator`)
// ============================================================================
struct StochasticStepOutput {
    bool isRest{false};
    bool tieActive{false};
    uint8_t midiNote{60};
    int8_t octaveShift{0};
    uint8_t ratchetCount{1};
    bool glideEnabled{false};
    uint16_t turingRegisterState{0};
};

class StochasticTuringGenerator {
public:
    StochasticTuringGenerator() noexcept { reset(); }

    void reset() noexcept {
        // Default C Major pentatonic weights on StepEnc 1..12
        noteWeights12_ = {80, 0, 50, 0, 60, 0, 0, 75, 0, 50, 0, 0};
        octaveJumpWeight_ = 15; // StepEnc 13
        restTieWeight_ = 10;    // StepEnc 14
        ratchetBurstWeight_ = 10; // StepEnc 15
        glideWeight_ = 15;      // StepEnc 16
        turingLockPercent_ = 0.0f; // Knob E: 0%..100%
        turingLoopLength_ = 8;     // Knob F: 1..16 steps
        turingShiftReg_ = 0xACE1U;
        baseMidiNote_ = 60;
        playhead_ = 0;
        for (std::size_t i = 0; i < kStepsPerBar; ++i) {
            loopNoteBuffer_[i] = static_cast<uint8_t>((i * 2) % 12);
            loopOctaveBuffer_[i] = 0;
        }
    }

    // Hardware Mapping: StepEnc 1-12 set chromatic note weights (0..100%),
    // StepEnc 13-16 set Octave Jump, Rest/Tie, Ratchet Burst, and Glide weights.
    void turnStepEncoderWeight(uint8_t encIdx0To15, int delta) noexcept {
        if (encIdx0To15 < 12) {
            int next = static_cast<int>(noteWeights12_[encIdx0To15]) + delta;
            noteWeights12_[encIdx0To15] = static_cast<uint8_t>(std::clamp(next, 0, 100));
        } else if (encIdx0To15 == 12) {
            octaveJumpWeight_ = static_cast<uint8_t>(std::clamp(static_cast<int>(octaveJumpWeight_) + delta, 0, 100));
        } else if (encIdx0To15 == 13) {
            restTieWeight_ = static_cast<uint8_t>(std::clamp(static_cast<int>(restTieWeight_) + delta, 0, 100));
        } else if (encIdx0To15 == 14) {
            ratchetBurstWeight_ = static_cast<uint8_t>(std::clamp(static_cast<int>(ratchetBurstWeight_) + delta, 0, 100));
        } else if (encIdx0To15 == 15) {
            glideWeight_ = static_cast<uint8_t>(std::clamp(static_cast<int>(glideWeight_) + delta, 0, 100));
        }
    }

    void setNoteWeight(std::size_t semitone0To11, uint8_t weight0To100) noexcept {
        if (semitone0To11 < 12) {
            noteWeights12_[semitone0To11] = std::min<uint8_t>(weight0To100, 100);
        }
    }
    uint8_t noteWeight(std::size_t semitone0To11) const noexcept {
        return (semitone0To11 < 12) ? noteWeights12_[semitone0To11] : 0;
    }

    void setArticulationWeights(uint8_t octJump, uint8_t restTie, uint8_t ratchet, uint8_t glide) noexcept {
        octaveJumpWeight_ = std::min<uint8_t>(octJump, 100);
        restTieWeight_ = std::min<uint8_t>(restTie, 100);
        ratchetBurstWeight_ = std::min<uint8_t>(ratchet, 100);
        glideWeight_ = std::min<uint8_t>(glide, 100);
    }

    // Knob E (Turing Lock): 0.0% = pure stochastic, 50.0% = bit-flipping mutations, 100.0% = locked N-step loop
    void setKnobE_TuringLock(float lockPercent) noexcept {
        turingLockPercent_ = std::clamp(lockPercent, 0.0f, 100.0f);
    }
    float turingLockPercent() const noexcept { return turingLockPercent_; }

    // Knob F (Turing Loop Length): 1..16 steps
    void setKnobF_LoopLength(uint8_t length1To16) noexcept {
        turingLoopLength_ = std::clamp<uint8_t>(length1To16, 1, 16);
    }
    uint8_t turingLoopLength() const noexcept { return turingLoopLength_; }

    void setBaseMidiNote(uint8_t note) noexcept {
        baseMidiNote_ = std::clamp<uint8_t>(note, 12, 108);
    }

    uint16_t turingRegisterState() const noexcept { return turingShiftReg_; }

    StochasticStepOutput nextStep(XorShift32& rng) noexcept {
        StochasticStepOutput out{};
        const uint8_t stepSlot = static_cast<uint8_t>(playhead_ % turingLoopLength_);

        // Advance 16-bit Turing Shift Register:
        // At 100% lock, feedback bit is recirculated without inversion.
        // Below 100% lock, probability of bit-flip increases up to 100% random at 0% lock.
        const uint16_t feedbackBit = (turingShiftReg_ >> (turingLoopLength_ - 1)) & 1U;
        const float mutationProb = (100.0f - turingLockPercent_) / 100.0f;
        const bool mutate = (mutationProb > 0.0f) && (rng.nextFloat01() < mutationProb);
        const uint16_t injectedBit = mutate ? (feedbackBit ^ 1U) : feedbackBit;
        turingShiftReg_ = static_cast<uint16_t>((turingShiftReg_ << 1U) | injectedBit);
        out.turingRegisterState = turingShiftReg_;

        // If mutating (or lock < 100% and mutation triggered), sample new degree from StepEnc 1-12 weights
        if (mutate) {
            loopNoteBuffer_[stepSlot] = sampleWeightedSemitone(rng);
            if (rng.nextBounded(100) < octaveJumpWeight_) {
                loopOctaveBuffer_[stepSlot] = (rng.nextBounded(2) == 0) ? -1 : 1;
            } else {
                loopOctaveBuffer_[stepSlot] = 0;
            }
        }

        const uint8_t degree = loopNoteBuffer_[stepSlot];
        const int8_t oct = loopOctaveBuffer_[stepSlot];
        out.octaveShift = oct;

        const int midi = static_cast<int>(baseMidiNote_) + static_cast<int>(degree) + (static_cast<int>(oct) * 12);
        out.midiNote = static_cast<uint8_t>(std::clamp(midi, 0, 127));

        // Articulations from StepEnc 14-16 (when locked at 100%, use deterministic bits from shift register)
        if (turingLockPercent_ >= 99.9f) {
            out.isRest = false;
            out.ratchetCount = 1;
            out.glideEnabled = ((turingShiftReg_ & 0x04U) != 0) && (glideWeight_ > 50);
        } else {
            const uint32_t restRoll = rng.nextBounded(100);
            out.isRest = (restRoll < (restTieWeight_ / 2));
            out.tieActive = (!out.isRest && restRoll < restTieWeight_);
            out.ratchetCount = (rng.nextBounded(100) < ratchetBurstWeight_)
                                   ? static_cast<uint8_t>(2 + rng.nextBounded(3))
                                   : 1;
            out.glideEnabled = (rng.nextBounded(100) < glideWeight_);
        }

        playhead_ = static_cast<uint8_t>((playhead_ + 1) % turingLoopLength_);
        return out;
    }

private:
    uint8_t sampleWeightedSemitone(XorShift32& rng) const noexcept {
        uint32_t totalWeight = 0;
        for (uint8_t w : noteWeights12_) {
            totalWeight += w;
        }
        if (totalWeight == 0) return 0;

        uint32_t pick = rng.nextBounded(totalWeight);
        uint32_t accum = 0;
        for (uint8_t i = 0; i < 12; ++i) {
            accum += noteWeights12_[i];
            if (pick < accum) {
                return i;
            }
        }
        return 0;
    }

    std::array<uint8_t, 12> noteWeights12_{};
    uint8_t octaveJumpWeight_{15};
    uint8_t restTieWeight_{10};
    uint8_t ratchetBurstWeight_{10};
    uint8_t glideWeight_{15};
    float turingLockPercent_{0.0f};
    uint8_t turingLoopLength_{8};
    uint16_t turingShiftReg_{0xACE1U};
    uint8_t baseMidiNote_{60};
    uint8_t playhead_{0};
    std::array<uint8_t, kStepsPerBar> loopNoteBuffer_{};
    std::array<int8_t, kStepsPerBar> loopOctaveBuffer_{};
};

// ============================================================================
// Enhancement 5: Euclidean Generator with Non-Destructive Manual Overrides
// (`EuclideanOverrideGenerator`)
// ============================================================================
enum class PadOverrideState : uint8_t {
    AutoEuclidean = 0, // Follows live Bjorklund E(k, n, rot)
    ForcedOn,          // Lit Cyan: always triggers regardless of Euclidean knobs
    ForcedMute         // Lit Dim Red: always muted regardless of Euclidean knobs
};

struct RGBColor {
    uint8_t r{0};
    uint8_t g{0};
    uint8_t b{0};
};

class EuclideanOverrideGenerator {
public:
    EuclideanOverrideGenerator() noexcept { reset(); }

    void reset() noexcept {
        stepsN_ = 16;
        pulsesK_ = 4;
        rotation_ = 0;
        accentPulsesK_ = 2;
        accentRotation_ = 0;
        manualForceOnMask_ = 0;
        manualForceMuteMask_ = 0;
        recomputeBitmasks();
    }

    // Hardware Mapping: Knobs A-D adjust Euclidean Steps (n), Pulses (k), Rotation, and Accent Pulses
    // without clearing `manualForceOnMask_` or `manualForceMuteMask_`.
    void setEuclideanParams(uint8_t stepsN, uint8_t pulsesK, uint8_t rotation,
                            uint8_t accentPulsesK = 0, uint8_t accentRotation = 0) noexcept {
        stepsN_ = std::clamp<uint8_t>(stepsN, 1, 16);
        pulsesK_ = std::min<uint8_t>(pulsesK, stepsN_);
        rotation_ = static_cast<uint8_t>(rotation % stepsN_);
        accentPulsesK_ = std::min<uint8_t>(accentPulsesK, stepsN_);
        accentRotation_ = static_cast<uint8_t>(accentRotation % stepsN_);
        recomputeBitmasks();
    }

    // Pressing any of the 16 RGB Step Pads cycles: AutoEuclidean -> ForcedOn (Cyan) -> ForcedMute (Dim Red) -> Auto
    void pressStepPadOverride(uint8_t padIdx0To15) noexcept {
        if (padIdx0To15 >= kStepsPerBar) return;
        const uint16_t bit = static_cast<uint16_t>(1U << padIdx0To15);
        const bool isForcedOn = (manualForceOnMask_ & bit) != 0;
        const bool isForcedMute = (manualForceMuteMask_ & bit) != 0;

        if (!isForcedOn && !isForcedMute) {
            manualForceOnMask_ |= bit;
            manualForceMuteMask_ &= static_cast<uint16_t>(~bit);
        } else if (isForcedOn) {
            manualForceOnMask_ &= static_cast<uint16_t>(~bit);
            manualForceMuteMask_ |= bit;
        } else {
            manualForceOnMask_ &= static_cast<uint16_t>(~bit);
            manualForceMuteMask_ &= static_cast<uint16_t>(~bit);
        }
    }

    void setStepOverrideState(uint8_t padIdx0To15, PadOverrideState state) noexcept {
        if (padIdx0To15 >= kStepsPerBar) return;
        const uint16_t bit = static_cast<uint16_t>(1U << padIdx0To15);
        switch (state) {
            case PadOverrideState::AutoEuclidean:
                manualForceOnMask_ &= static_cast<uint16_t>(~bit);
                manualForceMuteMask_ &= static_cast<uint16_t>(~bit);
                break;
            case PadOverrideState::ForcedOn:
                manualForceOnMask_ |= bit;
                manualForceMuteMask_ &= static_cast<uint16_t>(~bit);
                break;
            case PadOverrideState::ForcedMute:
                manualForceOnMask_ &= static_cast<uint16_t>(~bit);
                manualForceMuteMask_ |= bit;
                break;
        }
    }

    PadOverrideState stepOverrideState(uint8_t padIdx0To15) const noexcept {
        if (padIdx0To15 >= kStepsPerBar) return PadOverrideState::AutoEuclidean;
        const uint16_t bit = static_cast<uint16_t>(1U << padIdx0To15);
        if ((manualForceOnMask_ & bit) != 0) return PadOverrideState::ForcedOn;
        if ((manualForceMuteMask_ & bit) != 0) return PadOverrideState::ForcedMute;
        return PadOverrideState::AutoEuclidean;
    }

    void clearManualOverrides() noexcept {
        manualForceOnMask_ = 0;
        manualForceMuteMask_ = 0;
    }

    uint16_t rawEuclideanMask() const noexcept { return euclideanMask_; }
    uint16_t rawAccentMask() const noexcept { return accentMask_; }
    uint16_t manualForceOnMask() const noexcept { return manualForceOnMask_; }
    uint16_t manualForceMuteMask() const noexcept { return manualForceMuteMask_; }

    // Final trigger bitmask = (euclideanMask_ & ~manualForceMuteMask_) | manualForceOnMask_
    uint16_t effectiveTriggerMask() const noexcept {
        const uint16_t stepWindowMask = (stepsN_ >= 16) ? 0xFFFFU : static_cast<uint16_t>((1U << stepsN_) - 1U);
        return static_cast<uint16_t>(((euclideanMask_ & ~manualForceMuteMask_) | manualForceOnMask_) & stepWindowMask);
    }

    bool shouldTriggerStep(uint8_t stepIdx) const noexcept {
        const uint8_t wrapped = static_cast<uint8_t>(stepIdx % stepsN_);
        return ((effectiveTriggerMask() >> wrapped) & 1U) != 0;
    }

    bool isStepAccented(uint8_t stepIdx) const noexcept {
        const uint8_t wrapped = static_cast<uint8_t>(stepIdx % stepsN_);
        return ((accentMask_ >> wrapped) & 1U) != 0;
    }

    // Returns the RGB LED color for the physical IGNITE step button (0..15):
    // - Cyan (0, 255, 255) for manual Forced-On
    // - Dim Red (90, 0, 0) for manual Forced-Mute
    // - Bright Amber/White for active Euclidean pulse
    // - Off (0, 0, 0) for inactive Euclidean step
    RGBColor padLedColor(uint8_t padIdx0To15) const noexcept {
        const PadOverrideState ov = stepOverrideState(padIdx0To15);
        if (ov == PadOverrideState::ForcedOn) {
            return RGBColor{0, 255, 255}; // Cyan
        }
        if (ov == PadOverrideState::ForcedMute) {
            return RGBColor{90, 0, 0};    // Dim Red
        }
        if (padIdx0To15 < stepsN_ && shouldTriggerStep(padIdx0To15)) {
            return isStepAccented(padIdx0To15) ? RGBColor{255, 200, 40} : RGBColor{255, 110, 0};
        }
        return RGBColor{0, 0, 0};
    }

    uint8_t stepsN() const noexcept { return stepsN_; }
    uint8_t pulsesK() const noexcept { return pulsesK_; }
    uint8_t rotation() const noexcept { return rotation_; }

    // Static Bjorklund / Bresenham Euclidean rhythm bitmask computation E(k, n, rot)
    static uint16_t computeBjorklundMask(uint8_t k, uint8_t n, uint8_t rotation) noexcept {
        if (n == 0 || k == 0) return 0;
        const uint8_t clampedN = std::clamp<uint8_t>(n, 1, 16);
        if (k >= clampedN) {
            return (clampedN == 16) ? 0xFFFFU : static_cast<uint16_t>((1U << clampedN) - 1U);
        }

        uint16_t mask = 0;
        for (uint8_t i = 0; i < clampedN; ++i) {
            // Standard discrete Bjorklund distribution ensuring first step is a pulse before rotation
            if (((static_cast<uint32_t>(i) * static_cast<uint32_t>(k)) % static_cast<uint32_t>(clampedN)) < k) {
                const uint8_t rotatedIdx = static_cast<uint8_t>((i + rotation) % clampedN);
                mask |= static_cast<uint16_t>(1U << rotatedIdx);
            }
        }
        return mask;
    }

private:
    void recomputeBitmasks() noexcept {
        euclideanMask_ = computeBjorklundMask(pulsesK_, stepsN_, rotation_);
        accentMask_ = computeBjorklundMask(accentPulsesK_, stepsN_, accentRotation_);
    }

    uint8_t stepsN_{16};
    uint8_t pulsesK_{4};
    uint8_t rotation_{0};
    uint8_t accentPulsesK_{2};
    uint8_t accentRotation_{0};
    uint16_t euclideanMask_{0};
    uint16_t accentMask_{0};
    uint16_t manualForceOnMask_{0};
    uint16_t manualForceMuteMask_{0};
};

} // namespace ignite

#endif // IGNITE_ALGORITHMIC_ENGINES_HPP

