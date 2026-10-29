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

#ifndef IGNITE_SEQUENCER_CORE_HPP
#define IGNITE_SEQUENCER_CORE_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ignite {

// ============================================================================
// Constants & Hardware Limits (Behringer IGNITE BE 0716-AAF)
// ============================================================================
constexpr std::size_t kNumTracks = 64;
constexpr std::size_t kMaxStepsPerLane = 64;
constexpr std::size_t kStepsPerBar = 16;
constexpr std::size_t kMaxBarsPerPattern = 4; // 4 * 16 = 64 steps per polymetric lane
constexpr std::size_t kMaxScenes = 200;
constexpr std::size_t kNumMatricealLanes = 8;
constexpr uint32_t kPPQN960 = 960;
constexpr uint32_t kStockPPQN96 = 96;
constexpr uint32_t kTicksPerSixteenth = kPPQN960 / 4; // 240 ticks per 1/16th note
constexpr uint32_t kTicksPerBar4_4 = kPPQN960 * 4;    // 3840 ticks per 4/4 bar
constexpr int16_t kMinMicroTimingTicks = -239;
constexpr int16_t kMaxMicroTimingTicks = 239;

// ============================================================================
// Enhancement 8 (Part 1): 960 PPQN Sub-Tick Clock & Micro-Timing Scheduler
// ============================================================================
enum class StepDivision : uint8_t {
    Div_1_4 = 0,   // 960 ticks
    Div_1_8,       // 480 ticks
    Div_1_8T,      // 320 ticks
    Div_1_16,      // 240 ticks
    Div_1_16T,     // 160 ticks
    Div_1_32,      // 120 ticks
    Div_1_32T,     // 80 ticks
    Div_1_64,      // 60 ticks
    Div_1_64T      // 40 ticks
};

inline constexpr uint32_t divisionToTicks960(StepDivision div) noexcept {
    switch (div) {
        case StepDivision::Div_1_4:   return 960;
        case StepDivision::Div_1_8:   return 480;
        case StepDivision::Div_1_8T:  return 320;
        case StepDivision::Div_1_16:  return 240;
        case StepDivision::Div_1_16T: return 160;
        case StepDivision::Div_1_32:  return 120;
        case StepDivision::Div_1_32T: return 80;
        case StepDivision::Div_1_64:  return 60;
        case StepDivision::Div_1_64T: return 40;
    }
    return 240;
}

struct GrooveTemplate16 {
    std::array<int16_t, kStepsPerBar> tickOffsets{};     // -120..+120 ticks per 16th step
    std::array<int8_t, kStepsPerBar> velocityOffsets{};  // -64..+64 velocity delta
};

class HighResClock960 {
public:
    HighResClock960() noexcept { reset(); }

    void reset() noexcept {
        running_ = false;
        bpm_ = 120.0f;
        sampleRate_ = 48000.0f;
        swingPercent_ = 50.0f;
        polymetricResetBars_ = 0; // 0 = free-running polymetric (no forced reset)
        currentTick_ = 0;
        subTickSampleAccumulator_ = 0.0;
        trackMicroTiming_.fill(0);
        groove_ = GrooveTemplate16{};
    }

    void setBpm(float bpm) noexcept {
        bpm_ = std::clamp(bpm, 20.0f, 300.0f);
    }
    float bpm() const noexcept { return bpm_; }

    void setSampleRate(float sr) noexcept {
        sampleRate_ = std::clamp(sr, 8000.0f, 192000.0f);
    }
    float sampleRate() const noexcept { return sampleRate_; }

    // Swing from 50.0% (straight) to 75.0% (MPC triplet feel)
    void setSwingPercent(float swing) noexcept {
        swingPercent_ = std::clamp(swing, 50.0f, 75.0f);
    }
    float swingPercent() const noexcept { return swingPercent_; }

    void setTrackMicroTiming(std::size_t trackIdx, int16_t ticks) noexcept {
        if (trackIdx < kNumTracks) {
            trackMicroTiming_[trackIdx] = std::clamp(ticks, kMinMicroTimingTicks, kMaxMicroTimingTicks);
        }
    }

    int16_t trackMicroTiming(std::size_t trackIdx) const noexcept {
        return (trackIdx < kNumTracks) ? trackMicroTiming_[trackIdx] : 0;
    }

    void setGrooveTemplate(const GrooveTemplate16& groove) noexcept {
        for (std::size_t i = 0; i < kStepsPerBar; ++i) {
            groove_.tickOffsets[i] = std::clamp<int16_t>(groove.tickOffsets[i], -120, 120);
            groove_.velocityOffsets[i] = std::clamp<int8_t>(groove.velocityOffsets[i], -64, 64);
        }
    }

    const GrooveTemplate16& grooveTemplate() const noexcept { return groove_; }

    void setPolymetricResetBars(uint16_t bars) noexcept {
        polymetricResetBars_ = std::min<uint16_t>(bars, 200);
    }
    uint16_t polymetricResetBars() const noexcept { return polymetricResetBars_; }

