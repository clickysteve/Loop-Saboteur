// =============================================================================
//  OutputStage.h
//
//  Global post-mix character/colorisation stage. Eleven modes, plus a wet/dry
//  mix. Sits at the very end of the chain (after softLimit, before lookahead).
//
//  Header-only by design so the existing CMakeLists doesn't need to grow a
//  third source file. The DSP is kept self-contained and lightweight: each
//  mode has its own per-sample inline branch driven from primitives defined
//  at the top of this file (one-pole filters, peaking EQ, soft sat, delay
//  line wow/flutter, glue comp, click/crackle generator).
//
//  Threading: setMode() / setMix() are atomic and may be called from the UI
//  thread. process() reads them once at the start of each sample via local
//  caches. State (filter histories, oscillator phases, RNG) is touched only
//  from the audio thread.
// =============================================================================
#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdint>

namespace loopsab {

// -----------------------------------------------------------------------------
//  Mode enum + naming. Index order matches the menu and the preset XML —
//  do NOT reorder without bumping the Engine block version.
// -----------------------------------------------------------------------------
enum OutputStageMode
{
    kOutClean = 0,
    kOutCassette,
    kOutReelToReel,
    kOutDamaged,
    kOutVinyl,
    kOutBoombox,
    kOutAMRadio,
    kOutVHS,
    kOutBusComp,
    kOutLathe,
    kOutPhone,
    kNumOutputStages
};

inline const char* outputStageShortName (int m) noexcept
{
    switch (m)
    {
        case kOutClean:      return "Clean";
        case kOutCassette:   return "Cassette";
        case kOutReelToReel: return "Reel-to-Reel";
        case kOutDamaged:    return "Damaged";
        case kOutVinyl:      return "Vinyl";
        case kOutBoombox:    return "Boombox";
        case kOutAMRadio:    return "AM Radio";
        case kOutVHS:        return "VHS Hi-Fi";
        case kOutBusComp:    return "Bus Comp";
        case kOutLathe:      return "Lathe";
        case kOutPhone:      return "Phone Bus";
        default:             return "?";
    }
}

inline const char* outputStageDescription (int m) noexcept
{
    switch (m)
    {
        case kOutClean:      return "true bypass — no colour";
        case kOutCassette:   return "C90 wow + flutter + hiss";
        case kOutReelToReel: return "slow wow, head bump, glue";
        case kOutDamaged:    return "pitch dropouts, wide flutter";
        case kOutVinyl:      return "surface crackle + LP wobble";
        case kOutBoombox:    return "plastic-cabinet bandpass";
        case kOutAMRadio:    return "300 Hz–4 kHz, occasional static";
        case kOutVHS:        return "AGC pumping, narrow band";
        case kOutBusComp:    return "2-bus glue, no noise";
        case kOutLathe:      return "dub-plate emphasis";
        case kOutPhone:      return "voicemail bandpass + μ";
        default:             return "";
    }
}

// -----------------------------------------------------------------------------
//  Primitives
// -----------------------------------------------------------------------------

// One-pole low-pass. coeff = exp(-2π·fc/fs). y = y + a·(x - y) where a = 1-coeff.
struct OnePoleLP
{
    float a = 0.0f, y = 0.0f;
    void prepare (double fs, float fc) noexcept
    {
        const float coeff = std::exp (-juce::MathConstants<float>::twoPi
                                       * fc / (float) fs);
        a = 1.0f - coeff;
    }
    void reset() noexcept { y = 0.0f; }
    inline float process (float x) noexcept
    {
        y += a * (x - y);
        return y;
    }
};

// One-pole high-pass = input - low-pass output.
struct OnePoleHP
{
    OnePoleLP lp;
    void prepare (double fs, float fc) noexcept { lp.prepare (fs, fc); }
    void reset() noexcept { lp.reset(); }
    inline float process (float x) noexcept { return x - lp.process (x); }
};

// Simple biquad peaking EQ using RBJ cookbook coefficients (computed once).
struct BiquadPeak
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1L = 0, z2L = 0, z1R = 0, z2R = 0;

    void prepare (double fs, float freq, float Q, float gainDb) noexcept
    {
        const double w0    = juce::MathConstants<double>::twoPi * freq / fs;
        const double cosW0 = std::cos (w0);
        const double sinW0 = std::sin (w0);
        const double A     = std::pow (10.0, gainDb / 40.0);
        const double alpha = sinW0 / (2.0 * Q);

        const double b0d = 1.0 + alpha * A;
        const double b1d = -2.0 * cosW0;
        const double b2d = 1.0 - alpha * A;
        const double a0d = 1.0 + alpha / A;
        const double a1d = -2.0 * cosW0;
        const double a2d = 1.0 - alpha / A;

        b0 = (float) (b0d / a0d);
        b1 = (float) (b1d / a0d);
        b2 = (float) (b2d / a0d);
        a1 = (float) (a1d / a0d);
        a2 = (float) (a2d / a0d);
    }

    void reset() noexcept { z1L = z2L = z1R = z2R = 0.0f; }

    inline float processL (float x) noexcept
    {
        // Transposed Direct Form II.
        const float y = b0 * x + z1L;
        z1L = b1 * x - a1 * y + z2L;
        z2L = b2 * x - a2 * y;
        return y;
    }
    inline float processR (float x) noexcept
    {
        const float y = b0 * x + z1R;
        z1R = b1 * x - a1 * y + z2R;
        z2R = b2 * x - a2 * y;
        return y;
    }
};

// Low shelf via RBJ cookbook.
struct BiquadLowShelf
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1L = 0, z2L = 0, z1R = 0, z2R = 0;

    void prepare (double fs, float freq, float gainDb, float slope = 1.0f) noexcept
    {
        const double w0    = juce::MathConstants<double>::twoPi * freq / fs;
        const double cosW0 = std::cos (w0);
        const double sinW0 = std::sin (w0);
        const double A     = std::pow (10.0, gainDb / 40.0);
        const double alpha = (sinW0 / 2.0)
                              * std::sqrt ((A + 1.0 / A) * (1.0 / slope - 1.0) + 2.0);
        const double sqrtA2alpha = 2.0 * std::sqrt (A) * alpha;

        const double b0d =     A * ((A + 1) - (A - 1) * cosW0 + sqrtA2alpha);
        const double b1d = 2 * A * ((A - 1) - (A + 1) * cosW0);
        const double b2d =     A * ((A + 1) - (A - 1) * cosW0 - sqrtA2alpha);
        const double a0d =          (A + 1) + (A - 1) * cosW0 + sqrtA2alpha;
        const double a1d = -2 *    ((A - 1) + (A + 1) * cosW0);
        const double a2d =          (A + 1) + (A - 1) * cosW0 - sqrtA2alpha;

        b0 = (float) (b0d / a0d);
        b1 = (float) (b1d / a0d);
        b2 = (float) (b2d / a0d);
        a1 = (float) (a1d / a0d);
        a2 = (float) (a2d / a0d);
    }

    void reset() noexcept { z1L = z2L = z1R = z2R = 0.0f; }

    inline float processL (float x) noexcept
    {
        const float y = b0 * x + z1L;
        z1L = b1 * x - a1 * y + z2L;
        z2L = b2 * x - a2 * y;
        return y;
    }
    inline float processR (float x) noexcept
    {
        const float y = b0 * x + z1R;
        z1R = b1 * x - a1 * y + z2R;
        z2R = b2 * x - a2 * y;
        return y;
    }
};

