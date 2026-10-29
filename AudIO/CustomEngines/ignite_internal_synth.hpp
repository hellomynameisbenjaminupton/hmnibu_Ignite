/*
 * Copyright (c) 2026 Behringer IGNITE (BE 0716-AAF) Custom Firmware Contributors
 * Ported & adapted from the companion Behringer KICK (BE 0700-AAM) Custom Firmware Core.
 *
 * This file is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#ifndef IGNITE_INTERNAL_SYNTH_HPP
#define IGNITE_INTERNAL_SYNTH_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ignite {

constexpr std::size_t kNumInternalSynthVoices = 8;
constexpr std::size_t kNumFMOperators = 4;
constexpr std::size_t kGranularBufferSize = 4096;
constexpr float kTwoPi = 6.28318530717958647692f;

inline float midiNoteToHz(uint8_t note) noexcept {
    return 440.0f * std::pow(2.0f, (static_cast<int>(note) - 69) / 12.0f);
}

// ============================================================================
// Enhancement 8 (Part 2A): 4-Operator, 8-Algorithm FM Voice (`FM4Voice`)
// ============================================================================
struct FMOperatorParams {
    float ratio{1.0f};       // Frequency multiplier (0.5 .. 16.0)
    float level{0.8f};       // Output / modulation index (0.0 .. 4.0)
    float attackSec{0.005f}; // Attack time in seconds
    float decaySec{0.35f};   // Decay/Release time in seconds
};

class FM4Voice {
public:
    FM4Voice() noexcept { reset(); }

    void reset() noexcept {
        active_ = false;
        note_ = 60;
        baseFreqHz_ = 261.6256f;
        velocityGain_ = 0.8f;
        algorithm_ = 0;
        op4Feedback_ = 0.25f;
        prevOp4Sample_ = 0.0f;
        phases_.fill(0.0f);
        envLevels_.fill(0.0f);
        inAttack_.fill(false);

        ops_[0] = FMOperatorParams{1.0f, 1.0f, 0.003f, 0.45f};
        ops_[1] = FMOperatorParams{2.0f, 1.4f, 0.003f, 0.25f};
        ops_[2] = FMOperatorParams{3.0f, 0.9f, 0.002f, 0.18f};
        ops_[3] = FMOperatorParams{4.0f, 0.7f, 0.001f, 0.12f};
    }

    void setAlgorithm(uint8_t alg0To7) noexcept {
        algorithm_ = static_cast<uint8_t>(alg0To7 & 0x07U);
    }
    uint8_t algorithm() const noexcept { return algorithm_; }

    void setOp4Feedback(float fb) noexcept {
        op4Feedback_ = std::clamp(fb, 0.0f, 1.5f);
    }

    FMOperatorParams& operatorParams(std::size_t opIdx0To3) noexcept {
        return ops_[opIdx0To3 & 0x03U];
    }

    void noteOn(uint8_t midiNote, uint8_t velocity, uint8_t alg0To7 = 0) noexcept {
        note_ = std::min<uint8_t>(midiNote, 127);
        baseFreqHz_ = midiNoteToHz(note_);
        velocityGain_ = static_cast<float>(std::clamp<uint8_t>(velocity, 1, 127)) / 127.0f;
        algorithm_ = static_cast<uint8_t>(alg0To7 & 0x07U);
        phases_.fill(0.0f);
        envLevels_.fill(0.0f);
        inAttack_.fill(true);
        prevOp4Sample_ = 0.0f;
        active_ = true;
    }

    void noteOff() noexcept {
        inAttack_.fill(false);
    }

    bool isActive() const noexcept { return active_; }
    uint8_t currentNote() const noexcept { return note_; }

    float renderSample(float sampleRate) noexcept {
        if (!active_) return 0.0f;

        bool anyAlive = false;
        std::array<float, kNumFMOperators> env{};
        for (std::size_t i = 0; i < kNumFMOperators; ++i) {
            if (inAttack_[i]) {
                const float step = 1.0f / std::max(1.0f, ops_[i].attackSec * sampleRate);
                envLevels_[i] += step;
                if (envLevels_[i] >= 1.0f) {
                    envLevels_[i] = 1.0f;
                    inAttack_[i] = false;
                }
                anyAlive = true;
            } else {
                const float step = 1.0f / std::max(1.0f, ops_[i].decaySec * sampleRate);
                envLevels_[i] = std::max(0.0f, envLevels_[i] - step);
                if (envLevels_[i] > 1e-4f) anyAlive = true;
            }
            env[i] = envLevels_[i] * ops_[i].level;
        }

        if (!anyAlive) {
            active_ = false;
            return 0.0f;
        }

        // Advance operator phases
        for (std::size_t i = 0; i < kNumFMOperators; ++i) {
            const float freq = baseFreqHz_ * ops_[i].ratio;
            phases_[i] += (kTwoPi * freq) / sampleRate;
            if (phases_[i] >= kTwoPi) phases_[i] -= kTwoPi;
        }

        // Operator 4 (index 3) with self-feedback
        const float out4 = std::sin(phases_[3] + prevOp4Sample_ * op4Feedback_) * env[3];
        prevOp4Sample_ = out4;

        float out = 0.0f;
        switch (algorithm_ & 0x07U) {
            case 0: { // Alg 0: Op4 -> Op3 -> Op2 -> Op1
                const float out3 = std::sin(phases_[2] + out4) * env[2];
                const float out2 = std::sin(phases_[1] + out3) * env[1];
                out = std::sin(phases_[0] + out2) * env[0];
                break;
            }
            case 1: { // Alg 1: (Op4 + Op3) -> Op2 -> Op1
                const float out3 = std::sin(phases_[2]) * env[2];
                const float out2 = std::sin(phases_[1] + out4 + out3) * env[1];
                out = std::sin(phases_[0] + out2) * env[0];
                break;
            }
            case 2: { // Alg 2: ((Op4 -> Op3) + Op2) -> Op1
                const float out3 = std::sin(phases_[2] + out4) * env[2];
                const float out2 = std::sin(phases_[1]) * env[1];
                out = std::sin(phases_[0] + out3 + out2) * env[0];
                break;
            }
            case 3: { // Alg 3: Dual 2-Op Stacks (Op4 -> Op3) + (Op2 -> Op1)
                const float out3 = std::sin(phases_[2] + out4) * env[2];
                const float out2 = std::sin(phases_[1]) * env[1];
                const float out1 = std::sin(phases_[0] + out2) * env[0];
                out = 0.5f * (out1 + out3);
                break;
            }
            case 4: { // Alg 4: Op4 -> (Op1 + Op2 + Op3)
                const float out3 = std::sin(phases_[2] + out4) * env[2];
                const float out2 = std::sin(phases_[1] + out4) * env[1];
                const float out1 = std::sin(phases_[0] + out4) * env[0];
                out = (out1 + out2 + out3) / 3.0f;
                break;
            }
            case 5: { // Alg 5: Op4 -> Op3 -> (Op1 + Op2)
                const float out3 = std::sin(phases_[2] + out4) * env[2];
                const float out2 = std::sin(phases_[1] + out3) * env[1];
                const float out1 = std::sin(phases_[0] + out3) * env[0];
                out = 0.5f * (out1 + out2);
                break;
            }
            case 6: { // Alg 6: (Op4 -> Op3) + Op2 + Op1
                const float out3 = std::sin(phases_[2] + out4) * env[2];
                const float out2 = std::sin(phases_[1]) * env[1];
                const float out1 = std::sin(phases_[0]) * env[0];
                out = (out1 + out2 + out3) / 3.0f;
                break;
            }
            case 7: { // Alg 7: Pure 4-Operator Additive (Op1 + Op2 + Op3 + Op4)
                const float out3 = std::sin(phases_[2]) * env[2];
                const float out2 = std::sin(phases_[1]) * env[1];
                const float out1 = std::sin(phases_[0]) * env[0];
                out = 0.25f * (out1 + out2 + out3 + out4);
                break;
            }
        }

        return std::clamp(out * velocityGain_, -1.0f, 1.0f);
    }

private:
    bool active_{false};
    uint8_t note_{60};
    float baseFreqHz_{261.6256f};
    float velocityGain_{0.8f};
    uint8_t algorithm_{0};
    float op4Feedback_{0.25f};
    float prevOp4Sample_{0.0f};
    std::array<FMOperatorParams, kNumFMOperators> ops_{};
    std::array<float, kNumFMOperators> phases_{};
    std::array<float, kNumFMOperators> envLevels_{};
    std::array<bool, kNumFMOperators> inAttack_{};
};

// ============================================================================
// Enhancement 8 (Part 2B): Ported KICK Granular & Analog Drum Voice (`GranularVoice`)
// ============================================================================
class GranularVoice {
public:
    GranularVoice() noexcept {
        initSourceBuffer();
        reset();
    }

    void reset() noexcept {
        active_ = false;
        note_ = 60;
        pitchRatio_ = 1.0f;
        velocityGain_ = 0.8f;
        grainSizeSamples_ = 1024.0f;
        scanPosition_ = 0.0f;
        grainPhase_ = 0.0f;
        ampEnv_ = 0.0f;
    }

    void setGrainParameters(float grainSizeMs, float scanRate) noexcept {
        grainSizeSamples_ = std::clamp(grainSizeMs * 48.0f, 128.0f, 4096.0f);
        scanRate_ = std::clamp(scanRate, 0.05f, 4.0f);
    }

    void triggerGrainCloud(uint8_t midiNote, uint8_t velocity) noexcept {
        note_ = std::min<uint8_t>(midiNote, 127);
        pitchRatio_ = std::pow(2.0f, (static_cast<int>(note_) - 60) / 12.0f);
        velocityGain_ = static_cast<float>(std::clamp<uint8_t>(velocity, 1, 127)) / 127.0f;
        grainPhase_ = 0.0f;
        ampEnv_ = 1.0f;
        active_ = true;
    }

    bool isActive() const noexcept { return active_; }

    float renderSample(float sampleRate) noexcept {
        if (!active_) return 0.0f;

        // Hann window envelope for current grain
        const float normGrainPhase = grainPhase_ / std::max(1.0f, grainSizeSamples_);
        const float hannWindow = 0.5f * (1.0f - std::cos(kTwoPi * normGrainPhase));

        const std::size_t readIdx = static_cast<std::size_t>(scanPosition_ + grainPhase_ * pitchRatio_) % kGranularBufferSize;
        const float sample = sourceBuffer_[readIdx] * hannWindow * ampEnv_ * velocityGain_;

        grainPhase_ += 1.0f;
        if (grainPhase_ >= grainSizeSamples_) {
            grainPhase_ = 0.0f;
            scanPosition_ = std::fmod(scanPosition_ + 137.0f * scanRate_, static_cast<float>(kGranularBufferSize));
        }

        // Overall cloud decay
        const float decayStep = 1.0f / std::max(1.0f, 0.55f * sampleRate);
        ampEnv_ -= decayStep;
        if (ampEnv_ <= 0.0f) {
            ampEnv_ = 0.0f;
            active_ = false;
        }
        return std::clamp(sample, -1.0f, 1.0f);
    }

private:
    void initSourceBuffer() noexcept {
        // Rich harmonic multi-partial wavetable for granular scrubbing
        for (std::size_t i = 0; i < kGranularBufferSize; ++i) {
            const float phase = (kTwoPi * static_cast<float>(i)) / 256.0f;
            sourceBuffer_[i] = 0.55f * std::sin(phase) +
                               0.25f * std::sin(2.01f * phase) +
                               0.15f * std::sin(3.99f * phase) +
                               0.05f * std::sin(7.03f * phase);
        }
    }

    bool active_{false};
    uint8_t note_{60};
    float pitchRatio_{1.0f};
    float velocityGain_{0.8f};
    float grainSizeSamples_{1024.0f};
    float scanRate_{1.0f};
    float scanPosition_{0.0f};
    float grainPhase_{0.0f};
    float ampEnv_{0.0f};
    std::array<float, kGranularBufferSize> sourceBuffer_{};
};

// ============================================================================
// Integrated 8-Voice Internal Synth Engine (`IgniteInternalSynthEngine`)
// ============================================================================
enum class InternalSynthPatchType : uint8_t {
    Off = 0,
    FM4Op,
    GranularCloud,
    KickAnalogDrum
};

class IgniteInternalSynthEngine {
public:
    IgniteInternalSynthEngine() noexcept { reset(); }

    void reset() noexcept {
        sampleRate_ = 48000.0f;
        nextVoiceCursor_ = 0;
        for (auto& v : fmVoices_) v.reset();
        for (auto& g : granularVoices_) g.reset();
    }

    void setSampleRate(float sr) noexcept {
        sampleRate_ = std::clamp(sr, 8000.0f, 192000.0f);
    }

    void triggerPatch(InternalSynthPatchType patch, uint8_t midiNote, uint8_t velocity, uint8_t fmAlg = 0) noexcept {
        if (patch == InternalSynthPatchType::Off || velocity == 0) return;

        const std::size_t slot = nextVoiceCursor_ % kNumInternalSynthVoices;
        ++nextVoiceCursor_;

        if (patch == InternalSynthPatchType::FM4Op) {
            fmVoices_[slot].noteOn(midiNote, velocity, fmAlg);
        } else if (patch == InternalSynthPatchType::GranularCloud) {
            granularVoices_[slot].triggerGrainCloud(midiNote, velocity);
        } else if (patch == InternalSynthPatchType::KickAnalogDrum) {
            // Configure FM voice slot as punchy pitch-envelope KICK drum synth
            auto& drum = fmVoices_[slot];
            drum.operatorParams(0) = FMOperatorParams{1.0f, 1.2f, 0.001f, 0.28f};
            drum.operatorParams(1) = FMOperatorParams{1.5f, 2.2f, 0.0005f, 0.045f};
            drum.noteOn(std::clamp<uint8_t>(midiNote, 24, 60), velocity, 0);
        }
    }

    void renderStereoBlock(float* outLeft, float* outRight, std::size_t numFrames) noexcept {
        if (!outLeft || !outRight) return;
        for (std::size_t f = 0; f < numFrames; ++f) {
            float mixL = 0.0f;
            float mixR = 0.0f;
            for (std::size_t v = 0; v < kNumInternalSynthVoices; ++v) {
                const float fmSample = fmVoices_[v].renderSample(sampleRate_);
                const float grSample = granularVoices_[v].renderSample(sampleRate_);
                const float pan = (v & 1U) ? 0.65f : 0.35f;
                const float combined = (fmSample + grSample) * 0.35f;
                mixL += combined * (1.0f - pan);
                mixR += combined * pan;
            }
            outLeft[f] = std::clamp(mixL, -1.0f, 1.0f);
            outRight[f] = std::clamp(mixR, -1.0f, 1.0f);
        }
    }

    FM4Voice& fmVoice(std::size_t idx) noexcept { return fmVoices_[idx % kNumInternalSynthVoices]; }
    GranularVoice& granularVoice(std::size_t idx) noexcept { return granularVoices_[idx % kNumInternalSynthVoices]; }

private:
    float sampleRate_{48000.0f};
    std::size_t nextVoiceCursor_{0};
    std::array<FM4Voice, kNumInternalSynthVoices> fmVoices_{};
    std::array<GranularVoice, kNumInternalSynthVoices> granularVoices_{};
};

} // namespace ignite

#endif // IGNITE_INTERNAL_SYNTH_HPP