    void start() noexcept { running_ = true; }
    void stop() noexcept { running_ = false; }
    bool isRunning() const noexcept { return running_; }

    uint64_t currentTick() const noexcept { return currentTick_; }
    uint64_t currentBar() const noexcept { return currentTick_ / kTicksPerBar4_4; }
    uint64_t currentSixteenthStep() const noexcept { return currentTick_ / kTicksPerSixteenth; }

    double samplesPerTick() const noexcept {
        const double ticksPerSecond = (static_cast<double>(bpm_) / 60.0) * static_cast<double>(kPPQN960);
        return static_cast<double>(sampleRate_) / ticksPerSecond;
    }

    // Advances the clock by a given number of audio samples and returns how many 960-PPQN ticks elapsed
    uint32_t advanceAudioSamples(uint32_t numSamples) noexcept {
        if (!running_) return 0;
        subTickSampleAccumulator_ += static_cast<double>(numSamples);
        const double spt = samplesPerTick();
        uint32_t elapsedTicks = 0;
        while (subTickSampleAccumulator_ >= spt) {
            subTickSampleAccumulator_ -= spt;
            ++currentTick_;
            ++elapsedTicks;
        }
        return elapsedTicks;
    }

    // Directly step a specific number of 960-PPQN ticks (for deterministic tests and MIDI clock sync)
    void advanceTicks(uint32_t ticks = 1) noexcept {
        currentTick_ += ticks;
    }

    // Computes the exact scheduled tick for a given 16th-note step index on a track,
    // applying swing, groove template, and per-track micro-timing (-239..+239 ticks).
    int64_t scheduledTickForStep(std::size_t trackIdx, uint64_t stepIndex, StepDivision div = StepDivision::Div_1_16) const noexcept {
        const uint32_t baseStepTicks = divisionToTicks960(div);
        int64_t tick = static_cast<int64_t>(stepIndex) * static_cast<int64_t>(baseStepTicks);

        // Apply swing to odd-numbered steps
        if ((stepIndex & 1ULL) == 1ULL) {
            const float swingRatio = (swingPercent_ - 50.0f) / 50.0f; // 0.0 .. 0.5
            tick += static_cast<int64_t>( std::llround(swingRatio * static_cast<float>(baseStepTicks)) );
        }

        // Apply groove template offset
        const std::size_t grooveIdx = static_cast<std::size_t>(stepIndex % kStepsPerBar);
        tick += groove_.tickOffsets[grooveIdx];

        // Apply per-track micro-timing
        if (trackIdx < kNumTracks) {
            tick += trackMicroTiming_[trackIdx];
        }

        return std::max<int64_t>(0, tick);
    }

    bool shouldTriggerPolymetricReset(uint64_t tick) const noexcept {
        if (polymetricResetBars_ == 0 || tick == 0) return false;
        const uint64_t resetPeriodTicks = static_cast<uint64_t>(polymetricResetBars_) * kTicksPerBar4_4;
        return (tick % resetPeriodTicks) == 0;
    }

private:
    bool running_{false};
    float bpm_{120.0f};
    float sampleRate_{48000.0f};
    float swingPercent_{50.0f};
    uint16_t polymetricResetBars_{0};
    uint64_t currentTick_{0};
    double subTickSampleAccumulator_{0.0};
    std::array<int16_t, kNumTracks> trackMicroTiming_{};
    GrooveTemplate16 groove_{};
};

// ============================================================================
// Enhancement 1: Matriceal Polymetric Lanes (`MatricealTrackEngine`)
// ============================================================================
enum class MatricealLaneType : uint8_t {
    Gate = 0,         // Lane 1: Gate on/off (length 1-64, division 1/4..1/64T)
    Pitch,            // Lane 2: Semitone offset (-36..+36)
    Velocity,         // Lane 3: MIDI velocity (1..127)
    Octave,           // Lane 4: Octave offset (-3..+3)
    GlideGateLen,     // Lane 5: Gate length (5%..100%) & Portamento/Tie
    Ratchet,          // Lane 6: Sub-triggers (1x..8x)
    Probability,      // Lane 7: Probability (0%..100%) + A:B trig conditions
    ModCCCV           // Lane 8: 14-bit CV (0..16383) or 7-bit MIDI CC (0..127)
};

template <typename T>
struct PolymetricLane {
    std::array<T, kMaxStepsPerLane> values{};
    uint8_t length{16};                       // 1..64 independent loop length
    StepDivision division{StepDivision::Div_1_16};
    uint8_t playhead{0};                      // Current step 0..(length-1)
    uint32_t loopIteration{0};                // Counts how many times this lane has wrapped

    void setLength(uint8_t len) noexcept {
        length = std::clamp<uint8_t>(len, 1, static_cast<uint8_t>(kMaxStepsPerLane));
        if (playhead >= length) {
            playhead = static_cast<uint8_t>(playhead % length);
        }
    }

    const T& current() const noexcept {
        return values[playhead % length];
    }

    const T& atStep(std::size_t stepIndex) const noexcept {
        return values[stepIndex % length];
    }