// Soft tanh-ish saturator with adjustable drive.
inline float softSat (float x, float drive = 1.0f) noexcept
{
    return std::tanh (x * drive) / std::tanh (drive);
}

// Tiny short delay line for wow/flutter pitch wobble. Read with linear interp
// at a fractional sample offset modulated by an LFO.
template <int Capacity>
struct ShortDelay
{
    std::array<float, Capacity> buf {};
    int writePos = 0;

    void reset() noexcept { buf.fill (0.0f); writePos = 0; }

    inline void write (float x) noexcept
    {
        buf[writePos] = x;
        if (++writePos >= Capacity) writePos = 0;
    }

    // delaySamples: positive, fractional. < Capacity-1.
    inline float read (float delaySamples) const noexcept
    {
        float pos = (float) writePos - delaySamples;
        while (pos < 0) pos += Capacity;
        const int   i0   = (int) pos;
        const int   i1   = (i0 + 1 >= Capacity) ? 0 : i0 + 1;
        const float frac = pos - (float) i0;
        return buf[i0] + frac * (buf[i1] - buf[i0]);
    }
};

// Cheap envelope follower (peak detector with attack/release ms).
struct EnvFollower
{
    float aAtt = 0.0f, aRel = 0.0f, env = 0.0f;
    void prepare (double fs, float attMs, float relMs) noexcept
    {
        aAtt = std::exp (-1.0f / (0.001f * attMs * (float) fs));
        aRel = std::exp (-1.0f / (0.001f * relMs * (float) fs));
    }
    void reset() noexcept { env = 0.0f; }
    inline float process (float x) noexcept
    {
        const float ax = std::abs (x);
        const float a = (ax > env) ? aAtt : aRel;
        env = a * env + (1.0f - a) * ax;
        return env;
    }
};

// =============================================================================
//  OutputStage — the actual processor.
// =============================================================================
class OutputStage
{
public:
    OutputStage() = default;

    void prepare (double sampleRate) noexcept
    {
        fs = sampleRate;

        // Cassette filters
        cassetteHP60     .prepare (fs, 60.0f);
        cassetteLP9k     .prepare (fs, 9000.0f);
        cassetteHissLP   .prepare (fs, 6000.0f);  // shape white -> pinkish
        // Reel
        reelLP14k        .prepare (fs, 14000.0f);
        reelShelf        .prepare (fs, 80.0f, 2.0f);
        // Damaged
        damagedLP7k      .prepare (fs, 7000.0f);
        damagedHissLP    .prepare (fs, 4000.0f);
        damagedAGC       .prepare (fs, 5.0f, 60.0f);
        // Vinyl
        vinylRiaaHP      .prepare (fs, 30.0f);
        vinylTilt        .prepare (fs, 8000.0f, 0.7f, 1.5f); // gentle HF lift
        // Boombox
        boomboxHP150     .prepare (fs, 150.0f);
        boomboxLP8k      .prepare (fs, 8000.0f);
        boomboxHonk      .prepare (fs, 1200.0f, 1.2f, 4.0f);
        // AM radio
        amHP300          .prepare (fs, 300.0f);
        amLP4k           .prepare (fs, 4000.0f);
        amHissLP         .prepare (fs, 3500.0f);   // shape "broadcast band" hiss
        amProgEnv        .prepare (fs, 8.0f, 220.0f);
        // VHS
        vhsHP50          .prepare (fs, 50.0f);
        vhsLP12k         .prepare (fs, 12000.0f);
        vhsAGC           .prepare (fs, 3.0f, 80.0f);
        // Bus Comp
        busAGC           .prepare (fs, 12.0f, 180.0f);
        // Lathe
        latheBoost       .prepare (fs, 8000.0f, 0.7f, 3.0f);
        latheLP15k       .prepare (fs, 15000.0f);
        // Phone
        phoneHP400       .prepare (fs, 400.0f);
        phoneLP3k4       .prepare (fs, 3400.0f);

        wowDelay.reset();
        reset();
    }

    void reset() noexcept
    {
        cassetteHP60.reset(); cassetteLP9k.reset(); cassetteHissLP.reset();
        reelLP14k.reset(); reelShelf.reset();
        damagedLP7k.reset(); damagedHissLP.reset(); damagedAGC.reset();
        vinylRiaaHP.reset(); vinylTilt.reset();
        boomboxHP150.reset(); boomboxLP8k.reset(); boomboxHonk.reset();
        amHP300.reset(); amLP4k.reset(); amHissLP.reset(); amProgEnv.reset();
        amWashEnv = amWashTarget = 0.0f; amWashHold = 0;
        vhsHP50.reset(); vhsLP12k.reset(); vhsAGC.reset();
        busAGC.reset();
        latheBoost.reset(); latheLP15k.reset();
        phoneHP400.reset(); phoneLP3k4.reset();
        wowPhase = flutterPhase = vinylPhase = 0.0f;
        clickCountdown = staticCountdown = skipCountdown = 0;
        skipHoldL = skipHoldR = 0.0f; skipHoldSamples = 0;
        wowDelay.reset();
        rngState = 0xCAFE15B0u;
    }

    // Atomic setters — UI thread.
    void setMode (int m) noexcept
    {
        mode.store (juce::jlimit (0, (int) kNumOutputStages - 1, m));
    }
    void setMix (float m) noexcept
    {
        mix.store (juce::jlimit (0.0f, 1.0f, m));
    }
    // v0.7.0 — intensity scales how hard the output stage processing
    // is applied within the wet path. 0 = minimal colour, 1 = full effect.
    void setIntensity (float v) noexcept
    {
        intensity.store (juce::jlimit (0.0f, 1.0f, v));
    }
    int   getMode()      const noexcept { return mode.load(); }
    float getMix ()      const noexcept { return mix.load(); }
    float getIntensity() const noexcept { return intensity.load(); }

