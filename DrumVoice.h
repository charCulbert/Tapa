#pragma once

#include "Parameters.h"
#include "ResonatorBank.h"
#include "chardsp/chardsp_OscillatorPhase.h"
#include "chardsp/chardsp_SimpleDownsampler.h"
#include "chardsp/chardsp_DCBlocker.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace tapa
{

class DrumVoice
{
public:
    void prepare(double sampleRate) noexcept
    {
        rate = sampleRate * 4.0;
        noiseSmoothing = 1.0 - std::exp(-tau * 8000.0 / rate);
        for (auto& phase : phases) phase.prepare(rate);
        dc.prepare(sampleRate);
        dc.setCutoff(5.0);
        reset();
    }

    void reset() noexcept
    {
        active = false;
        visual.fill(0);
        previous = carry = age = feedbackSample = 0.0;
        downsampler.reset();
        dc.reset();
        metal.reset();
        for (auto& phase : phases) phase.reset();
    }

    void trigger(const std::array<double, parameters.size()>& values, float velocity, int note = 36) noexcept
    {
        for (size_t i = 0; i < p.size(); ++i)
            p[i] = std::isfinite(values[i])
                     ? std::clamp(values[i], parameters[i].minimum, parameters[i].maximum)
                     : parameters[i].defaultValue;
        tuning = 440.0 * std::exp2((std::clamp(note, 0, 127) - 69) / 12.0);
        noiseAmount = p[7] * .01;
        metalMix = std::clamp((noiseAmount - .1) / .5, 0.0, 1.0);
        metalMix = metalMix * metalMix * (3.0 - 2.0 * metalMix);
        // Cymbal material tracks time and pitch more gently than the FM drum body.
        const auto keyOffset = 60.0 - std::clamp(note, 0, 127);
        decayMs = p[0] * std::clamp(std::exp2(keyOffset * (1.0 - .75 * metalMix) / 24.0), 0.5, 2.0);
        strength = std::isfinite(velocity) ? std::clamp<double>(velocity, 0.0, 1.0) : 0.0;
        carry = previous;
        age = 0.0;
        active = true;
        drive = std::pow(10.0, p[3] / 20.0);
        punch = p[1] * 0.01;
        clickAmount = p[2] * 0.01;
        feedback = p[4] * 0.006;
        feedbackSample = noiseLow = noiseBandHigh = noiseBandLow = 0.0;
        const auto metalPitch = std::clamp(std::exp2(-keyOffset / 48.0), .5, 2.0);
        auto clusterBlend = std::clamp((noiseAmount - .9) * 10.0, 0.0, 1.0);
        clusterBlend *= clusterBlend * (3.0 - 2.0 * clusterBlend);
        clusterAmount = clusterBlend * 4.0 * clickAmount * (1.0 - clickAmount);
        auto noiseCutoff = (1.0 - metalMix) * std::clamp(tuning * p[5] * 8.0, 180.0, 14000.0)
                               + metalMix * std::clamp(6500 * std::pow(p[5] / 2.6, .15) * metalPitch, 3500.0, 14000.0);
        const auto clapCutoff = std::clamp(1800.0 * std::pow(p[5], .3) * metalPitch, 900.0, 4500.0);
        noiseCutoff += clusterAmount * (clapCutoff - noiseCutoff);
        noiseHighSmoothing = 1.0 - std::exp(-tau * noiseCutoff / rate);
        noiseLowSmoothing = 1.0 - std::exp(-tau * noiseCutoff * .2 / rate);
        noiseSeed = 0x74617061;
        metal.trigger(rate, 420 * metalPitch, p[5], punch, decayMs, p[4] * .01, clickAmount);
        for (auto& phase : phases) phase.reset();
    }

    const std::array<float, 3>& visualState() const noexcept { return visual; }
    double bodyDecayMs() const noexcept { return decayMs; }

    double next() noexcept
    {
        visual.fill(0);
        std::array<double, 4> samples {};
        for (auto& sample : samples)
        {
            if (!active) continue;
            const auto pitch = tuning * std::exp2(24.0 * decay(age, 24.0) / 12.0);
            phases[0].setFrequency(pitch);
            phases[1].setFrequency(tuning * p[5]);
            phases[2].setFrequency(tuning * p[5] * 4.0);
            const auto modulator = wave(phases[1].getPhase() + feedback * feedback * 1.5 * feedbackSample,
                                        punch * 0.3);
            // One oversampled-step delay bounds the feedback loop inside the oscillator.
            feedbackSample = modulator;
            const auto slap = 5.0 * punch * envelope(age, 0.3, p[6] >= 0.5 ? decayMs : 40.0 + feedback * 90.0)
                            * modulator;
            noiseSeed ^= noiseSeed << 13;
            noiseSeed ^= noiseSeed >> 17;
            noiseSeed ^= noiseSeed << 5;
            const auto noise = static_cast<double>(noiseSeed) / 2147483648.0 - 1.0;
            noiseLow += noiseSmoothing * (noise - noiseLow);
            auto morph = std::clamp((clickAmount - 0.35) / 0.65, 0.0, 1.0);
            morph = morph * morph * (3.0 - 2.0 * morph);
            const auto metallic = std::sin(tau * phases[2].getPhase()
                                         + noiseLow * morph * 12.0 + slap * morph);
            const auto click = (1.0 - clusterAmount) * std::min(1.0, clickAmount * 3.0)
                             * envelope(age, 0.1, 2.0 + 38.0 * clickAmount * clickAmount)
                             * ((1.0 - morph) * noiseLow + morph * ((1.0 - metalMix) * metallic + metalMix * noise));
            const auto bodyLevel = envelope(age, 0.25, decayMs) * strength;
            visual = { static_cast<float>(bodyLevel), static_cast<float>(slap), static_cast<float>(click) };
            noiseBandHigh += noiseHighSmoothing * (noise - noiseBandHigh);
            noiseBandLow += noiseLowSmoothing * (noise - noiseBandLow);
            const auto noiseTail = (noiseBandHigh - noiseBandLow) * 2.5;
            auto washLevel = envelope(age, .25 + metalMix * 8 * (1.0 - clickAmount),
                                           decayMs * (1.0 - metalMix * clickAmount * .55)) * strength;
            if (clusterAmount > 0.0)
            {
                // Separated noise strikes merge into a softer tail for clap-like articulation.
                const auto spacing = .007 + .012 * clickAmount;
                const auto burst = [&](double start, double duration) {
                    return age < start ? 0.0 : envelope(age - start, .15, duration);
                };
                const auto cluster = .85 * burst(0, 10) + .7 * burst(spacing, 10)
                                   + burst(spacing * 2, 10) + .55 * burst(spacing * 2, decayMs * .7);
                washLevel += clusterAmount * (cluster * strength - washLevel);
            }
            const auto ringing = metalMix > 0 && noiseAmount < 1
                ? metal.next(noise * envelope(age, .1, 3 + 45 * (1.0 - clickAmount))) : 0.0;
            visual[2] += static_cast<float>(noiseTail * noiseAmount * washLevel
                                         + ringing * metalMix * (1.0 - noiseAmount) * strength);
            const auto body = std::sin(tau * phases[0].getPhase() + slap);
            const auto tonalLevel = (1.0 - metalMix) * (1.0 - noiseAmount);
            const auto shaped = std::tanh(body * drive) * bodyLevel * tonalLevel
                              + std::tanh(ringing * drive) * metalMix * (1.0 - noiseAmount) * strength
                              + std::tanh(noiseTail * drive) * noiseAmount * washLevel
                              + std::tanh(click * drive * 0.65) * strength;
            // Preserve the outgoing sample while the phase-reset body fades in.
            const auto blend = std::min(1.0, age / 0.001);
            sample = shaped * blend + carry * (1.0 - blend);
            previous = sample;
            for (auto& phase : phases) (void) phase.advance();
            age += 1.0 / rate;
            if (age > decayMs * 0.002 + 0.01 + clusterAmount * .04) active = false;
        }
        return 0.5 * std::tanh(dc.process(downsampler.process(samples)));
    }

private:
    static double decay(double seconds, double milliseconds) noexcept
    {
        return std::exp(-6.90775527898 * seconds / (milliseconds * 0.001));
    }

    static double envelope(double seconds, double attackMs, double decayMs) noexcept
    {
        const auto attack = attackMs * 0.001;
        return seconds < attack ? seconds / attack : decay(seconds - attack, decayMs);
    }

    static double wave(double phase, double shape) noexcept
    {
        const auto sine = std::sin(tau * phase);
        // Blend sine and third harmonic before phase modulation.
        return (sine + shape * 0.5 * std::sin(3.0 * tau * phase)) / (1.0 + shape * 0.5);
    }

    static constexpr double tau = 6.28318530717958647692;
    std::array<chardsp::OscillatorPhase<double>, 3> phases;
    chardsp::SimpleDownsampler<double, 4> downsampler;
    chardsp::DCBlocker<double> dc;
    ResonatorBank metal;
    std::array<double, parameters.size()> p {};
    std::array<float, 3> visual {};
    double rate = 192000.0, age = 0.0, strength = 0.0, tuning = 1.0, decayMs = 280.0;
    double previous = 0.0, carry = 0.0, drive = 1.0, punch = 0.5, clickAmount = 0.3;
    double feedback = 0.0, feedbackSample = 0.0;
    uint32_t noiseSeed = 0x74617061;
    double noiseLow = 0.0, noiseSmoothing = 0.2;
    double clusterAmount = 0.0;
    double noiseAmount = 0.0, metalMix = 0.0, noiseBandHigh = 0.0, noiseBandLow = 0.0;
    double noiseHighSmoothing = 0.0, noiseLowSmoothing = 0.0;
    bool active = false;
};

} // namespace tapa