    void advance() noexcept {
        ++playhead;
        if (playhead >= length) {
            playhead = 0;
            ++loopIteration;
        }
    }

    void resetPlayhead() noexcept {
        playhead = 0;
        loopIteration = 0;
    }
};

struct GlideGateCell {
    uint8_t gateLengthPercent{75}; // 5% to 100% (100% = Tie)
    uint8_t glideAmount{0};        // 0..127 portamento time
    bool glideEnabled{false};
};

struct ProbabilityCell {
    uint8_t probabilityPercent{100}; // 0% to 100%
    uint8_t trigCondA{1};            // Triggers on pass A ...
    uint8_t trigCondB{1};            // ... out of every B loops (1:1 = always)
};

struct EvaluatedStepEvent {
    bool triggered{false};
    uint8_t midiNote{60};
    uint8_t velocity{100};
    int8_t octaveOffset{0};
    uint8_t gateLengthPercent{75};
    bool tieActive{false};
    bool glideEnabled{false};
    uint8_t glideAmount{0};
    uint8_t ratchetCount{1};
    uint8_t probabilityPercent{100};
    uint8_t modCC7Bit{0};
    uint16_t modCV14Bit{0};
    std::array<uint8_t, kNumMatricealLanes> lanePlayheads{};
};

class MatricealTrackEngine {
public:
    MatricealTrackEngine() noexcept { resetToDefaults(); }

    void resetToDefaults() noexcept {
        baseMidiNote_ = 60; // Middle C (C4)
        activeRow_ = MatricealLaneType::Gate;
        activeBarPage_ = 0;
        activeScene_ = 1;

        gateLane_.setLength(16);
        gateLane_.values.fill(0);
        gateLane_.resetPlayhead();

        pitchLane_.setLength(16);
        pitchLane_.values.fill(0);
        pitchLane_.resetPlayhead();

        velocityLane_.setLength(16);
        velocityLane_.values.fill(100);
        velocityLane_.resetPlayhead();

        octaveLane_.setLength(16);
        octaveLane_.values.fill(0);
        octaveLane_.resetPlayhead();

        glideGateLenLane_.setLength(16);
        glideGateLenLane_.values.fill(GlideGateCell{75, 0, false});
        glideGateLenLane_.resetPlayhead();

        ratchetLane_.setLength(16);
        ratchetLane_.values.fill(1);
        ratchetLane_.resetPlayhead();

        probabilityLane_.setLength(16);
        probabilityLane_.values.fill(ProbabilityCell{100, 1, 1});
        probabilityLane_.resetPlayhead();

        modCCCVLane_.setLength(16);
        modCCCVLane_.values.fill(0);
        modCCCVLane_.resetPlayhead();
    }

    // --- Hardware Controls: ROW, BAR, Step Buttons 1-16, StepEnc 1-16 ---

    // Turn ROW encoder to select the active parameter lane (0..7)
    void turnRowEncoder(int delta) noexcept {
        int next = (static_cast<int>(activeRow_) + delta) % static_cast<int>(kNumMatricealLanes);
        if (next < 0) next += static_cast<int>(kNumMatricealLanes);
        activeRow_ = static_cast<MatricealLaneType>(next);
    }

    void setActiveRow(MatricealLaneType lane) noexcept { activeRow_ = lane; }
    MatricealLaneType activeRow() const noexcept { return activeRow_; }

    // Turn BAR encoder to select active 16-step bar page (0..3 for 64 steps)
    void turnBarEncoder(int delta) noexcept {
        int next = std::clamp(static_cast<int>(activeBarPage_) + delta, 0, static_cast<int>(kMaxBarsPerPattern - 1));
        activeBarPage_ = static_cast<uint8_t>(next);
    }

    void setActiveBarPage(uint8_t page) noexcept {
        activeBarPage_ = std::min<uint8_t>(page, static_cast<uint8_t>(kMaxBarsPerPattern - 1));
    }
    uint8_t activeBarPage() const noexcept { return activeBarPage_; }

    void setActiveScene(uint8_t scene) noexcept {
        activeScene_ = std::clamp<uint8_t>(scene, 1, static_cast<uint8_t>(kMaxScenes));
    }
    uint8_t activeScene() const noexcept { return activeScene_; }

    // Hold BAR + Step Button N (1..16) to set the active lane's independent loop length
    void setLaneLoopLengthViaBarAndStep(uint8_t stepButton1To16) noexcept {
        const uint8_t clampedBtn = std::clamp<uint8_t>(stepButton1To16, 1, 16);
        const uint8_t newLength = static_cast<uint8_t>(activeBarPage_ * kStepsPerBar + clampedBtn);
        setLaneLength(activeRow_, newLength);
    }