    // Per-sample, in-place, stereo. Called after softLimit, before lookahead.
    inline void process (float& L, float& R) noexcept
    {
        const int   m = mode.load (std::memory_order_relaxed);
        const float w = mix .load (std::memory_order_relaxed);
        const float k = intensity.load (std::memory_order_relaxed);

        if (m == kOutClean || w <= 0.0001f)
            return;

        const float dryL = L, dryR = R;
        float wetL = L, wetR = R;

        switch (m)
        {
            case kOutCassette:   processCassette   (wetL, wetR); break;
            case kOutReelToReel: processReel       (wetL, wetR); break;
            case kOutDamaged:    processDamaged    (wetL, wetR); break;
            case kOutVinyl:      processVinyl      (wetL, wetR); break;
            case kOutBoombox:    processBoombox    (wetL, wetR); break;
            case kOutAMRadio:    processAMRadio    (wetL, wetR); break;
            case kOutVHS:        processVHS        (wetL, wetR); break;
            case kOutBusComp:    processBusComp    (wetL, wetR); break;
            case kOutLathe:      processLathe      (wetL, wetR); break;
            case kOutPhone:      processPhone      (wetL, wetR); break;
            default: break;
        }

        // v0.7.0 — intensity scales how much of the mode's processing
        // is applied before the wet/dry mix. At k=0 the wet path equals
        // dry (no character), at k=1 you get the full effect.
        wetL = dryL + (wetL - dryL) * k;
        wetR = dryR + (wetR - dryR) * k;

        L = dryL + (wetL - dryL) * w;
        R = dryR + (wetR - dryR) * w;
    }

private:
    // -------- Per-mode DSP --------
    inline void processCassette (float& L, float& R) noexcept
    {
        // v0.7.0 — boosted wow/flutter/hiss/sat for more obvious character.
        wowDelay.write (0.5f * (L + R));
        wowPhase     += (float) (juce::MathConstants<double>::twoPi * 0.4 / fs);
        flutterPhase += (float) (juce::MathConstants<double>::twoPi * 8.0 / fs);
        if (wowPhase     > juce::MathConstants<float>::twoPi) wowPhase     -= juce::MathConstants<float>::twoPi;
        if (flutterPhase > juce::MathConstants<float>::twoPi) flutterPhase -= juce::MathConstants<float>::twoPi;
        const float wow  = std::sin (wowPhase)     * 10.0f;   // ±10 samples (was ±6)
        const float flut = std::sin (flutterPhase) * 3.0f;    // ±3 samples (was ±1.5)
        const float jit  = (whiteNoise() * 0.8f);
        const float dly  = juce::jmax (1.0f, 24.0f + wow + flut + jit);
        const float wobbleMono = wowDelay.read (dly);

        L = 0.75f * L + 0.25f * wobbleMono;   // was 85/15
        R = 0.75f * R + 0.25f * wobbleMono;

        // EQ shape.
        L = cassetteHP60.process (L); R = cassetteHP60.process (R);
        L = cassetteLP9k.process (L); R = cassetteLP9k.process (R);

        // Pinkish hiss.
        const float hiss = cassetteHissLP.process (whiteNoise()) * 0.008f;  // was 0.0035
        L += hiss; R += hiss;

        // Tape saturation.
        L = softSat (L, 1.8f);   // was 1.4
        R = softSat (R, 1.8f);
    }

    inline void processReel (float& L, float& R) noexcept
    {
        // v0.7.0 — boosted wow + saturation for more audible character.
        wowDelay.write (0.5f * (L + R));
        wowPhase += (float) (juce::MathConstants<double>::twoPi * 0.25 / fs);
        if (wowPhase > juce::MathConstants<float>::twoPi) wowPhase -= juce::MathConstants<float>::twoPi;
        const float wow = std::sin (wowPhase) * 5.0f;          // was ±3
        const float dly = juce::jmax (1.0f, 16.0f + wow);
        const float wobbleMono = wowDelay.read (dly);
        L = 0.85f * L + 0.15f * wobbleMono;                    // was 92/8
        R = 0.85f * R + 0.15f * wobbleMono;

        // Head bump + gentle HF roll.
        L = reelShelf.processL (L); R = reelShelf.processR (R);
        L = reelLP14k.process (L);  R = reelLP14k.process (R);

        // Tape compression — heavier drive.
        L = softSat (L, 1.5f);                                 // was 1.15
        R = softSat (R, 1.5f);
    }

    inline void processDamaged (float& L, float& R) noexcept
    {
        // Wide irregular flutter via heavy delay-line modulation.
        wowDelay.write (0.5f * (L + R));
        wowPhase     += (float) (juce::MathConstants<double>::twoPi * 0.6 / fs);
        flutterPhase += (float) (juce::MathConstants<double>::twoPi * 11.0 / fs);
        if (wowPhase     > juce::MathConstants<float>::twoPi) wowPhase     -= juce::MathConstants<float>::twoPi;
        if (flutterPhase > juce::MathConstants<float>::twoPi) flutterPhase -= juce::MathConstants<float>::twoPi;
        const float wow  = std::sin (wowPhase) * 18.0f;
        const float flut = std::sin (flutterPhase) * 6.0f * (0.5f + 0.5f * std::sin (wowPhase * 1.7f));
        const float jit  = whiteNoise() * 3.0f;
        const float dly  = juce::jmax (1.0f, 60.0f + wow + flut + jit);
        const float wobbleMono = wowDelay.read (dly);
        L = 0.6f * L + 0.4f * wobbleMono;
        R = 0.6f * R + 0.4f * wobbleMono;

        // Hard HF roll-off.
        L = damagedLP7k.process (L); R = damagedLP7k.process (R);

        // AGC pumping (envelope-driven gain duck) — irregular.
        const float env = damagedAGC.process (0.5f * (L + R));
        const float duck = 1.0f / (1.0f + 4.0f * env);
        L *= duck; R *= duck;

        // Loud noisy hiss floor.
        const float hiss = damagedHissLP.process (whiteNoise()) * 0.04f;
        L += hiss; R += hiss;

        // Random brief pitch dropouts (gain dips).
        if (--skipCountdown <= 0)
        {
            // ~1 dropout per second avg.
            skipCountdown = 1 + (int) (uniform() * fs * 2.0);
            skipHoldSamples = (int) (uniform() * 0.012 * fs);  // up to 12ms
            skipHoldL = L; skipHoldR = R;
        }
        if (skipHoldSamples > 0)
        {
            L *= 0.25f; R *= 0.25f;
            --skipHoldSamples;
        }
    }