    void setLaneLength(MatricealLaneType lane, uint8_t length) noexcept {
        switch (lane) {
            case MatricealLaneType::Gate:         gateLane_.setLength(length); break;
            case MatricealLaneType::Pitch:        pitchLane_.setLength(length); break;
            case MatricealLaneType::Velocity:     velocityLane_.setLength(length); break;
            case MatricealLaneType::Octave:       octaveLane_.setLength(length); break;
            case MatricealLaneType::GlideGateLen: glideGateLenLane_.setLength(length); break;
            case MatricealLaneType::Ratchet:      ratchetLane_.setLength(length); break;
            case MatricealLaneType::Probability:  probabilityLane_.setLength(length); break;
            case MatricealLaneType::ModCCCV:      modCCCVLane_.setLength(length); break;
        }
    }

    uint8_t laneLength(MatricealLaneType lane) const noexcept {
        switch (lane) {
            case MatricealLaneType::Gate:         return gateLane_.length;
            case MatricealLaneType::Pitch:        return pitchLane_.length;
            case MatricealLaneType::Velocity:     return velocityLane_.length;
            case MatricealLaneType::Octave:       return octaveLane_.length;
            case MatricealLaneType::GlideGateLen: return glideGateLenLane_.length;
            case MatricealLaneType::Ratchet:      return ratchetLane_.length;
            case MatricealLaneType::Probability:  return probabilityLane_.length;
            case MatricealLaneType::ModCCCV:      return modCCCVLane_.length;
        }
        return 16;
    }

    void setLaneDivision(MatricealLaneType lane, StepDivision div) noexcept {
        switch (lane) {
            case MatricealLaneType::Gate:         gateLane_.division = div; break;
            case MatricealLaneType::Pitch:        pitchLane_.division = div; break;
            case MatricealLaneType::Velocity:     velocityLane_.division = div; break;
            case MatricealLaneType::Octave:       octaveLane_.division = div; break;
            case MatricealLaneType::GlideGateLen: glideGateLenLane_.division = div; break;
            case MatricealLaneType::Ratchet:      ratchetLane_.division = div; break;
            case MatricealLaneType::Probability:  probabilityLane_.division = div; break;
            case MatricealLaneType::ModCCCV:      modCCCVLane_.division = div; break;
        }
    }

    // Press one of the 16 RGB step buttons (0..15) to toggle gate on the active bar page
    void pressStepButton(uint8_t padIdx0To15) noexcept {
        if (padIdx0To15 >= kStepsPerBar) return;
        const std::size_t idx = static_cast<std::size_t>(activeBarPage_) * kStepsPerBar + padIdx0To15;
        gateLane_.values[idx] = (gateLane_.values[idx] == 0) ? 1 : 0;
    }

    // Turn StepEnc 1..16 (index 0..15) to edit the active lane's parameter for that step
    void turnStepEncoder(uint8_t encIdx0To15, int delta) noexcept {
        if (encIdx0To15 >= kStepsPerBar) return;
        const std::size_t idx = static_cast<std::size_t>(activeBarPage_) * kStepsPerBar + encIdx0To15;

        switch (activeRow_) {
            case MatricealLaneType::Gate: {
                gateLane_.values[idx] = (delta > 0) ? 1 : 0;
                break;
            }
            case MatricealLaneType::Pitch: {
                int next = static_cast<int>(pitchLane_.values[idx]) + delta;
                pitchLane_.values[idx] = static_cast<int8_t>(std::clamp(next, -36, 36));
                break;
            }
            case MatricealLaneType::Velocity: {
                int next = static_cast<int>(velocityLane_.values[idx]) + delta;
                velocityLane_.values[idx] = static_cast<uint8_t>(std::clamp(next, 1, 127));
                break;
            }
            case MatricealLaneType::Octave: {
                int next = static_cast<int>(octaveLane_.values[idx]) + delta;
                octaveLane_.values[idx] = static_cast<int8_t>(std::clamp(next, -3, 3));
                break;
            }
            case MatricealLaneType::GlideGateLen: {
                int next = static_cast<int>(glideGateLenLane_.values[idx].gateLengthPercent) + delta;
                glideGateLenLane_.values[idx].gateLengthPercent = static_cast<uint8_t>(std::clamp(next, 5, 100));
                glideGateLenLane_.values[idx].glideEnabled = (glideGateLenLane_.values[idx].gateLengthPercent >= 95);
                break;
            }
            case MatricealLaneType::Ratchet: {
                int next = static_cast<int>(ratchetLane_.values[idx]) + delta;
                ratchetLane_.values[idx] = static_cast<uint8_t>(std::clamp(next, 1, 8));
                break;
            }
            case MatricealLaneType::Probability: {
                int next = static_cast<int>(probabilityLane_.values[idx].probabilityPercent) + delta;
                probabilityLane_.values[idx].probabilityPercent = static_cast<uint8_t>(std::clamp(next, 0, 100));
                break;
            }
            case MatricealLaneType::ModCCCV: {
                int next = static_cast<int>(modCCCVLane_.values[idx]) + (delta * 128);
                modCCCVLane_.values[idx] = static_cast<uint16_t>(std::clamp(next, 0, 16383));
                break;
            }
        }
    }