    inline void processVinyl (float& L, float& R) noexcept
    {
        // v0.7.0 — boosted wow, surface noise, and crackle for more character.
        wowDelay.write (0.5f * (L + R));
        vinylPhase += (float) (juce::MathConstants<double>::twoPi * 0.55 / fs);
        if (vinylPhase > juce::MathConstants<float>::twoPi) vinylPhase -= juce::MathConstants<float>::twoPi;
        const float wow = std::sin (vinylPhase) * 4.0f;          // was ±2
        const float dly = juce::jmax (1.0f, 8.0f + wow);
        const float wobbleMono = wowDelay.read (dly);
        L = 0.88f * L + 0.12f * wobbleMono;                      // was 95/5
        R = 0.88f * R + 0.12f * wobbleMono;

        // RIAA-ish HF tilt via gentle peaking lift then HP.
        L = vinylTilt.processL (L); R = vinylTilt.processR (R);
        L = vinylRiaaHP.process (L); R = vinylRiaaHP.process (R);

        // Continuous surface noise.
        const float surface = (whiteNoise() * 0.007f);           // was 0.0035
        L += surface; R += surface;

        // Crackle: occasional impulsive clicks.
        if (--clickCountdown <= 0)
        {
            clickCountdown = 1 + (int) (uniform() * fs * 0.5);   // ~4 clicks/s (was ~3)
            const float popAmp = 0.08f + 0.22f * (float) uniform();  // was 0.05+0.15
            const float popSign = (uniform() < 0.5) ? -1.0f : 1.0f;
            L += popAmp * popSign; R += popAmp * popSign;
        }
    }

    inline void processBoombox (float& L, float& R) noexcept
    {
        // Plastic-cabinet bandpass + mid honk.
        L = boomboxHP150.process (L); R = boomboxHP150.process (R);
        L = boomboxLP8k.process  (L); R = boomboxLP8k.process  (R);
        L = boomboxHonk .processL (L); R = boomboxHonk .processR (R);

        // Slight collapse to mono — speaker mush.
        const float mono = 0.5f * (L + R);
        L = 0.7f * L + 0.3f * mono;
        R = 0.7f * R + 0.3f * mono;

        // Soft saturation to suggest cone overload.
        L = softSat (L, 1.3f);
        R = softSat (R, 1.3f);
    }

    inline void processAMRadio (float& L, float& R) noexcept
    {
        // Narrow band 300 Hz – 4 kHz.
        L = amHP300.process (L); R = amHP300.process (R);
        L = amLP4k .process (L); R = amLP4k .process (R);

        // Crossover-style distortion: tiny zero-crossing kink.
        auto kink = [] (float x) noexcept
        {
            const float t = 0.02f;
            if (x >  t) return x - t * 0.5f;
            if (x < -t) return x + t * 0.5f;
            return x * 0.5f;
        };
        L = kink (L); R = kink (R);

        // Mostly mono — AM is single-channel.
        const float mono = 0.5f * (L + R);
        L = 0.3f * L + 0.7f * mono;
        R = 0.3f * R + 0.7f * mono;

        // v0.40.1 — believable broadcast hiss.
        // 1) Continuous bandlimited carrier hiss at ~ -50 dBFS. Always
        //    present, the way a tuned-but-quiet AM station sounds.
        // 2) Slow envelope-shaped "wash" that drifts between a low and
        //    high value over hundreds of ms. Replaces the abrupt 40 ms
        //    bursts the previous version used.
        // 3) Both noise sources are ducked under loud program material
        //    via an envelope follower — same way a real AGC keeps the
        //    carrier roughly constant relative to the modulation.
        const float prog = amProgEnv.process (mono);
        const float duck = 1.0f / (1.0f + 8.0f * prog);   // 0..1, drops with louder material

        // Slow wash retargeting. New target every ~250–800 ms.
        if (--amWashHold <= 0)
        {
            amWashHold = (int) (fs * (0.25 + uniform() * 0.55));
            // Mostly low (~0.2) with rare excursions toward 1.0.
            const double pick = uniform();
            amWashTarget = (pick < 0.85) ? (float) (0.10 + uniform() * 0.25)
                                         : (float) (0.50 + uniform() * 0.50);
        }
        // Smooth toward target — ~50 ms time constant, no clicks.
        const float washA = 1.0f - std::exp (-1.0f / (0.05f * (float) fs));
        amWashEnv += washA * (amWashTarget - amWashEnv);

        // Bandlimited hiss source (white through LP).
        const float rawHiss = amHissLP.process (whiteNoise());
        // Floor + wash, both ducked.
        const float hissAmp = (0.0035f + 0.018f * amWashEnv) * duck;
        const float hiss    = rawHiss * hissAmp;
        L += hiss; R += hiss;
    }

    inline void processVHS (float& L, float& R) noexcept
    {
        // Narrow band.
        L = vhsHP50 .process (L); R = vhsHP50 .process (R);
        L = vhsLP12k.process (L); R = vhsLP12k.process (R);

        // AGC pumping — fast attack, slow release ducker.
        const float env  = vhsAGC.process (0.5f * (L + R));
        const float duck = 1.0f / (1.0f + 2.5f * env);
        L *= duck; R *= duck;

        // Occasional head-switching tick.
        if (--clickCountdown <= 0)
        {
            clickCountdown = 1 + (int) (uniform() * fs * 1.8);  // ~once / 0.9s avg
            const float tick = 0.02f * ((uniform() < 0.5) ? -1.0f : 1.0f);
            L += tick; R += tick;
        }
    }

    inline void processBusComp (float& L, float& R) noexcept
    {
        // Pure 2-bus glue — no EQ, no noise.
        const float env = busAGC.process (0.5f * (L + R));
        // Threshold ~ -12 dBFS = 0.25, ratio ~3:1. GR scales gently above thr.
        const float thr   = 0.25f;
        const float over  = juce::jmax (0.0f, env - thr);
        const float gr    = 1.0f / (1.0f + 2.0f * over);   // up to ~-3 dB at hot
        const float make  = 1.05f;                          // tiny makeup
        L *= gr * make; R *= gr * make;
    }

    inline void processLathe (float& L, float& R) noexcept
    {
        // Pre-emphasis on highs, soft clip on lows.
        L = latheBoost.processL (L); R = latheBoost.processR (R);
        L = latheLP15k.process  (L); R = latheLP15k.process  (R);

        // Soft tape-cut compression and subtle harmonic add.
        L = softSat (L, 1.4f);
        R = softSat (R, 1.4f);

        // Rare skip — short repeat.
        if (--skipCountdown <= 0)
        {
            skipCountdown = 1 + (int) (uniform() * fs * 8.0);   // ~once / 4s avg
            skipHoldSamples = (int) (uniform() * 0.006 * fs);   // up to 6ms
            skipHoldL = L; skipHoldR = R;
        }
        if (skipHoldSamples > 0)
        {
            L = skipHoldL; R = skipHoldR;
            --skipHoldSamples;
        }
    }

    inline void processPhone (float& L, float& R) noexcept
    {
        // Heavy bandpass.
        L = phoneHP400.process (L); R = phoneHP400.process (R);
        L = phoneLP3k4.process (L); R = phoneLP3k4.process (R);

        // Mu-law-ish soft companding into 8-step quant.
        auto mulawQuant = [] (float x) noexcept
        {
            const float mu = 32.0f;
            const float c  = (x >= 0 ? 1.0f : -1.0f)
                           * std::log (1.0f + mu * std::abs (x))
                           / std::log (1.0f + mu);
            const float q  = std::round (c * 8.0f) / 8.0f;
            return (q >= 0 ? 1.0f : -1.0f)
                 * (std::pow (1.0f + mu, std::abs (q)) - 1.0f) / mu;
        };
        L = mulawQuant (L); R = mulawQuant (R);

        // Mostly mono.
        const float mono = 0.5f * (L + R);
        L = 0.2f * L + 0.8f * mono;
        R = 0.2f * R + 0.8f * mono;
    }

    // -------- Helpers --------
    inline float whiteNoise() noexcept
    {
        // xorshift32 → [-1, +1]
        rngState ^= rngState << 13;
        rngState ^= rngState >> 17;
        rngState ^= rngState << 5;
        return (float) ((int32_t) rngState) * (1.0f / 2147483648.0f);
    }
    inline double uniform() noexcept
    {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 17;
        rngState ^= rngState << 5;
        return (double) (rngState & 0x7FFFFFFFu) / 2147483648.0;
    }

    // -------- State --------
    double fs = 44100.0;
    std::atomic<int>   mode      { kOutClean };
    std::atomic<float> mix       { 1.0f };
    std::atomic<float> intensity { 0.5f };   // v0.7.0 — effect depth (0=subtle, 1=full)

    // Filters per mode.
    OnePoleHP cassetteHP60;
    OnePoleLP cassetteLP9k, cassetteHissLP;
    OnePoleLP reelLP14k;
    BiquadLowShelf reelShelf;
    OnePoleLP damagedLP7k, damagedHissLP;
    EnvFollower damagedAGC;
    OnePoleHP vinylRiaaHP;
    BiquadPeak vinylTilt;
    OnePoleHP boomboxHP150;
    OnePoleLP boomboxLP8k;
    BiquadPeak boomboxHonk;
    OnePoleHP amHP300;
    OnePoleLP amLP4k;
    OnePoleLP amHissLP;
    OnePoleHP vhsHP50;
    OnePoleLP vhsLP12k;
    EnvFollower vhsAGC, busAGC;
    BiquadPeak latheBoost;
    OnePoleLP latheLP15k;
    OnePoleHP phoneHP400;
    OnePoleLP phoneLP3k4;

    // Wow/flutter delay line. 64 samples is enough for ±18 sample modulation.
    ShortDelay<128> wowDelay;
    float wowPhase = 0.0f, flutterPhase = 0.0f, vinylPhase = 0.0f;

    // Clicks / static / skips.
    int clickCountdown   = 0;
    int staticCountdown  = 0; int staticHoldSamples = 0;
    int skipCountdown    = 0; int skipHoldSamples   = 0;
    float skipHoldL = 0.0f, skipHoldR = 0.0f;

    // v0.40.1 — AM Radio static rework. The old impl fired ~40 ms bursts of
    // 0.12-amplitude white noise once every few seconds, which the user
    // accurately described as "random bursts" rather than radio static.
    // Replaced with a quiet continuous hiss + a slow envelope-shaped
    // "wash" that fades in/out over hundreds of ms, plus an envelope
    // follower so the noise ducks under loud program material the way an
    // AGC'd carrier would. State for the wash:
    float amWashEnv      = 0.0f;   // current wash gain
    float amWashTarget   = 0.0f;   // target gain the env is chasing
    int   amWashHold     = 0;      // samples remaining at target before retargeting
    EnvFollower amProgEnv;         // tracks program loudness for ducking

    // RNG. xorshift32 — no allocation, predictable timing, deterministic
    // across runs. Audio-thread safe.
    uint32_t rngState = 0xCAFE15B0u;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OutputStage)
};

// -----------------------------------------------------------------------------
//  CrossfadingOutputStage — wraps two OutputStage instances in an A/B ping-pong
//  arrangement so the master bus can swap mode/mix on Act change without the
//  pop you'd get from re-initialising filter histories mid-block.
//
//  Switch protocol:
//    1. UI / processBlock calls setTarget(mode, mix).
//    2. If the requested values match the active stage's running values it's
//       a no-op (stage atomics still write through so live tweaking works).
//    3. Otherwise the inactive stage is reset(), configured with the target
//       mode/mix, and a kFadeSamples linear crossfade kicks off. Both stages
//       run during the fade; output is (1-t)*active + t*incoming.
//    4. When the fade finishes, the incoming stage becomes active.
//
//  Threading: setTarget is called from the audio thread (processBlock). The
//  underlying OutputStage's mode/mix atomics make UI-thread reads safe.
// -----------------------------------------------------------------------------
class CrossfadingOutputStage
{
public:
    static constexpr int kFadeSamples = 384;   // ~8ms @ 48k

    // JUCE_DECLARE_NON_COPYABLE below deletes the copy ctor, which counts
    // as a user-declared constructor and suppresses the implicit default
    // ctor. Re-declare it explicitly so the Processor can default-construct
    // the CrossfadingOutputStage member.
    CrossfadingOutputStage() = default;

    void prepare (double sampleRate) noexcept
    {
        a.prepare (sampleRate);
        b.prepare (sampleRate);
        activeIsA   = true;
        fadeRemain  = 0;
        currentMode = a.getMode();
        currentMix  = a.getMix();
    }

    void reset() noexcept
    {
        a.reset(); b.reset();
        fadeRemain = 0;
    }

    // Push the desired mode/mix/intensity; triggers a crossfade if anything changed.
    void setTarget (int mode, float mix, float intens = 0.5f) noexcept
    {
        mode   = juce::jlimit (0, (int) kNumOutputStages - 1, mode);
        mix    = juce::jlimit (0.0f, 1.0f, mix);
        intens = juce::jlimit (0.0f, 1.0f, intens);

        if (mode == currentMode
            && std::abs (mix    - currentMix)    < 1.0e-4f
            && std::abs (intens - currentIntens) < 1.0e-4f)
            return;

        // v0.7.0 — intensity-only changes update the active stage directly
        // (no crossfade needed — it's just a gain scaling, not a mode change
        // that requires filter re-init).
        if (mode == currentMode && std::abs (mix - currentMix) < 1.0e-4f)
        {
            (activeIsA ? a : b).setIntensity (intens);
            currentIntens = intens;
            return;
        }

        // Configure the *inactive* stage to be the incoming one.
        OutputStage& incoming = activeIsA ? b : a;
        incoming.reset();
        incoming.setMode      (mode);
        incoming.setMix       (mix);
        incoming.setIntensity (intens);

        currentMode   = mode;
        currentMix    = mix;
        currentIntens = intens;
        fadeRemain    = kFadeSamples;
    }