    void setTrigCondition(std::size_t stepIdx, uint8_t condA, uint8_t condB) noexcept {
        if (stepIdx < kMaxStepsPerLane) {
            probabilityLane_.values[stepIdx].trigCondB = std::clamp<uint8_t>(condB, 1, 8);
            probabilityLane_.values[stepIdx].trigCondA = std::clamp<uint8_t>(condA, 1, probabilityLane_.values[stepIdx].trigCondB);
        }
    }

    void setBaseMidiNote(uint8_t note) noexcept {
        baseMidiNote_ = std::clamp<uint8_t>(note, 0, 127);
    }
    uint8_t baseMidiNote() const noexcept { return baseMidiNote_; }

    // Evaluates current polymetric state across all 8 independent lanes and advances their playheads
    EvaluatedStepEvent evaluateAndAdvance(uint8_t randomRoll0To99 = 0) noexcept {
        EvaluatedStepEvent ev{};
        ev.lanePlayheads = {
            gateLane_.playhead,
            pitchLane_.playhead,
            velocityLane_.playhead,
            octaveLane_.playhead,
            glideGateLenLane_.playhead,
            ratchetLane_.playhead,
            probabilityLane_.playhead,
            modCCCVLane_.playhead
        };

        const bool gateOn = (gateLane_.current() != 0);
        const ProbabilityCell prob = probabilityLane_.current();
        const uint32_t currentPass = (gateLane_.loopIteration % prob.trigCondB) + 1;
        const bool condMet = (currentPass == prob.trigCondA);
        const bool probMet = (prob.probabilityPercent >= 100) ||
                             (prob.probabilityPercent > 0 && (randomRoll0To99 % 100) < prob.probabilityPercent);

        ev.triggered = gateOn && condMet && probMet;

        const int8_t pitchSemis = pitchLane_.current();
        const int8_t octOffset = octaveLane_.current();
        const int rawMidi = static_cast<int>(baseMidiNote_) + static_cast<int>(pitchSemis) + (static_cast<int>(octOffset) * 12);
        ev.midiNote = static_cast<uint8_t>(std::clamp(rawMidi, 0, 127));
        ev.velocity = velocityLane_.current();
        ev.octaveOffset = octOffset;

        const GlideGateCell gg = glideGateLenLane_.current();
        ev.gateLengthPercent = gg.gateLengthPercent;
        ev.tieActive = (gg.gateLengthPercent >= 100);
        ev.glideEnabled = gg.glideEnabled;
        ev.glideAmount = gg.glideAmount;

        ev.ratchetCount = ratchetLane_.current();
        ev.probabilityPercent = prob.probabilityPercent;
        ev.modCV14Bit = modCCCVLane_.current();
        ev.modCC7Bit = static_cast<uint8_t>(ev.modCV14Bit >> 7);

        // Advance all 8 independent lanes (each wraps at its own length 1..64)
        gateLane_.advance();
        pitchLane_.advance();
        velocityLane_.advance();
        octaveLane_.advance();
        glideGateLenLane_.advance();
        ratchetLane_.advance();
        probabilityLane_.advance();
        modCCCVLane_.advance();

        return ev;
    }

    void resetAllPlayheads() noexcept {
        gateLane_.resetPlayhead();
        pitchLane_.resetPlayhead();
        velocityLane_.resetPlayhead();
        octaveLane_.resetPlayhead();
        glideGateLenLane_.resetPlayhead();
        ratchetLane_.resetPlayhead();
        probabilityLane_.resetPlayhead();
        modCCCVLane_.resetPlayhead();
    }

    // Direct accessors for unit tests & firmware engines
    PolymetricLane<uint8_t>& gateLane() noexcept { return gateLane_; }
    const PolymetricLane<uint8_t>& gateLane() const noexcept { return gateLane_; }
    PolymetricLane<int8_t>& pitchLane() noexcept { return pitchLane_; }
    const PolymetricLane<int8_t>& pitchLane() const noexcept { return pitchLane_; }
    PolymetricLane<uint8_t>& velocityLane() noexcept { return velocityLane_; }
    const PolymetricLane<uint8_t>& velocityLane() const noexcept { return velocityLane_; }
    PolymetricLane<int8_t>& octaveLane() noexcept { return octaveLane_; }
    const PolymetricLane<int8_t>& octaveLane() const noexcept { return octaveLane_; }
    PolymetricLane<GlideGateCell>& glideGateLenLane() noexcept { return glideGateLenLane_; }
    const PolymetricLane<GlideGateCell>& glideGateLenLane() const noexcept { return glideGateLenLane_; }
    PolymetricLane<uint8_t>& ratchetLane() noexcept { return ratchetLane_; }
    const PolymetricLane<uint8_t>& ratchetLane() const noexcept { return ratchetLane_; }
    PolymetricLane<ProbabilityCell>& probabilityLane() noexcept { return probabilityLane_; }
    const PolymetricLane<ProbabilityCell>& probabilityLane() const noexcept { return probabilityLane_; }
    PolymetricLane<uint16_t>& modCCCVLane() noexcept { return modCCCVLane_; }
    const PolymetricLane<uint16_t>& modCCCVLane() const noexcept { return modCCCVLane_; }

private:
    uint8_t baseMidiNote_{60};
    MatricealLaneType activeRow_{MatricealLaneType::Gate};
    uint8_t activeBarPage_{0};
    uint8_t activeScene_{1};