    // Per-sample stereo process. Always called from the audio thread.
    inline void process (float& L, float& R) noexcept
    {
        if (fadeRemain <= 0)
        {
            (activeIsA ? a : b).process (L, R);
            return;
        }

        // Two-stage crossfade. Both run in parallel on the same input.
        float lA = L, rA = R;
        float lB = L, rB = R;
        a.process (lA, rA);
        b.process (lB, rB);

        const float t = 1.0f - (float) fadeRemain / (float) kFadeSamples;  // 0..1
        if (activeIsA)
        {
            // A is fading out, B is fading in
            L = lA * (1.0f - t) + lB * t;
            R = rA * (1.0f - t) + rB * t;
        }
        else
        {
            L = lB * (1.0f - t) + lA * t;
            R = rB * (1.0f - t) + rA * t;
        }

        if (--fadeRemain == 0)
            activeIsA = ! activeIsA;
    }

    int   getMode()      const noexcept { return currentMode; }
    float getMix ()      const noexcept { return currentMix;  }
    float getIntensity() const noexcept { return currentIntens; }

private:
    OutputStage a, b;
    bool  activeIsA     = true;
    int   fadeRemain    = 0;
    int   currentMode   = kOutClean;
    float currentMix    = 0.0f;
    float currentIntens = 0.5f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CrossfadingOutputStage)
};

// -----------------------------------------------------------------------------
//  Interpolation enum — used by the buffer-read function in the processor.
// -----------------------------------------------------------------------------
enum InterpMode
{
    kInterpLinear = 0,
    kInterpDrop,
    kInterpCubic,
    kNumInterpModes
};

inline const char* interpShortName (int m) noexcept
{
    switch (m)
    {
        case kInterpLinear: return "Linear";
        case kInterpDrop:   return "Drop-sample";
        case kInterpCubic:  return "Cubic";
        default:            return "?";
    }
}

inline const char* interpDescription (int m) noexcept
{
    switch (m)
    {
        case kInterpLinear: return "default — clean and quick";
        case kInterpDrop:   return "no interp — SP-1200 grit at pitch";
        case kInterpCubic:  return "Hermite — cleaner than linear";
        default:            return "";
    }
}

// -----------------------------------------------------------------------------
//  Filter mode enum — used by the Tone/Res filter in the processor.
//  v0.5.0 — replaces the old hardcoded bipolar tilt EQ with selectable
//  modes: Tilt (legacy default), LP, HP, BP.
// -----------------------------------------------------------------------------
enum FilterMode
{
    kFilterTilt = 0,   // legacy bipolar: dark ← 0.5 → bright
    kFilterLP,         // full-range low-pass, Tone = cutoff
    kFilterHP,         // full-range high-pass, Tone = cutoff
    kFilterBP,         // bandpass, Tone = centre freq, Res = Q
    kNumFilterModes
};

inline const char* filterShortName (int m) noexcept
{
    switch (m)
    {
        case kFilterTilt: return "Tilt";
        case kFilterLP:   return "LP";
        case kFilterHP:   return "HP";
        case kFilterBP:   return "BP";
        default:          return "?";
    }
}

inline const char* filterDescription (int m) noexcept
{
    switch (m)
    {
        case kFilterTilt: return "default — bipolar dark/bright tilt EQ";
        case kFilterLP:   return "resonant low-pass filter";
        case kFilterHP:   return "resonant high-pass filter";
        case kFilterBP:   return "resonant bandpass filter";
        default:          return "";
    }
}

// -----------------------------------------------------------------------------
//  Drive type enum — saturation character for the DRIVE knob.
// -----------------------------------------------------------------------------
enum DriveType
{
    kDriveTape = 0,    // default — soft tape saturation (tanh)
    kDriveTube,        // asymmetric even-harmonic (shifted tanh)
    kDriveDiode,       // hard clip, odd harmonics
    kDriveFuzz,        // extreme asymmetric — broken speaker vibe
    kNumDriveTypes
};

inline const char* driveShortName (int m) noexcept
{
    switch (m)
    {
        case kDriveTape:  return "Tape";
        case kDriveTube:  return "Tube";
        case kDriveDiode: return "Diode";
        case kDriveFuzz:  return "Fuzz";
        default:          return "?";
    }
}

inline const char* driveDescription (int m) noexcept
{
    switch (m)
    {
        case kDriveTape:  return "default — soft tape saturation";
        case kDriveTube:  return "asymmetric, warm even harmonics";
        case kDriveDiode: return "hard clip, odd harmonics";
        case kDriveFuzz:  return "extreme — broken speaker";
        default:          return "";
    }
}

// -----------------------------------------------------------------------------
//  Fold topology enum — wavefolder shape for the FOLD knob.
// -----------------------------------------------------------------------------
enum FoldTopology
{
    kFoldSine = 0,     // default — smooth sine fold
    kFoldTriangle,     // sharper, more metallic
    kFoldAsymmetric,   // different fold on +/- half → even harmonics
    kNumFoldTopologies
};

inline const char* foldShortName (int m) noexcept
{
    switch (m)
    {
        case kFoldSine:       return "Sine";
        case kFoldTriangle:   return "Triangle";
        case kFoldAsymmetric: return "Asymmetric";
        default:              return "?";
    }
}

inline const char* foldDescription (int m) noexcept
{
    switch (m)
    {
        case kFoldSine:       return "default — smooth sine fold";
        case kFoldTriangle:   return "sharper, metallic harmonics";
        case kFoldAsymmetric: return "even + odd harmonics mix";
        default:              return "";
    }
}

// -----------------------------------------------------------------------------
//  Shimmer octave enum — pitch interval for the SHIMMER feedback.
// -----------------------------------------------------------------------------
enum ShimmerOctave
{
    kShimmerUp1 = 0,   // default — +1 octave
    kShimmerUp2,       // +2 octaves
    kShimmerDown1,     // -1 octave
    kShimmerFifth,     // +7 semitones (perfect fifth)
    kShimmerFifthOct,  // +7 and +12 (dual: fifth + octave)
    kNumShimmerOctaves
};

inline const char* shimmerShortName (int m) noexcept
{
    switch (m)
    {
        case kShimmerUp1:      return "+1 Oct";
        case kShimmerUp2:      return "+2 Oct";
        case kShimmerDown1:    return "-1 Oct";
        case kShimmerFifth:    return "+5th";
        case kShimmerFifthOct: return "+5th/Oct";
        default:               return "?";
    }
}

inline const char* shimmerDescription (int m) noexcept
{
    switch (m)
    {
        case kShimmerUp1:      return "default — octave-up halo";
        case kShimmerUp2:      return "two octaves up — crystalline";
        case kShimmerDown1:    return "octave down — subharmonic wash";
        case kShimmerFifth:    return "perfect fifth — organ-like";
        case kShimmerFifthOct: return "fifth + octave — full chord";
        default:               return "";
    }
}

// -----------------------------------------------------------------------------
//  Smear character enum — delay type for the SMEAR knob.
// -----------------------------------------------------------------------------
enum SmearCharacter
{
    kSmearBlur = 0,    // default — short delay blur
    kSmearDiffuse,     // allpass-based, reverb-like wash
    kSmearFreeze,      // infinite feedback, captures a moment
    kNumSmearCharacters
};