    PolymetricLane<uint8_t> gateLane_;
    PolymetricLane<int8_t> pitchLane_;
    PolymetricLane<uint8_t> velocityLane_;
    PolymetricLane<int8_t> octaveLane_;
    PolymetricLane<GlideGateCell> glideGateLenLane_;
    PolymetricLane<uint8_t> ratchetLane_;
    PolymetricLane<ProbabilityCell> probabilityLane_;
    PolymetricLane<uint16_t> modCCCVLane_;
};

// ============================================================================
// Enhancement 2: Master Harmonizer & 63-Track Follower Bus (`MasterHarmonizerBus`)
// ============================================================================
enum class HarmonizerFollowMode : uint8_t {
    Off = 0,          // Pass note unmodified
    ScaleQuantize,    // Quantize to nearest allowed scale degree in scaleMask12
    ChordToneLock,    // Snap arpeggios/melodies to activeChordNotes
    BassRootFollow,   // Lock basslines to bassNote (or 5th on dominant steps)
    VoiceLedPad       // Minimize semitone movement between chord changes
};

// Standard 12-bit scale interval masks (bit i = 1 means semitone i above root is in scale)
namespace ScaleMasks {
    constexpr uint16_t kChromatic      = 0b111111111111; // All 12 notes
    constexpr uint16_t kIonianMajor    = 0b101010110101; // 0, 2, 4, 5, 7, 9, 11
    constexpr uint16_t kNaturalMinor   = 0b010110101101; // 0, 2, 3, 5, 7, 8, 10
    constexpr uint16_t kDorian         = 0b011010101101; // 0, 2, 3, 5, 7, 9, 10
    constexpr uint16_t kPhrygian       = 0b010110100111; // 0, 1, 3, 5, 7, 8, 10
    constexpr uint16_t kLydian         = 0b101011010101; // 0, 2, 4, 6, 7, 9, 11
    constexpr uint16_t kMixolydian     = 0b011010110101; // 0, 2, 4, 5, 7, 9, 10
    constexpr uint16_t kHarmonicMinor  = 0b100110101101; // 0, 2, 3, 5, 7, 8, 11
}

struct HarmonicContext {
    uint8_t rootNote{0};                          // 0 = C, 1 = C#, ..., 11 = B
    uint16_t scaleMask12{ScaleMasks::kIonianMajor}; // 12-bit relative interval mask
    std::array<uint8_t, 6> activeChordNotes{60, 64, 67, 71, 0, 0};
    uint8_t activeChordSize{4};                   // 1..6 active chord tones
    uint8_t bassNote{36};                         // C2 default bass note
    float tensionLevel{0.0f};                     // 0.0f .. 1.0f
    uint8_t voicingSpread{0};                     // Knob A: 0..3
    uint8_t inversion{0};                         // Knob B: 0..3
    uint8_t modalInterchange{0};                  // Knob C: 0..127
    uint16_t strumTimingTicks{0};                 // Knob D: 0..240 ticks at 960 PPQN
};

class MasterHarmonizerBus {
public:
    MasterHarmonizerBus() noexcept { reset(); }

    void reset() noexcept {
        masterTrackIndex_ = 0; // Track 1 (index 0) is Master Chord & Scale Track by default
        context_ = HarmonicContext{};
        followModes_.fill(HarmonizerFollowMode::Off);
        lastVoiceLedNotes_.fill(60);
    }

    void setMasterTrackIndex(std::size_t trackIdx) noexcept {
        if (trackIdx < kNumTracks) {
            masterTrackIndex_ = trackIdx;
        }
    }
    std::size_t masterTrackIndex() const noexcept { return masterTrackIndex_; }

    void setTrackFollowMode(std::size_t trackIdx, HarmonizerFollowMode mode) noexcept {
        if (trackIdx < kNumTracks) {
            followModes_[trackIdx] = mode;
        }
    }

    HarmonizerFollowMode trackFollowMode(std::size_t trackIdx) const noexcept {
        return (trackIdx < kNumTracks) ? followModes_[trackIdx] : HarmonizerFollowMode::Off;
    }

    void setHarmonicContext(const HarmonicContext& ctx) noexcept {
        context_ = ctx;
        context_.rootNote = static_cast<uint8_t>(ctx.rootNote % 12);
        context_.activeChordSize = std::clamp<uint8_t>(ctx.activeChordSize, 1, 6);
        if (context_.scaleMask12 == 0) {
            context_.scaleMask12 = ScaleMasks::kIonianMajor;
        }
        context_.tensionLevel = std::clamp(ctx.tensionLevel, 0.0f, 1.0f);
    }

    const HarmonicContext& harmonicContext() const noexcept { return context_; }