inline const char* smearShortName (int m) noexcept
{
    switch (m)
    {
        case kSmearBlur:    return "Blur";
        case kSmearDiffuse: return "Diffuse";
        case kSmearFreeze:  return "Freeze";
        default:            return "?";
    }
}

inline const char* smearDescription (int m) noexcept
{
    switch (m)
    {
        case kSmearBlur:    return "default — short delay smear";
        case kSmearDiffuse: return "allpass wash — reverb-like";
        case kSmearFreeze:  return "infinite hold — frozen moment";
        default:            return "";
    }
}

// -----------------------------------------------------------------------------
//  Stutter window enum — micro-loop behaviour for the STUTTER knob.
// -----------------------------------------------------------------------------
enum StutterWindow
{
    kStutterFixed = 0,    // default — constant window size
    kStutterDecaying,     // window shrinks → accelerating stutter
    kStutterGrowing,      // window grows → decelerating stutter
    kNumStutterWindows
};

inline const char* stutterShortName (int m) noexcept
{
    switch (m)
    {
        case kStutterFixed:    return "Fixed";
        case kStutterDecaying: return "Decaying";
        case kStutterGrowing:  return "Growing";
        default:               return "?";
    }
}

inline const char* stutterDescription (int m) noexcept
{
    switch (m)
    {
        case kStutterFixed:    return "default — steady loop size";
        case kStutterDecaying: return "shrinking — builds tension";
        case kStutterGrowing:  return "expanding — winds down";
        default:               return "";
    }
}

// -----------------------------------------------------------------------------
//  Varispeed curve enum — brake shape for the VARISPEED knob.
// -----------------------------------------------------------------------------
enum VarispeedCurve
{
    kVariLinear = 0,    // default — constant deceleration
    kVariExponential,   // DJ brake feel
    kVariSudden,        // plays at speed, drops at end
    kNumVarispeedCurves
};

inline const char* varispeedShortName (int m) noexcept
{
    switch (m)
    {
        case kVariLinear:      return "Linear";
        case kVariExponential: return "Exponential";
        case kVariSudden:      return "Sudden";
        default:               return "?";
    }
}

inline const char* varispeedDescription (int m) noexcept
{
    switch (m)
    {
        case kVariLinear:      return "default — steady slow-down";
        case kVariExponential: return "DJ brake — fast then crawl";
        case kVariSudden:      return "full speed then stop";
        default:               return "";
    }
}

// -----------------------------------------------------------------------------
//  Slide curve enum — pitch-bend shape for the SLIDE knob.
// -----------------------------------------------------------------------------
enum SlideCurve
{
    kSlideLinear = 0,      // default — constant-rate bend
    kSlideExponential,     // accelerating bend
    kSlideLog,             // fast start, slow finish
    kSlideSCurve,          // ease-in-out
    kNumSlideCurves
};

inline const char* slideShortName (int m) noexcept
{
    switch (m)
    {
        case kSlideLinear:      return "Linear";
        case kSlideExponential: return "Exponential";
        case kSlideLog:         return "Log";
        case kSlideSCurve:      return "S-Curve";
        default:                return "?";
    }
}

inline const char* slideDescription (int m) noexcept
{
    switch (m)
    {
        case kSlideLinear:      return "default — constant-rate bend";
        case kSlideExponential: return "accelerating — tape spin-up";
        case kSlideLog:         return "fast start, slow finish — braking";
        case kSlideSCurve:      return "ease in/out — smooth S";
        default:                return "";
    }
}

// -----------------------------------------------------------------------------
//  Reverse mode enum — playback direction logic for the REVERSE knob.
// -----------------------------------------------------------------------------
enum ReverseMode
{
    kReverseRandom = 0,    // default — probability-based coin flip
    kReverseAlternate,     // every other slice reverses
    kReversePalindrome,    // plays forward then backward within slice
    kReversePingPong,      // alternates fwd/bwd across judder hits
    kNumReverseModes
};

inline const char* reverseShortName (int m) noexcept
{
    switch (m)
    {
        case kReverseRandom:     return "Random";
        case kReverseAlternate:  return "Alternate";
        case kReversePalindrome: return "Palindrome";
        case kReversePingPong:   return "Ping-Pong";
        default:                 return "?";
    }
}

inline const char* reverseDescription (int m) noexcept
{
    switch (m)
    {
        case kReverseRandom:     return "default — coin-flip per slice";
        case kReverseAlternate:  return "every other slice reversed";
        case kReversePalindrome: return "fwd then bwd within slice";
        case kReversePingPong:   return "alternates across judder hits";
        default:                 return "";
    }
}

// -----------------------------------------------------------------------------
//  Tape mode enum — wow/flutter blend for the TAPE knob.
// -----------------------------------------------------------------------------
enum TapeMode
{
    kTapeClassic = 0,  // default — blended wow + flutter
    kTapeWowOnly,      // slow drift only — warped vinyl
    kTapeFlutterOnly,  // fast wobble only — VHS
    kTapeExtreme,      // both maxed with wider deviation
    kNumTapeModes
};

inline const char* tapeShortName (int m) noexcept
{
    switch (m)
    {
        case kTapeClassic:     return "Classic";
        case kTapeWowOnly:     return "Wow Only";
        case kTapeFlutterOnly: return "Flutter Only";
        case kTapeExtreme:     return "Extreme";
        default:               return "?";
    }
}

inline const char* tapeDescription (int m) noexcept
{
    switch (m)
    {
        case kTapeClassic:     return "default — blended wow + flutter";
        case kTapeWowOnly:     return "slow drift — warped vinyl feel";
        case kTapeFlutterOnly: return "fast wobble — VHS character";
        case kTapeExtreme:     return "both maxed — wide deviation";
        default:               return "";
    }
}

// -----------------------------------------------------------------------------
//  Ring mod waveform enum — modulator shape for the RING MOD knob.
// -----------------------------------------------------------------------------
enum RingModWave
{
    kRingSine = 0,     // default — classic sine ring mod
    kRingSquare,       // octave-down harshness
    kRingTriangle,     // mellower, softer harmonics
    kRingSaw,          // asymmetric edge
    kNumRingModWaves
};

inline const char* ringModWaveShortName (int m) noexcept
{
    switch (m)
    {
        case kRingSine:     return "Sine";
        case kRingSquare:   return "Square";
        case kRingTriangle: return "Triangle";
        case kRingSaw:      return "Saw";
        default:            return "?";
    }
}

inline const char* ringModWaveDescription (int m) noexcept
{
    switch (m)
    {
        case kRingSine:     return "default — classic ring mod";
        case kRingSquare:   return "octave-down, hard harmonics";
        case kRingTriangle: return "mellower, softer character";
        case kRingSaw:      return "asymmetric edge";
        default:            return "";
    }
}