    // Set a chord from root MIDI note and intervals, then apply Knobs A-D (Spread, Inversion, Modal Interchange, Strum)
    void setMasterChord(uint8_t chordRootMidi, const std::array<int8_t, 4>& baseIntervals = {0, 4, 7, 11}) noexcept {
        baseChordRootMidi_ = std::clamp<uint8_t>(chordRootMidi, 12, 108);
        baseIntervals_ = baseIntervals;
        rebuildVoicingFromKnobs();
    }

    // Hardware Mapping: Knobs A-D on the Master Chord Track
    // Knob A: Chord Voicing Spread (0..127 -> 0..3 spread levels)
    // Knob B: Inversion (0..127 -> 0..3 inversions)
    // Knob C: Borrowed / Modal Interchange (0..127 -> borrows flat-3rd, flat-6th, flat-7th, or Lydian #4)
    // Knob D: Strum Timing (0..127 -> 0..240 ticks at 960 PPQN)
    void setMasterKnobsAD(uint8_t knobA_Spread, uint8_t knobB_Inversion, uint8_t knobC_Modal, uint8_t knobD_Strum) noexcept {
        context_.voicingSpread = static_cast<uint8_t>(std::clamp<int>(knobA_Spread / 32, 0, 3));
        context_.inversion = static_cast<uint8_t>(std::clamp<int>(knobB_Inversion / 32, 0, 3));
        context_.modalInterchange = std::min<uint8_t>(knobC_Modal, 127);
        context_.strumTimingTicks = static_cast<uint16_t>((static_cast<uint32_t>(std::min<uint8_t>(knobD_Strum, 127)) * 240U) / 127U);
        context_.tensionLevel = static_cast<float>(context_.modalInterchange) / 127.0f;
        rebuildVoicingFromKnobs();
    }

    // Transforms a follower track's raw MIDI note according to its HarmonizerFollowMode
    uint8_t harmonizeNote(std::size_t trackIdx, uint8_t inputMidiNote) noexcept {
        const uint8_t clampedInput = std::min<uint8_t>(inputMidiNote, 127);
        if (trackIdx >= kNumTracks || trackIdx == masterTrackIndex_) {
            return clampedInput;
        }

        const HarmonizerFollowMode mode = followModes_[trackIdx];
        switch (mode) {
            case HarmonizerFollowMode::Off:
                return clampedInput;

            case HarmonizerFollowMode::ScaleQuantize:
                return quantizeToScale(clampedInput);

            case HarmonizerFollowMode::ChordToneLock:
                return snapToChordTone(clampedInput);

            case HarmonizerFollowMode::BassRootFollow:
                return computeBassFollowNote(clampedInput);

            case HarmonizerFollowMode::VoiceLedPad: {
                const uint8_t targetRef = lastVoiceLedNotes_[trackIdx];
                const uint8_t ledNote = computeVoiceLedNote(targetRef, clampedInput);
                lastVoiceLedNotes_[trackIdx] = ledNote;
                return ledNote;
            }
        }
        return clampedInput;
    }

    void setTrackLastVoiceLedNote(std::size_t trackIdx, uint8_t note) noexcept {
        if (trackIdx < kNumTracks) {
            lastVoiceLedNotes_[trackIdx] = std::min<uint8_t>(note, 127);
        }
    }

    uint8_t trackLastVoiceLedNote(std::size_t trackIdx) const noexcept {
        return (trackIdx < kNumTracks) ? lastVoiceLedNotes_[trackIdx] : 60;
    }

    // Returns the strum delay in 960-PPQN ticks for chord voice index (0..5)
    uint32_t chordVoiceStrumDelayTicks(uint8_t voiceIndex) const noexcept {
        return static_cast<uint32_t>(voiceIndex) * static_cast<uint32_t>(context_.strumTimingTicks);
    }

private:
    void rebuildVoicingFromKnobs() noexcept {
        std::array<int, 4> intervals = {
            baseIntervals_[0], baseIntervals_[1], baseIntervals_[2], baseIntervals_[3]
        };

        // Apply Knob C (Modal Interchange):
        // > 96: Borrowed Minor iv / Phrygian (flatten 3rd and 7th)
        // > 64: Borrowed Dorian / Mixolydian (flatten 7th)
        // > 32: Lydian #4 / Tension 9th
        if (context_.modalInterchange >= 96) {
            if (intervals[1] == 4) intervals[1] = 3; // Major 3rd -> Minor 3rd
            if (intervals[3] == 11) intervals[3] = 10; // Maj7 -> Min7
            context_.scaleMask12 = ScaleMasks::kNaturalMinor;
        } else if (context_.modalInterchange >= 64) {
            if (intervals[3] == 11) intervals[3] = 10;
            context_.scaleMask12 = ScaleMasks::kMixolydian;
        } else if (context_.modalInterchange >= 32) {
            context_.scaleMask12 = ScaleMasks::kLydian;
        }

        // Apply Knob B (Inversion 0..3): rotate bottom `inv` voices up an octave (+12)
        for (uint8_t i = 0; i < context_.inversion && i < 4; ++i) {
            intervals[i] += 12;
        }
        std::sort(intervals.begin(), intervals.end());

        // Apply Knob A (Voicing Spread 0..3):
        // Spread 1: Drop-2 (raise 2nd highest voice by +12)
        // Spread 2: Open voicing (raise odd indices by +12)
        // Spread 3: Wide cinematic (spread across 2 octaves)
        if (context_.voicingSpread == 1) {
            intervals[2] += 12;
        } else if (context_.voicingSpread == 2) {
            intervals[1] += 12;
            intervals[3] += 12;
        } else if (context_.voicingSpread >= 3) {
            intervals[1] += 12;
            intervals[2] += 12;
            intervals[3] += 24;
        }
        std::sort(intervals.begin(), intervals.end());

        context_.activeChordSize = 4;
        for (std::size_t i = 0; i < 4; ++i) {
            const int note = static_cast<int>(baseChordRootMidi_) + intervals[i];
            context_.activeChordNotes[i] = static_cast<uint8_t>(std::clamp(note, 0, 127));
        }
        context_.bassNote = static_cast<uint8_t>(std::clamp(static_cast<int>(baseChordRootMidi_) - 24, 12, 60));
    }