// -----------------------------------------------------------------------------
//  Chaos distribution enum — random character for the CHAOS knob.
// -----------------------------------------------------------------------------
enum ChaosDistribution
{
    kChaosUniform = 0,   // default — uniform random
    kChaosGaussian,      // clustered centre, occasional spikes
    kChaosDrunkWalk,     // Brownian — smooth wandering
    kChaosBipolarSnap,   // hard ±1 only — dramatic on/off
    kNumChaosDistributions
};

inline const char* chaosShortName (int m) noexcept
{
    switch (m)
    {
        case kChaosUniform:     return "Uniform";
        case kChaosGaussian:    return "Gaussian";
        case kChaosDrunkWalk:   return "Drunk Walk";
        case kChaosBipolarSnap: return "Bipolar Snap";
        default:                return "?";
    }
}

inline const char* chaosDescription (int m) noexcept
{
    switch (m)
    {
        case kChaosUniform:     return "default — uniform random";
        case kChaosGaussian:    return "clustered, occasional spikes";
        case kChaosDrunkWalk:   return "Brownian — smooth wandering";
        case kChaosBipolarSnap: return "hard +/- only — glitch switch";
        default:                return "";
    }
}

// -----------------------------------------------------------------------------
//  Feedback character enum — re-injection flavour for the FEEDBACK knob.
// -----------------------------------------------------------------------------
enum FeedbackCharacter
{
    kFeedbackClean = 0,    // default — direct re-injection
    kFeedbackFiltered,     // LP at ~2 kHz before re-inject
    kFeedbackSaturated,    // soft-clip before re-inject
    kFeedbackDucked,       // sidechain-compressed against input
    kNumFeedbackCharacters
};

inline const char* feedbackShortName (int m) noexcept
{
    switch (m)
    {
        case kFeedbackClean:     return "Clean";
        case kFeedbackFiltered:  return "Filtered";
        case kFeedbackSaturated: return "Saturated";
        case kFeedbackDucked:    return "Ducked";
        default:                 return "?";
    }
}

inline const char* feedbackDescription (int m) noexcept
{
    switch (m)
    {
        case kFeedbackClean:     return "default — direct re-injection";
        case kFeedbackFiltered:  return "LP at 2 kHz — tames shrillness";
        case kFeedbackSaturated: return "soft-clip — thickens, never shrill";
        case kFeedbackDucked:    return "sidechain — swells in gaps";
        default:                 return "";
    }
}

// -----------------------------------------------------------------------------
//  Stretch mode enum — granular behaviour for the STRETCH knob.
// -----------------------------------------------------------------------------
enum StretchMode
{
    kStretchStandard = 0,  // default — basic overlapping grains
    kStretchPaulstretch,   // massive overlap for frozen textures
    kStretchSpectral,      // FFT freeze — holds frequency content
    kStretchFormant,       // preserves formant peaks
    kNumStretchModes
};

inline const char* stretchShortName (int m) noexcept
{
    switch (m)
    {
        case kStretchStandard:    return "Standard";
        case kStretchPaulstretch: return "Paulstretch";
        case kStretchSpectral:    return "Spectral";
        case kStretchFormant:     return "Formant";
        default:                  return "?";
    }
}

inline const char* stretchDescription (int m) noexcept
{
    switch (m)
    {
        case kStretchStandard:    return "default — overlapping grains";
        case kStretchPaulstretch: return "massive overlap — frozen texture";
        case kStretchSpectral:    return "FFT freeze — holds spectrum";
        case kStretchFormant:     return "preserves vocal character";
        default:                  return "";
    }
}

// -----------------------------------------------------------------------------
//  Judder shape enum — retrigger timing for the JUDDER knob.
// -----------------------------------------------------------------------------
enum JudderShape
{
    kJudderEven = 0,       // default — evenly spaced
    kJudderAccelerating,   // retriggers get closer — machine-gun ramp
    kJudderDecelerating,   // retriggers spread apart — bouncing ball
    kJudderRandom,         // jittered timing
    kNumJudderShapes
};

inline const char* judderShortName (int m) noexcept
{
    switch (m)
    {
        case kJudderEven:         return "Even";
        case kJudderAccelerating: return "Accelerating";
        case kJudderDecelerating: return "Decelerating";
        case kJudderRandom:       return "Random";
        default:                  return "?";
    }
}

inline const char* judderDescription (int m) noexcept
{
    switch (m)
    {
        case kJudderEven:         return "default — evenly spaced";
        case kJudderAccelerating: return "closer together — ramp up";
        case kJudderDecelerating: return "spread apart — bouncing ball";
        case kJudderRandom:       return "jittered — unpredictable";
        default:                  return "";
    }
}

// -----------------------------------------------------------------------------
//  Lookback behaviour enum — how the capture window is selected.
// -----------------------------------------------------------------------------
enum LookbackBehaviour
{
    kLookbackFixed = 0,      // default — exact lookback window
    kLookbackJittered,       // small random offset each slice
    kLookbackQuantised,      // snap to nearest beat boundary
    kNumLookbackBehaviours
};

inline const char* lookbackShortName (int m) noexcept
{
    switch (m)
    {
        case kLookbackFixed:     return "Fixed";
        case kLookbackJittered:  return "Jittered";
        case kLookbackQuantised: return "Quantised";
        default:                 return "?";
    }
}

inline const char* lookbackDescription (int m) noexcept
{
    switch (m)
    {
        case kLookbackFixed:     return "default — exact lookback window";
        case kLookbackJittered:  return "random offset per slice — subtle variation";
        case kLookbackQuantised: return "snap to nearest beat — rhythmic coherence";
        default:                 return "";
    }
}

// -----------------------------------------------------------------------------
//  Decay curve enum — amplitude envelope shape for slice playback.
// -----------------------------------------------------------------------------
enum DecayCurve
{
    kDecayLinear = 0,        // default — straight line
    kDecayExponential,       // natural tail — fast drop then slow fade
    kDecayLogarithmic,       // sustains then drops sharply
    kDecayGate,              // instant cut — no fade
    kNumDecayCurves
};

inline const char* decayShortName (int m) noexcept
{
    switch (m)
    {
        case kDecayLinear:      return "Linear";
        case kDecayExponential: return "Exponential";
        case kDecayLogarithmic: return "Logarithmic";
        case kDecayGate:        return "Gate";
        default:                return "?";
    }
}

inline const char* decayDescription (int m) noexcept
{
    switch (m)
    {
        case kDecayLinear:      return "default — straight line fade";
        case kDecayExponential: return "natural tail — fast drop, slow fade";
        case kDecayLogarithmic: return "holds then drops — sustained presence";
        case kDecayGate:        return "instant cut — no fade at all";
        default:                return "";
    }
}

} // namespace loopsab