    bool isNoteInScale(uint8_t midiNote) const noexcept {
        const int rel = ((static_cast<int>(midiNote) - static_cast<int>(context_.rootNote)) % 12 + 12) % 12;
        return ((context_.scaleMask12 >> rel) & 1U) != 0;
    }

    uint8_t quantizeToScale(uint8_t note) const noexcept {
        if (isNoteInScale(note)) return note;
        for (int dist = 1; dist <= 6; ++dist) {
            if (static_cast<int>(note) - dist >= 0 && isNoteInScale(static_cast<uint8_t>(note - dist))) {
                return static_cast<uint8_t>(note - dist);
            }
            if (static_cast<int>(note) + dist <= 127 && isNoteInScale(static_cast<uint8_t>(note + dist))) {
                return static_cast<uint8_t>(note + dist);
            }
        }
        return note;
    }

    uint8_t snapToChordTone(uint8_t note) const noexcept {
        if (context_.activeChordSize == 0) return note;
        const int targetOctaveBase = (static_cast<int>(note) / 12) * 12;
        int bestNote = context_.activeChordNotes[0];
        int bestDist = 999;

        for (uint8_t i = 0; i < context_.activeChordSize; ++i) {
            const int chordPitchClass = static_cast<int>(context_.activeChordNotes[i]) % 12;
            for (int octShift = -12; octShift <= 12; octShift += 12) {
                const int candidate = std::clamp(targetOctaveBase + chordPitchClass + octShift, 0, 127);
                const int dist = std::abs(candidate - static_cast<int>(note));
                if (dist < bestDist) {
                    bestDist = dist;
                    bestNote = candidate;
                }
            }
        }
        return static_cast<uint8_t>(bestNote);
    }

    uint8_t computeBassFollowNote(uint8_t inputNote) const noexcept {
        const int targetOctave = (static_cast<int>(inputNote) / 12) * 12;
        const int bassPitchClass = static_cast<int>(context_.bassNote) % 12;
        const int inputRel = ((static_cast<int>(inputNote) - bassPitchClass) % 12 + 12) % 12;
        // Preserve dominant fifth (+7 semitones) if bassline step is playing a 5th, otherwise lock to root
        const int interval = (inputRel == 7) ? 7 : 0;
        return static_cast<uint8_t>(std::clamp(targetOctave + bassPitchClass + interval, 0, 127));
    }

    uint8_t computeVoiceLedNote(uint8_t previousNote, uint8_t inputNote) const noexcept {
        if (context_.activeChordSize == 0) return inputNote;
        // Select a chord tone pitch class using inputNote's degree index, then place it in the octave
        // that minimizes semitone movement from `previousNote`.
        const uint8_t chordIdx = static_cast<uint8_t>(inputNote % context_.activeChordSize);
        const int chordPitchClass = static_cast<int>(context_.activeChordNotes[chordIdx]) % 12;
        const int prevOctaveBase = (static_cast<int>(previousNote) / 12) * 12;

        int bestCandidate = static_cast<int>(previousNote);
        int minMovement = 999;
        for (int octShift = -12; octShift <= 12; octShift += 12) {
            const int candidate = std::clamp(prevOctaveBase + chordPitchClass + octShift, 0, 127);
            const int movement = std::abs(candidate - static_cast<int>(previousNote));
            if (movement < minMovement) {
                minMovement = movement;
                bestCandidate = candidate;
            }
        }
        return static_cast<uint8_t>(bestCandidate);
    }

    std::size_t masterTrackIndex_{0};
    uint8_t baseChordRootMidi_{60};
    std::array<int8_t, 4> baseIntervals_{0, 4, 7, 11};
    HarmonicContext context_{};
    std::array<HarmonizerFollowMode, kNumTracks> followModes_{};
    std::array<uint8_t, kNumTracks> lastVoiceLedNotes_{};
};

} // namespace ignite

#endif // IGNITE_SEQUENCER_CORE_HPP
