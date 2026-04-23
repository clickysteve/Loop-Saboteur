#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <chrono>

// ============================================================================
//  Grid-locked breakbeat chopper. See PluginProcessor.h for the overall shape.
// ============================================================================

namespace
{
    // Semitones -> playback-rate multiplier. Rate = 2^(st/12).
    // Used for the static Pitch knob and for the per-judder Slide
    // multiplier, so it lives here as a tiny shared helper.
    inline double semitonesToRate (double semitones) noexcept
    {
        return std::pow (2.0, semitones / 12.0);
    }

    // v0.13 — output safety soft clipper.
    // Unity-linear in the quiet region, then bends asymptotically
    // toward a ceiling as the input grows past the threshold. Cheap
    // (one divide, one multiply-add), symmetric around 0, and has
    // gentle knee character rather than the hard fold of a tanh at
    // extreme inputs. Threshold 0.9 keeps normal program material
    // untouched (anything below -1dBFS is pure bypass), the ceiling
    // at 1.0 is a hard brick-wall the output can never exceed.
    // Used both on the final output sum and — more importantly — on
    // the feedback tap back into the circular buffer, where an
    // unclamped loop at 72%+ feedback with Shimmer/Mix/Drive all up
    // will otherwise accumulate energy without bound.
    inline float softLimit (float x) noexcept
    {
        constexpr float threshold = 0.9f;
        constexpr float ceiling   = 1.0f;
        constexpr float range     = ceiling - threshold;
        if (x >  threshold)
        {
            const float over = x - threshold;
            return threshold + range * (over / (range + over));
        }
        if (x < -threshold)
        {
            const float over = -threshold - x;
            return -threshold - range * (over / (range + over));
        }
        return x;
    }

    // Division choice strings, in order. Keep the index enum-ish so
    // divisionIndexToQuarters below matches one-for-one.
    // v0.37 — extended upward to 4 bars for long-form / drone / pad
    // modulation. 4 bars × 16 steps = 64-bar patterns. We stop at 4
    // bars because a single slice at 60 BPM is 16 sec, still safely
    // inside the 32-sec circular buffer (see kBufferSeconds). 8 bars
    // would sit right on the buffer edge and combine badly with
    // RATE at 0.5× — bump kBufferSeconds to 64 first if you ever
    // want to go there.
    static const juce::StringArray kDivisionChoices {
        "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4",
        "1/2", "1 bar", "2 bars", "4 bars"
    };

    inline double divisionIndexToQuarters (int idx) noexcept
    {
        // Each entry is the length of one division in quarter notes.
        // 1/16T means "three notes in the space of two 1/16ths" →
        // each triplet = (1/8) / 3 = 1/6 of a quarter.
        static const double vals[] = {
            0.125,        // 0: 1/32
            1.0 / 6.0,    // 1: 1/16T
            0.25,         // 2: 1/16
            1.0 / 3.0,    // 3: 1/8T
            0.5,          // 4: 1/8
            1.0,          // 5: 1/4
            2.0,          // 6: 1/2       (v0.37)
            4.0,          // 7: 1 bar     (v0.37)
            8.0,          // 8: 2 bars    (v0.37)
            16.0          // 9: 4 bars    (v0.37)
        };
        idx = juce::jlimit (0, (int) (sizeof(vals)/sizeof(vals[0])) - 1, idx);
        return vals[idx];
    }

    // v0.33 — LFO rate list. First entry is "Off" (a sentinel — the LFO
    // engine skips rateIdx == 0 entirely). The rest run from fast
    // musical divisions up to multi-bar drifts. This is dedicated to
    // LFOs and intentionally separate from the sequencer's
    // divisionIndexToQuarters so the two can evolve independently.
    static const juce::StringArray kLfoDivisionChoices {
        "Off",
        "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4",
        "1/2", "1 bar", "2 bars", "4 bars", "8 bars", "16 bars"
    };

    inline double lfoDivisionIndexToQuarters (int idx) noexcept
    {
        // Index 0 = Off. Caller MUST check for 0 before calling; we
        // return 0.0 as a safety sentinel but that would divide-by-zero
        // the PPQ phase math.
        static const double vals[] = {
            0.0,          // 0: Off (sentinel — never used; caller skips)
            0.125,        // 1: 1/32
            1.0 / 6.0,    // 2: 1/16T
            0.25,         // 3: 1/16
            1.0 / 3.0,    // 4: 1/8T
            0.5,          // 5: 1/8
            1.0,          // 6: 1/4
            2.0,          // 7: 1/2
            4.0,          // 8: 1 bar
            8.0,          // 9: 2 bars
            16.0,         // 10: 4 bars
            32.0,         // 11: 8 bars
            64.0          // 12: 16 bars
        };
        idx = juce::jlimit (0, (int) (sizeof(vals)/sizeof(vals[0])) - 1, idx);
        return vals[idx];
    }

    // v0.37 — FREE-mode rate table. The sync toggle now genuinely
    // reinterprets the rate knob: SYNC uses bar divisions locked to
    // PPQ, FREE uses Hz and accumulates on the sample clock
    // independent of host tempo or transport. Index 0 = Off, indices
    // 1..12 parallel the division table so one rateIdx atomic drives
    // both meanings. Values span glacial drift → audio-rate
    // modulation, roughly matching Ableton / Serum free-LFO ranges.
    static const juce::StringArray kLfoHzChoices {
        "Off",
        "0.05 Hz", "0.1 Hz", "0.25 Hz", "0.5 Hz",
        "1 Hz", "2 Hz", "4 Hz", "8 Hz",
        "12 Hz", "16 Hz", "20 Hz", "30 Hz"
    };

    inline double lfoHzIndexToHz (int idx) noexcept
    {
        static const double vals[] = {
            0.0,    // 0: Off
            0.05,   // 1
            0.1,    // 2
            0.25,   // 3
            0.5,    // 4
            1.0,    // 5
            2.0,    // 6
            4.0,    // 7
            8.0,    // 8
            12.0,   // 9
            16.0,   // 10
            20.0,   // 11
            30.0    // 12
        };
        idx = juce::jlimit (0, (int) (sizeof(vals)/sizeof(vals[0])) - 1, idx);
        return vals[idx];
    }

    static const juce::StringArray kLookbackChoices {
        "1/128", "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars"
    };

    inline double lookbackIndexToQuarters (int idx) noexcept
    {
        // Quarter notes per lookback step. The short end gives you
        // "grab what just happened" style snappy echoes; the long end
        // grabs whole phrases from bars ago. Pushed down to 1/128 so
        // micro-lookbacks land you deep in near-feedback runaway
        // territory — think classic sampler self-feeding loop.
        static const double vals[] = {
            0.03125, // 1/128
            0.0625,  // 1/64
            0.125,   // 1/32
            0.25,    // 1/16
            0.5,     // 1/8
            1.0,     // 1/4
            2.0,     // 1/2
            4.0,     // 1 bar
            8.0,     // 2 bars
            16.0     // 4 bars
        };
        idx = juce::jlimit (0, (int) (sizeof(vals)/sizeof(vals[0])) - 1, idx);
        return vals[idx];
    }

    // Extended Judder Div range: 1/128 at the sizzling top end down
    // to 1/2 for lazy dubby repeats. Includes triplets all the way
    // through so you can get Squarepusher-style 1/8T machine-gun
    // flams or a slow 1/2T pad-chop.
    static const juce::StringArray kJudderDivChoices {
        "1/128", "1/64", "1/32T", "1/32", "1/16T", "1/16",
        "1/8T", "1/8", "1/4T", "1/4", "1/2T", "1/2"
    };

    // Rate = playback speed multiplier. Also pitches the slice (no
    // formant preservation — this is the Amiga/tracker feel where
    // rate and pitch are the same knob). 1/8x plays the slice eight
    // times slower (and two octaves down); 4x is four times faster.
    static const juce::StringArray kRateChoices {
        "1/8x", "1/4x", "1/2x", "1x", "2x", "4x"
    };

    inline double rateIndexToMultiplier (int idx) noexcept
    {
        static const double vals[] = { 0.125, 0.25, 0.5, 1.0, 2.0, 4.0 };
        idx = juce::jlimit (0, (int) (sizeof(vals)/sizeof(vals[0])) - 1, idx);
        return vals[idx];
    }

    inline double judderDivIndexToQuarters (int idx) noexcept
    {
        static const double vals[] = {
            0.03125,      // 1/128
            0.0625,       // 1/64
            1.0 / 12.0,   // 1/32T
            0.125,        // 1/32
            1.0 / 6.0,    // 1/16T
            0.25,         // 1/16
            1.0 / 3.0,    // 1/8T
            0.5,          // 1/8
            2.0 / 3.0,    // 1/4T
            1.0,          // 1/4
            4.0 / 3.0,    // 1/2T
            2.0           // 1/2
        };
        idx = juce::jlimit (0, (int) (sizeof(vals)/sizeof(vals[0])) - 1, idx);
        return vals[idx];
    }

    // Sequencer step rate — independent from DIVISION. This controls how
    // fast the sequencer advances from step to step, while DIVISION still
    // drives slice length. Lets Steve have long slices on a fast grid or
    // vice versa.
    static const juce::StringArray kSeqRateChoices {
        "1/32", "1/16", "1/16T", "1/8", "1/8T", "1/4", "1/4T", "1/2", "1 bar"
    };

    inline double seqRateIndexToQuarters (int idx) noexcept
    {
        static const double vals[] = {
            0.125,              // 1/32
            0.25,               // 1/16
            0.25  * 2.0 / 3.0, // 1/16T
            0.5,                // 1/8
            0.5   * 2.0 / 3.0, // 1/8T
            1.0,                // 1/4
            1.0   * 2.0 / 3.0, // 1/4T
            2.0,                // 1/2
            4.0                 // 1 bar
        };
        idx = juce::jlimit (0, (int) (sizeof(vals)/sizeof(vals[0])) - 1, idx);
        return vals[idx];
    }

    // --- v0.7 scale quantise ------------------------------------------
    // Twelve-tone roots so Steve can pick in absolute pitch-class land.
    static const juce::StringArray kScaleRootChoices {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };

    // Scales as 12-bit masks (bit 0 = root, bit 11 = major 7). Index 0 is
    // "Off" which means "don't snap at all" — a true bypass, distinct
    // from "Chromatic" which would be a no-op anyway.
    struct ScaleEntry { const char* name; int mask; };
    static const ScaleEntry kScaleTable[] = {
        { "Off",          0 },                 // bypass — no quantise
        { "Chromatic",    0b111111111111 },    // every semitone in (identical to off, kept for clarity)
        { "Major",        0b101010110101 },    // C D E F G A B
        { "Minor",        0b010110101101 },    // C D Eb F G Ab Bb (natural)
        { "Dorian",       0b011010101101 },    // C D Eb F G A Bb
        { "Phrygian",     0b010110101011 },    // C Db Eb F G Ab Bb
        { "Mixolydian",   0b011010110101 },    // C D E F G A Bb
        { "Harmonic Min", 0b100110101101 },    // C D Eb F G Ab B
        { "Whole Tone",   0b010101010101 },    // C D E F# G# A#
        { "Penta Minor",  0b010010101001 },    // C Eb F G Bb
        { "Blues",        0b010011101001 },    // C Eb F F# G Bb
    };

    static constexpr int kNumScales = (int) (sizeof(kScaleTable) / sizeof(kScaleTable[0]));

    static juce::StringArray makeScaleTypeChoices()
    {
        juce::StringArray out;
        for (int i = 0; i < kNumScales; ++i)
            out.add (kScaleTable[i].name);
        return out;
    }

    // Find the nearest in-scale semitone to `absSemitones` for the given
    // root + scale mask. "Nearest" resolves ties toward 0 (neutral).
    // Returns the quantised value in semitones.
    inline float snapSemitonesToScale (float absSemitones, int root, int scaleIdx) noexcept
    {
        scaleIdx = juce::jlimit (0, kNumScales - 1, scaleIdx);
        const int mask = kScaleTable[scaleIdx].mask;
        if (mask == 0 || mask == 0b111111111111)
            return absSemitones;  // Off / Chromatic — bypass

        const float t = absSemitones - (float) root;
        const int   baseInt = (int) std::floor (t);
        const float frac    = t - (float) baseInt;

        // Scan outward from baseInt for the closest set-bit pitch class.
        // At most 6 steps in either direction before we wrap pitch class.
        int bestOffset = 0;
        float bestDist = 1.0e6f;
        for (int off = -6; off <= 6; ++off)
        {
            const int pc = ((baseInt + off) % 12 + 12) % 12;
            if ((mask & (1 << pc)) == 0)
                continue;
            const float dist = std::abs ((float) off - frac);
            if (dist < bestDist)
            {
                bestDist = dist;
                bestOffset = off;
            }
        }

        return (float) (baseInt + bestOffset) + (float) root;
    }
}

// ----------------------------------------------------------------------------
//  Parameter layout
// ----------------------------------------------------------------------------
juce::AudioProcessorValueTreeState::ParameterLayout
LoopSaboteurProcessor::createParameterLayout()
{
    using APF = juce::AudioParameterFloat;
    using API = juce::AudioParameterInt;
    using APB = juce::AudioParameterBool;
    using APC = juce::AudioParameterChoice;
    using NR  = juce::NormalisableRange<float>;

    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    // v0.13 — shared percentage display helpers for knobs whose raw
    // value is 0..1 but which read more naturally as 0..100%. Applied
    // at parameter level so the host's plugin header strip and automation
    // displays match what the plugin's own slider shows.
    auto pctString = [] (float v, int /*maxLen*/)
    {
        return juce::String ((int) std::round (v * 100.0f)) + "%";
    };
    auto pctValue = [] (const juce::String& s)
    {
        return juce::jlimit (0.0f, 1.0f,
                             (float) (s.retainCharacters ("0123456789.").getDoubleValue() / 100.0));
    };

    // Chance — probability per grid boundary of firing a slice.
    // Default 0.3 so a steady breakbeat chop happens without the user
    // having to reach for the knob first.
    // v0.13 — now displayed as a percentage so "30%" reads unambiguously
    // rather than "0.3" which could be mistaken for a normalized level.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamChance, 2 }, "Chance",
        NR (0.0f, 1.0f, 0.001f), 1.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // Division — drives both the trigger grid and the slice length.
    // Default 1/16, the classic hip-hop chop resolution.
    layout.add (std::make_unique<APC>(
        juce::ParameterID { kParamDivision, 1 }, "Division",
        kDivisionChoices, 2));

    // Lookback — how far back the slice source is, snapped to musical
    // values so the grabbed audio always lands in phrase. Short lookbacks
    // give "grab what just happened" echo tails; long lookbacks grab
    // whole phrases. Default 1/16 (index 3 in the extended list that
    // now reaches down to 1/128 for near-feedback runaway territory).
    layout.add (std::make_unique<APC>(
        juce::ParameterID { kParamLookback, 1 }, "Lookback",
        kLookbackChoices, 3));

    // Judder — number of rhythmic retriggers per slice. 1 = no stutter,
    // kMaxJudder = long cascading pitched-delay tail. Pair with DECAY
    // and SLIDE for the Panda Bear "snare goes up up up up" sound.
    layout.add (std::make_unique<API>(
        juce::ParameterID { kParamJudder, 1 }, "Judder", 1, kMaxJudder, 1));

    // Judder Div — rate of the stutters. Shorter than Division for the
    // tight machine-gun flavour, or equal to Division for steady repeats.
    // Default index 3 = "1/32" in the extended 1/128..1/2 list.
    layout.add (std::make_unique<APC>(
        juce::ParameterID { kParamJudderDiv, 1 }, "Judder Div",
        kJudderDivChoices, 3));

    // Pitch — semitones, applied as a playback-rate change. No formant
    // correction: pitching up also speeds the slice up.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamPitch, 1 }, "Pitch",
        NR (-24.0f, 24.0f, 0.01f), 0.0f));

    // Slide — per-judder semitone bump. Set Judder to 12, Slide to +2
    // and Decay to ~0.85 for a Mr Noah pitched-up vocal tail.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamSlide, 1 }, "Slide",
        NR (-12.0f, 12.0f, 0.01f), 0.0f));

    // Decay — amplitude multiplier applied at every judder retrigger.
    // 1.0 = no decay (every repeat is equally loud, machine-gun feel).
    // 0.0 = only the first hit sounds, all repeats silent.
    // ~0.85 = natural dub-delay tail.
    //
    // v0.13 — the percentage display was only set on the UI slider's
    // textFromValueFunction; the parameter itself reported a raw 0..1
    // number to the host and Logic's plugin header strip was showing
    // "1.0" instead of "80%". Adding a parameter-level stringFromValue
    // fixes both the host readout and keeps the slider text consistent.
    // (Helpers defined once above for both Chance and Decay.)
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamDecay, 1 }, "Decay",
        NR (0.0f, 1.0f, 0.001f), 0.8f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // Reverse — probability that a new slice plays backward.
    // 0 = always forward, 1 = always reversed, 0.5 = coin flip per fire.
    // Reversed slices still respect pitch and slide, so Reverse + Slide
    // gives you the classic tape-rewind-into-pitched-tail thing.
    // v0.13 — displayed as %.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamReverse, 1 }, "Reverse",
        NR (0.0f, 1.0f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // Rate — playback speed multiplier, stepped. 1x is unity. Slower
    // rates stretch the slice and drop pitch (poor-man's time stretch
    // territory at 1/4x with Pitch +24, which roughly cancels out);
    // faster rates chipmunk it. Independent from the per-judder Slide.
    layout.add (std::make_unique<APC>(
        juce::ParameterID { kParamRate, 1 }, "Rate",
        kRateChoices, 3));

    // v0.21 — CRUSH split into BITS (bit-depth reduction) and RATE
    // (sample-rate decimation). kParamCrunch stays as the bit-depth
    // knob for state compatibility with older saves.
    //
    // Display: BITS shows "16-bit" → "2-bit" (mirrors applyCrunch:
    // bits = 16 - int(v * 14)), "OFF" at 0%.
    // RATE shows "÷1" → "÷48" (mirrors applyCrunch:
    // hold = 1 + int(v² * 47)), "OFF" at 0%.
    auto bitsString = [] (float v, int /*maxLen*/)
    {
        if (v < 0.001f) return juce::String ("OFF");
        const int bits = 16 - (int) (v * 14.0f);
        return juce::String (bits) + "-bit";
    };
    auto bitsValue = [] (const juce::String& text) -> float
    {
        if (text.containsIgnoreCase ("off")) return 0.0f;
        const int bits = text.getIntValue();
        if (bits >= 16) return 0.0f;
        if (bits <= 2)  return 1.0f;
        return (float) (16 - bits) / 14.0f;
    };

    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamCrunch, 1 }, "Bits",
        NR (0.0f, 1.0f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (bitsString)
            .withValueFromStringFunction (bitsValue)));

    // v0.44 — RATE capped at ~2kHz. maxHold is SR-dependent but the
    // APVTS string functions don't have access to the live SR, so we
    // use a representative 44.1k (hold max ≈ 22). The real audio path
    // and the editor display both use the actual SR at runtime.
    auto rateString = [] (float v, int /*maxLen*/)
    {
        if (v < 0.001f) return juce::String ("OFF");
        const float t = v * v;
        const int maxHold = 22;  // ~2kHz @ 44.1k
        const int hold = 1 + (int) (t * (float) (maxHold - 1));
        return juce::String ("/") + juce::String (hold);
    };
    auto rateValue = [] (const juce::String& text) -> float
    {
        if (text.containsIgnoreCase ("off")) return 0.0f;
        const int hold = text.getTrailingIntValue();
        if (hold <= 1)  return 0.0f;
        const int maxHold = 22;
        if (hold >= maxHold) return 1.0f;
        return std::sqrt ((float) (hold - 1) / (float) (maxHold - 1));
    };

    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamCrushRate, 1 }, "Crunch Rate",
        NR (0.0f, 1.0f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (rateString)
            .withValueFromStringFunction (rateValue)));

    // Mix — how much of the chop is layered on top of the dry during
    // an active slice. 0 = dry only, 1 = dry + full-level chop. The
    // dry signal always plays at full; Mix only controls the wet
    // tail's loudness. This is the pitched-delay layering mode;
    // beat-repeat replace mode will come back as a toggle later.
    // v0.13 — displayed as %.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamMix, 2 }, "Mix",
        NR (0.0f, 1.0f, 0.001f), 0.5f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // v0.42.6 — Global Mix: master wet/dry fader that scales ALL Acts'
    // individual mix values. 100% = per-Act mix is used as-is. 0% = fully
    // dry regardless of Act. NOT stored per-Act — it's a session-wide
    // performance control.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamGlobalMix, 1 }, "Global Mix",
        NR (0.0f, 1.0f, 0.001f), 0.5f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // Freeze — when true, the circular buffer stops capturing. The
    // voice keeps chopping whatever was in there at the moment of
    // freeze. Fun for grabbing a snapshot and wrecking it.
    layout.add (std::make_unique<APB>(
        juce::ParameterID { kParamFreeze, 1 }, "Freeze", false));

    // Seq Rate — how fast the scene sequencer advances from step to
    // step. Independent from Division so you can have a slow
    // sequencer clock while scenes still chop at fine divisions.
    layout.add (std::make_unique<APC>(
        juce::ParameterID { kParamSeqRate, 1 }, "Seq Rate",
        kSeqRateChoices, 1)); // default 1/16

    // --- FX row (tape / sampler character) --------------------------------
    // All six are per-Act (baked into scenes on STORE) so you can have
    // Act A clean and Act D smeared into oblivion.

    // v0.13 — all FX/mangle knobs below are now % displayed via the
    // shared pctString/pctValue helpers declared at the top of
    // createParameterLayout. Raw 0..1 value unchanged — just the
    // host/plugin readout.

    // DRIVE — pre-voice tanh saturation. 0 = unity, 1 = crushed.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamDrive, 1 }, "Drive",
        NR (0.0f, 1.0f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // TONE — bipolar tilt EQ. 0.5 = bypass (no effect).
    // Below 0.5: LP darkening (SVF cutoff drops toward ~30 Hz).
    // Above 0.5: HF shelf boost (air/brightness).
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamTone, 2 }, "Tone",
        NR (0.0f, 1.0f, 0.001f), 0.5f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // FEEDBACK — voice output re-injected into the circular buffer so
    // subsequent lookbacks grab the previous fire's residue. The S950
    // "runaway loop" move. Capped well below 1.0 to keep DC in check.
    // Max raw = 0.95, so displayed max = "95%".
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamFeedback, 1 }, "Feedback",
        NR (0.0f, 0.95f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // TAPE — combined wow (slow drift) + flutter (fast wobble) + subtle
    // hiss. Ramps from clean through gentle drift to full mangled
    // cassette. Replaces the old separate Wow/Flutter/Air knobs.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamTape, 1 }, "Tape",
        NR (0.0f, 1.0f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // RING MOD — ring modulation effect.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamRingMod, 1 }, "Ring Mod",
        NR (0.0f, 1.0f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // VARISPEED — pitch and time-stretching effect.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamVarispeed, 1 }, "Varispeed",
        NR (0.0f, 1.0f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // STEREO — stereo spread and width effect.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamStereo, 1 }, "Stereo",
        NR (0.0f, 1.0f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // STRETCH — time-stretch effect.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamStretch, 1 }, "Stretch",
        NR (0.0f, 1.0f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // --- mangle row (v0.6) ---------------------------------------------
    // Six more per-Act destroyers. They sit below the tape row in the
    // UI and are where the really nasty moves live.

    // SHIMMER — aliased-residual bit-crush. At low values adds a
    // glistening high-end sparkle; at high values turns slices into
    // buzzing ring-modded artefacts. Separate from CRUSH (which
    // quantises into lo-fi grit) — SHIMMER is the brighter flavour.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamShimmer, 1 }, "Shimmer",
        NR (0.0f, 1.0f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // RES — resonance of the TONE filter. Pairs with TONE: when TONE
    // is fully open RES has nothing to peak, but pull TONE down and
    // RES lets you ping the cutoff for filter-sweep screams.
    // Max raw = 0.98 so displayed max = "98%".
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamRes, 1 }, "Res",
        NR (0.0f, 0.98f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // FOLD — sine wavefolder. At 0 the signal is untouched; at 1 you
    // get buzzy metallic fold-over harmonics. Fun with drums.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamFold, 1 }, "Fold",
        NR (0.0f, 1.0f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // SHAPE — bipolar transient control (v0.5.0, was GATE).
    // Centre (0) = bypass. Left (negative) = Soft (attack softening,
    // LPG-style decay). Right (positive) = Snappy (transient emphasis,
    // fast attack boost). Replaces the old Gate's 0..1 LPG-only range
    // with a wider creative palette.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamGate, 1 }, "Shape",
        NR (-1.0f, 1.0f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int) -> juce::String
            {
                if (std::abs (v) < 0.005f) return "0%";
                const int pct = juce::roundToInt (v * 100.0f);
                return (v < 0.0f ? juce::String (pct) + "% Soft"
                                 : "+" + juce::String (pct) + "% Snappy");
            })
            .withValueFromStringFunction ([] (const juce::String& s) -> float
            {
                return s.getFloatValue() * 0.01f;
            })));

    // SMEAR — short delay line with feedback the voice bleeds into.
    // Wet blur that makes chops sound "glued" and haunted. At max
    // it's basically a comb filter going Ursula-K-Le-Guin on the tail.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamSmear, 1 }, "Smear",
        NR (0.0f, 1.0f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamStutter, 1 }, "Stutter",
        NR (0.0f, 1.0f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // CHAOS — the crazy one. Rolls a dice every sample; if it lands,
    // it either holds the last sample, drops to zero, flips polarity,
    // or jumps read position by a few samples. At 1.0 the slice
    // disintegrates into random static. Use sparingly. Or don't.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamChaos, 1 }, "Chaos",
        NR (0.0f, 1.0f, 0.001f), 0.0f,
        juce::AudioParameterFloatAttributes()
            .withStringFromValueFunction (pctString)
            .withValueFromStringFunction (pctValue)));

    // --- v0.7 scale quantise (global — not per-Act) -------------------
    // Snaps PITCH to the nearest in-scale semitone. SLIDE is deliberately
    // left free: it's a per-judder delta and quantising it kills its
    // pitched-tail character. Default root C, default scale Off so the
    // plugin behaves identically to v0.6 until the user opts in.
    layout.add (std::make_unique<APC>(
        juce::ParameterID { kParamScaleRoot, 1 }, "Scale Root",
        kScaleRootChoices, 0));
    layout.add (std::make_unique<APC>(
        juce::ParameterID { kParamScaleType, 1 }, "Scale",
        makeScaleTypeChoices(), 0));  // 0 = Off

    // Global Swing — delays every other fire for a shuffled feel.
    // Range 0.5–0.75 matches the drum-machine convention: 50% =
    // straight, 66.67% = triplet, 75% = dotted-eighth.
    layout.add (std::make_unique<APF>(
        juce::ParameterID { kParamSwing, 1 }, "Swing",
        NR (0.5f, 0.75f, 0.001f), 0.5f,
        AudioParameterFloatAttributes()
            .withStringFromValueFunction ([] (float v, int)
            {
                return juce::String ((int) std::round (v * 100.0f)) + "%";
            })));

    return layout;
}

// ----------------------------------------------------------------------------
//  Static helpers: ratio table + lockable-param slot map
// ----------------------------------------------------------------------------
namespace
{
    // A:B trig condition table. Index 0 is the "no lock" sentinel
    // (inherit Act chance); 1..kNumRatios-1 are actual A:B conditions.
    //
    // v0.12 — semantics changed. Earlier versions were visit-counted
    // ("fire A of every B visits to this step"). The new rule is
    // pattern-play based: the step fires on the Ath pattern play out
    // of every B consecutive pattern plays, so 3:3 fires only on the
    // 3rd play (every B plays), and N:N entries are now meaningful
    // (they're the "on the last play of every cycle" flavour). Full
    // table: 1:2, 1:3, 2:3, 3:3, 1:4, 2:4, 3:4, 4:4, 1:5, 2:5, 3:5,
    // 4:5, 5:5, 1:6, 1:7.
    // v0.38 — extended with conditional-trigger sentinels beyond the A:B
    // ratios. Non-ratio kinds are encoded with n<0 so getRatioNM still
    // returns them without collision:
    //     n = -1, m = 0  → IF PREV  (fire iff the previous step fired)
    //     n = -2, m = 0  → NOT PREV (fire iff the previous step did NOT fire)
    //     n = -3, m = 0  → ONCE     (fire only on the first pattern pass
    //                                after transport start, then latch off)
    //     n = -10, m = P → random % (P percent roll each visit, where
    //                                P is stored directly in m: 25,50,75)
    // Callers that care about "is this a ratio" should check m > 0 AND
    // n > 0 (because random-% also uses m, but with n == -10).
    struct RatioEntry { int n, m; };
    static const RatioEntry kRatioTable[] = {
        {  0, 0 },  // 0 = unlocked / inherit Act chance
        {  1, 2 },
        {  2, 2 },
        {  1, 3 },
        {  2, 3 },
        {  3, 3 },
        {  1, 4 },
        {  2, 4 },
        {  3, 4 },
        {  4, 4 },
        {  1, 5 },
        {  2, 5 },
        {  3, 5 },
        {  4, 5 },
        {  5, 5 },
        {  1, 6 },
        {  1, 7 },
        { -1, 0 },   // 17: PREV
        { -2, 0 },   // 18: !PREV
        { -3, 0 },   // 19: ONCE
        { -10, 25 }, // 20: 25% random per visit
        { -10, 50 }, // 21: 50%
        { -10, 75 }  // 22: 75%
    };
    static_assert ((int) (sizeof(kRatioTable) / sizeof(kRatioTable[0]))
                       == LoopSaboteurProcessor::kNumRatios,
                   "kRatioTable must match kNumRatios");

    // Stable lock-slot layout for p-locks. The order is what gets
    // serialized — don't reorder without bumping the save-file format
    // or adding name-based translation in setStateInformation.
    struct LockableEntry { const char* id; };
    static const LockableEntry kLockable[] = {
        { LoopSaboteurProcessor::kParamDivision },
        { LoopSaboteurProcessor::kParamLookback },
        { LoopSaboteurProcessor::kParamRate     },
        { LoopSaboteurProcessor::kParamJudder   },
        { LoopSaboteurProcessor::kParamJudderDiv },
        { LoopSaboteurProcessor::kParamPitch    },
        { LoopSaboteurProcessor::kParamSlide    },
        { LoopSaboteurProcessor::kParamDecay    },
        { LoopSaboteurProcessor::kParamReverse  },
        { LoopSaboteurProcessor::kParamCrunch   },
        { LoopSaboteurProcessor::kParamCrushRate },
        { LoopSaboteurProcessor::kParamMix      },
        { LoopSaboteurProcessor::kParamDrive    },
        { LoopSaboteurProcessor::kParamTone     },
        { LoopSaboteurProcessor::kParamFeedback },
        { LoopSaboteurProcessor::kParamTape     },
        { LoopSaboteurProcessor::kParamRingMod  },
        { LoopSaboteurProcessor::kParamVarispeed },
        { LoopSaboteurProcessor::kParamStereo   },
        { LoopSaboteurProcessor::kParamStretch  },
        { LoopSaboteurProcessor::kParamShimmer  },
        { LoopSaboteurProcessor::kParamRes      },
        { LoopSaboteurProcessor::kParamFold     },
        { LoopSaboteurProcessor::kParamGate     },
        { LoopSaboteurProcessor::kParamSmear    },
        { LoopSaboteurProcessor::kParamStutter  },
        { LoopSaboteurProcessor::kParamChaos    },
        { LoopSaboteurProcessor::kParamChance   }, // also lockable — per-step act chance override
        { LoopSaboteurProcessor::kParamGlobalMix } // v0.42.6 — session-wide wet/dry scaler
    };
    static_assert ((int) (sizeof(kLockable) / sizeof(kLockable[0]))
                       == LoopSaboteurProcessor::kNumLockableParams,
                   "kLockable must match kNumLockableParams");
}

void LoopSaboteurProcessor::getRatioNM (int ratioIdx, int& n, int& m) noexcept
{
    ratioIdx = juce::jlimit (0, kNumRatios - 1, ratioIdx);
    n = kRatioTable[ratioIdx].n;
    m = kRatioTable[ratioIdx].m;
}

juce::String LoopSaboteurProcessor::ratioLabel (int ratioIdx)
{
    // v0.12 — "A:B" label. We used to append a percentage
    // ("1/2  ·  50%") which is meaningless under the new pattern-play
    // semantics (3:3 is NOT 100%, it's "every 3rd play"), so the label
    // is now just the trig condition.
    // v0.38 — new sentinel kinds (n < 0) render as short mnemonics
    // instead of "N:M" since they're not ratios.
    ratioIdx = juce::jlimit (0, kNumRatios - 1, ratioIdx);
    if (ratioIdx == 0) return "--";
    const int n = kRatioTable[ratioIdx].n;
    const int m = kRatioTable[ratioIdx].m;
    if (n == -1 && m == 0) return "PREV";
    if (n == -2 && m == 0) return "!PREV";
    if (n == -3 && m == 0) return "ONCE";
    if (n == -10) return juce::String (m) + "%";
    return juce::String (n) + ":" + juce::String (m);
}

juce::String LoopSaboteurProcessor::ratioShortLabel (int ratioIdx)
{
    // Same as ratioLabel but reserved for cell overlays; kept as a
    // separate function in case we decide to shrink the on-cell form
    // later (e.g. drop to a colour code for big patterns).
    ratioIdx = juce::jlimit (0, kNumRatios - 1, ratioIdx);
    if (ratioIdx == 0) return "--";
    const int n = kRatioTable[ratioIdx].n;
    const int m = kRatioTable[ratioIdx].m;
    if (n == -1 && m == 0) return "PREV";
    if (n == -2 && m == 0) return "!PREV";
    if (n == -3 && m == 0) return "ONCE";
    if (n == -10) return juce::String (m) + "%";
    return juce::String (n) + ":" + juce::String (m);
}

int LoopSaboteurProcessor::migrateRatioFromV09 (int oldIdx) noexcept
{
    // v0.9 had 17 entries (visit-counted semantics):
    //  0 -, 1 1:2, 2 2:2, 3 1:3, 4 2:3, 5 3:3, 6 1:4, 7 2:4, 8 3:4,
    //  9 4:4, 10 1:5, 11 2:5, 12 3:5, 13 4:5, 14 5:5, 15 1:6, 16 1:8
    //
    // v0.12 brings back the N:N entries under pattern-
    // play semantics, BUT in v0.9 those meant "always fire", not "only
    // the last play of the cycle" — so we still collapse them to 0
    // (unlocked) to preserve the authored behaviour. 1:8 has no
    // equivalent in the v0.12 table and collapses to 0 as well.
    static const int map[] = {
        0,   // 0 unlocked
        1,   // 1:2 -> v0.12 1 (1:2)
        0,   // 2:2 -> unlocked (was 100% visit-count)
        2,   // 1:3 -> v0.12 2 (1:3)
        3,   // 2:3 -> v0.12 3 (2:3)
        0,   // 3:3 -> unlocked (was 100% visit-count)
        5,   // 1:4 -> v0.12 5 (1:4)
        6,   // 2:4 -> v0.12 6 (2:4)
        7,   // 3:4 -> v0.12 7 (3:4)
        0,   // 4:4 -> unlocked
        9,   // 1:5 -> v0.12 9  (1:5)
        10,  // 2:5 -> v0.12 10 (2:5)
        11,  // 3:5 -> v0.12 11 (3:5)
        12,  // 4:5 -> v0.12 12 (4:5)
        0,   // 5:5 -> unlocked
        14,  // 1:6 -> v0.12 14 (1:6)
        0    // 1:8 -> unlocked (no 1:8 in v0.12)
    };
    if (oldIdx < 0 || oldIdx >= (int) (sizeof (map) / sizeof (map[0])))
        return 0;
    return map[oldIdx];
}

int LoopSaboteurProcessor::migrateRatioFromV11 (int oldIdx) noexcept
{
    // v0.10/v0.11 had 13 entries (visit-counted semantics):
    //  0 -, 1 1:2, 2 1:3, 3 2:3, 4 1:4, 5 2:4, 6 3:4, 7 1:5, 8 2:5,
    //  9 3:5, 10 4:5, 11 1:6, 12 1:8
    //
    // v0.12 reshuffles the table to put 3:3, 4:4, 5:5 in their proper
    // slots and gains 1:7 in place of 1:8. Remap the matching A:B
    // entries; 1:8 has no counterpart and collapses to 0.
    static const int map[] = {
        0,   // 0 unlocked
        1,   // 1:2 -> 1
        2,   // 1:3 -> 2
        3,   // 2:3 -> 3
        5,   // 1:4 -> 5  (slot 4 is now 3:3)
        6,   // 2:4 -> 6
        7,   // 3:4 -> 7
        9,   // 1:5 -> 9  (slot 8 is now 4:4)
        10,  // 2:5 -> 10
        11,  // 3:5 -> 11
        12,  // 4:5 -> 12
        14,  // 1:6 -> 14 (slot 13 is now 5:5)
        0    // 1:8 -> unlocked (no 1:8 in v0.12 — closest would be
             //                  1:7 at slot 15 but that changes timing)
    };
    if (oldIdx < 0 || oldIdx >= (int) (sizeof (map) / sizeof (map[0])))
        return 0;
    return map[oldIdx];
}

int LoopSaboteurProcessor::lockableIndexForId (const juce::String& id)
{
    for (int i = 0; i < kNumLockableParams; ++i)
        if (id == kLockable[i].id)
            return i;
    return -1;
}

const juce::String& LoopSaboteurProcessor::lockableIdForIndex (int slot)
{
    static const juce::String empty;
    if (slot < 0 || slot >= kNumLockableParams)
        return empty;
    // Juce strings are pooled on demand; store one per slot so we can
    // return a stable reference. Built lazily on first call.
    static juce::String cache[kNumLockableParams];
    static bool built = false;
    if (! built)
    {
        for (int i = 0; i < kNumLockableParams; ++i)
            cache[i] = juce::String (kLockable[i].id);
        built = true;
    }
    return cache[slot];
}

// ----------------------------------------------------------------------------
//  Construction
// ----------------------------------------------------------------------------
LoopSaboteurProcessor::LoopSaboteurProcessor()
    : AudioProcessor (BusesProperties()
                        .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                        .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "state", createParameterLayout())
{
    // Cache raw atomic-float pointers once. These are stable for the
    // lifetime of the APVTS, so processBlock can load() them lock-free.
    pChance    = apvts.getRawParameterValue (kParamChance);
    pDivision  = apvts.getRawParameterValue (kParamDivision);
    pLookback  = apvts.getRawParameterValue (kParamLookback);
    pJudder    = apvts.getRawParameterValue (kParamJudder);
    pJudderDiv = apvts.getRawParameterValue (kParamJudderDiv);
    pPitch     = apvts.getRawParameterValue (kParamPitch);
    pSlide     = apvts.getRawParameterValue (kParamSlide);
    pDecay     = apvts.getRawParameterValue (kParamDecay);
    pReverse   = apvts.getRawParameterValue (kParamReverse);
    pRate      = apvts.getRawParameterValue (kParamRate);
    pCrunch    = apvts.getRawParameterValue (kParamCrunch);
    pCrushRate = apvts.getRawParameterValue (kParamCrushRate);
    pMix       = apvts.getRawParameterValue (kParamMix);
    pGlobalMix = apvts.getRawParameterValue (kParamGlobalMix);
    globalMixLockSlot = lockableIndexForId (kParamGlobalMix);
    pFreeze    = apvts.getRawParameterValue (kParamFreeze);
    pSeqRate   = apvts.getRawParameterValue (kParamSeqRate);
    pDrive     = apvts.getRawParameterValue (kParamDrive);
    pTone      = apvts.getRawParameterValue (kParamTone);
    pFeedback  = apvts.getRawParameterValue (kParamFeedback);
    pTape      = apvts.getRawParameterValue (kParamTape);
    pRingMod   = apvts.getRawParameterValue (kParamRingMod);
    pVarispeed = apvts.getRawParameterValue (kParamVarispeed);
    pStereo    = apvts.getRawParameterValue (kParamStereo);
    pStretch   = apvts.getRawParameterValue (kParamStretch);
    pShimmer   = apvts.getRawParameterValue (kParamShimmer);
    pRes       = apvts.getRawParameterValue (kParamRes);
    pFold      = apvts.getRawParameterValue (kParamFold);
    pGate      = apvts.getRawParameterValue (kParamGate);
    pSmear     = apvts.getRawParameterValue (kParamSmear);
    pStutter   = apvts.getRawParameterValue (kParamStutter);
    pChaos     = apvts.getRawParameterValue (kParamChaos);
    pScaleRoot = apvts.getRawParameterValue (kParamScaleRoot);
    pScaleType = apvts.getRawParameterValue (kParamScaleType);
    pSwing     = apvts.getRawParameterValue (kParamSwing);

    initSceneDefaults();

    // v0.38 — scan user-preset folder once at construction. Cheap: just
    // enumerates .lpsbact files and parses their userName attribute.
    // The editor rebuilds its menu from this list each time it's opened.
    rescanUserPresets();

    // v0.7.0 — load saved UI defaults (tooltips, page-follow, etc.)
    // so new instances start with the user's preferred settings.
    loadGlobalDefaults();

    // v0.33 — per-LFO default rates. All four LFOs default to Off (rateIdx 0)
    // is actually what the audio engine expects now, BUT giving each LFO a
    // different sensible default rate makes the MOD page instantly legible
    // when opened for the first time. Depths stay 0 (audible off) so this
    // is cosmetic until the user raises depth or targets a param.
    //   LFO 0 = 1 bar (rateIdx 8)
    //   LFO 1 = 2 bars (rateIdx 9)
    //   LFO 2 = 4 bars (rateIdx 10)
    //   LFO 3 = 8 bars (rateIdx 11)
    {
        constexpr int kDefaultRates[kNumLfosPerAct] = { 8, 9, 10, 11 };
        for (int si = 0; si < kNumScenes; ++si)
            for (int li = 0; li < kNumLfosPerAct; ++li)
                lfos[si][li].rateIdx.store (kDefaultRates[li]);
    }

    // v0.10 — auto-mirror knob changes into the selected Act. Listen
    // to every lockable parameter (which is the same set of knobs the
    // Acts store) plus Chance and Division-family controls so every
    // user-facing knob change is captured. Registering on specific IDs
    // keeps the callback fast — one per change rather than one per all
    // params.
    static const char* const kMirrorIds[] = {
        kParamDivision, kParamLookback, kParamRate, kParamJudder, kParamJudderDiv,
        kParamPitch, kParamSlide, kParamDecay, kParamReverse, kParamCrunch, kParamCrushRate,
        kParamMix, kParamChance, kParamDrive, kParamTone, kParamFeedback,
        kParamTape, kParamRingMod, kParamVarispeed, kParamStereo, kParamStretch, kParamShimmer, kParamRes,
        kParamFold, kParamGate, kParamSmear, kParamStutter, kParamChaos
    };
    for (auto* id : kMirrorIds)
        apvts.addParameterListener (id, this);
}

LoopSaboteurProcessor::~LoopSaboteurProcessor()
{
    // Unregister the APVTS listener so lingering callbacks can't
    // reach a half-destructed processor. Mirror the list in the ctor.
    static const char* const kMirrorIds[] = {
        kParamDivision, kParamLookback, kParamRate, kParamJudder, kParamJudderDiv,
        kParamPitch, kParamSlide, kParamDecay, kParamReverse, kParamCrunch, kParamCrushRate,
        kParamMix, kParamChance, kParamDrive, kParamTone, kParamFeedback,
        kParamTape, kParamRingMod, kParamVarispeed, kParamStereo, kParamStretch, kParamShimmer, kParamRes,
        kParamFold, kParamGate, kParamSmear, kParamStutter, kParamChaos
    };
    for (auto* id : kMirrorIds)
        apvts.removeParameterListener (id, this);
}

// v0.10 — mirror any knob change into the currently-selected Act's
// stored values, and mark the Act as "stored" so the Act button shows
// its populated dot. Called by the APVTS for whichever parameter id
// we registered. Runs on the message thread for UI drags and on the
// audio thread for host automation — either is fine because we only
// touch atomics.
void LoopSaboteurProcessor::parameterChanged (const juce::String& paramId, float newValue)
{
    if (suppressAutoMirror.load()) return;

    const int sel = selectedScene.load();
    if (sel < 0 || sel >= kNumScenes) return;
    auto& s = scenes[sel];

    // Route each id to its SceneData field. The dispatch is a cascade
    // of equals-checks — cheap compared to an indirect mirror table
    // and avoids the need to keep a second parallel array.
    if      (paramId == kParamDivision)  s.divisionIdx .store ((int) std::round (newValue));
    else if (paramId == kParamLookback)  s.lookbackIdx .store ((int) std::round (newValue));
    else if (paramId == kParamRate)      s.rateIdx     .store ((int) std::round (newValue));
    else if (paramId == kParamJudder)    s.judderCount .store (juce::jlimit (1, kMaxJudder,
                                                                              (int) std::round (newValue)));
    else if (paramId == kParamJudderDiv) s.judderDivIdx.store ((int) std::round (newValue));
    else if (paramId == kParamPitch)     s.pitchSt      .store (newValue);
    else if (paramId == kParamSlide)     s.slideSt      .store (newValue);
    else if (paramId == kParamDecay)     s.decay        .store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamReverse)   s.reverseChance.store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamCrunch)    s.crunch       .store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamCrushRate) s.crushRate    .store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamMix)       s.mix          .store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamChance)    s.chance       .store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamDrive)     s.drive        .store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamTone)      s.tone         .store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamFeedback)  s.feedback     .store (juce::jlimit (0.0f, 0.95f, newValue));
    else if (paramId == kParamTape)      s.tape         .store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamRingMod)   s.ringMod      .store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamVarispeed) s.varispeed    .store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamStereo)    s.stereo       .store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamStretch)   s.stretch      .store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamShimmer)   s.shimmer      .store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamRes)       s.res          .store (juce::jlimit (0.0f, 0.98f, newValue));
    else if (paramId == kParamFold)      s.fold         .store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamGate)      s.gate         .store (juce::jlimit (-1.0f, 1.0f,  newValue));
    else if (paramId == kParamSmear)     s.smear        .store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamStutter)   s.stutter      .store (juce::jlimit (0.0f, 1.0f,  newValue));
    else if (paramId == kParamChaos)     s.chaos        .store (juce::jlimit (0.0f, 1.0f,  newValue));

    sceneStored[sel].store (true);

    // BUG 1 fix: user edited a knob, so the scene is no longer matching a factory preset
    if (!applyingPreset)
    {
        scenePresetIdx[sel].store (-1);
        s.cageSilence.store (false);   // v0.7.0 — tweaking any knob exits Cage mode
    }
}

// ----------------------------------------------------------------------------
//  Scene + sequencer management. Writers run on the message thread,
//  readers on the audio thread — all via atomics, no locks.
// ----------------------------------------------------------------------------
void LoopSaboteurProcessor::initSceneDefaults()
{
    // Every scene starts as a copy of the default knob values so
    // newly-stored scenes aren't silent or surprising.
    for (auto& s : scenes)
    {
        s.divisionIdx  .store (2);      // 1/16
        s.lookbackIdx  .store (3);      // 1/16 (index shifted after prepending 1/128)
        s.rateIdx      .store (3);      // 1x
        s.judderCount  .store (1);
        s.judderDivIdx .store (3);      // 1/32 in the extended list
        s.pitchSt      .store (0.0f);
        s.slideSt      .store (0.0f);
        s.decay        .store (0.8f);
        s.reverseChance.store (0.0f);
        s.crunch       .store (0.0f);
        s.crushRate    .store (0.0f);
        s.mix          .store (0.5f);
        s.chance       .store (1.0f);
        s.drive        .store (0.0f);
        s.tone         .store (0.5f);   // bipolar centre = bypass
        s.feedback     .store (0.0f);
        s.tape         .store (0.0f);
        s.ringMod      .store (0.0f);
        s.varispeed    .store (0.0f);
        s.stereo       .store (0.0f);
        s.stretch      .store (0.0f);
        s.shimmer      .store (0.0f);
        s.res          .store (0.0f);
        s.fold         .store (0.0f);
        s.gate         .store (0.0f);
        s.smear        .store (0.0f);
        s.stutter      .store (0.0f);
        s.chaos        .store (0.0f);

        // v0.5.1 — per-Act Character mode selections.
        s.driveType      .store (0);
        s.foldTopology   .store (0);
        s.shimmerOctave  .store (0);
        s.smearCharacter .store (0);
        s.stutterWindow  .store (0);
        s.varispeedCurve .store (0);
        s.slideCurve     .store (0);
        s.reverseMode    .store (0);
        s.tapeMode       .store (0);
        s.ringModWave    .store (0);
        s.chaosDistribution .store (0);
        s.feedbackCharacter .store (0);
        s.stretchMode    .store (0);
        s.judderShape    .store (0);
        s.lookbackBehaviour .store (0);
        s.decayCurve    .store (0);
        s.cageSilence   .store (false);  // v0.7.0
    }

    for (auto& s : sceneStored)
        s.store (false);

    // BUG 1 fix: initialize per-scene preset tracking to "no preset"
    for (auto& p : scenePresetIdx)
        p.store (-1);

    for (auto& s : steps)
        s.store (-1);

    // v0.7 — per-step arrays all reset to their "inert" defaults.
    for (auto& r : stepRatio)       r.store (0);       // 0 = no lock
    for (auto& f : stepFreezeHold)  f.store (false);
    for (auto& r : stepRatchet)     r.store (1);       // 1 = no ratchet (v0.12)
    for (auto& m : stepMute)        m.store (false);   // v0.13 — unmuted
    for (int i = 0; i < kMaxSteps; ++i)
        stepRatioVisit[i] = 0;
    ratioLastSeenSeqStep = -1;
    ratioLastAllowed     = true;
    ratioPatternPlayCount = 0;
    lastSeqStepAbsForRatio = -1;
    pendingRatchet = {};

    // P-lock array — NaN means "not locked".
    const float nanF = std::nanf ("");
    for (int s = 0; s < kMaxSteps; ++s)
        for (int k = 0; k < kNumLockableParams; ++k)
            stepParamLock[s * kNumLockableParams + k].store (nanF);

    // Input peak ring for the waveform strip — start silent.
    for (auto& p : peakRing) p.store (0.0f);
    peakWritePos   .store (0);
    peakAccum       = 0.0f;
    peakAccumCount  = 0;

    momentaryFreeze.store (false);

    seqLength         .store (kDefaultSteps);
    seqOn             .store (false);
    selectedScene     .store (0);
    currentPlayingStep.store (-1);
}

void LoopSaboteurProcessor::storeKnobsAsScene (int sceneIdx)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    auto& s = scenes[sceneIdx];
    s.divisionIdx  .store ((int) std::round (pDivision->load()));
    s.lookbackIdx  .store ((int) std::round (pLookback->load()));
    s.rateIdx      .store ((int) std::round (pRate->load()));
    s.judderCount  .store (juce::jlimit (1, kMaxJudder, (int) std::round (pJudder->load())));
    s.judderDivIdx .store ((int) std::round (pJudderDiv->load()));
    s.pitchSt      .store (pPitch->load());
    s.slideSt      .store (pSlide->load());
    s.decay        .store (juce::jlimit (0.0f, 1.0f, pDecay->load()));
    s.reverseChance.store (juce::jlimit (0.0f, 1.0f, pReverse->load()));
    s.crunch       .store (juce::jlimit (0.0f, 1.0f, pCrunch->load()));
    s.crushRate    .store (juce::jlimit (0.0f, 1.0f, pCrushRate->load()));
    s.mix          .store (juce::jlimit (0.0f, 1.0f, pMix->load()));
    s.chance       .store (juce::jlimit (0.0f, 1.0f, pChance->load()));
    s.drive        .store (juce::jlimit (0.0f, 1.0f,  pDrive   ->load()));
    s.tone         .store (juce::jlimit (0.0f, 1.0f,  pTone    ->load()));
    s.feedback     .store (juce::jlimit (0.0f, 0.95f, pFeedback->load()));
    s.tape         .store (juce::jlimit (0.0f, 1.0f,  pTape    ->load()));
    s.ringMod      .store (juce::jlimit (0.0f, 1.0f,  pRingMod ->load()));
    s.varispeed    .store (juce::jlimit (0.0f, 1.0f,  pVarispeed->load()));
    s.stereo       .store (juce::jlimit (0.0f, 1.0f,  pStereo  ->load()));
    s.stretch      .store (juce::jlimit (0.0f, 1.0f,  pStretch ->load()));
    s.shimmer      .store (juce::jlimit (0.0f, 1.0f,  pShimmer ->load()));
    s.res          .store (juce::jlimit (0.0f, 0.98f, pRes     ->load()));
    s.fold         .store (juce::jlimit (0.0f, 1.0f,  pFold    ->load()));
    s.gate         .store (juce::jlimit (-1.0f, 1.0f,  pGate    ->load()));
    s.smear        .store (juce::jlimit (0.0f, 1.0f,  pSmear   ->load()));
    s.stutter      .store (juce::jlimit (0.0f, 1.0f,  pStutter ->load()));
    s.chaos        .store (juce::jlimit (0.0f, 1.0f,  pChaos   ->load()));
    sceneStored[sceneIdx].store (true);
}

bool LoopSaboteurProcessor::isSceneStored (int sceneIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return false;
    return sceneStored[sceneIdx].load();
}

// v0.12 — reset an Act to the same "factory fresh" values that
// initSceneDefaults uses for a new plugin instance. Earlier versions
// used a separate "pass-through silent" set (chance=0, mix=0) which
// turned out to quietly break non-seq mode: after Clear All Acts the
// live CHANCE/MIX knobs were pushed to 0 too (via loadSceneToKnobs),
// so freehand firing went dead and it LOOKED like the plugin was
// broken. Using real audible defaults keeps non-seq mode alive.
// Clears sceneStored[] so the Act-button tint drops back to "default".
void LoopSaboteurProcessor::resetSceneToDefaults (int sceneIdx)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    auto& s = scenes[sceneIdx];
    // v0.35 — audible default. Previous "4'33\"" ground state (chance=0,
    // mix=0) was a nice concept but confused real workflow: loading the
    // plugin made nothing happen, so people thought it was broken. We
    // now default to Chance 100%, Mix 50%, and zero on every destructive
    // effect. The plugin will slice and play at 1/16 straight away, with
    // the wet signal sitting at equal level to the dry — a usable
    // "pass-through slicer" starting point that makes the first dial of
    // a knob actually musical.
    s.divisionIdx  .store (2);      // 1/16 — same as the factory construction
    s.lookbackIdx  .store (3);      // 1/16 lookback
    s.rateIdx      .store (3);      // 1.0x (no pitch on rate)
    s.judderCount  .store (1);      // 1 judder = no retrigger
    s.judderDivIdx .store (3);
    s.pitchSt      .store (0.0f);
    s.slideSt      .store (0.0f);
    s.decay        .store (0.8f);   // same as initSceneDefaults
    s.reverseChance.store (0.0f);
    s.crunch       .store (0.0f);
    s.crushRate    .store (0.0f);
    s.mix          .store (0.5f);   // 50% wet — audible but not overwhelming
    s.chance       .store (1.0f);   // 100% — slice fires on every grid tick
    s.drive        .store (0.0f);
    s.tone         .store (0.5f);   // bipolar centre = filter bypassed
    s.feedback     .store (0.0f);
    s.tape         .store (0.0f);
    s.ringMod      .store (0.0f);
    s.varispeed    .store (0.0f);
    s.stereo       .store (0.0f);
    s.stretch      .store (0.0f);
    s.shimmer      .store (0.0f);
    s.res          .store (0.0f);
    s.fold         .store (0.0f);
    s.gate         .store (0.0f);
    s.smear        .store (0.0f);
    s.stutter      .store (0.0f);
    s.chaos        .store (0.0f);
    sceneStored[sceneIdx].store (false);
    scenePresetIdx[sceneIdx].store (-1);  // BUG 1 fix: reset no longer matches a preset

    // v0.38 — by default we also wipe the 4 MODs on this Act so that
    // clearing an Act is a truly clean slate. Users who want their
    // modulation rigs to survive a clear can flip the
    // "Preserve MODs when clearing Acts" toggle in the ACT menu.
    if (! preserveLfosOnClear.load())
        resetLfosForScene (sceneIdx);
}

// v0.38 — reset the 4 LFO slots for a single Act back to their factory
// defaults. Matches the defaults declared in LfoState so the Act looks
// like a freshly-loaded one (no MOD activity). Called from
// resetSceneToDefaults when the user has not opted into preserving MODs.
void LoopSaboteurProcessor::resetLfosForScene (int sceneIdx)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    for (int li = 0; li < kNumLfosPerAct; ++li)
    {
        auto& lfo = lfos[sceneIdx][li];
        lfo.shape         .store (kLfoSine);
        lfo.rateIdx       .store (0);           // 0 = Off (no modulation)
        lfo.depth         .store (1.0f);
        lfo.targetSlot    .store (-1);          // no target
        lfo.crossTargetLfo.store (-1);          // no cross-mod
        lfo.crossDepth    .store (1.0f);
        lfo.sync          .store (true);
        lfo.attackMs      .store (5.0f);
        lfo.holdMs        .store (0.0f);
        lfo.decayMs       .store (200.0f);
        lfo.trigMode      .store (kTrigActiveSteps);
        // Runtime state is audio-thread-only; it will naturally settle
        // on the next block, but zero the output so UI peek-reads don't
        // see a stale cyan level.
        lfo.lastOutput = 0.0f;
        lfo.envStage   = 0;
        lfo.envPos     = 0.0;
        lfo.envValue   = 0.0f;
        lfo.envPending = false;
    }
}

// v0.11 — clear every Act back to pass-through defaults and clear the
// populated flags. Convenience wrapper for the "Clear all acts" menu
// entry: walks the eight slots and calls resetSceneToDefaults, then
// reloads whichever Act is currently selected into the knobs so the UI
// redraws from the newly-clean state.
void LoopSaboteurProcessor::resetAllScenesToDefaults()
{
    for (int i = 0; i < kNumScenes; ++i)
        resetSceneToDefaults (i);
}

// ============================================================================
//  v0.38 — User presets. Simple file-based save/load for a single Act.
//
//  Lives at ~/Library/Application Support/LoopSaboteur/UserPresets on macOS
//  (equivalent per-platform). Files are ordinary .lpsbact XML so they're
//  hand-editable, shareable, version-controllable, and load/save round-trip
//  cleanly with the existing Export Act flow.
//
//  The in-memory list is cached and rebuilt by rescanUserPresets(); the
//  editor rebuilds its preset menu after every save/delete so the new
//  entry shows up immediately.
// ============================================================================
juce::File LoopSaboteurProcessor::getUserPresetsDir()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                    .getChildFile ("LoopSaboteur")
                    .getChildFile ("UserPresets");
    if (! base.exists())
        base.createDirectory();   // idempotent
    return base;
}

bool LoopSaboteurProcessor::saveCurrentActAsUserPreset (int sceneIdx,
                                                       const juce::String& displayName,
                                                       const juce::String& category)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes)    return false;
    const auto trimmed = displayName.trim();
    if (trimmed.isEmpty())                          return false;

    auto xml = serializeAct (sceneIdx);
    if (xml == nullptr)                             return false;

    // Stash the display name inside the XML itself so a user who renames
    // the file doesn't lose their original label. The menu prefers this
    // over the filename when present.
    xml->setAttribute ("userName", trimmed);

    // v0.7.0 — if a category is provided, save into a sub-folder.
    auto dir = getUserPresetsDir();
    const auto catTrimmed = category.trim();
    if (catTrimmed.isNotEmpty())
    {
        dir = dir.getChildFile (juce::File::createLegalFileName (catTrimmed));
        if (! dir.exists())
            dir.createDirectory();
    }

    const auto safe   = juce::File::createLegalFileName (trimmed);
    auto       target = dir.getChildFile (safe + ".lpsbact");
    // Avoid clobbering an existing file by appending "-N".
    int suffix = 2;
    while (target.existsAsFile())
        target = dir.getChildFile (safe + "-" + juce::String (suffix++) + ".lpsbact");

    const bool ok = xml->writeTo (target, juce::XmlElement::TextFormat());
    if (ok)
        rescanUserPresets();
    return ok;
}

void LoopSaboteurProcessor::rescanUserPresets()
{
    const auto dir = getUserPresetsDir();
    juce::Array<juce::File> files;
    // v0.7.0 — scan recursively so sub-folders become user categories.
    dir.findChildFiles (files, juce::File::findFiles, /*recursive*/ true, "*.lpsbact");

    // Sort by display name (natural order) so the menu is stable.
    std::vector<UserPresetEntry> fresh;
    fresh.reserve ((size_t) files.size());
    for (const auto& f : files)
    {
        UserPresetEntry e;
        e.file = f;
        if (auto doc = juce::XmlDocument::parse (f))
            e.name = doc->getStringAttribute ("userName",
                                              f.getFileNameWithoutExtension());
        else
            e.name = f.getFileNameWithoutExtension();
        // v0.7.0 — derive category from the sub-folder name relative to
        // the UserPresets root. Files in the root get an empty category.
        const auto parent = f.getParentDirectory();
        if (parent != dir)
            e.category = parent.getFileName();
        fresh.push_back (std::move (e));
    }
    std::sort (fresh.begin(), fresh.end(),
               [] (const UserPresetEntry& a, const UserPresetEntry& b)
               {
                   // Group by category first, then by name within category.
                   const int catCmp = a.category.compareNatural (b.category);
                   if (catCmp != 0) return catCmp < 0;
                   return a.name.compareNatural (b.name) < 0;
               });

    const juce::ScopedLock sl (userPresetsLock);
    userPresets = std::move (fresh);
}

int LoopSaboteurProcessor::getNumUserPresets() const noexcept
{
    const juce::ScopedLock sl (userPresetsLock);
    return (int) userPresets.size();
}

juce::String LoopSaboteurProcessor::getUserPresetName (int idx) const
{
    const juce::ScopedLock sl (userPresetsLock);
    if (idx < 0 || idx >= (int) userPresets.size()) return {};
    return userPresets[(size_t) idx].name;
}

juce::String LoopSaboteurProcessor::getUserPresetCategory (int idx) const
{
    const juce::ScopedLock sl (userPresetsLock);
    if (idx < 0 || idx >= (int) userPresets.size()) return {};
    return userPresets[(size_t) idx].category;
}

juce::StringArray LoopSaboteurProcessor::getUserPresetCategories() const
{
    const juce::ScopedLock sl (userPresetsLock);
    juce::StringArray cats;
    for (const auto& e : userPresets)
        if (e.category.isNotEmpty() && ! cats.contains (e.category))
            cats.add (e.category);
    cats.sort (true);
    return cats;
}

bool LoopSaboteurProcessor::renameUserPresetCategory (const juce::String& oldName,
                                                      const juce::String& newName)
{
    if (oldName.isEmpty() || newName.trim().isEmpty()) return false;
    const auto dir = getUserPresetsDir();
    auto oldDir = dir.getChildFile (juce::File::createLegalFileName (oldName));
    if (! oldDir.isDirectory()) return false;
    auto newDir = dir.getChildFile (juce::File::createLegalFileName (newName.trim()));
    if (newDir.exists()) return false;  // target already exists
    const bool ok = oldDir.moveFileTo (newDir);
    if (ok) rescanUserPresets();
    return ok;
}

bool LoopSaboteurProcessor::deleteUserPresetCategory (const juce::String& categoryName)
{
    if (categoryName.isEmpty()) return false;
    const auto dir = getUserPresetsDir();
    auto catDir = dir.getChildFile (juce::File::createLegalFileName (categoryName));
    if (! catDir.isDirectory()) return false;

    // Move all .lpsbact files to the root, then remove the empty folder.
    juce::Array<juce::File> files;
    catDir.findChildFiles (files, juce::File::findFiles, false, "*.lpsbact");
    for (const auto& f : files)
        f.moveFileTo (dir.getChildFile (f.getFileName()));
    catDir.deleteRecursively (false);
    rescanUserPresets();
    return true;
}

// v0.7.0 — persist UI preferences to a shared config file so every
// new plugin instance starts with the user's preferred settings.
void LoopSaboteurProcessor::saveGlobalDefaults()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                    .getChildFile ("LoopSaboteur");
    if (! base.exists()) base.createDirectory();

    juce::XmlElement xml ("LpsbDefaults");
    xml.setAttribute ("tooltips",       tooltipsEnabled.load());
    xml.setAttribute ("waveform",       waveformVisible.load());
    xml.setAttribute ("pageFollow",     pageFollowsPlayhead.load());
    xml.setAttribute ("uiScale",        (double) uiScale.load());
    xml.setAttribute ("preserveLfos",   preserveLfosOnClear.load());
    xml.setAttribute ("autoFollow",     autoFollowLength.load());

    xml.writeTo (base.getChildFile ("defaults.xml"), juce::XmlElement::TextFormat());
}

void LoopSaboteurProcessor::loadGlobalDefaults()
{
    auto file = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                    .getChildFile ("LoopSaboteur")
                    .getChildFile ("defaults.xml");
    if (! file.existsAsFile()) return;

    auto xml = juce::XmlDocument::parse (file);
    if (xml == nullptr || xml->getTagName() != "LpsbDefaults") return;

    tooltipsEnabled     .store (xml->getBoolAttribute ("tooltips",     true));
    waveformVisible     .store (xml->getBoolAttribute ("waveform",     true));
    pageFollowsPlayhead .store (xml->getBoolAttribute ("pageFollow",   false));
    uiScale             .store ((float) xml->getDoubleAttribute ("uiScale", 1.0));
    preserveLfosOnClear .store (xml->getBoolAttribute ("preserveLfos", false));
    autoFollowLength    .store (xml->getBoolAttribute ("autoFollow",   false));
}

juce::File LoopSaboteurProcessor::getUserPresetFile (int idx) const
{
    const juce::ScopedLock sl (userPresetsLock);
    if (idx < 0 || idx >= (int) userPresets.size()) return {};
    return userPresets[(size_t) idx].file;
}

bool LoopSaboteurProcessor::applyUserPreset (int sceneIdx, int userIdx)
{
    const auto f = getUserPresetFile (userIdx);
    if (f == juce::File()) return false;
    auto doc = juce::XmlDocument::parse (f);
    if (doc == nullptr)    return false;
    if (! loadActFromXml (*doc, sceneIdx)) return false;
    // Loaded from file, not a factory index — mark as such.
    scenePresetIdx[sceneIdx].store (-1);
    return true;
}

bool LoopSaboteurProcessor::deleteUserPreset (int userIdx)
{
    const auto f = getUserPresetFile (userIdx);
    if (f == juce::File())                   return false;
    if (! f.existsAsFile())                  return false;
    const bool ok = f.deleteFile();
    if (ok)
        rescanUserPresets();
    return ok;
}

// v0.11 — stamp a random Act onto each step cell, with probability
// `density` of being populated vs cleared. Only Acts that already
// hold non-default values are candidates — otherwise the sequencer
// would be filled with silent acts and sound broken. Cells that lose
// the roll are cleared (-1) so the result is a fresh lay.
void LoopSaboteurProcessor::randomizeStepPlacements (float density)
{
    density = juce::jlimit (0.0f, 1.0f, density);

    // Collect the indices of populated Acts as the pool to stamp from.
    std::vector<int> pool;
    pool.reserve (kNumScenes);
    for (int i = 0; i < kNumScenes; ++i)
        if (sceneStored[i].load())
            pool.push_back (i);

    // Fallback: if nothing is populated, just clear.
    if (pool.empty())
    {
        for (int i = 0; i < kMaxSteps; ++i)
            steps[i].store (-1);
        return;
    }

    const int len = seqLength.load();
    for (int i = 0; i < len; ++i)
    {
        const float roll = rng.nextFloat();
        if (roll < density)
        {
            const int idx = pool[(size_t) rng.nextInt ((int) pool.size())];
            steps[i].store (idx);
        }
        else
        {
            steps[i].store (-1);
        }
    }
    // Clear any steps beyond the pattern length so a shorter pattern
    // doesn't carry stale randomness on its tail pages.
    for (int i = len; i < kMaxSteps; ++i)
        steps[i].store (-1);
}

// Roll new values for every knob of every populated Act. intensity
// scales the blend between "current value" and "pure random": 1.0
// replaces entirely, 0.0 is a no-op. Distribution is uniform in
// parameter space, not perceptual — the point is surprise, not
// musical sanity.
void LoopSaboteurProcessor::randomizeActValues (float intensity, bool forceAllActs)
{
    intensity = juce::jlimit (0.0f, 1.0f, intensity);
    if (intensity <= 0.0f) return;

    auto blend = [intensity] (float current, float target)
    {
        return current + intensity * (target - current);
    };

    // v0.13 — "randomize values" used to silently no-op when no Acts
    // were populated yet: Steve installed a fresh build, clicked
    // RANDOM -> Re-roll Act values and nothing visibly happened because
    // the selected Act was still "unstored". Now we always include the
    // currently selected Act (marking it stored on the fly), and the
    // forceAllActs flag from randomizeEverything() pulls in every slot.
    const int selected = selectedScene.load();

    // v0.32 — musical randomisation.
    //
    // Pre-v0.32 we rolled every knob across its full range with flat
    // distribution. That reliably produced noise: with chaos, stutter,
    // smear, fold, res and feedback all landing mid-to-high simultaneously
    // there's nothing of the source left to hear. New approach:
    //
    //   1. Weighted integer choices lean on common-music values
    //      (1/16, 1/32 divisions; short lookbacks; low judder).
    //   2. Skewed 0..1 rolls using power-curves so "average" really means
    //      "tame" instead of 0.5.
    //   3. Anchor knobs (mix, chance, decay) stay in broadly-musical
    //      ranges — no more landing on 10% mix or 0% chance.
    //   4. "Destruction budget": of the most aggressive knobs (crunch,
    //      feedback, res, fold, chaos, stutter, smear, ringMod) pick at
    //      most TWO to push high per Act. The rest stay subdued, so each
    //      Act gets a distinct character instead of a wall of noise.

    // Weighted integer pick: indices listed in `weights` (higher weight =
    // more likely). Returns one of `values` picked according to weights.
    auto weightedPick = [this] (const std::vector<int>& values,
                                const std::vector<int>& weights)
    {
        int total = 0;
        for (int w : weights) total += w;
        int roll = rng.nextInt (juce::jmax (1, total));
        for (size_t k = 0; k < values.size(); ++k)
        {
            roll -= weights[k];
            if (roll < 0) return values[k];
        }
        return values.back();
    };

    // Skewed 0..1 roll. curve > 1 biases toward 0; curve < 1 biases toward 1.
    auto rndSkew = [this] (float curve)
    {
        return std::pow (rng.nextFloat(), curve);
    };

    auto rnd01 = [this] { return rng.nextFloat(); };

    for (int i = 0; i < kNumScenes; ++i)
    {
        const bool isSelected = (i == selected);
        const bool shouldRandomize = sceneStored[i].load() || forceAllActs || isSelected;
        if (! shouldRandomize)
            continue;

        if (! sceneStored[i].load())
            sceneStored[i].store (true);

        auto& s = scenes[i];

        // --- Integer choices (weighted toward musical values) ---
        // Division: prefer 1/16 and 1/8 (the meat-and-potatoes chop
        // sizes), allow 1/8T / 1/16T for swung feel, and give the
        // long divisions (1/2 .. 4 bars) a small chance so RANDOM can
        // occasionally land you on a drone-length slice.
        //   0=1/32 1=1/16T 2=1/16 3=1/8T 4=1/8 5=1/4
        //   6=1/2  7=1 bar 8=2 bars 9=4 bars  (v0.37)
        // v0.37 — previous version referenced indices 6/7 back when
        // they meant 1/8T/1/16T and got silently clamped; rewritten
        // to match the current table.
        s.divisionIdx .store (weightedPick ({2, 4, 1, 3, 0, 5, 6, 7, 8, 9},
                                            {28, 22, 14, 12, 8, 6, 4, 3, 2, 1}));

        // Lookback: mostly "now" or very short (1/128..1/16). Avoid
        // multi-bar lookback unless you're specifically after that sound.
        //   0=now 1=1/128 2=1/64 3=1/16 4=1/8 5=1/4 6=1/2 7=1 8=2 9=4
        s.lookbackIdx .store (weightedPick ({0, 1, 2, 3, 4, 5, 6, 7, 8, 9},
                                            {20, 18, 16, 15, 12, 8, 5, 3, 2, 1}));

        // Rate (fire cadence): bias toward mid values.
        s.rateIdx     .store (weightedPick ({2, 3, 4, 1, 5, 0, 6, 7},
                                            {22, 20, 18, 12, 10, 8, 6, 4}));

        // Judder count: mostly 1-4 retriggers, occasional burst.
        // Very rarely a runaway (10+) because that often just rings out.
        s.judderCount .store (weightedPick ({1, 2, 3, 4, 5, 6, 8, 12, 20},
                                            {40, 20, 12, 8, 6, 5, 4, 3, 2}));

        // Judder div: similar to lookback — short ratchets are more musical.
        s.judderDivIdx.store (weightedPick ({1, 2, 3, 4, 5, 0, 6, 7, 8, 9},
                                            {20, 22, 18, 14, 10, 6, 5, 3, 1, 1}));

        // --- Pitch / slide (musically-weighted) ---
        // Prefer ±0 / ±5 / ±7 / ±12. Rarely anything in between.
        static const int pitchChoices[]   = {0, -5, -7, 5, 7, -12, 12, -3, 3, -2, 2};
        static const int pitchWeights[]   = {35, 12, 10, 12, 10, 6,  5, 4, 3, 2, 1};
        int pidx = (int) weightedPick (
            std::vector<int> (std::begin (pitchChoices), std::end (pitchChoices)),
            std::vector<int> (std::begin (pitchWeights), std::end (pitchWeights)));
        // Add small detune (±0.3 st) so non-zero rolls aren't robotic.
        float pitchTarget = (float) pidx + (rnd01() - 0.5f) * 0.6f;
        s.pitchSt.store (blend (s.pitchSt.load(), pitchTarget));

        // Slide: most of the time zero; sometimes a subtle fall/rise.
        float slideTarget = (rnd01() < 0.65f) ? 0.0f
                                              : (rnd01() * 4.0f - 2.0f) * rndSkew (1.5f);
        s.slideSt.store (blend (s.slideSt.load(), slideTarget));

        // --- Destruction budget: at most TWO aggressive knobs go hot. ---
        // Every Act picks a 'character' (or two) instead of maxing every
        // destructive param. All others are clamped to a tame cap (0-0.22).
        enum DestructiveKnob { KCrunch, KCrushRate, KFeedback, KRes, KFold,
                               KChaos, KStutter, KSmear, KRingMod, KNumDestructive };
        bool hot[KNumDestructive] = {};
        const int numHot = (rnd01() < 0.35f) ? 1 : (rnd01() < 0.85f ? 2 : 0);
        for (int h = 0; h < numHot; ++h)
            hot[rng.nextInt (KNumDestructive)] = true;

        auto rollDestructive = [&] (bool isHot, float hotMax, float tameMax)
        {
            if (isHot)
                return 0.35f + rndSkew (0.7f) * (hotMax - 0.35f);
            return rndSkew (2.2f) * tameMax;
        };

        s.crunch   .store (blend (s.crunch   .load(),
                                  rollDestructive (hot[KCrunch],    0.80f, 0.18f)));
        s.crushRate.store (blend (s.crushRate.load(),
                                  rollDestructive (hot[KCrushRate], 0.75f, 0.15f)));
        s.feedback .store (blend (s.feedback .load(),
                                  rollDestructive (hot[KFeedback],  0.70f, 0.22f)));
        s.res      .store (blend (s.res      .load(),
                                  rollDestructive (hot[KRes],       0.75f, 0.18f)));
        s.fold     .store (blend (s.fold     .load(),
                                  rollDestructive (hot[KFold],      0.60f, 0.15f)));
        s.chaos    .store (blend (s.chaos    .load(),
                                  rollDestructive (hot[KChaos],     0.55f, 0.10f)));
        s.stutter  .store (blend (s.stutter  .load(),
                                  rollDestructive (hot[KStutter],   0.65f, 0.18f)));
        s.smear    .store (blend (s.smear    .load(),
                                  rollDestructive (hot[KSmear],     0.55f, 0.15f)));
        s.ringMod  .store (blend (s.ringMod  .load(),
                                  rollDestructive (hot[KRingMod],   0.60f, 0.12f)));

        // --- Anchors: mix, chance, decay stay musical ---
        // Mix mostly 60-100% so you can actually hear the effect.
        s.mix    .store (blend (s.mix    .load(), 0.60f + rnd01() * 0.40f));
        // Chance mostly 70-100% so the grid fires reliably.
        s.chance .store (blend (s.chance .load(), 0.70f + rnd01() * 0.30f));
        // Decay mid-range; avoid the extreme short (choke) or long (drone).
        s.decay  .store (blend (s.decay  .load(), 0.35f + rnd01() * 0.55f));

        // --- Flavour knobs (moderate distributions) ---
        s.drive     .store (blend (s.drive     .load(), rndSkew (1.4f) * 0.75f));
        s.tone      .store (blend (s.tone      .load(), 0.25f + rnd01() * 0.60f)); // bias toward bright-ish
        s.tape      .store (blend (s.tape      .load(), rndSkew (2.0f) * 0.45f));
        s.varispeed .store (blend (s.varispeed .load(), rndSkew (2.2f) * 0.40f));
        s.stereo    .store (blend (s.stereo    .load(), rndSkew (1.8f) * 0.55f));
        s.stretch   .store (blend (s.stretch   .load(), rndSkew (2.0f) * 0.50f));
        s.shimmer   .store (blend (s.shimmer   .load(), rndSkew (2.2f) * 0.45f));
        s.gate      .store (blend (s.gate      .load(), rndSkew (2.5f) * 0.35f));

        // Reverse: mostly off; occasional sprinkle.
        float revTarget = (rnd01() < 0.7f) ? 0.0f : rnd01() * 0.35f;
        s.reverseChance.store (blend (s.reverseChance.load(), revTarget));
    }
}

// Maximum chaos — step placements, Act values, and a
// sprinkling of per-step ratio locks. Called by the RANDOMIZE button's
// "EVERYTHING" option. The caller is expected to reload the current
// Act into the knobs afterward so the UI reflects the damage.
// v0.13 — factory Act presets. These are hand-picked "character" slots:
// pass-through defaults + a few tweaks in a named direction so the user
// can bootstrap a scene with one click. Values were tuned against the
// same live-chop source Steve uses so they land in broadly-musical
// territory instead of the full-random scatter.
//
// Order of fields in the initialiser below matches the ActPreset struct:
//   { category, name,
//     divisionIdx, lookbackIdx, rateIdx, judderCount, judderDivIdx,
//     pitchSt, slideSt, decay, reverseChance, crunch, crushRate, mix, chance,
//     drive, tone, feedback, tape, ringMod, varispeed, stereo,
//     shimmer, res, fold, gate, smear, stutter, chaos }
//
// divisionIdx entries: 0=1/2 1=1/4 2=1/16 3=1/32 4=1/64 5=1/128 6=1/8T 7=1/16T
// lookbackIdx entries: 0=now 1=1/128 2=1/64 3=1/16 4=1/8 5=1/4 6=1/2 7=1 8=2 9=4
static const LoopSaboteurProcessor::ActPreset kActPresets[] =
{
    // "Jungle Stretch" — dialled in from Steve's favourite live combo.
    // 1/16 slices for the classic chopped-amen chop, 1/64
    // lookback for tight grab windows, full Mix and 100% Chance so
    // every grid boundary fires. Drive at 35% + Tone wide open gives
    // it bite; Feedback 46% keeps the loop self-feeding without
    // runaway (the v0.13 output soft-limiter has your back if you
    // push further). A whisper of Flutter (5%) for a tape-y wobble.
    { "Presets", "Jungle Stretch",
      2, 1, 3, 1, 3,
      0.0f, 0.0f, 0.58f, 0.0f, 0.25f, 0.0f, 1.0f, 1.0f,
      0.35f, 0.5f, 0.46f, 0.05f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },

    // "2am Trippy" — late-night hypnotic stutter. 1/16 division with
    // 1/8 lookback keeps the slices tight but not frantic. Judder 10
    // at 1/4 div gives a long, rolling retrigger cascade; the -2 st
    // Slide pulls pitch down across each burst for a woozy descending
    // feel. A touch of Crush (13%) and Fold (7%) roughen the texture
    // without destroying it, and Smear at 21% bleeds the tail into
    // the next hit for a dreamy, smudged quality. Full Mix / full
    // Chance so it takes over completely.
    { "Presets", "2am Trippy",
      2, 4, 3, 10, 9,
      0.0f, -2.0f, 0.4f, 0.0f, 0.13f, 0.0f, 1.0f, 1.0f,
      0.24f, 0.5f, 0.3f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.07f, 0.0f, 0.21f, 0.0f, 0.0f },

    // "Rubber Ducky" — pitched up +5 st for a chipmunk / rubber-band
    // quality, 1/32 division with 1/64 lookback for ultra-tight micro-
    // slices. Judder 11 at 1/4 gives a long buzzy retrigger burst.
    // Tone rolled dark to warm the pitched-up harmonics, Feedback
    // at 37% with Tape 8% adds a wobbly self-feeding loop. Shimmer
    // cranked to 91% with Res 26% gives it that squeaky, plasticky
    // sheen. Fold at 8% and Smear at 57% smudge the edges into a
    // gooey, cartoonish texture. Full Mix / full Chance.
    { "Presets", "Rubber Ducky",
      0, 1, 3, 11, 9,
      5.0f, 0.0f, 0.56f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f,
      0.0f, 0.335f, 0.37f, 0.08f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.91f, 0.26f, 0.08f, 0.0f, 0.57f, 0.0f, 0.0f },

    // "Whip It" — snappy 1/4-note slices at 4x rate with light pitch
    // shimmer and moderate smear. Feedback and tape give it a loopy,
    // elastic quality that cracks like a whip on transients.
    { "Presets", "Whip It",
      5, 5, 6, 15, 4,
      0.33f, -1.12f, 0.35f, 0.19f, 0.16f, 0.0f, 0.78f, 0.79f,
      0.04f, 0.34f, 0.33f, 0.31f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.52f, 0.40f, 0.30f, 0.14f, 0.44f, 0.0f, 0.18f },

    // "Night Owl" — high-pitched (+11.75 st) slices with heavy drive
    // and tape flutter for a dark, fluttery, nocturnal texture. Gate
    // at 51% chops the tail while resonance and shimmer add an eerie
    // sheen.
    { "Presets", "Night Owl",
      5, 5, 6, 17, 4,
      11.75f, -1.55f, 0.69f, 0.04f, 0.06f, 0.0f, 0.59f, 0.89f,
      0.88f, 0.075f, 0.33f, 0.57f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.58f, 0.65f, 0.13f, 0.51f, 0.14f, 0.0f, 0.27f },

    // "Digital After All" — pitched down -7 st with full decay and tone,
    // heavy drive, and max mix. 32 judders at 1/128 with 1/32 division
    // create a dense, robotic retrigger wall. Tape flutter and smear
    // add just enough movement to keep it alive.
    { "Presets", "Digital After All",
      0, 1, 3, 32, 0,
      -7.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f,
      0.43f, 0.5f, 0.07f, 0.25f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 0.0f, 0.12f, 0.0f, 0.15f },

    // "Trash Can Sinatra" — pitched down -7 st with matching -7 st slide
    // for a deep, swooping descent. 1 bar lookback with 1/16 division
    // and 15 judders at 1/16 creates a stuttery, lo-fi crooner vibe.
    // High feedback (58%) with a touch of fold and crush.
    { "Presets", "Trash Can Sinatra",
      2, 7, 3, 15, 5,
      -7.0f, -7.0f, 0.42f, 0.0f, 0.10f, 0.0f, 1.0f, 1.0f,
      0.06f, 0.5f, 0.58f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.04f, 0.0f, 0.0f, 0.0f, 0.0f },

    // "Snare Roll" — clean 1/32 division retrigger with 13 judders at
    // 1/16 for a tight, machine-gun snare fill. No tonal shaping, no
    // pitch shift, 74% decay for a natural roll fade. A touch of fold
    // (18%) adds grit.
    { "Presets", "Snare Roll",
      0, 3, 3, 13, 5,
      0.0f, 0.0f, 0.74f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f,
      0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.18f, 0.0f, 0.0f, 0.0f, 0.0f },

    // "Pad Stabber" — pitched up +7 st with full reverse and 25% crush
    // for choppy, bitty pad stabs. 8 judders at 1/32 with 1/16 division
    // give a tight rhythmic burst. Drive and shimmer add warmth.
    { "Presets", "Pad Stabber",
      2, 1, 3, 8, 2,
      7.0f, 0.0f, 0.66f, 1.0f, 0.25f, 0.0f, 1.0f, 0.54f,
      0.21f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.20f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },

    // "Satan's Organ" — full reverse, full decay at half-speed with 13
    // judders at 1/32 for a demonic, smeared-out organ drone. Tape at
    // 30% and smear at 69% give it a queasy, warped quality. Fold adds
    // grit.
    { "Presets", "Satan's Organ",
      2, 3, 2, 13, 3,
      0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.59f,
      0.13f, 0.27f, 0.29f, 0.30f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.18f, 0.0f, 0.69f, 0.0f, 0.0f },

    // "aye, but gently" — a reversed shimmer pad with a huge upward glide.
    // Pitched -7 st with a +6 st slide so every slice swoops up from deep
    // bass territory back toward pitch as it plays. 1/16 division, 1/16
    // lookback, single judder — so it's one long, languid slice rather
    // than a ratchet. Full reverse flips the tails so they swell in
    // instead of decaying out. Tone open at 95% keeps the top air alive,
    // Stereo at 62% throws it wide, Shimmer at 59% adds the octave halo,
    // Smear at 46% bleeds the slices into each other, Fold at 32% adds a
    // soft rounded harmonic, and a pinch of Tape (18%) and CrushRate (11%)
    // lend it just enough grain to sit in a mix. Mix parked at 38% so it
    // colours the source rather than taking over — the title is a Scots
    // "aye" of cautious assent, and the preset plays the same role.
    { "Presets", "aye, but gently",
      /* div,look,rate,jud,judDiv */  2, 3, 3, 1, 3,
      /* pitch, slide, decay, rev, crunch, crushR, mix, chance */
         -7.0f, 6.0f, 0.871f, 1.0f, 0.006f, 0.107f, 0.385f, 1.0f,
      /* drive, tone, feedback, tape, ringMod, varispeed, stereo, stretch */
         0.162f, 0.946f, 0.039f, 0.181f, 0.0f, 0.01f, 0.623f, 0.0f,
      /* shimmer, res, fold, gate, smear, stutter, chaos */
         0.594f, 0.0f, 0.319f, 0.0f, 0.459f, 0.0f, 0.0f,
      /* crushAll, fxOnDryDrive, fxOnDryTone, fxOnDryRingMod, fxOnDryFold */
         false, false, false, false, false,
      /* lfos[] — four sync'd sines parked in a rising stagger of rates
         (2 bars, 3 bars, 4 bars, 8 bars), all unassigned. Cosmetic — if
         the user wants motion they can assign targets, but the default
         stance is still. trigMode=1 (legato) matches the exported file. */
      {
          { /*shape=Sine*/ 0, /*rate=2 bars*/  8, /*depth*/ 1.0f,
            /*target*/    -1, /*sync*/      true,
            /*A*/ 5.0f, /*H*/ 0.0f, /*D*/ 200.0f,
            /*trig*/ 1, /*xTarget*/ -1, /*xDepth*/ 1.0f },
          { /*shape=Sine*/ 0, /*rate=3 bars*/  9, /*depth*/ 1.0f,
            /*target*/    -1, /*sync*/      true,
            /*A*/ 5.0f, /*H*/ 0.0f, /*D*/ 200.0f,
            /*trig*/ 1, /*xTarget*/ -1, /*xDepth*/ 1.0f },
          { /*shape=Sine*/ 0, /*rate=4 bars*/ 10, /*depth*/ 1.0f,
            /*target*/    -1, /*sync*/      true,
            /*A*/ 5.0f, /*H*/ 0.0f, /*D*/ 200.0f,
            /*trig*/ 1, /*xTarget*/ -1, /*xDepth*/ 1.0f },
          { /*shape=Sine*/ 0, /*rate=8 bars*/ 11, /*depth*/ 1.0f,
            /*target*/    -1, /*sync*/      true,
            /*A*/ 5.0f, /*H*/ 0.0f, /*D*/ 200.0f,
            /*trig*/ 1, /*xTarget*/ -1, /*xDepth*/ 1.0f }
      }
    },

    // "Roll Down" — pitched-down ratcheting roll with 23 judders at 1/16,
    // heavy feedback for a descending machine-gun snare.
    { "Presets", "Roll Down",
      2, 3, 3, 23, 5,
      -3.0f, -5.0f, 0.70f, 0.0f, 0.09f, 0.0f, 1.0f, 1.0f,
      0.0f, 0.5f, 0.54f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.18f, 0.0f, 0.0f, 0.0f, 0.0f },

    // "Cage 4'33\"" — John Cage's 1952 silent work as a preset.
    // v0.7.0 — now a true silence easter egg: Mix at 100% wet, Chance
    // at 0 so no voice fires, and the cageSilence flag (set in
    // applyActPreset by name-match) zeroes the output buffer. Result:
    // absolute silence, just like the original piece.
    { "Presets", "Cage 4'33\"",
      2, 3, 3, 1, 3,
      0.0f, 0.0f, 0.8f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
      0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f },

    // "A Little Crispy" — light bit-crush texture with a musical gate.
    // 1/32 division, 1/16 lookback at 1x rate, single judder at 1/32.
    // PAULA crunch (23%) with decimation (25%) adds gritty aliasing,
    // Tone bright (69%), and a whisper of Gate (8%) trims tails.
    // Mix at 64% blends the crud under the source. Quantised lookback
    // keeps slices rhythmically coherent.
    { "Presets", "A Little Crispy",
      /* div,look,rate,jud,judDiv */  0, 3, 3, 1, 3,
      /* pitch, slide, decay, rev, crunch, crushR, mix, chance */
         0.0f, 0.0f, 0.163f, 0.0f, 0.229f, 0.248f, 0.644f, 1.0f,
      /* drive, tone, feedback, tape, ringMod, varispeed, stereo, stretch */
         0.0f, 0.694f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
      /* shimmer, res, fold, gate, smear, stutter, chaos */
         0.0f, 0.0f, 0.0f, 0.083f, 0.0f, 0.0f, 0.0f,
      /* crushAll */ false, /* fxOnDry */ false, false, false, false,
      /* lfos — all unassigned, matching the .lpsbact (all targets = -1) */
      {
          { 0, 8, 1.0f, -1, true, 5.0f, 0.0f, 200.0f, 1, -1, 1.0f },
          { 0, 9, 1.0f, -1, true, 5.0f, 0.0f, 200.0f, 1, -1, 1.0f },
          { 0, 10, 1.0f, -1, true, 5.0f, 0.0f, 200.0f, 1, -1, 1.0f },
          { 0, 11, 1.0f, -1, true, 5.0f, 0.0f, 200.0f, 1, -1, 1.0f }
      },
      /* ENGINE: stage, mix, intensity, interp, crushAA, crushMu, filter, splitMode, splitHz */
         0, 0.0f, 0.5f, 0, /*crushAA*/ false, /*crushMu*/ false, 0, 0, 2600.0f,
      /* CHARACTER: stereoMode, ringModQuant, driveType..decayCurve */
         0, false, 0, 0, 0, 0, 0, 0, 0,
         /*reverseMode*/ 1, /*tapeMode*/ 1, 0, 0, 0, 0, 0,
         /*lookbackBehaviour*/ 2, 0
    },

    // v0.38 — "Pretty Machine" — the first preset that actively uses the
    // MOD system. Steve's dialled-in Act: 1/16 slices at 1/2x rate, ~2 st
    // pitch bump with -1 st slide, 73% stretch and a generous 75% ring
    // mod. The magic is the MODs: MOD 1 slow-sweeps RING MOD (slot 16)
    // with a free-running 1 Hz saw at -0.54 depth, and MOD 2 wiggles
    // PITCH (slot 5) with a free-running 4 Hz sine at 0.25 depth. Both
    // run FREE (not host-sync'd) — the category name "Quantise These"
    // is an invitation to flip them to sync if you want a different
    // flavour, not a description of how they ship. MODs 3 and 4 are
    // parked on 4 bar / 8 bar sine with no target, cosmetically
    // matching Steve's MOD-page screenshot. Knob values below are
    // copied verbatim from Steve's exported .lpsbact. Users who tweak
    // and like their own settings better can re-save via "Save as
    // user preset…".
    { "Presets", "Pretty Machine",
      /* div,look,rate,jud,judDiv */  2, 3, 2, 1, 3,
      /* pitch, slide, decay, rev, crunch, crushR, mix, chance */
         1.9999994f, -1.0000002f, 0.8000000f, 0.0f, 0.0f, 0.0f, 0.4960000f, 1.0f,
      /* drive, tone, feedback, tape, ringMod, varispeed, stereo, stretch */
         0.0f, 0.5f, 0.0f, 0.0f, 0.7480000f, 0.0f, 0.0f, 0.7270001f,
      /* shimmer, res, fold, gate, smear, stutter, chaos */
         0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0930000f,
      /* crushAll, fxOnDryDrive, fxOnDryTone, fxOnDryRingMod, fxOnDryFold */
         false, false, false, false, false,
      /* lfos[] — matches Steve's MOD-page screenshot. */
      {
          // MOD 1 — Saw on RING MOD (slot 16), FREE 1 Hz, depth -0.54.
          // Free-run rather than sync'd: the ring-mod amount drifts
          // against the host grid, which is exactly the tension the
          // "Quantise These" category is named after. Negative depth
          // flips the saw's ramp polarity so the ring-mod glides DOWN
          // across each cycle rather than up.
          { /*shape=Saw*/      2, /*rate=1 Hz*/   5, /*depth*/ -0.54f,
            /*target*/        16, /*sync=FREE*/ false,
            /*A*/ 5.0f, /*H*/ 0.0f, /*D*/ 200.0f,
            /*trig*/ 0, /*xTarget*/ -1, /*xDepth*/ 1.0f },
          // MOD 2 — Sine on PITCH (slot 5), FREE 4 Hz, depth 0.25.
          // Fast free-running sine on pitch gives a gentle vibrato that
          // rides over the slower ring-mod sweep without locking to it.
          { /*shape=Sine*/     0, /*rate=4 Hz*/   7, /*depth*/ 0.25f,
            /*target*/         5, /*sync=FREE*/ false,
            /*A*/ 5.0f, /*H*/ 0.0f, /*D*/ 200.0f,
            /*trig*/ 0, /*xTarget*/ -1, /*xDepth*/ 1.0f },
          // MOD 3 — Sine @ 4 bars, unassigned. Rate mirrors the default
          // cosmetic rate so the MOD page looks the same as Steve's
          // screenshot when the preset is loaded.
          { /*shape=Sine*/     0, /*rate=4 bars*/ 10, /*depth*/ 1.00f,
            /*target*/        -1, /*sync*/     true,
            /*A*/ 5.0f, /*H*/ 0.0f, /*D*/ 200.0f,
            /*trig*/ 0, /*xTarget*/ -1, /*xDepth*/ 1.0f },
          // MOD 4 — Sine @ 8 bars, unassigned.
          { /*shape=Sine*/     0, /*rate=8 bars*/ 11, /*depth*/ 1.00f,
            /*target*/        -1, /*sync*/     true,
            /*A*/ 5.0f, /*H*/ 0.0f, /*D*/ 200.0f,
            /*trig*/ 0, /*xTarget*/ -1, /*xDepth*/ 1.0f }
      }
    },

    // "Seek Out Space" — a shimmery delay/reverb wash with no pitch, no
    // drive, no glitch — just space. 1/16 division with 1/32 lookback,
    // 1x rate, 9 judders at 1/4 for a rolling diffusion tail. Shimmer
    // at 68% builds an octave halo, Smear at 48% bleeds slices into a
    // continuous wash, Stereo at 55% throws it wide. A whisper of Tape
    // (4%) and Varispeed (3%) keeps it alive without wobbling. Tone
    // tilted slightly bright, Mix at 64% so it wraps around the source
    // rather than drowning it. No LFOs — what you dial is what you get.
    { "Presets", "Seek Out Space",
      /* div,look,rate,jud,judDiv */  2, 2, 3, 9, 9,
      /* pitch, slide, decay, rev, crunch, crushR, mix, chance */
         0.0f, 0.0f, 0.80f, 0.0f, 0.0f, 0.0f, 0.64f, 1.0f,
      /* drive, tone, feedback, tape, ringMod, varispeed, stereo, stretch */
         0.0f, 0.60f, 0.0f, 0.04f, 0.0f, 0.03f, 0.55f, 0.0f,
      /* shimmer, res, fold, gate, smear, stutter, chaos */
         0.68f, 0.0f, 0.0f, 0.0f, 0.48f, 0.0f, 0.0f },

    // "Cup Fungus" — dark, crusty lo-fi texture. 1/16 division with 1/16
    // lookback, single judder at 1/32 so it's one simple chop per hit.
    // The character comes from the PAULA section: 8-bit crush with /3
    // sample-rate decimation applied to the whole signal (crushAll on).
    // Tone rolled way dark (69%), Tape at 43% adds heavy flutter, and
    // a touch of Stretch (13%) smears the slices. Shimmer at 32% and
    // Varispeed 9% add subtle movement. Full mix, full chance.
    { "Presets", "Cup Fungus",
      /* div,look,rate,jud,judDiv */  2, 3, 3, 1, 3,
      /* pitch, slide, decay, rev, crunch, crushR, mix, chance */
         0.0f, 0.0f, 0.80f, 0.0f, 0.571f, 0.206f, 1.0f, 1.0f,
      /* drive, tone, feedback, tape, ringMod, varispeed, stereo, stretch */
         0.0f, 0.155f, 0.0f, 0.43f, 0.0f, 0.09f, 0.0f, 0.13f,
      /* shimmer, res, fold, gate, smear, stutter, chaos */
         0.32f, 0.0f, 0.0f, 0.0f, 0.03f, 0.0f, 0.0f,
      /* crushAll */ true },

    // "Pattern Gap Handler" — 1/4 division with 1/64 lookback for a wide
    // slice that grabs a very recent micro-window. No pitch shift, no
    // judder, so it's a straight chop at quarter-note boundaries — great
    // for filling gaps in drum patterns with glitchy fragments. The colour
    // comes from FOLD (54%) and SHIMMER (68%) stacking harmonic crunch on
    // top of aliased HF sparkle, with DRIVE (35%) warming the result.
    // VARISPEED (56%) adds a drunken speed-drift to each slice, and
    // STUTTER (55%) micro-loops the tail for a buzzy digital decay. BITS
    // at 13-bit dusts the surface with lo-fi grit. Full Mix / full Chance.
    { "Presets", "Pattern Gap Handler",
      /* div,look,rate,jud,judDiv */  5, 1, 1, 1, 1,
      /* pitch, slide, decay, rev, crunch, crushR, mix, chance */
         0.0f, 0.0f, 0.58f, 0.0f, 0.214f, 0.0f, 1.0f, 1.0f,
      /* drive, tone, feedback, tape, ringMod, varispeed, stereo, stretch */
         0.35f, 0.5f, 0.0f, 0.0f, 0.0f, 0.56f, 0.0f, 0.0f,
      /* shimmer, res, fold, gate, smear, stutter, chaos */
         0.68f, 0.0f, 0.54f, 0.0f, 0.0f, 0.55f, 0.0f },

    // "Loopbreaker" — Steve's signature live preset. 1/8 triplet division
    // with 1/4 lookback gives a syncopated chop-and-grab feel. Diode drive
    // (driveType=2) into triangle fold (foldTopology=1) for crunchy
    // harmonics. Tape wow-only mode at 16% adds drift. Beat-quantised
    // lookback (lookbackBehaviour=2) keeps slices rhythmically coherent.
    // Two AHD envelopes (MOD 1 + 3) trigger on active steps with a sine
    // LFO cross-modulating MOD 1's depth for evolving movement.
    { "Presets", "Loopbreaker",
      /* div,look,rate,jud,judDiv */  6, 5, 3, 1, 5,
      /* pitch, slide, decay, rev, crunch, crushR, mix, chance */
         0.0f, 0.0f, 0.8f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f,
      /* drive, tone, feedback, tape, ringMod, varispeed, stereo, stretch */
         0.304f, 0.5f, 0.0f, 0.16f, 0.0f, 0.0f, 0.134f, 0.0f,
      /* shimmer, res, fold, gate, smear, stutter, chaos */
         0.0f, 0.0f, 0.475f, 0.0f, 0.0f, 0.0f, 0.099f,
      /* crushAll, fxOnDryDrive, fxOnDryTone, fxOnDryRingMod, fxOnDryFold */
         true, false, false, false, false,
      /* MODs: shape, rate, depth, target, sync, aMs, hMs, dMs, trig, xTgt, xDep */
      { { 3, 5, 1.0f, 1, true, 5.0f, 0.0f, 200.0f, 1, -1, 1.0f },
        { 0, 1, 1.0f, 1, true, 5.0f, 0.0f, 200.0f, 1,  0, 1.0f },
        { 3, 8, 0.47f, 19, true, 5.0f, 0.0f, 200.0f, 1, -1, 1.0f },
        { 0, 11, 1.0f, -1, true, 5.0f, 0.0f, 200.0f, 1, -1, 1.0f } },
      /* ENGINE: stage, mix, intensity, interp, crushAA, crushMu, filter, splitMode, splitHz */
         0, 0.0f, 0.5f, 0, true, true, 0, 0, 500.0f,
      /* CHARACTER: stereoMode, ringModQuant, driveType..decayCurve */
         0, false, /*driveType*/ 2, /*foldTopology*/ 1, 0, 0, 0, 0, 0,
         0, /*tapeMode*/ 1, 0, 0, 0, 0, 0,
         /*lookbackBehaviour*/ 2, 0
    },
};

int LoopSaboteurProcessor::getNumActPresets() noexcept
{
    return (int) (sizeof (kActPresets) / sizeof (kActPresets[0]));
}

const LoopSaboteurProcessor::ActPreset& LoopSaboteurProcessor::getActPreset (int idx) noexcept
{
    idx = juce::jlimit (0, getNumActPresets() - 1, idx);
    return kActPresets[idx];
}

void LoopSaboteurProcessor::applyActPreset (int sceneIdx, int presetIdx)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    if (presetIdx < 0 || presetIdx >= getNumActPresets()) return;

    applyingPreset = true;  // Suppress auto-mirror reset of preset idx

    const auto& p = kActPresets[presetIdx];
    auto& s = scenes[sceneIdx];

    s.divisionIdx .store (p.divisionIdx);
    s.lookbackIdx .store (p.lookbackIdx);
    s.rateIdx     .store (p.rateIdx);
    s.judderCount .store (p.judderCount);
    s.judderDivIdx.store (p.judderDivIdx);
    s.pitchSt      .store (p.pitchSt);
    s.slideSt      .store (p.slideSt);
    s.decay        .store (p.decay);
    s.reverseChance.store (p.reverseChance);
    s.crunch       .store (p.crunch);
    s.crushRate    .store (p.crushRate);
    s.mix          .store (p.mix);
    s.chance       .store (p.chance);
    s.drive    .store (p.drive);
    s.tone     .store (p.tone);
    s.feedback .store (p.feedback);
    s.tape     .store (p.tape);
    s.ringMod  .store (p.ringMod);
    s.varispeed.store (p.varispeed);
    s.stereo   .store (p.stereo);
    s.stretch  .store (p.stretch);
    s.shimmer  .store (p.shimmer);
    s.res     .store (p.res);
    s.fold    .store (p.fold);
    s.gate    .store (p.gate);
    s.smear   .store (p.smear);
    s.stutter .store (p.stutter);
    s.chaos   .store (p.chaos);

    // v0.43 — apply FX-on-dry flags carried by the preset.
    s.crushAll      .store (p.crushAll);
    s.fxOnDryDrive  .store (p.fxOnDryDrive);
    s.fxOnDryTone   .store (p.fxOnDryTone);
    s.fxOnDryRingMod.store (p.fxOnDryRingMod);
    s.fxOnDryFold   .store (p.fxOnDryFold);

    // v0.7.0 — Cage 4'33" easter egg: detect by name and set flag
    // so the output path zeroes all audio for true silence.
    s.cageSilence.store (juce::String (p.name) == "Cage 4'33\"");

    // v0.7.0 — ENGINE settings carried by the preset.
    s.engineStage      .store (p.engineStage);
    s.engineMix        .store (p.engineMix);
    s.engineIntensity  .store (p.engineIntensity);
    s.interpMode    .store (p.interpMode);
    s.crushAA       .store (p.presetCrushAA);
    s.crushMu       .store (p.presetCrushMu);
    s.filterMode    .store (p.filterMode);
    s.freqSplitMode .store (p.freqSplitMode);
    s.freqSplitHz   .store (p.freqSplitHz);

    // v0.7.0 — CHARACTER settings carried by the preset.
    s.stereoMode        .store (p.stereoMode);
    s.ringModQuant      .store (p.ringModQuant);
    s.driveType         .store (p.driveType);
    s.foldTopology      .store (p.foldTopology);
    s.shimmerOctave     .store (p.shimmerOctave);
    s.smearCharacter    .store (p.smearCharacter);
    s.stutterWindow     .store (p.stutterWindow);
    s.varispeedCurve    .store (p.varispeedCurve);
    s.slideCurve        .store (p.slideCurve);
    s.reverseMode       .store (p.reverseMode);
    s.tapeMode          .store (p.tapeMode);
    s.ringModWave       .store (p.ringModWave);
    s.chaosDistribution .store (p.chaosDistribution);
    s.feedbackCharacter .store (p.feedbackCharacter);
    s.stretchMode       .store (p.stretchMode);
    s.judderShape       .store (p.judderShape);
    s.lookbackBehaviour .store (p.lookbackBehaviour);
    s.decayCurve        .store (p.decayCurve);

    // v0.38 — also apply the 4 MODs carried by this preset. Reset first
    // so a preset that uses fewer than 4 MODs actively wipes any stale
    // modulation that the slot had. If the preset is one of the old
    // "no modulation" entries, all four lfos[] defaults are Off with
    // targetSlot=-1, which matches the post-reset state anyway.
    resetLfosForScene (sceneIdx);
    for (int li = 0; li < kNumLfosPerAct; ++li)
    {
        const auto& pl = p.lfos[li];
        auto& lfo = lfos[sceneIdx][li];
        lfo.shape         .store (pl.shape);
        lfo.rateIdx       .store (pl.rateIdx);
        lfo.depth         .store (pl.depth);
        lfo.targetSlot    .store (pl.targetSlot);
        lfo.sync          .store (pl.sync);
        lfo.attackMs      .store (pl.attackMs);
        lfo.holdMs        .store (pl.holdMs);
        lfo.decayMs       .store (pl.decayMs);
        lfo.trigMode      .store (pl.trigMode);
        lfo.crossTargetLfo.store (pl.crossTargetLfo);
        lfo.crossDepth    .store (pl.crossDepth);
        lfo.envFollowGain .store (pl.envFollowGain);
    }

    // Mark the slot as populated so Randomise/patterns include it in
    // their Act pool, and so the button shows the populated tint.
    sceneStored[sceneIdx].store (true);

    // Record which preset is now active for this scene (BUG 1 fix)
    scenePresetIdx[sceneIdx].store (presetIdx);

    applyingPreset = false;  // Resume normal auto-mirror behavior
}

void LoopSaboteurProcessor::randomizeEverything()
{
    // v0.13 — "EVERYTHING" now means every single A-H Act always gets
    // re-rolled, not just the ones the user had already populated.
    // The old behaviour kept untouched slots safe, but Steve called it
    // out as a bug: EVERYTHING should actually re-roll everything.
    for (int i = 0; i < kNumScenes; ++i)
        sceneStored[i].store (true);

    randomizeActValues      (1.0f, /*forceAllActs*/ true);
    randomizeStepPlacements (0.65f);

    // Sprinkle ratio locks on ~1 in 3 populated steps so the pattern
    // plays with non-100% firing probability on top of the chance knob.
    const int len = seqLength.load();
    for (int i = 0; i < len; ++i)
    {
        if (steps[i].load() < 0) continue;
        if (rng.nextFloat() < 0.33f)
            stepRatio[i].store (1 + rng.nextInt (kNumRatios - 1));
        else
            stepRatio[i].store (0);
    }
}

// v0.12 — musical placement helpers. These share a common tail: pick a
// random populated Act for each "on" step, leaving everything else
// cleared. If no Acts are populated there's nothing to stamp, so bail.
namespace
{
    // Collect indices of Acts that have stored user values so a musical
    // generator has something to draw from. If nothing is populated the
    // caller should bail — stamping -1 everywhere would silently clear
    // whatever was on the grid.
    std::vector<int> collectPopulatedScenes (const std::atomic<bool>* stored,
                                             int numScenes)
    {
        std::vector<int> pool;
        pool.reserve ((size_t) numScenes);
        for (int i = 0; i < numScenes; ++i)
            if (stored[i].load())
                pool.push_back (i);
        return pool;
    }
}

// Stamp steps on every accent-beat position. The accent interval is
// taken from the settings popup (default 4 = four on the floor). A
// random populated Act is chosen for each hit so the pattern still
// sounds varied even though the rhythm is rigid.
void LoopSaboteurProcessor::placeFourOnTheFloor()
{
    const auto pool = collectPopulatedScenes (sceneStored, kNumScenes);
    if (pool.empty()) return;

    const int len      = seqLength.load();
    const int accent   = juce::jmax (1, gridAccentInterval.load());

    for (int i = 0; i < kMaxSteps; ++i)
    {
        if (i < len && (i % accent) == 0)
        {
            const int idx = pool[(size_t) rng.nextInt ((int) pool.size())];
            steps[i].store (idx);
        }
        else
        {
            steps[i].store (-1);
        }
    }
}

// Stamp steps on the half-accent positions — the classic "and" beats.
// For accent=4 this lands on 3, 7, 11, 15. If accent is 1 (no grid
// accent) there's no "off" position so this falls through to cleared.
void LoopSaboteurProcessor::placeOffbeats()
{
    const auto pool = collectPopulatedScenes (sceneStored, kNumScenes);
    if (pool.empty()) return;

    const int len    = seqLength.load();
    const int accent = juce::jmax (1, gridAccentInterval.load());
    const int offset = accent / 2;                  // 0 if accent==1

    for (int i = 0; i < kMaxSteps; ++i)
    {
        const bool hit = (i < len)
                      && (accent > 1)
                      && ((i % accent) == offset);
        if (hit)
        {
            const int idx = pool[(size_t) rng.nextInt ((int) pool.size())];
            steps[i].store (idx);
        }
        else
        {
            steps[i].store (-1);
        }
    }
}

// v0.13 — new musical placement helpers. Each one stamps a random
// populated Act onto a fixed set of hit positions and clears everything
// else, so they compose with the "random Act pool" idiom used by Four
// on the Floor, Offbeats, and Euclidean above.
namespace
{
    // Small helper: stamp `hits[]` (size len), picking a random Act from
    // pool for each hit, clearing everything else.
    void applyHitMask (std::atomic<int>* steps,
                       int kMaxSteps,
                       int len,
                       const std::vector<bool>& hits,
                       const std::vector<int>& pool,
                       juce::Random& rng)
    {
        for (int i = 0; i < kMaxSteps; ++i)
        {
            const bool hit = (i < len) && hits[(size_t) i];
            if (hit)
            {
                const int idx = pool[(size_t) rng.nextInt ((int) pool.size())];
                steps[i].store (idx);
            }
            else
            {
                steps[i].store (-1);
            }
        }
    }
}

// Downbeats only — a single hit at the start of every accent group
// (steps 0, accent, 2*accent...). Sparse but anchoring.
void LoopSaboteurProcessor::placeDownbeats()
{
    const auto pool = collectPopulatedScenes (sceneStored, kNumScenes);
    if (pool.empty()) return;

    const int len    = seqLength.load();
    const int accent = juce::jmax (1, gridAccentInterval.load());

    std::vector<bool> hits ((size_t) len, false);
    for (int i = 0; i < len; i += (accent * 2))
        hits[(size_t) i] = true;
    applyHitMask (steps, kMaxSteps, len, hits, pool, rng);
}

// Backbeat — snare on 2 and 4 (or their equivalents at non-4 accent).
// For accent=4 this lands on steps 4, 12, 20, 28... (the "2" and "4"
// of a 4/4 groove). Leaves 0 and 8 empty so the kick can sit there.
void LoopSaboteurProcessor::placeBackbeat()
{
    const auto pool = collectPopulatedScenes (sceneStored, kNumScenes);
    if (pool.empty()) return;

    const int len    = seqLength.load();
    const int accent = juce::jmax (1, gridAccentInterval.load());

    std::vector<bool> hits ((size_t) len, false);
    for (int i = accent; i < len; i += (accent * 2))
        hits[(size_t) i] = true;
    applyHitMask (steps, kMaxSteps, len, hits, pool, rng);
}

// Syncopated — hits on the "e" and "a" sixteenths (offset 1 and 3 in
// every accent block for accent=4). Lots of push and pull.
void LoopSaboteurProcessor::placeSyncopated()
{
    const auto pool = collectPopulatedScenes (sceneStored, kNumScenes);
    if (pool.empty()) return;

    const int len    = seqLength.load();
    const int accent = juce::jmax (2, gridAccentInterval.load());

    std::vector<bool> hits ((size_t) len, false);
    for (int i = 0; i < len; ++i)
    {
        const int inBlock = i % accent;
        if (inBlock == 1 || inBlock == accent - 1)
            hits[(size_t) i] = true;
    }
    applyHitMask (steps, kMaxSteps, len, hits, pool, rng);
}

// Saturated — every active step gets a hit. The "roll" button. Pairs
// well with low CHANCE (per-step firing probability) and with ratio
// locks for thin-out.
void LoopSaboteurProcessor::placeSaturated()
{
    const auto pool = collectPopulatedScenes (sceneStored, kNumScenes);
    if (pool.empty()) return;

    const int len = seqLength.load();
    std::vector<bool> hits ((size_t) len, true);
    applyHitMask (steps, kMaxSteps, len, hits, pool, rng);
}

// Sparse / Busy — density-based random scatter. Thin wrappers around
// randomizeStepPlacements so the menu can offer a couple of quick-fire
// density presets instead of only the coin-flip default.
void LoopSaboteurProcessor::placeSparse() { randomizeStepPlacements (0.25f); }
void LoopSaboteurProcessor::placeBusy()   { randomizeStepPlacements (0.75f); }

// 3-3-2 clave — the backbone of Afro-Cuban and countless hip-hop
// samples. For accent=4, len=16 it lands on steps 0, 3, 6, 10, 12.
void LoopSaboteurProcessor::placeClave()
{
    const auto pool = collectPopulatedScenes (sceneStored, kNumScenes);
    if (pool.empty()) return;

    const int len    = seqLength.load();
    const int accent = juce::jmax (1, gridAccentInterval.load());
    // 3-3-2 pattern in units of (accent / 4). If accent=4, unit=1 and
    // the groups are 3, 3, 2 sixteenths — classic son clave.
    const int unit = juce::jmax (1, accent / 4);

    std::vector<bool> hits ((size_t) len, false);
    int pos = 0;
    const int groups[] = { 3, 3, 2 };
    int g = 0;
    while (pos < len)
    {
        hits[(size_t) pos] = true;
        pos += groups[g] * unit;
        g = (g + 1) % 3;
    }
    applyHitMask (steps, kMaxSteps, len, hits, pool, rng);
}

// Euclidean rhythm distribution (Bjorklund / Bresenham). Spreads
// `pulses` hits as evenly as possible across `seqLength` steps so E(3,8)
// gives the tresillo, E(5,8) the cinquillo, etc. Uses the Bresenham
// line-drawing formulation because it's a one-pass O(n) loop and
// produces the same result as the canonical Bjorklund algorithm for the
// rotations we care about.
void LoopSaboteurProcessor::placeEuclidean (int pulses)
{
    const auto pool = collectPopulatedScenes (sceneStored, kNumScenes);
    if (pool.empty()) return;

    const int len = seqLength.load();
    if (len <= 0) return;

    pulses = juce::jlimit (0, len, pulses);

    for (int i = 0; i < kMaxSteps; ++i)
    {
        bool hit = false;
        if (i < len && pulses > 0)
        {
            // Bresenham: step i is a hit iff the floor of i*k/n jumps.
            const int prev = (i       * pulses) / len;
            const int curr = ((i + 1) * pulses) / len;
            hit = (curr > prev);
        }

        if (hit)
        {
            const int idx = pool[(size_t) rng.nextInt ((int) pool.size())];
            steps[i].store (idx);
        }
        else
        {
            steps[i].store (-1);
        }
    }
}

// Push a scene's values into the APVTS parameters so the knobs show
// them. v0.10 — with auto-mirror in place this is also used whenever
// the user selects a different Act (including after a reset-to-defaults)
// and the knobs need to snap to match. We set the suppressAutoMirror
// flag so the resulting parameterChanged callbacks don't just write
// the newly-loaded values right back onto the same Act.
void LoopSaboteurProcessor::loadSceneToKnobs (int sceneIdx)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    auto& s = scenes[sceneIdx];

    suppressAutoMirror.store (true);
    auto setParam = [this] (const char* id, float v)
    {
        if (auto* p = apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (v));
    };

    setParam (kParamDivision,  (float) s.divisionIdx .load());
    setParam (kParamLookback,  (float) s.lookbackIdx .load());
    setParam (kParamRate,      (float) s.rateIdx     .load());
    setParam (kParamJudder,    (float) s.judderCount .load());
    setParam (kParamJudderDiv, (float) s.judderDivIdx.load());
    setParam (kParamPitch,     s.pitchSt      .load());
    setParam (kParamSlide,     s.slideSt      .load());
    setParam (kParamDecay,     s.decay        .load());
    setParam (kParamReverse,   s.reverseChance.load());
    setParam (kParamCrunch,    s.crunch       .load());
    setParam (kParamCrushRate, s.crushRate    .load());
    setParam (kParamMix,       s.mix          .load());
    setParam (kParamChance,    s.chance       .load());
    setParam (kParamDrive,     s.drive        .load());
    setParam (kParamTone,      s.tone         .load());
    setParam (kParamFeedback,  s.feedback     .load());
    setParam (kParamTape,      s.tape         .load());
    setParam (kParamRingMod,   s.ringMod      .load());
    setParam (kParamVarispeed, s.varispeed    .load());
    setParam (kParamStereo,    s.stereo       .load());
    setParam (kParamStretch,   s.stretch      .load());
    setParam (kParamShimmer,   s.shimmer      .load());
    setParam (kParamRes,       s.res          .load());
    setParam (kParamFold,      s.fold         .load());
    setParam (kParamGate,      s.gate         .load());
    setParam (kParamSmear,     s.smear        .load());
    setParam (kParamStutter,   s.stutter      .load());
    setParam (kParamChaos,     s.chaos        .load());
    suppressAutoMirror.store (false);
}

void LoopSaboteurProcessor::setStepScene (int stepIdx, int sceneIdx)
{
    if (stepIdx < 0 || stepIdx >= kMaxSteps)                return;
    if (sceneIdx < -1 || sceneIdx >= kNumScenes)            return;
    steps[stepIdx].store (sceneIdx);
    applyAutoFollowIfEnabled();
}

int LoopSaboteurProcessor::getStepScene (int stepIdx) const noexcept
{
    if (stepIdx < 0 || stepIdx >= kMaxSteps) return -1;
    return steps[stepIdx].load();
}

void LoopSaboteurProcessor::setStepRatio (int stepIdx, int ratioIdx)
{
    if (stepIdx < 0 || stepIdx >= kMaxSteps) return;
    stepRatio[stepIdx].store (juce::jlimit (0, kNumRatios - 1, ratioIdx));
    // Reset the visit counter so the new ratio window starts clean.
    // ratioLastSeenSeqStep is touched from the audio thread so we just
    // invalidate it here; the next processBlock visit will refresh it.
    stepRatioVisit[stepIdx] = 0;
    ratioLastSeenSeqStep    = -1;
    applyAutoFollowIfEnabled();
}

int LoopSaboteurProcessor::getStepRatio (int stepIdx) const noexcept
{
    if (stepIdx < 0 || stepIdx >= kMaxSteps) return 0;
    return stepRatio[stepIdx].load();
}

void LoopSaboteurProcessor::setStepFreeze (int stepIdx, bool held)
{
    if (stepIdx < 0 || stepIdx >= kMaxSteps) return;
    stepFreezeHold[stepIdx].store (held);
    applyAutoFollowIfEnabled();
}

bool LoopSaboteurProcessor::getStepFreeze (int stepIdx) const noexcept
{
    if (stepIdx < 0 || stepIdx >= kMaxSteps) return false;
    return stepFreezeHold[stepIdx].load();
}

// v0.13 — per-step mute. Lives outside the "populated" predicate used
// by AUTO-follow so muting an otherwise blank step doesn't extend the
// pattern length; muting a populated step also doesn't shrink it,
// because the step is still stamped / locked / offset — the mute is
// purely a fire-gate.
void LoopSaboteurProcessor::setStepMute (int stepIdx, bool muted)
{
    if (stepIdx < 0 || stepIdx >= kMaxSteps) return;
    stepMute[stepIdx].store (muted);
}

bool LoopSaboteurProcessor::getStepMute (int stepIdx) const noexcept
{
    if (stepIdx < 0 || stepIdx >= kMaxSteps) return false;
    return stepMute[stepIdx].load();
}

// v0.30 — LFO accessors.
void LoopSaboteurProcessor::setLfoShape (int sceneIdx, int lfoIdx, int shape)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return;
    lfos[sceneIdx][lfoIdx].shape.store (juce::jlimit (0, kNumLfoShapes - 1, shape));
}

int LoopSaboteurProcessor::getLfoShape (int sceneIdx, int lfoIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return 0;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return 0;
    return lfos[sceneIdx][lfoIdx].shape.load();
}

void LoopSaboteurProcessor::setLfoRateIdx (int sceneIdx, int lfoIdx, int rateIdx)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return;
    lfos[sceneIdx][lfoIdx].rateIdx.store (juce::jlimit (0, 20, rateIdx));
}

int LoopSaboteurProcessor::getLfoRateIdx (int sceneIdx, int lfoIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return 0;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return 0;
    return lfos[sceneIdx][lfoIdx].rateIdx.load();
}

void LoopSaboteurProcessor::setLfoDepth (int sceneIdx, int lfoIdx, float depth)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return;
    // v0.34 — depth is now bipolar [-1, +1]. Negative values invert the
    // modulation direction, so negative depth + AHD envelope gives a
    // downward pluck on the target param.
    lfos[sceneIdx][lfoIdx].depth.store (juce::jlimit (-1.0f, 1.0f, depth));
}

float LoopSaboteurProcessor::getLfoDepth (int sceneIdx, int lfoIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return 0.0f;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return 0.0f;
    return lfos[sceneIdx][lfoIdx].depth.load();
}

// v0.34 — sync toggle (PPQ-locked vs free-running).
void LoopSaboteurProcessor::setLfoSync (int sceneIdx, int lfoIdx, bool sync)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return;
    lfos[sceneIdx][lfoIdx].sync.store (sync);
}

bool LoopSaboteurProcessor::getLfoSync (int sceneIdx, int lfoIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return true;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return true;
    return lfos[sceneIdx][lfoIdx].sync.load();
}

// v0.34 — envelope AHD setters / getters. All times in milliseconds.
void LoopSaboteurProcessor::setLfoAttackMs (int sceneIdx, int lfoIdx, float ms)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return;
    lfos[sceneIdx][lfoIdx].attackMs.store (juce::jlimit (0.1f, 2000.0f, ms));
}

float LoopSaboteurProcessor::getLfoAttackMs (int sceneIdx, int lfoIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return 5.0f;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return 5.0f;
    return lfos[sceneIdx][lfoIdx].attackMs.load();
}

void LoopSaboteurProcessor::setLfoHoldMs (int sceneIdx, int lfoIdx, float ms)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return;
    lfos[sceneIdx][lfoIdx].holdMs.store (juce::jlimit (0.0f, 2000.0f, ms));
}

float LoopSaboteurProcessor::getLfoHoldMs (int sceneIdx, int lfoIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return 0.0f;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return 0.0f;
    return lfos[sceneIdx][lfoIdx].holdMs.load();
}

void LoopSaboteurProcessor::setLfoDecayMs (int sceneIdx, int lfoIdx, float ms)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return;
    lfos[sceneIdx][lfoIdx].decayMs.store (juce::jlimit (0.1f, 2000.0f, ms));
}

float LoopSaboteurProcessor::getLfoDecayMs (int sceneIdx, int lfoIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return 200.0f;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return 200.0f;
    return lfos[sceneIdx][lfoIdx].decayMs.load();
}

void LoopSaboteurProcessor::setLfoTrigMode (int sceneIdx, int lfoIdx, int mode)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return;
    lfos[sceneIdx][lfoIdx].trigMode.store (juce::jlimit (0, kNumTrigModes - 1, mode));
}

int LoopSaboteurProcessor::getLfoTrigMode (int sceneIdx, int lfoIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return kTrigActiveSteps;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return kTrigActiveSteps;
    return lfos[sceneIdx][lfoIdx].trigMode.load();
}

void LoopSaboteurProcessor::setLfoEnvFollowGain (int sceneIdx, int lfoIdx, float g)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return;
    lfos[sceneIdx][lfoIdx].envFollowGain.store (juce::jlimit (1.0f, 10.0f, g));
}

float LoopSaboteurProcessor::getLfoEnvFollowGain (int sceneIdx, int lfoIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return 1.0f;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return 1.0f;
    return lfos[sceneIdx][lfoIdx].envFollowGain.load();
}

void LoopSaboteurProcessor::setLfoTargetSlot (int sceneIdx, int lfoIdx, int slot)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return;
    lfos[sceneIdx][lfoIdx].targetSlot.store (juce::jlimit (-1, kNumLockableParams - 1, slot));
}

int LoopSaboteurProcessor::getLfoTargetSlot (int sceneIdx, int lfoIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return -1;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return -1;
    return lfos[sceneIdx][lfoIdx].targetSlot.load();
}

// --- v0.38: cross-mod (MOD modulates MOD) ----------------------------------
// The source LFO reads the TARGET lfo's lastOutput (one sample stale) and
// scales its own depth by (1 + crossDepth * targetLastOutput). One-sample
// stale keeps the 4 LFOs independent at the sample loop level — no ordering
// dependency, no feedback explosions. Self-mod (source == target) is
// permitted and behaves like tanh-ish self-limiting because the value
// can't grow faster than the stale feedback allows.
void LoopSaboteurProcessor::setLfoCrossTarget (int sceneIdx, int lfoIdx, int targetLfoIdx)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return;
    lfos[sceneIdx][lfoIdx].crossTargetLfo.store (
        juce::jlimit (-1, kNumLfosPerAct - 1, targetLfoIdx));
}

int LoopSaboteurProcessor::getLfoCrossTarget (int sceneIdx, int lfoIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return -1;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return -1;
    return lfos[sceneIdx][lfoIdx].crossTargetLfo.load();
}

void LoopSaboteurProcessor::setLfoCrossDepth (int sceneIdx, int lfoIdx, float depth)
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return;
    lfos[sceneIdx][lfoIdx].crossDepth.store (juce::jlimit (-1.0f, 1.0f, depth));
}

float LoopSaboteurProcessor::getLfoCrossDepth (int sceneIdx, int lfoIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return 0.0f;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return 0.0f;
    return lfos[sceneIdx][lfoIdx].crossDepth.load();
}

double LoopSaboteurProcessor::getLfoPhase (int sceneIdx, int lfoIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return 0.0;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return 0.0;
    return lfos[sceneIdx][lfoIdx].phase;
}

float LoopSaboteurProcessor::getLfoOutput (int sceneIdx, int lfoIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return 0.0f;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return 0.0f;
    return lfos[sceneIdx][lfoIdx].lastOutput;
}

float LoopSaboteurProcessor::getLfoLastOutput (int sceneIdx, int lfoIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return 0.0f;
    if (lfoIdx < 0 || lfoIdx >= kNumLfosPerAct) return 0.0f;
    return lfos[sceneIdx][lfoIdx].lastOutput;
}

// v0.34 — restart any envelope-shaped LFOs whose trigger mode matches
// this event kind. Called from the audio thread scheduler at four
// possible points. Mapping is monotonic: a Slice event fires kTrigSlice;
// an ActiveStep event fires kTrigActiveSteps AND kTrigSlice semantics
// are handled by calling at both points (we only signal once per point).
//
// The actual restart happens in the per-sample LFO loop (envPending
// is consumed there). This keeps all audio-thread state writes in one
// place and avoids tearing between scheduler and engine.
void LoopSaboteurProcessor::fireLfoTriggers (int sceneIdx, int eventKind) noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    for (int li = 0; li < kNumLfosPerAct; ++li)
    {
        auto& lfo = lfos[sceneIdx][li];
        if (lfo.shape.load() != kLfoEnvAHD) continue;  // only envelopes retrigger
        const int mode = lfo.trigMode.load();

        bool match = false;
        switch (mode)
        {
            case kTrigSlice:                match = (eventKind == kEventSlice); break;
            case kTrigActiveSteps:          match = (eventKind == kEventActiveStep); break;
            case kTrigActiveStepsPlusLocks: match = (eventKind == kEventActiveStepPlusLock); break;
            case kTrigAllSteps:             match = (eventKind == kEventAllStep); break;
            default: break;
        }
        if (match)
            lfo.envPending = true;  // consumed at top of next sample's LFO loop
    }
}

bool LoopSaboteurProcessor::anyLfoTargetsSlot (int sceneIdx, int slot) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return false;
    if (slot < 0 || slot >= kNumLockableParams) return false;
    for (int li = 0; li < kNumLfosPerAct; ++li)
    {
        const auto& lfo = lfos[sceneIdx][li];
        // v0.34 — "active" check now depends on shape. Envelopes don't
        // use rateIdx (which is Off==0 for LFO modes), so we only require
        // a non-zero depth and the right target slot when the shape is
        // an envelope.
        const int shape = lfo.shape.load();
        const bool isEnv = (shape == kLfoEnvAHD);
        if (! isEnv && lfo.rateIdx.load() <= 0) continue;  // Off (LFO only)
        if (std::abs (lfo.depth.load()) < 0.0001f) continue;
        if (lfo.targetSlot.load() == slot) return true;
    }
    return false;
}

bool LoopSaboteurProcessor::isValidRatchet (int r) noexcept
{
    // v0.12 — keep the ratchet table short and musically useful. 1 is
    // the "no ratchet" default; 2/3/4 are the common triplet/sixteenth
    // subdivisions; 6/8 are the high-density "drum fill" counts.
    return r == 1 || r == 2 || r == 3 || r == 4 || r == 6 || r == 8;
}

static int clampRatchet (int r) noexcept
{
    // Non-valid values snap DOWN to the nearest valid entry to keep
    // the behaviour predictable when loading a save that was edited
    // by hand (or from a hypothetical future version).
    if (r <= 1) return 1;
    if (r == 2) return 2;
    if (r == 3) return 3;
    if (r == 4 || r == 5) return 4;
    if (r == 6 || r == 7) return 6;
    return 8;
}

void LoopSaboteurProcessor::setStepRatchet (int stepIdx, int ratchet)
{
    if (stepIdx < 0 || stepIdx >= kMaxSteps) return;
    stepRatchet[stepIdx].store (clampRatchet (ratchet));
    applyAutoFollowIfEnabled();
}

int LoopSaboteurProcessor::getStepRatchet (int stepIdx) const noexcept
{
    if (stepIdx < 0 || stepIdx >= kMaxSteps) return 1;
    return stepRatchet[stepIdx].load();
}

void LoopSaboteurProcessor::setStepLock (int stepIdx, int lockSlot, float value)
{
    if (stepIdx < 0 || stepIdx >= kMaxSteps)          return;
    if (lockSlot < 0 || lockSlot >= kNumLockableParams) return;
    stepParamLock[stepIdx * kNumLockableParams + lockSlot].store (value);
    applyAutoFollowIfEnabled();
}

float LoopSaboteurProcessor::getStepLock (int stepIdx, int lockSlot) const noexcept
{
    if (stepIdx < 0 || stepIdx >= kMaxSteps)          return std::nanf ("");
    if (lockSlot < 0 || lockSlot >= kNumLockableParams) return std::nanf ("");
    return stepParamLock[stepIdx * kNumLockableParams + lockSlot].load();
}

void LoopSaboteurProcessor::clearStepLocks (int stepIdx)
{
    if (stepIdx < 0 || stepIdx >= kMaxSteps) return;
    const float nanF = std::nanf ("");
    for (int k = 0; k < kNumLockableParams; ++k)
        stepParamLock[stepIdx * kNumLockableParams + k].store (nanF);
    applyAutoFollowIfEnabled();
}

bool LoopSaboteurProcessor::stepHasAnyLock (int stepIdx) const noexcept
{
    if (stepIdx < 0 || stepIdx >= kMaxSteps) return false;
    for (int k = 0; k < kNumLockableParams; ++k)
    {
        const float v = stepParamLock[stepIdx * kNumLockableParams + k].load();
        if (! std::isnan (v))
            return true;
    }
    return false;
}

void LoopSaboteurProcessor::clearAllSteps()
{
    const float nanF = std::nanf ("");
    for (int i = 0; i < kMaxSteps; ++i)
    {
        steps[i]          .store (-1);
        stepRatio[i]      .store (0);
        stepFreezeHold[i] .store (false);
        stepRatchet[i]    .store (1);
        stepMute[i]       .store (false);
        stepRatioVisit[i] = 0;
        for (int k = 0; k < kNumLockableParams; ++k)
            stepParamLock[i * kNumLockableParams + k].store (nanF);
    }
    currentPlayingStep.store (-1);
    ratioLastSeenSeqStep = -1;
    ratioLastAllowed     = true;
    ratioPatternPlayCount = 0;
    lastSeqStepAbsForRatio = -1;
    pendingRatchet = {};
    applyAutoFollowIfEnabled();
}

// v0.30 — single-level undo. Captures every per-step atomic into a
// plain struct so the editor can offer "undo last destructive action".
void LoopSaboteurProcessor::pushUndoSnapshot()
{
    auto& s = undoSnapshot;
    s.seqLen = seqLength.load();
    for (int i = 0; i < kMaxSteps; ++i)
    {
        s.steps[i]   = steps[i].load();
        s.ratio[i]   = stepRatio[i].load();
        s.freeze[i]  = stepFreezeHold[i].load();
        s.ratchet[i] = stepRatchet[i].load();
        s.mute[i]    = stepMute[i].load();
        for (int k = 0; k < kNumLockableParams; ++k)
            s.locks[i * kNumLockableParams + k] = stepParamLock[i * kNumLockableParams + k].load();
    }
    undoAvailable.store (true);
}

bool LoopSaboteurProcessor::popUndo()
{
    if (! undoAvailable.load()) return false;

    const auto& s = undoSnapshot;
    seqLength.store (s.seqLen);
    for (int i = 0; i < kMaxSteps; ++i)
    {
        steps[i]          .store (s.steps[i]);
        stepRatio[i]      .store (s.ratio[i]);
        stepFreezeHold[i] .store (s.freeze[i]);
        stepRatchet[i]    .store (s.ratchet[i]);
        stepMute[i]       .store (s.mute[i]);
        for (int k = 0; k < kNumLockableParams; ++k)
            stepParamLock[i * kNumLockableParams + k].store (s.locks[i * kNumLockableParams + k]);
    }
    undoAvailable.store (false);
    return true;
}

// v0.30 — round-trip state validation. Takes a full snapshot of
// every persisted field, serialises + deserialises via the normal
// getStateInformation / setStateInformation path, then checks every
// field against the snapshot. Returns "" on success or a description
// of every mismatch found. No side-effects beyond the round-trip
// itself (the plugin state ends up at the same values it started at,
// modulo floating-point identity).
juce::String LoopSaboteurProcessor::validateStateRoundTrip()
{
    // 1. Snapshot current state.
    struct Snap
    {
        int   steps[kMaxSteps];
        int   ratio[kMaxSteps];
        bool  freeze[kMaxSteps];
        int   ratchet[kMaxSteps];
        bool  mute[kMaxSteps];
        float locks[kMaxSteps * kNumLockableParams];
        int   seqLen;
        bool  seqOnV;
        int   selectedSceneV;
        // scenes
        struct S { int div; float chance; int lb, rate, jc, jd; float pitch, slide, decay, rev, crunch, crushR, mix;
                   float drive, tone, fb, tape, ring, vari, stereo, stretch;
                   float shimmer, res, fold, gate, smear, stutter, chaos; bool stored; };
        S scenes[kNumScenes];
    };
    Snap snap;
    snap.seqLen = seqLength.load();
    snap.seqOnV = seqOn.load();
    snap.selectedSceneV = selectedScene.load();
    for (int i = 0; i < kMaxSteps; ++i)
    {
        snap.steps[i]   = steps[i].load();
        snap.ratio[i]   = stepRatio[i].load();
        snap.freeze[i]  = stepFreezeHold[i].load();
        snap.ratchet[i] = stepRatchet[i].load();
        snap.mute[i]    = stepMute[i].load();
        for (int k = 0; k < kNumLockableParams; ++k)
            snap.locks[i * kNumLockableParams + k] = stepParamLock[i * kNumLockableParams + k].load();
    }
    for (int i = 0; i < kNumScenes; ++i)
    {
        auto& sc = scenes[i];
        auto& ss = snap.scenes[i];
        ss = { sc.divisionIdx.load(), sc.chance.load(), sc.lookbackIdx.load(),
               sc.rateIdx.load(), sc.judderCount.load(), sc.judderDivIdx.load(),
               sc.pitchSt.load(), sc.slideSt.load(), sc.decay.load(), sc.reverseChance.load(),
               sc.crunch.load(), sc.crushRate.load(), sc.mix.load(),
               sc.drive.load(), sc.tone.load(), sc.feedback.load(), sc.tape.load(),
               sc.ringMod.load(), sc.varispeed.load(), sc.stereo.load(), sc.stretch.load(),
               sc.shimmer.load(), sc.res.load(), sc.fold.load(), sc.gate.load(),
               sc.smear.load(), sc.stutter.load(), sc.chaos.load(),
               sceneStored[i].load() };
    }

    // 2. Serialise.
    juce::MemoryBlock blob;
    getStateInformation (blob);

    // 3. Deserialise.
    setStateInformation (blob.getData(), (int) blob.getSize());

    // 4. Compare.
    juce::StringArray diffs;
    auto chkI = [&] (const juce::String& name, int expected, int actual)
    {
        if (expected != actual)
            diffs.add (name + ": expected " + juce::String (expected) + " got " + juce::String (actual));
    };
    auto chkF = [&] (const juce::String& name, float expected, float actual)
    {
        // Both NaN → equal. One NaN → mismatch. Otherwise 1e-6 tolerance.
        if (std::isnan (expected) && std::isnan (actual)) return;
        if (std::isnan (expected) || std::isnan (actual) || std::abs (expected - actual) > 1e-5f)
            diffs.add (name + ": expected " + juce::String (expected, 6) + " got " + juce::String (actual, 6));
    };
    chkI ("seqLen",       snap.seqLen, seqLength.load());
    chkI ("seqOn",        (int) snap.seqOnV, (int) seqOn.load());
    chkI ("selectedScene", snap.selectedSceneV, selectedScene.load());
    for (int i = 0; i < kMaxSteps; ++i)
    {
        auto pre = "step[" + juce::String (i) + "].";
        chkI (pre + "scene",   snap.steps[i],   steps[i].load());
        chkI (pre + "ratio",   snap.ratio[i],   stepRatio[i].load());
        chkI (pre + "freeze",  (int) snap.freeze[i],  (int) stepFreezeHold[i].load());
        chkI (pre + "ratchet", snap.ratchet[i], stepRatchet[i].load());
        chkI (pre + "mute",    (int) snap.mute[i],    (int) stepMute[i].load());
        for (int k = 0; k < kNumLockableParams; ++k)
            chkF (pre + "lock[" + juce::String (k) + "]",
                  snap.locks[i * kNumLockableParams + k],
                  stepParamLock[i * kNumLockableParams + k].load());
    }
    for (int i = 0; i < kNumScenes; ++i)
    {
        auto pre = "scene[" + juce::String (i) + "].";
        auto& ss = snap.scenes[i];
        auto& sc = scenes[i];
        chkI (pre + "div",     ss.div,     sc.divisionIdx.load());
        chkF (pre + "chance",  ss.chance,  sc.chance.load());
        chkI (pre + "lb",      ss.lb,      sc.lookbackIdx.load());
        chkI (pre + "rate",    ss.rate,    sc.rateIdx.load());
        chkI (pre + "jc",      ss.jc,      sc.judderCount.load());
        chkI (pre + "jd",      ss.jd,      sc.judderDivIdx.load());
        chkF (pre + "pitch",   ss.pitch,   sc.pitchSt.load());
        chkF (pre + "slide",   ss.slide,   sc.slideSt.load());
        chkF (pre + "decay",   ss.decay,   sc.decay.load());
        chkF (pre + "rev",     ss.rev,     sc.reverseChance.load());
        chkF (pre + "crunch",  ss.crunch,  sc.crunch.load());
        chkF (pre + "crushR",  ss.crushR,  sc.crushRate.load());
        chkF (pre + "mix",     ss.mix,     sc.mix.load());
        chkF (pre + "drive",   ss.drive,   sc.drive.load());
        chkF (pre + "tone",    ss.tone,    sc.tone.load());
        chkF (pre + "fb",      ss.fb,      sc.feedback.load());
        chkF (pre + "tape",    ss.tape,    sc.tape.load());
        chkF (pre + "ring",    ss.ring,    sc.ringMod.load());
        chkF (pre + "vari",    ss.vari,    sc.varispeed.load());
        chkF (pre + "stereo",  ss.stereo,  sc.stereo.load());
        chkF (pre + "stretch", ss.stretch, sc.stretch.load());
        chkF (pre + "shimmer", ss.shimmer, sc.shimmer.load());
        chkF (pre + "res",     ss.res,     sc.res.load());
        chkF (pre + "fold",    ss.fold,    sc.fold.load());
        chkF (pre + "gate",    ss.gate,    sc.gate.load());
        chkF (pre + "smear",   ss.smear,   sc.smear.load());
        chkF (pre + "stutter", ss.stutter, sc.stutter.load());
        chkF (pre + "chaos",   ss.chaos,   sc.chaos.load());
        chkI (pre + "stored",  (int) ss.stored, (int) sceneStored[i].load());
    }
    return diffs.joinIntoString ("\n");
}

// ============================================================================
//  v0.30 — Debug instrumentation
// ============================================================================
LoopSaboteurProcessor::VoiceSnapshot LoopSaboteurProcessor::getVoiceSnapshot() const noexcept
{
    return dbgVoiceSnap;  // trivial copy, may tear — fine for debug display
}

bool LoopSaboteurProcessor::debugDumpBufferToWav (const juce::File& destFile) const
{
    if (circularBufferSize <= 0) return false;

    destFile.deleteFile();
    std::unique_ptr<juce::FileOutputStream> fos (destFile.createOutputStream());
    if (fos == nullptr) return false;

    juce::WavAudioFormat wav;
    const int ch = circularBuffer.getNumChannels();
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (fos.get(), sampleRate, (unsigned int) ch, 24, {}, 0));
    if (writer == nullptr) return false;
    fos.release(); // writer owns the stream now

    // Write the entire circular buffer starting from writePos (oldest sample)
    // so the WAV plays back in chronological order.
    juce::AudioBuffer<float> ordered (ch, circularBufferSize);
    for (int c = 0; c < ch; ++c)
    {
        auto* src = circularBuffer.getReadPointer (c);
        auto* dst = ordered.getWritePointer (c);
        const int wp = writePos;  // snapshot — may tear by a sample, fine
        const int tail = circularBufferSize - wp;
        std::memcpy (dst,        src + wp, sizeof (float) * (size_t) tail);
        std::memcpy (dst + tail, src,      sizeof (float) * (size_t) wp);
    }
    return writer->writeFromAudioSampleBuffer (ordered, 0, circularBufferSize);
}

juce::String LoopSaboteurProcessor::debugDumpStepGrid() const
{
    // Compact one-line-per-page format:
    //   A---B-C-A---B-C-   (scene letters, - for empty, X for muted)
    //   r2  p3  f   r4     (annotations: rN=ratchet, pN=ratio, f=freeze)
    const int len = seqLength.load();
    juce::String out;
    const char sceneChars[] = "ABCDEFGH";

    for (int page = 0; page < kNumPages; ++page)
    {
        const int pageStart = page * kStepsPerPage;
        if (pageStart >= len) break;

        juce::String line1, line2;
        for (int s = 0; s < kStepsPerPage && (pageStart + s) < len; ++s)
        {
            const int i = pageStart + s;
            const int sc = steps[i].load();
            const bool muted = stepMute[i].load();

            if (muted)
                line1 += "X";
            else if (sc >= 0 && sc < kNumScenes)
                line1 += juce::String::charToString (sceneChars[sc]);
            else
                line1 += "-";

            // Annotations
            juce::String ann;
            const int rat = stepRatchet[i].load();
            const int ratio = stepRatio[i].load();
            const bool frz = stepFreezeHold[i].load();
            if (rat > 1) ann += "r" + juce::String (rat);
            if (ratio > 0) ann += "p" + juce::String (ratio);
            if (frz) ann += "f";

            // Pad annotation to match the single-char step above
            // (we'll show annotations on a second line)
            if (ann.isEmpty()) ann = ".";
            line2 += ann.paddedRight (' ', 4);
        }

        out += "Page " + juce::String::charToString ('A' + page)
             + ": " + line1 + "\n"
             + "       " + line2 + "\n";
    }
    return out;
}

juce::String LoopSaboteurProcessor::debugDumpAllLocks() const
{
    juce::String out;
    int count = 0;

    for (int i = 0; i < kMaxSteps; ++i)
    {
        bool hasAny = false;
        juce::String stepStr = "Step " + juce::String (i + 1).paddedLeft ('0', 2) + ": ";

        for (int k = 0; k < kNumLockableParams; ++k)
        {
            const float v = stepParamLock[i * kNumLockableParams + k].load();
            if (! std::isnan (v))
            {
                if (hasAny) stepStr += ", ";
                stepStr += "slot" + juce::String (k) + "=" + juce::String (v, 3);
                hasAny = true;
                ++count;
            }
        }
        if (hasAny)
            out += stepStr + "\n";
    }

    if (count == 0)
        return "No parameter locks set.";
    return juce::String (count) + " locks across " + juce::String (kMaxSteps) + " steps:\n\n" + out;
}

// v0.9 — AUTO-follow pattern length helpers.
//
// computeAutoFollowLength walks every step and returns the smallest
// kStepsPerPage-multiple (16, 32, 48, 64) that contains every step
// marked "populated". A step counts as populated if it has any of:
//   * an Act stamp (steps[i] >= 0)
//   * a ratio lock (stepRatio[i] > 0)
//   * a per-step freeze hold
//   * any p-lock slot populated
//
// Called by every state mutator (setStepScene, setStepRatio,
// setStepFreeze, setStepLock, clearStepLocks, clearAllSteps)
// so the user sees the pattern length flip between 16/32/48/64 as
// they edit. Also called by the editor when the AUTO toggle is flipped
// on.
//
// Note: this touches audio-thread-readable atomics from whichever
// thread called the mutator (message thread in practice). That's fine —
// seqLength is an atomic<int> and the audio thread already loads it
// under relaxed ordering.
int LoopSaboteurProcessor::computeAutoFollowLength () const noexcept
{
    int highest = -1;
    for (int i = 0; i < kMaxSteps; ++i)
    {
        bool populated = false;
        if (steps[i].load() >= 0)                       populated = true;
        else if (stepRatio[i].load() > 0)               populated = true;
        else if (stepFreezeHold[i].load())              populated = true;
        else if (stepRatchet[i].load() > 1)             populated = true;  // v0.12
        else
        {
            for (int k = 0; k < kNumLockableParams; ++k)
            {
                const float v = stepParamLock[i * kNumLockableParams + k].load();
                if (! std::isnan (v)) { populated = true; break; }
            }
        }
        if (populated) highest = i;
    }

    if (highest < 0)
        return kStepsPerPage;   // empty pattern — stay at a single page

    // Round (highest + 1) up to the next kStepsPerPage multiple.
    const int needed = ((highest / kStepsPerPage) + 1) * kStepsPerPage;
    return juce::jlimit (kStepsPerPage, (int) kMaxSteps, needed);
}

void LoopSaboteurProcessor::applyAutoFollowIfEnabled () noexcept
{
    if (! autoFollowLength.load()) return;
    const int newLen = computeAutoFollowLength();
    if (newLen != seqLength.load())
        seqLength.store (newLen);
}

float LoopSaboteurProcessor::getPeak (int idx) const noexcept
{
    if (idx < 0 || idx >= kPeakRingSize) return 0.0f;
    return peakRing[idx].load();
}

float LoopSaboteurProcessor::getSlicePeak (int idx) const noexcept
{
    if (idx < 0 || idx >= kSlicePeakRingSize) return 0.0f;
    return slicePeakRing[idx].load();
}

void LoopSaboteurProcessor::setSeqLength (int newLength)
{
    seqLength.store (juce::jlimit (2, kMaxSteps, newLength));
    // v0.43 — reset step tracking so the new length is picked up
    // cleanly. Without this, curSeqStepAbs % newLen can land on a
    // surprising step, and restarting transport inherits the stale
    // wrappedStep cache.
    lastWrappedStep = -1;
    ratioPatternPlayCount = 0;
    lastSeqStepAbsForRatio = -1;
}

void LoopSaboteurProcessor::setSeqOn (bool on)
{
    seqOn.store (on);
    if (! on)
        currentPlayingStep.store (-1);
}

void LoopSaboteurProcessor::setSelectedScene (int sceneIdx)
{
    selectedScene.store (juce::jlimit (0, kNumScenes - 1, sceneIdx));
}

// BUG 1 fix: per-scene preset accessors
int LoopSaboteurProcessor::getScenePresetIdx (int sceneIdx) const noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return -1;
    return scenePresetIdx[sceneIdx].load();
}

void LoopSaboteurProcessor::setScenePresetIdx (int sceneIdx, int presetIdx) noexcept
{
    if (sceneIdx < 0 || sceneIdx >= kNumScenes) return;
    scenePresetIdx[sceneIdx].store (presetIdx);
}

void LoopSaboteurProcessor::prepareToPlay (double sr, int /*samplesPerBlock*/)
{
    sampleRate = sr;

    // v0.40 — Engine: prepare the post-mix Output Stage at the host SR.
    outputStage.prepare (sampleRate);

    circularBufferSize = (int) std::ceil (sampleRate * kBufferSeconds);
    circularBuffer.setSize (2, circularBufferSize,
                            /*keepExistingContent*/ false,
                            /*clearExtraSpace*/      true,
                            /*avoidReallocating*/    false);
    circularBuffer.clear();
    writePos = 0;

    // SMEAR delay line — 200 ms stereo buffer. Long enough for a
    // distinct blurry repeat at slow tempos, short enough to stay
    // in "smear" rather than "echo" territory.
    smearBufferSize = juce::jmax (64, (int) std::ceil (sampleRate * 0.200));
    smearBuffer.setSize (2, smearBufferSize,
                         /*keepExistingContent*/ false,
                         /*clearExtraSpace*/      true,
                         /*avoidReallocating*/    false);
    smearBuffer.clear();
    smearWritePos = 0;
    smearFeedAmount = 0.0f;

    // v0.5.0 — reset allpass state for Diffuse smear mode
    smearApStateL = 0.0f;
    smearApStateR = 0.0f;

    // v0.12 — lookahead delay buffer. Sized to the max lookahead so
    // we never need to reallocate when the user changes the setting.
    // A few extra samples of headroom so we never trip over the
    // read/write pointers at the exact boundary.
    lookaheadBufferSize = juce::jmax (64, (int) std::ceil (sampleRate * (kMaxLookaheadMs / 1000.0)) + 16);
    lookaheadBuffer.setSize (2, lookaheadBufferSize,
                             /*keepExistingContent*/ false,
                             /*clearExtraSpace*/      true,
                             /*avoidReallocating*/    false);
    lookaheadBuffer.clear();
    lookaheadWritePos = 0;
    // Recompute samples-from-ms with the fresh sample rate, and tell
    // the host how much latency to compensate for.
    const int ms      = lookaheadMs.load();
    const int samples = juce::jlimit (0, lookaheadBufferSize - 1,
                                      (int) std::round (sampleRate * (ms / 1000.0)));
    lookaheadSamples.store (samples);
    setLatencySamples (samples);

    lastPpq = -1.0;
    lastWrappedStep = -1;
    seqOriginSet = false;
    lastExpectedStep = 0;
    seqSampleCounter = 0;
    wasPlaying = false;
    voice = {};
    muteGateGain = 1.0f;        // v0.30
    globalCrushState.reset();   // v0.22
    // v0.30 — reset global FX state.
    globalSvfLowL = globalSvfBandL = 0.0f;
    globalSvfLowR = globalSvfBandR = 0.0f;
    globalRingModPhase = 0.0;
    // v0.30 — reset LFO running state.
    // v0.32 — lfos is now [kNumScenes][kNumLfosPerAct], so double-loop.
    for (int si = 0; si < kNumScenes; ++si)
    {
        for (int li = 0; li < kNumLfosPerAct; ++li)
        {
            lfos[si][li].phase = 0.0;
            lfos[si][li].sAndHValue = 0.0f;
            lfos[si][li].lastOutput = 0.0f;
            // v0.34 — envelope runtime reset.
            lfos[si][li].envStage = 0;
            lfos[si][li].envPos   = 0.0;
            lfos[si][li].envValue = 0.0f;
            lfos[si][li].envPending = false;
            lfos[si][li].envFollowValue = 0.0f;  // v0.6.0
        }
    }
    std::fill (std::begin (lfoModOffset), std::end (lfoModOffset), 0.0f);
}

// v0.12 — message-thread entry point for changing the lookahead
// amount. Recomputes the sample count from the current sample rate,
// clamps to the allocated buffer size, clears stale delay contents so
// the user doesn't hear a blip of old output, and reports the new
// latency to the host.
void LoopSaboteurProcessor::setLookaheadMs (int ms)
{
    ms = juce::jlimit (0, kMaxLookaheadMs, ms);
    lookaheadMs.store (ms);

    const int samples = (sampleRate > 0.0)
        ? juce::jlimit (0, juce::jmax (0, lookaheadBufferSize - 1),
                        (int) std::round (sampleRate * (ms / 1000.0)))
        : 0;
    lookaheadSamples.store (samples);

    lookaheadBuffer.clear();
    lookaheadWritePos = 0;

    setLatencySamples (samples);
}

void LoopSaboteurProcessor::releaseResources()
{
}

#ifndef JucePlugin_PreferredChannelConfigurations
bool LoopSaboteurProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // v0.42.5 — Expanded from matched-channels-only to include mono→stereo.
    // User testing flagged that Logic refused to load Loop Saboteur as a
    // stereo processor on mono tracks, which matters for a plugin whose
    // whole point is sprinkling stereo-widening effects onto the output.
    // The legal configurations we advertise to hosts are:
    //
    //     mono   → mono    (trivial pass-through stereo processing)
    //     stereo → stereo  (the common case)
    //     mono   → stereo  (NEW — Logic's "Mono → Stereo" track option)
    //
    // We deliberately reject stereo → mono: downmixing a stereo source
    // into a single channel inside a plugin whose character comes from
    // independent per-channel FX would throw away most of what makes
    // the plugin sound the way it does. Hosts that want a mono output
    // should collapse post-plugin, not pre-.
    const auto& mainOut = layouts.getMainOutputChannelSet();
    const auto& mainIn  = layouts.getMainInputChannelSet();

    const bool monoIn    = mainIn  == juce::AudioChannelSet::mono();
    const bool stereoIn  = mainIn  == juce::AudioChannelSet::stereo();
    const bool monoOut   = mainOut == juce::AudioChannelSet::mono();
    const bool stereoOut = mainOut == juce::AudioChannelSet::stereo();

    if ((! monoIn && ! stereoIn) || (! monoOut && ! stereoOut))
        return false;

    if (monoIn   && monoOut)   return true;   // 1 → 1
    if (stereoIn && stereoOut) return true;   // 2 → 2
    if (monoIn   && stereoOut) return true;   // 1 → 2  (new)

    return false;                             // 2 → 1 explicitly rejected
}
#endif

// ----------------------------------------------------------------------------
//  Helpers
// ----------------------------------------------------------------------------

int LoopSaboteurProcessor::wrappedBufferIndex (double pos) const noexcept
{
    int idx = (int) std::floor (pos);
    idx %= circularBufferSize;
    if (idx < 0)
        idx += circularBufferSize;
    return idx;
}

// v0.30 — interpolated read from the circular buffer.
// v0.40 — three-way: Linear (default, original), Drop-sample (no interp,
// SP-1200 grit at non-unity pitch), Cubic (Hermite, cleaner than linear).
// The choice is a global Engine setting; switching it changes the entire
// character of the plugin's pitched playback.
float LoopSaboteurProcessor::readBufferInterp (int channel, double pos) const noexcept
{
    return readBufferInterp (channel, pos, interpMode.load (std::memory_order_relaxed));
}

// v0.41 — per-voice variant. The voice bakes its source Act's interp at
// fire time and passes it in here so a slice keeps its character through
// Act changes / sequencer step boundaries.
float LoopSaboteurProcessor::readBufferInterp (int channel, double pos, int mode) const noexcept
{
    const auto* buf = circularBuffer.getReadPointer (channel);
    const int   idx0 = wrappedBufferIndex (pos);

    if (mode == loopsab::kInterpDrop)
    {
        // Pure zero-order hold. Floor() the position, no interpolation.
        return buf[idx0];
    }

    if (mode == loopsab::kInterpCubic)
    {
        // 4-point Hermite (Catmull-Rom). Smoother than linear; suitable
        // for "studio" character and Reel-to-Reel stacks.
        const int idxM1 = (idx0 == 0) ? (circularBufferSize - 1) : (idx0 - 1);
        const int idx1  = (idx0 + 1 >= circularBufferSize) ? 0 : (idx0 + 1);
        const int idx2  = (idx1 + 1 >= circularBufferSize) ? 0 : (idx1 + 1);
        const float ym1 = buf[idxM1];
        const float y0  = buf[idx0];
        const float y1  = buf[idx1];
        const float y2  = buf[idx2];
        const float t   = (float) (pos - std::floor (pos));

        // Catmull-Rom cubic Hermite:
        //   c0 = y0
        //   c1 = 0.5 (y1 - y_-1)
        //   c2 = y_-1 - 2.5 y0 + 2 y1 - 0.5 y2
        //   c3 = 0.5 (y2 - y_-1) + 1.5 (y0 - y1)
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * t + c2) * t + c1) * t + y0;
    }

    // Default: linear (original v0.30 path).
    const int idx1 = (idx0 + 1 >= circularBufferSize) ? 0 : idx0 + 1;
    const float frac = (float) (pos - std::floor (pos));
    return buf[idx0] + frac * (buf[idx1] - buf[idx0]);
}

float LoopSaboteurProcessor::quantiseToScale (float semitones) const noexcept
{
    const int root      = (int) std::round (pScaleRoot->load());
    const int scaleIdx  = (int) std::round (pScaleType->load());
    return snapSemitonesToScale (semitones, root, scaleIdx);
}

// Overlay a step's p-locks on top of a ShapingParams. Each non-NaN slot
// rewrites the corresponding field. Called from the audio thread at
// fire time, immediately after paramsFromScene returns — so the scene's
// baked values are the base and the locks are a surgical overlay.
void LoopSaboteurProcessor::applyStepLocks (ShapingParams& p, int stepIdx) const
{
    if (stepIdx < 0 || stepIdx >= kMaxSteps) return;

    auto get = [this, stepIdx] (int slot) -> float
    {
        return stepParamLock[stepIdx * kNumLockableParams + slot].load();
    };

    // Look up each lockable parameter by name, read its lock slot, and
    // apply if non-NaN. The name→slot mapping is stable (see kLockable).
    auto overlayFloat = [&] (const char* id, float& target, float lo, float hi)
    {
        const int slot = lockableIndexForId (id);
        if (slot < 0) return;
        const float v = get (slot);
        if (! std::isnan (v))
            target = juce::jlimit (lo, hi, v);
    };

    auto overlayInt = [&] (const char* id, int& target, int lo, int hi)
    {
        const int slot = lockableIndexForId (id);
        if (slot < 0) return;
        const float v = get (slot);
        if (! std::isnan (v))
            target = juce::jlimit (lo, hi, (int) std::round (v));
    };

    overlayInt   (kParamDivision,  p.divisionIdx,  0, 16);
    overlayInt   (kParamLookback,  p.lookbackIdx,  0, 16);
    overlayInt   (kParamRate,      p.rateIdx,      0, 8);
    overlayInt   (kParamJudder,    p.judderCount,  1, kMaxJudder);
    overlayInt   (kParamJudderDiv, p.judderDivIdx, 0, 16);
    overlayFloat (kParamPitch,     p.pitchSt,      -24.0f, 24.0f);
    overlayFloat (kParamSlide,     p.slideSt,      -12.0f, 12.0f);
    overlayFloat (kParamDecay,     p.decay,        0.0f, 1.0f);
    overlayFloat (kParamReverse,   p.reverseChance, 0.0f, 1.0f);
    overlayFloat (kParamCrunch,    p.crunch,       0.0f, 1.0f);
    overlayFloat (kParamCrushRate, p.crushRate,    0.0f, 1.0f);
    overlayFloat (kParamMix,       p.mix,          0.0f, 1.0f);
    overlayFloat (kParamDrive,     p.drive,        0.0f, 1.0f);
    overlayFloat (kParamTone,      p.tone,         0.0f, 1.0f);
    overlayFloat (kParamFeedback,  p.feedback,     0.0f, 0.95f);
    overlayFloat (kParamTape,      p.tape,         0.0f, 1.0f);
    overlayFloat (kParamRingMod,   p.ringMod,      0.0f, 1.0f);
    overlayFloat (kParamVarispeed, p.varispeed,    0.0f, 1.0f);
    overlayFloat (kParamStereo,    p.stereo,       0.0f, 1.0f);
    overlayFloat (kParamStretch,   p.stretch,      0.0f, 1.0f);
    overlayFloat (kParamShimmer,   p.shimmer,      0.0f, 1.0f);
    overlayFloat (kParamRes,       p.res,          0.0f, 0.98f);
    overlayFloat (kParamFold,      p.fold,         0.0f, 1.0f);
    overlayFloat (kParamGate,      p.gate,         -1.0f, 1.0f);
    overlayFloat (kParamSmear,     p.smear,        0.0f, 1.0f);
    overlayFloat (kParamStutter,   p.stutter,      0.0f, 1.0f);
    overlayFloat (kParamChaos,     p.chaos,        0.0f, 1.0f);
    overlayFloat (kParamGlobalMix, p.globalMix,    0.0f, 1.0f);
    // kParamChance is present in the lock slot list so it shows up in
    // lock edit UI but it's applied at the decision site, not here.
}

// Build a ShapingParams from the current knob values (freehand mode).
LoopSaboteurProcessor::ShapingParams LoopSaboteurProcessor::paramsFromKnobs() const
{
    ShapingParams p;
    p.divisionIdx   = (int) std::round (pDivision ->load());
    p.lookbackIdx   = (int) std::round (pLookback ->load());
    p.rateIdx       = (int) std::round (pRate     ->load());
    p.judderCount   = juce::jlimit (1, kMaxJudder, (int) std::round (pJudder->load()));
    p.judderDivIdx  = (int) std::round (pJudderDiv->load());
    p.pitchSt       = quantiseToScale (pPitch->load());
    p.slideSt       = pSlide  ->load();
    p.decay         = juce::jlimit (0.0f, 1.0f, pDecay  ->load());
    p.reverseChance = juce::jlimit (0.0f, 1.0f, pReverse->load());
    p.crunch        = juce::jlimit (0.0f, 1.0f, pCrunch   ->load());
    p.crushRate     = juce::jlimit (0.0f, 1.0f, pCrushRate->load());
    p.mix           = juce::jlimit (0.0f, 1.0f, pMix      ->load());
    p.globalMix     = juce::jlimit (0.0f, 1.0f, pGlobalMix->load());
    p.drive         = juce::jlimit (0.0f, 1.0f,  pDrive   ->load());
    p.tone          = juce::jlimit (0.0f, 1.0f,  pTone    ->load());
    p.feedback      = juce::jlimit (0.0f, 0.95f, pFeedback->load());
    p.tape          = juce::jlimit (0.0f, 1.0f,  pTape    ->load());
    p.ringMod       = juce::jlimit (0.0f, 1.0f,  pRingMod ->load());
    p.varispeed     = juce::jlimit (0.0f, 1.0f,  pVarispeed->load());
    p.stereo        = juce::jlimit (0.0f, 1.0f,  pStereo  ->load());
    p.stretch       = juce::jlimit (0.0f, 1.0f,  pStretch ->load());
    p.shimmer       = juce::jlimit (0.0f, 1.0f,  pShimmer ->load());
    p.res           = juce::jlimit (0.0f, 0.98f, pRes     ->load());
    p.fold          = juce::jlimit (0.0f, 1.0f,  pFold    ->load());
    p.gate          = juce::jlimit (-1.0f, 1.0f,  pGate    ->load());
    p.smear         = juce::jlimit (0.0f, 1.0f,  pSmear   ->load());
    p.stutter       = juce::jlimit (0.0f, 1.0f,  pStutter ->load());
    p.chaos         = juce::jlimit (0.0f, 1.0f,  pChaos   ->load());

    // v0.41 — Engine bundle. In live-knobs mode we fall through to the
    // currently-selected scene's stored Engine, or to the legacy global
    // atomics if no scene is selected. This keeps freehand fires aligned
    // with whatever engine character the master bus is currently using.
    {
        const int idx = selectedScene.load();
        if (idx >= 0 && idx < kNumScenes)
        {
            p.engineStage = scenes[idx].engineStage.load();
            p.engineMix   = scenes[idx].engineMix  .load();
            p.interpMode    = scenes[idx].interpMode .load();
            p.crushAA       = scenes[idx].crushAA    .load();
            p.crushMu       = scenes[idx].crushMu    .load();
            p.crushAll      = scenes[idx].crushAll   .load();
            p.filterMode    = scenes[idx].filterMode .load();
            p.freqSplitMode = scenes[idx].freqSplitMode.load();
            p.freqSplitHz   = scenes[idx].freqSplitHz .load();
            p.driveType      = scenes[idx].driveType.load();
            p.foldTopology   = scenes[idx].foldTopology.load();
            p.shimmerOctave  = scenes[idx].shimmerOctave.load();
            p.smearCharacter = scenes[idx].smearCharacter.load();
            p.stutterWindow  = scenes[idx].stutterWindow.load();
            p.varispeedCurve = scenes[idx].varispeedCurve.load();
            p.slideCurve     = scenes[idx].slideCurve.load();
            p.reverseMode    = scenes[idx].reverseMode.load();
            p.tapeMode       = scenes[idx].tapeMode.load();
            p.ringModWave    = scenes[idx].ringModWave.load();
            p.chaosDistribution = scenes[idx].chaosDistribution.load();
            p.feedbackCharacter = scenes[idx].feedbackCharacter.load();
            p.stretchMode    = scenes[idx].stretchMode.load();
            p.judderShape    = scenes[idx].judderShape.load();
            p.lookbackBehaviour = scenes[idx].lookbackBehaviour.load();
            p.decayCurve    = scenes[idx].decayCurve.load();

            // v0.42 — per-Act stereo / FX-on-Dry / ring mod quantise.
            p.stereoMode     = juce::jlimit (0, 3, scenes[idx].stereoMode.load());
            p.fxOnDryDrive   = scenes[idx].fxOnDryDrive  .load();
            p.fxOnDryTone    = scenes[idx].fxOnDryTone   .load();
            p.fxOnDryRingMod = scenes[idx].fxOnDryRingMod.load();
            p.fxOnDryFold    = scenes[idx].fxOnDryFold   .load();
            p.ringModQuant   = scenes[idx].ringModQuant  .load();
        }
        else
        {
            p.engineStage = outputStage.getMode();
            p.engineMix   = outputStage.getMix();
            p.interpMode    = interpMode    .load();
            p.crushAA       = crushAntiAlias.load();
            p.crushMu       = crushMuLaw    .load();
            p.crushAll      = crushAllAudio .load();
            p.filterMode    = 0;  // legacy global = Tilt
            p.freqSplitMode = 0;  // legacy global = Full Range
            p.freqSplitHz   = 500.0f;
            p.driveType      = 0;
            p.foldTopology   = 0;
            p.shimmerOctave  = 0;
            p.smearCharacter = 0;
            p.stutterWindow  = 0;
            p.varispeedCurve = 0;
            p.slideCurve     = 0;
            p.reverseMode    = 0;
            p.tapeMode      = 0;
            p.ringModWave   = 0;
            p.chaosDistribution = 0;
            p.feedbackCharacter = 0;
            p.stretchMode   = 0;
            p.judderShape   = 0;
            p.lookbackBehaviour = 0;
            p.decayCurve   = 0;

            // v0.42 — fall through to the legacy global atomics when no
            // Act is selected. Keeps freehand fires consistent with
            // session-state loads from older versions.
            p.stereoMode     = juce::jlimit (0, 3, stereoMode.load());
            p.fxOnDryDrive   = globalDrive    .load();
            p.fxOnDryTone    = globalTone     .load();
            p.fxOnDryRingMod = globalRingMod  .load();
            p.fxOnDryFold    = globalFold     .load();
            p.ringModQuant   = ringModQuantise.load();
        }
    }

    // v0.30 — apply LFO modulation offsets.
    // Each lockable param index maps to a ShapingParams field.
    // We clamp after adding so the LFO can't push values out of range.
    auto applyMod = [this] (float& field, const juce::String& paramId, float lo, float hi)
    {
        const int slot = lockableIndexForId (paramId);
        if (slot >= 0)
            field = juce::jlimit (lo, hi, field + lfoModOffset[slot]);
    };
    applyMod (p.mix,           kParamMix,       0.0f, 1.0f);
    applyMod (p.decay,         kParamDecay,     0.0f, 1.0f);
    applyMod (p.reverseChance, kParamReverse,   0.0f, 1.0f);
    applyMod (p.crunch,        kParamCrunch,    0.0f, 1.0f);
    applyMod (p.crushRate,     kParamCrushRate, 0.0f, 1.0f);
    applyMod (p.pitchSt,       kParamPitch,    -48.0f, 48.0f);
    applyMod (p.slideSt,       kParamSlide,    -24.0f, 24.0f);
    applyMod (p.drive,         kParamDrive,     0.0f, 1.0f);
    applyMod (p.tone,          kParamTone,      0.0f, 1.0f);
    applyMod (p.feedback,      kParamFeedback,  0.0f, 1.0f);
    applyMod (p.tape,          kParamTape,      0.0f, 1.0f);
    applyMod (p.ringMod,       kParamRingMod,   0.0f, 1.0f);
    applyMod (p.varispeed,     kParamVarispeed, 0.0f, 1.0f);
    applyMod (p.stereo,        kParamStereo,    0.0f, 1.0f);
    applyMod (p.stretch,       kParamStretch,   0.0f, 1.0f);
    applyMod (p.shimmer,       kParamShimmer,   0.0f, 1.0f);
    applyMod (p.res,           kParamRes,       0.0f, 1.0f);
    applyMod (p.fold,          kParamFold,      0.0f, 1.0f);
    applyMod (p.gate,          kParamGate,      -1.0f, 1.0f);
    applyMod (p.smear,         kParamSmear,     0.0f, 1.0f);
    applyMod (p.stutter,       kParamStutter,   0.0f, 1.0f);
    applyMod (p.chaos,         kParamChaos,     0.0f, 1.0f);
    applyMod (p.globalMix,    kParamGlobalMix, 0.0f, 1.0f);

    return p;
}

// Build a ShapingParams from a stored scene.
LoopSaboteurProcessor::ShapingParams LoopSaboteurProcessor::paramsFromScene (int i) const
{
    const int idx = juce::jlimit (0, kNumScenes - 1, i);
    const auto& s = scenes[idx];
    ShapingParams p;
    p.divisionIdx   = s.divisionIdx  .load();
    p.lookbackIdx   = s.lookbackIdx  .load();
    p.rateIdx       = s.rateIdx      .load();
    p.judderCount   = juce::jlimit (1, kMaxJudder, s.judderCount.load());
    p.judderDivIdx  = s.judderDivIdx .load();
    p.pitchSt       = quantiseToScale (s.pitchSt.load());
    p.slideSt       = s.slideSt      .load();
    p.decay         = juce::jlimit (0.0f, 1.0f, s.decay        .load());
    p.reverseChance = juce::jlimit (0.0f, 1.0f, s.reverseChance.load());
    p.crunch        = juce::jlimit (0.0f, 1.0f, s.crunch       .load());
    p.crushRate     = juce::jlimit (0.0f, 1.0f, s.crushRate    .load());
    p.mix           = juce::jlimit (0.0f, 1.0f, s.mix          .load());
    p.globalMix     = juce::jlimit (0.0f, 1.0f, pGlobalMix->load()); // session-wide, not per-Act
    p.drive         = juce::jlimit (0.0f, 1.0f,  s.drive   .load());
    p.tone          = juce::jlimit (0.0f, 1.0f,  s.tone    .load());
    p.feedback      = juce::jlimit (0.0f, 0.95f, s.feedback.load());
    p.tape          = juce::jlimit (0.0f, 1.0f,  s.tape    .load());
    p.ringMod       = juce::jlimit (0.0f, 1.0f,  s.ringMod .load());
    p.varispeed     = juce::jlimit (0.0f, 1.0f,  s.varispeed.load());
    p.stereo        = juce::jlimit (0.0f, 1.0f,  s.stereo  .load());
    p.stretch       = juce::jlimit (0.0f, 1.0f,  s.stretch .load());
    p.shimmer       = juce::jlimit (0.0f, 1.0f,  s.shimmer .load());
    p.res           = juce::jlimit (0.0f, 0.98f, s.res     .load());
    p.fold          = juce::jlimit (0.0f, 1.0f,  s.fold    .load());
    p.gate          = juce::jlimit (-1.0f, 1.0f,  s.gate    .load());
    p.smear         = juce::jlimit (0.0f, 1.0f,  s.smear   .load());
    p.stutter       = juce::jlimit (0.0f, 1.0f,  s.stutter .load());
    p.chaos         = juce::jlimit (0.0f, 1.0f,  s.chaos   .load());

    // v0.41 — per-Act ENGINE.
    p.engineStage      = s.engineStage.load();
    p.engineMix        = juce::jlimit (0.0f, 1.0f, s.engineMix.load());
    p.engineIntensity  = juce::jlimit (0.0f, 1.0f, s.engineIntensity.load());
    p.interpMode       = s.interpMode .load();
    p.crushAA       = s.crushAA    .load();
    p.crushMu       = s.crushMu    .load();
    p.crushAll      = s.crushAll   .load();
    p.filterMode    = s.filterMode .load();
    p.freqSplitMode = s.freqSplitMode.load();
    p.freqSplitHz   = s.freqSplitHz .load();
    p.driveType      = s.driveType.load();
    p.foldTopology   = s.foldTopology.load();
    p.shimmerOctave  = s.shimmerOctave.load();
    p.smearCharacter = s.smearCharacter.load();
    p.stutterWindow  = s.stutterWindow.load();
    p.varispeedCurve = s.varispeedCurve.load();
    p.slideCurve     = s.slideCurve.load();
    p.reverseMode    = s.reverseMode.load();
    p.tapeMode       = s.tapeMode.load();
    p.ringModWave    = s.ringModWave.load();
    p.chaosDistribution = s.chaosDistribution.load();
    p.feedbackCharacter = s.feedbackCharacter.load();
    p.stretchMode    = s.stretchMode.load();
    p.judderShape    = s.judderShape.load();
    p.lookbackBehaviour = s.lookbackBehaviour.load();
    p.decayCurve    = s.decayCurve.load();

    // v0.42 — per-Act stereo mode + FX-on-Dry + ring mod quantise.
    p.stereoMode     = juce::jlimit (0, 3, s.stereoMode.load());
    p.fxOnDryDrive   = s.fxOnDryDrive  .load();
    p.fxOnDryTone    = s.fxOnDryTone   .load();
    p.fxOnDryRingMod = s.fxOnDryRingMod.load();
    p.fxOnDryFold    = s.fxOnDryFold   .load();
    p.ringModQuant   = s.ringModQuant  .load();

    // v0.30 — apply LFO modulation offsets.
    auto applyMod = [this] (float& field, const juce::String& paramId, float lo, float hi)
    {
        const int slot = lockableIndexForId (paramId);
        if (slot >= 0)
            field = juce::jlimit (lo, hi, field + lfoModOffset[slot]);
    };
    applyMod (p.mix,           kParamMix,       0.0f, 1.0f);
    applyMod (p.decay,         kParamDecay,     0.0f, 1.0f);
    applyMod (p.reverseChance, kParamReverse,   0.0f, 1.0f);
    applyMod (p.crunch,        kParamCrunch,    0.0f, 1.0f);
    applyMod (p.crushRate,     kParamCrushRate, 0.0f, 1.0f);
    applyMod (p.pitchSt,       kParamPitch,    -48.0f, 48.0f);
    applyMod (p.slideSt,       kParamSlide,    -24.0f, 24.0f);
    applyMod (p.drive,         kParamDrive,     0.0f, 1.0f);
    applyMod (p.tone,          kParamTone,      0.0f, 1.0f);
    applyMod (p.feedback,      kParamFeedback,  0.0f, 1.0f);
    applyMod (p.tape,          kParamTape,      0.0f, 1.0f);
    applyMod (p.ringMod,       kParamRingMod,   0.0f, 1.0f);
    applyMod (p.varispeed,     kParamVarispeed, 0.0f, 1.0f);
    applyMod (p.stereo,        kParamStereo,    0.0f, 1.0f);
    applyMod (p.stretch,       kParamStretch,   0.0f, 1.0f);
    applyMod (p.shimmer,       kParamShimmer,   0.0f, 1.0f);
    applyMod (p.res,           kParamRes,       0.0f, 1.0f);
    applyMod (p.fold,          kParamFold,      0.0f, 1.0f);
    applyMod (p.gate,          kParamGate,      -1.0f, 1.0f);
    applyMod (p.smear,         kParamSmear,     0.0f, 1.0f);
    applyMod (p.stutter,       kParamStutter,   0.0f, 1.0f);
    applyMod (p.chaos,         kParamChaos,     0.0f, 1.0f);
    applyMod (p.globalMix,    kParamGlobalMix, 0.0f, 1.0f);

    return p;
}

// Configure the voice from a ShapingParams snapshot. Called the
// instant a grid boundary crossing decides to fire, no matter whether
// the params came from the knobs or from a stored scene — this keeps
// freehand mode and sequencer mode sharing one engine.
void LoopSaboteurProcessor::fireVoice (const ShapingParams& params, double samplesPerQuarter, int sourceSceneIdx, double durationOverrideQ, double slicePosOverride)
{
    // Unpack the ShapingParams locally for clarity.
    const int    divisionIdx   = params.divisionIdx;
    const int    lookbackIdx   = params.lookbackIdx;
    const int    judderCount   = params.judderCount;
    const int    judderDivIdx  = params.judderDivIdx;
    const int    rateIdx       = params.rateIdx;
    const float  pitchSt       = params.pitchSt;
    const float  slideSt       = params.slideSt;
    const float  decay         = params.decay;
    const float  reverseChance = params.reverseChance;

    // v0.44 — ratchet sub-fires override the division length so
    // each sub-hit fills exactly its share of the step duration.
    const double divisionQ  = (durationOverrideQ > 0.0) ? durationOverrideQ
                                                        : divisionIndexToQuarters (divisionIdx);
    const double lookbackQ  = lookbackIndexToQuarters  (lookbackIdx);
    const double judderDivQ = judderDivIndexToQuarters (judderDivIdx);
    const double rateMult   = rateIndexToMultiplier    (rateIdx);

    // Slice source anchor: exactly lookbackQ quarter-notes behind the
    // current write head. Because we fire on a grid boundary, this
    // lands on the equivalent grid position N beats back.
    //
    const double lookbackSamples = lookbackQ * samplesPerQuarter;

    auto wrapSamplePos = [this] (double s)
    {
        while (s < 0.0)                          s += (double) circularBufferSize;
        while (s >= (double) circularBufferSize) s -= (double) circularBufferSize;
        return s;
    };

    // v0.44 — ratchet retrigger: if a slice position override is
    // provided, replay from that exact buffer position instead of
    // computing a new one from writePos-lookback. This makes ratchet
    // sub-fires replay the same chop as the first hit.
    //
    // v0.6.0 — Lookback behaviour modifies the capture position:
    //   Fixed (0):     exact lookback window (default)
    //   Jittered (1):  random offset ±12.5% of lookback length per slice
    //   Quantised (2): snap to nearest beat boundary within lookback
    double adjustedLookback = lookbackSamples;
    if (slicePosOverride < 0.0)  // only modify when not a ratchet retrigger
    {
        switch (params.lookbackBehaviour)
        {
            default:
            case 0:  // Fixed — no change
                break;
            case 1:  // Jittered — ±12.5% random offset
            {
                const double jitterRange = lookbackSamples * 0.125;
                adjustedLookback += (rng.nextDouble() * 2.0 - 1.0) * jitterRange;
                if (adjustedLookback < 0.0) adjustedLookback = 0.0;
                break;
            }
            case 2:  // Quantised — snap to nearest beat boundary
            {
                if (samplesPerQuarter > 0.0)
                {
                    const double beats = adjustedLookback / samplesPerQuarter;
                    const double snapped = std::round (beats);
                    adjustedLookback = juce::jmax (0.0, snapped * samplesPerQuarter);
                }
                break;
            }
        }
    }
    const double sliceStartPos = (slicePosOverride >= 0.0)
        ? slicePosOverride
        : wrapSamplePos ((double) writePos - adjustedLookback);

    // Slice length = one full division (in output samples). Judder
    // subdivides this into N hits of judderDivSamples each.
    const int divisionSamples  = juce::jmax (4, (int) std::round (divisionQ  * samplesPerQuarter));
    const int judderDivSamples = juce::jmax (4, (int) std::round (judderDivQ * samplesPerQuarter));

    // Decide forward vs reverse for this fire. Mode-aware branching.
    // v0.5.0 — reverseMode determines the decision strategy.
    bool reversed = false;
    switch (params.reverseMode)
    {
        default:
        case 0:  // Random — coin flip based on reverseChance
            reversed = rng.nextFloat() < reverseChance;
            break;
        case 1:  // Alternate — every other fire
            reversed = reverseChance > 0.01f && ((fireCounter & 1) != 0);
            break;
        case 2:  // Palindrome — TODO: implement true palindrome in per-sample loop
            reversed = false;  // for now, same as Random with 50% chance
            reversed = rng.nextFloat() < 0.5f;
            break;
        case 3:  // Ping-Pong — alternates based on fire count
            reversed = reverseChance > 0.01f && ((fireCounter & 1) != 0);
            break;
    }
    fireCounter++;

    // Rate magnitude = pitch semitones × rate multiplier. Signed rate
    // encodes direction — readPos += rate handles both naturally.
    const double pitchRate = semitonesToRate (pitchSt) * rateMult;
    const double signedRate = reversed ? -pitchRate : pitchRate;

    // Pick the retrig reset position. Forward: the oldest sample in
    // the slice region. Reverse: the newest sample, so readPos walks
    // backward through the source span on each sub-trigger.
    //
    // The span covered in source samples per sub-trigger equals the
    // output-sample count × rate magnitude. We use the per-retrig
    // length (judderDivSamples) if juddering, otherwise the full
    // divisionSamples. This means every retrig replays the same span,
    // which is what you want.
    const int subOutputSamples = (judderCount > 1) ? judderDivSamples : divisionSamples;
    const double sourceSpanSamples = (double) subOutputSamples * pitchRate;

    const double retrigStartPos = reversed
        ? wrapSamplePos (sliceStartPos + sourceSpanSamples)
        : sliceStartPos;

    // Configure the voice in one shot.
    voice.active          = true;
    voice.fireIndex       = fireCounter - 1;  // v0.5.0 — capture fire index for mode-aware branching
    voice.readPos         = retrigStartPos;
    voice.retrigStartPos  = retrigStartPos;
    voice.rate            = signedRate;
    voice.slideFactor     = semitonesToRate (slideSt);   // slide magnifies rate magnitude

    if (judderCount > 1)
    {
        // Rhythmic stutter: play judderDivSamples, jump back, repeat.
        // Total footprint = judderCount * judderDivSamples output samples.
        // If that runs past the next grid trigger it's fine — the next
        // fire just steals the voice.
        voice.judderIntervalSamples = judderDivSamples;
        voice.judderCountdown       = judderDivSamples;
        voice.juddersRemaining      = judderCount - 1;
        voice.samplesRemaining      = judderDivSamples * judderCount;
        voice.totalSamples          = voice.samplesRemaining;

        // v0.5.0 — Judder shape: adjust first interval for non-even modes
        if (voice.voiceJudderShape == 1)  // Accelerating — first interval longest
        {
            // Scale first interval to ~1.8x, subsequent get shorter
            voice.judderCountdown = (int) (judderDivSamples * 1.8f);
        }
        else if (voice.voiceJudderShape == 2)  // Decelerating — first interval shortest
        {
            voice.judderCountdown = (int) (judderDivSamples * 0.4f);
        }
        else if (voice.voiceJudderShape == 3)  // Random — jittered first interval
        {
            const float jitter = 0.5f + rng.nextFloat();  // 0.5x to 1.5x
            voice.judderCountdown = (int) (judderDivSamples * jitter);
        }
    }
    else
    {
        // No stutter: play the full division slice and stop.
        voice.judderIntervalSamples = 0;
        voice.judderCountdown       = 0;
        voice.juddersRemaining      = 0;
        voice.samplesRemaining      = divisionSamples;
        voice.totalSamples          = voice.samplesRemaining;
    }

    voice.crushState.reset();

    // Decay tail: start at unity and multiply on every judder hit.
    voice.currentGain    = 1.0f;
    voice.decayPerJudder = decay;

    // Bake per-voice mix and crunch from the ShapingParams so scenes
    // can own their own dry/wet and grit independently of any global
    // live knob movement.
    // v0.42.6 — global mix scales the per-Act mix. Both mod offset
    // and per-step p-locks have already been baked into params.globalMix.
    voice.voiceMix    = params.mix * params.globalMix;
    voice.voiceCrunch    = params.crunch;
    voice.voiceCrushRate = params.crushRate;

    // Bake the FX row. Same contract: captured at fire time so the
    // voice keeps its character for its whole lifetime.
    voice.voiceDrive    = params.drive;
    voice.voiceTone     = params.tone;
    voice.voiceFeedback = params.feedback;
    voice.voiceTape     = params.tape;
    voice.voiceRingMod  = params.ringMod;
    voice.voiceVarispeed = params.varispeed;
    voice.voiceStereo   = params.stereo;
    voice.voiceStretch  = params.stretch;

    // Bake the mangle row too.
    voice.voiceShimmer  = params.shimmer;
    voice.voiceRes      = params.res;
    voice.voiceFold     = params.fold;
    voice.voiceGate     = params.gate;
    voice.voiceSmear    = params.smear;
    voice.voiceStutter  = params.stutter;
    voice.voiceChaos    = params.chaos;

    // v0.41 — bake per-Act ENGINE flags onto the voice so slice-level
    // interp + crunch character travel with the fire even if the user
    // switches Acts before this slice ends.
    voice.voiceInterpMode    = juce::jlimit (0, (int) loopsab::kNumInterpModes - 1, params.interpMode);
    voice.voiceCrushAA       = params.crushAA;
    voice.voiceCrushMu       = params.crushMu;
    voice.voiceFilterMode    = juce::jlimit (0, (int) loopsab::kNumFilterModes - 1, params.filterMode);
    voice.voiceFreqSplitMode = juce::jlimit (0, 2, params.freqSplitMode);
    voice.voiceFreqSplitHz   = juce::jlimit (20.0f, 20000.0f, params.freqSplitHz);
    voice.voiceDriveType      = juce::jlimit (0, (int) loopsab::kNumDriveTypes - 1, params.driveType);
    voice.voiceFoldTopology   = juce::jlimit (0, (int) loopsab::kNumFoldTopologies - 1, params.foldTopology);
    voice.voiceShimmerOctave  = juce::jlimit (0, (int) loopsab::kNumShimmerOctaves - 1, params.shimmerOctave);
    voice.voiceSmearCharacter = juce::jlimit (0, (int) loopsab::kNumSmearCharacters - 1, params.smearCharacter);
    voice.voiceStutterWindow  = juce::jlimit (0, (int) loopsab::kNumStutterWindows - 1, params.stutterWindow);
    voice.voiceVarispeedCurve = juce::jlimit (0, (int) loopsab::kNumVarispeedCurves - 1, params.varispeedCurve);
    voice.voiceSlideCurve     = juce::jlimit (0, (int) loopsab::kNumSlideCurves - 1, params.slideCurve);
    voice.voiceReverseMode    = juce::jlimit (0, (int) loopsab::kNumReverseModes - 1, params.reverseMode);
    voice.voiceTapeMode       = juce::jlimit (0, (int) loopsab::kNumTapeModes - 1, params.tapeMode);
    voice.voiceRingModWave    = juce::jlimit (0, (int) loopsab::kNumRingModWaves - 1, params.ringModWave);
    voice.voiceChaosDistribution = juce::jlimit (0, (int) loopsab::kNumChaosDistributions - 1, params.chaosDistribution);
    voice.voiceFeedbackCharacter = juce::jlimit (0, (int) loopsab::kNumFeedbackCharacters - 1, params.feedbackCharacter);
    voice.voiceStretchMode    = juce::jlimit (0, (int) loopsab::kNumStretchModes - 1, params.stretchMode);
    voice.voiceJudderShape    = juce::jlimit (0, (int) loopsab::kNumJudderShapes - 1, params.judderShape);
    voice.voiceLookbackBehaviour = juce::jlimit (0, (int) loopsab::kNumLookbackBehaviours - 1, params.lookbackBehaviour);
    voice.voiceDecayCurve    = juce::jlimit (0, (int) loopsab::kNumDecayCurves - 1, params.decayCurve);
    voice.xoverLpL = 0.0f; voice.xoverLpR = 0.0f;
    voice.xoverHpL = 0.0f; voice.xoverHpR = 0.0f;
    voice.voiceRingModQuant = params.ringModQuant;            // v0.42
    voice.sceneIdx        = sourceSceneIdx;

    // Hoist SMEAR to processor scope so the delay tail keeps pouring
    // out with this Act's character after the voice has ended. Each
    // new fire resets it to that Act's value.
    smearFeedAmount     = params.smear;

    // Reset filter state so each fire starts clean (no leftover DC
    // from a previous voice's tail bleeding into the next slice).
    voice.svfLowL  = 0.0f; voice.svfBandL = 0.0f;
    voice.svfLowR  = 0.0f; voice.svfBandR = 0.0f;

    // Reset mangle state machines so previous voice's final sample
    // doesn't ghost into the next slice as a stuck sample-hold.
    voice.shimmerHeldL  = 0.0f;
    voice.shimmerHeldR  = 0.0f;
    voice.shimmerCounter = 0;
    voice.chaosHeldL    = 0.0f;
    voice.chaosHeldR    = 0.0f;
    voice.gateEnvelope  = 1.0f;   // starts fully open, decays from here
    voice.gateLpgL      = 0.0f;
    voice.gateLpgR      = 0.0f;
    voice.gateLpg2L     = 0.0f;
    voice.gateLpg2R     = 0.0f;
    voice.stutterLoopLen   = 0;     // v0.22 — reset stutter for fresh capture
    voice.stutterWritePos  = 0;
    voice.stutterReadPos   = 0;
    voice.stutterCaptured  = false;

    // v0.30 — anti-click fade: start at zero and ramp up.
    voice.fadeGain = 0.0f;
    voice.fadingIn = true;

    // Randomise LFO phase per fire so consecutive slices don't all
    // wobble in lockstep; looks more tape-machine-lifelike.
    voice.tapeWowPhase     = (double) rng.nextFloat() * juce::MathConstants<double>::twoPi;
    voice.tapeFlutterPhase = (double) rng.nextFloat() * juce::MathConstants<double>::twoPi;
    voice.ringModPhase     = (double) rng.nextFloat() * juce::MathConstants<double>::twoPi;
    // v0.25 — stereo mode setup. Cache the mode and initialise the
    // per-voice state so the per-sample loop doesn't branch on atomics.
    // v0.42 — stereo mode is now per-Act; take it from the ShapingParams
    // so the voice keeps its source Act's stereo character even if the
    // user flips Acts mid-flight.
    voice.stereoModeCache  = juce::jlimit (0, 3, params.stereoMode);
    switch (voice.stereoModeCache)
    {
        default:
        case kStereoRandom:
            voice.stereoPan = rng.nextFloat() * 2.0f - 1.0f;
            break;
        case kStereoPingPong:
            voice.stereoPan = (stereoPingPongSide++ & 1) ? 1.0f : -1.0f;
            break;
        case kStereoAutoPan:
            voice.stereoPan = 0.0f;  // not used; LFO drives pan live
            voice.stereoLfoPhase = (double) rng.nextFloat() * juce::MathConstants<double>::twoPi;
            break;
        case kStereoHaas:
            voice.stereoPan = (rng.nextFloat() > 0.5f) ? 1.0f : -1.0f;
            // Delay up to ~25ms scaled by knob depth
            voice.stereoHaasSamples = (int) (getSampleRate() * 0.025 * (double) voice.voiceStereo);
            break;
    }

    // v0.25 — granular stretch: initialise grain readers at the voice's
    // start position. v0.5.0 — 4 grains at ~120ms for smoother sound.
    voice.stretchSourcePos = voice.readPos;
    for (int g = 0; g < PlaybackVoice::kNumStretchGrains; ++g)
        voice.stretchGrainPos[g] = voice.readPos;
    voice.stretchGrainLen   = juce::jmax (512, (int) (getSampleRate() * 0.12));
    voice.stretchGrainCount = 0;

}

// v0.21 — CRUSH split into independent BITS (bit-depth reduction) and
// RATE (sample-rate decimation). Both knobs are 0..1 and either can
// be used alone for pure bit-crush or pure aliasing.
//
// Optional mu-law companding (crushMuLaw flag): applies a logarithmic
// encoding before quantisation and decodes after, so quiet signals
// keep more resolution — exactly how the S950/SP-1200 worked. Without
// it you get a straight linear quantiser (harsher, more digital).
//
// Optional anti-alias LP (crushAntiAlias flag): a one-pole filter at
// the effective Nyquist after the S&H stage. Real vintage samplers had
// hardware output filters that caught the aliasing and folded it back
// into warmth. Without it you get the raw, fizzy, digital aliasing.
void LoopSaboteurProcessor::applyCrunch (float& l, float& r, float bitAmount, float rateAmount, CrushState& cs)
{
    applyCrunch (l, r, bitAmount, rateAmount, cs,
                 crushAntiAlias.load(), crushMuLaw.load());
}

// v0.41 — per-voice variant. Voice bakes its source Act's crush flags at
// fire time and passes them in here so per-Act character holds steady.
void LoopSaboteurProcessor::applyCrunch (float& l, float& r, float bitAmount, float rateAmount,
                                         CrushState& cs, bool useAA, bool useMu)
{
    const bool hasBits = bitAmount > 0.0001f;
    const bool hasRate = rateAmount > 0.0001f;
    if (! hasBits && ! hasRate)
        return;

    // --- RATE: sample-and-hold decimation --------------------------------
    if (hasRate)
    {
        // v0.6.0 — power-1.3 curve: noticeably bites earlier than the
        // old quadratic, but still accelerates into the extremes.
        // Cap at ~2 kHz effective rate regardless of session SR.
        const int maxHold = juce::jmax (2, (int) (sampleRate / 2000.0));
        const float t  = std::pow (rateAmount, 1.3f);
        const int srHold = 1 + (int) (t * (float) (maxHold - 1));

        // Jittered hold period for organic, hardware-like feel.
        int holdPeriod = srHold;
        if (srHold > 2)
        {
            const int jit = (int) ((cs.sampleCounter * 0x9E3779B1u) >> 30);
            holdPeriod = juce::jmax (1, srHold + (jit < 2 ? 0 : (jit == 2 ? -1 : 1)));
        }

        if ((cs.sampleCounter % holdPeriod) == 0)
        {
            cs.held[0] = l;
            cs.held[1] = r;
        }
        l = cs.held[0];
        r = cs.held[1];

        // Anti-alias one-pole LP at the effective Nyquist.
        // Sqrt of the naive coefficient keeps the filter gentler —
        // real hardware reconstruction filters roll off more
        // gradually than a raw 1/N one-pole. This preserves
        // brightness while still taming the worst aliasing fizz.
        if (useAA && srHold > 1)
        {
            const float aaCoeff = juce::jlimit (0.05f, 1.0f, std::sqrt (1.0f / (float) srHold));
            cs.aaL += aaCoeff * (l - cs.aaL);
            cs.aaR += aaCoeff * (r - cs.aaR);
            l = cs.aaL;
            r = cs.aaR;
        }
    }

    // --- BITS: bit-depth reduction ---------------------------------------
    if (hasBits)
    {
        // 0% = 16-bit, 100% = ~2-bit.
        const int bits = 16 - (int) (bitAmount * 14.0f);       // 16..2
        const float levels = (float) (1 << juce::jmax (1, bits - 1));

        if (useMu)
        {
            // Mu-law companding (mu=255, telephony standard). Compress
            // before quantise, expand after. Gives more resolution to
            // quiet signals — the S950 / SP-1200 secret ingredient.
            const float mu = 255.0f;
            auto compress = [mu] (float x)
            {
                const float sign = x < 0.0f ? -1.0f : 1.0f;
                const float ax = std::abs (x);
                return sign * std::log (1.0f + mu * ax) / std::log (1.0f + mu);
            };
            auto expand = [mu] (float x)
            {
                const float sign = x < 0.0f ? -1.0f : 1.0f;
                const float ax = std::abs (x);
                return sign * (std::pow (1.0f + mu, ax) - 1.0f) / mu;
            };

            l = expand (std::round (compress (l) * levels) / levels);
            r = expand (std::round (compress (r) * levels) / levels);
        }
        else
        {
            // Straight linear quantisation.
            l = std::round (l * levels) / levels;
            r = std::round (r * levels) / levels;
        }
    }
}

// ----------------------------------------------------------------------------
//  Audio processing
// ----------------------------------------------------------------------------
void LoopSaboteurProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    // v0.30 — CPU watchdog: timestamp entry.
    const auto dbgBlockStart = std::chrono::high_resolution_clock::now();

    const int numSamples  = buffer.getNumSamples();
    const int numChannels = juce::jmin (buffer.getNumChannels(),
                                        circularBuffer.getNumChannels());

    // v0.42.5 — Mono → stereo handling. When the host hands us more
    // output channels than input channels (i.e. loaded as a Mono→Stereo
    // insert on a mono track in Logic), JUCE sizes `buffer` to
    // max(in, out) channels. Channel 0 contains the mono source;
    // channels 1..N are output-only and arrive garbage/silent. The
    // rest of processBlock treats `numChannels` as stereo-capable, so
    // we duplicate channel 0 into the extra output channels *before*
    // any DSP runs. That way stereo widening, per-side FX, and dry-
    // signal passthrough all operate on real audio on both sides
    // rather than silence on the R, which previously produced a
    // half-volume lopsided output.
    //
    // The old code cleared these extra channels with `buffer.clear()`;
    // that was fine for the matched-only configuration previously
    // supported but broke the moment we opened up 1→2.
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.copyFrom (ch, 0, buffer, 0, 0, numSamples);

    // --- transport info -------------------------------------------------
    double bpm       = 120.0;
    double ppqStart  = 0.0;
    bool   isPlaying = false;

    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto b = pos->getBpm())
                if (*b > 0.1) bpm = *b;  // reject invalid/zero BPM
            if (auto p = pos->getPpqPosition()) ppqStart  = *p;
            isPlaying = pos->getIsPlaying();
        }
    }

    // v0.33 — publish transport snapshot for the UI (LFO scope preview).
    lastKnownBpm.store (bpm);
    isHostPlaying.store (isPlaying);

    const double samplesPerQuarter = sampleRate * 60.0 / bpm;
    const double quartersPerSample = 1.0 / samplesPerQuarter;

    // Reset phrase tracking whenever transport is stopped, so the next
    // play starts cleanly without replaying stale boundary crossings.
    if (! isPlaying)
    {
        lastPpq = -1.0;
        lastWrappedStep = -1;
        seqOriginSet = false;
        lastExpectedStep = 0;
        seqSampleCounter = 0;
        lastSeqStepAbsForRatio = -1;
        currentPlayingStep.store (-1);  // v0.18 — clear playhead so step 0 registers as a change on next play
    }

    // v0.29 — detect play-start transition. Force a clean re-latch
    // so the sequencer always begins at step 0, even if the DAW's
    // reported ppq jumps around during pre-roll/count-in.
    const bool justStartedPlaying = (isPlaying && ! wasPlaying);
    wasPlaying = isPlaying;
    if (justStartedPlaying)
    {
        seqOriginSet     = false;
        lastExpectedStep = 0;
        seqSampleCounter = 0;   // v0.29 — reset step counter on play
        lastPpq          = -1.0;
        lastWrappedStep  = -1;
        ratioPatternPlayCount = 0;  // v0.43 — reset on play start
        lastSeqStepAbsForRatio = -1;
        // v0.38 — conditional-trigger state resets on each transport
        // play-start. ONCE re-arms (fires its single hit again); IF PREV
        // / NOT PREV start with a clean "nothing fired yet" history so
        // the very first pass behaves predictably.
        for (int i = 0; i < kMaxSteps; ++i)
        {
            stepLastFireResult[i] = false;
            onceStepFired[i]      = false;
        }
    }

    // --- snapshot per-block control values ------------------------------
    // v0.30 — CHANCE, SWING, and FREEZE are now read per-sample inside
    // the loop for tighter automation response. Only the grid-structure
    // parameters (division, seq rate, seq on, seq length) are cached
    // per-block because they define the grid geometry and don't benefit
    // from sub-block resolution.
    const int   divisionIdx  = (int) std::round (pDivision->load());
    const int   seqRateIdx   = (int) std::round (pSeqRate->load());
    const bool  seqEnabled   = seqOn.load();
    const int   seqLen       = juce::jlimit (2, kMaxSteps, seqLength.load());

    const double divisionQ = divisionIndexToQuarters (divisionIdx);
    const double seqStepQ  = seqRateIndexToQuarters  (seqRateIdx);

    // v0.6 firing model — "Option C":
    //   * Freehand mode fires at the current Division knob rate.
    //   * Sequencer mode advances the step pointer at SEQ RATE but
    //     fires at the CURRENTLY-ACTIVE Act's own Division. This means
    //     if every step is Act A with Division 1/16, you get the same
    //     16 fires/bar as freehand-with-1/16, regardless of SEQ RATE.
    //     Same-Act-everywhere now sounds identical to running the
    //     plugin straight, which matches the user's mental model.

    auto* const* io = buffer.getArrayOfWritePointers();
    auto* const* cb = circularBuffer.getArrayOfWritePointers();

    // v0.30 — debug force-fire: immediately fire a voice with knob values.
    if (dbgForceFire.exchange (false))
        fireVoice (paramsFromKnobs(), samplesPerQuarter);

    // v0.30 — cache the global-FX toggles once per block. These are
    // UI-driven bools (not automatable) so per-block is fine.
    // v0.41 — crushAll is per-Act now: read from the currently-selected
    // Act, fall back to the legacy global if there's no Act focus.
    bool gfxCrush;
    {
        const int sIdx = selectedScene.load();
        gfxCrush = (sIdx >= 0 && sIdx < kNumScenes)
                 ? scenes[sIdx].crushAll.load()
                 : crushAllAudio.load();
    }
    // v0.9.1 — FX-on-Dry flags are per-Act. When the sequencer is on,
    // resolve them from the Act on the currently-playing step so they
    // follow the sequence rather than persisting from the selected tab.
    // When seq is off, use the selected scene. Initialised here as a
    // default; updated inside the sample loop when activeSceneForFire
    // is known.
    bool gfxDrive = false, gfxTone = false, gfxRingMod = false;
    bool gfxFold = false,  gfxRingModQuant = false;
    int  gfxSceneIdx = -1;  // tracks which scene the flags were last read from

    auto resolveGfxFlags = [&] (int sIdx) {
        if (sIdx == gfxSceneIdx) return;  // same scene — no change
        gfxSceneIdx = sIdx;
        if (sIdx >= 0 && sIdx < kNumScenes)
        {
            gfxDrive        = scenes[sIdx].fxOnDryDrive  .load();
            gfxTone         = scenes[sIdx].fxOnDryTone   .load();
            gfxRingMod      = scenes[sIdx].fxOnDryRingMod.load();
            gfxFold         = scenes[sIdx].fxOnDryFold   .load();
            gfxRingModQuant = scenes[sIdx].ringModQuant  .load();
        }
        else
        {
            gfxDrive        = globalDrive    .load();
            gfxTone         = globalTone     .load();
            gfxRingMod      = globalRingMod  .load();
            gfxFold         = globalFold     .load();
            gfxRingModQuant = ringModQuantise.load();
        }
    };
    // Initial resolve from the selected scene (seq-off default).
    resolveGfxFlags (selectedScene.load());
    bool anyGlobalFx = gfxCrush || gfxDrive || gfxTone || gfxRingMod || gfxFold;

    // --- sample loop ----------------------------------------------------
    for (int i = 0; i < numSamples; ++i)
    {
        // v0.30 — save clean input before any processing. Used by the
        // global-FX crossfade at the end so MIX 0% always = untouched source.
        const float cleanL = io[0][i];
        const float cleanR = (numChannels > 1) ? io[1][i] : cleanL;

        // v0.5.0 — envelope follower input level (peak of current sample).
        envFollowInputLevel = juce::jmax (std::abs (cleanL), std::abs (cleanR));

        // 1. Work out the currently-playing sequencer step first, because
        //    per-step Freeze holds need to be known BEFORE we decide
        //    whether to capture the incoming audio into the ring.
        int       wrappedStep        = -1;
        int       activeSceneForFire = -1;
        long long curSeqStepAbs      = -1;  // monotonic — ratio counter cache key
        double    samplePpq          = 0.0;
        if (isPlaying)
        {
            samplePpq = ppqStart + (double) i * quartersPerSample;
            if (seqEnabled)
            {
                // v0.44 — origin-latched step pointer. On the first
                // playing sample after transport starts, record the PPQ
                // as the origin. All subsequent step calculations are
                // relative to that origin so the sequencer always starts
                // from step 0 regardless of which bar the DAW plays from.
                //
                // v0.5.0 — re-latch when PPQ jumps backwards (DAW loop
                // restart). Without this, looping from the same bar gives
                // a different sequencer position each time because the
                // origin was captured on the very first play and never
                // updated.
                const bool loopRestart = (lastPpq >= 0.0 && samplePpq < lastPpq - seqStepQ);
                if (! seqOriginSet || loopRestart)
                {
                    seqOriginStep = (long long) std::floor (samplePpq / seqStepQ);
                    seqOriginSet  = true;
                    // Full state reset so the sequencer starts cleanly —
                    // same as a transport play-start.
                    lastWrappedStep = -1;
                    lastPpq = -1.0;
                    ratioPatternPlayCount = 0;
                    lastSeqStepAbsForRatio = -1;
                    lastExpectedStep = 0;
                    for (int s = 0; s < kMaxSteps; ++s)
                    {
                        stepLastFireResult[s] = false;
                        onceStepFired[s]      = false;
                    }
                }
                curSeqStepAbs = (long long) std::floor (samplePpq / seqStepQ) - seqOriginStep;
                // Ensure non-negative (negative PPQ during count-in).
                if (curSeqStepAbs < 0) curSeqStepAbs = 0;

                // v0.11 — transport mode transforms the monotonic step
                // counter into the wrapped index that actually reads from
                // the grid. Forward is the old behaviour; Reverse mirrors
                // it; Ping-Pong walks a triangle of period 2*(seqLen-1);
                // Random uses a cheap XOR hash so the same counter always
                // lands on the same "random" step (deterministic, bar-
                // locked — no skipping across retriggers).
                const int mode = seqTransport.load();
                long long fwdMod = ((curSeqStepAbs % seqLen) + seqLen) % seqLen;
                int mapped = (int) fwdMod;

                if (mode == kTransportReverse)
                {
                    mapped = (seqLen - 1) - (int) fwdMod;
                }
                else if (mode == kTransportPingPong && seqLen > 1)
                {
                    // Triangle wave: period 2*(seqLen - 1). Values 0..L-1
                    // forward, then L-2..1 back.
                    const long long period = 2 * (long long)(seqLen - 1);
                    long long t = ((curSeqStepAbs % period) + period) % period;
                    mapped = (t < seqLen) ? (int) t : (int) (period - t);
                }
                else if (mode == kTransportRandom)
                {
                    // Splitmix-style hash of the monotonic step count.
                    // Low bits modulo seqLen give a deterministic but
                    // pseudo-random walk across the grid.
                    uint64_t x = (uint64_t) curSeqStepAbs + 0x9E3779B97F4A7C15ULL;
                    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
                    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
                    x =  x ^ (x >> 31);
                    mapped = (int) (x % (uint64_t) seqLen);
                }

                wrappedStep = juce::jlimit (0, seqLen - 1, mapped);
                activeSceneForFire = steps[wrappedStep].load();
                currentPlayingStep.store (wrappedStep);

                // v0.9.1 — resolve FX-on-dry from the active step's Act
                // so the dry-path effects follow the sequence.
                {
                    const int fxScene = (activeSceneForFire >= 0)
                                         ? activeSceneForFire
                                         : selectedScene.load();
                    resolveGfxFlags (fxScene);
                    anyGlobalFx = gfxCrush || gfxDrive || gfxTone || gfxRingMod || gfxFold;
                }

                // v0.14 — muted-step cut on step entry. Kill the voice
                // the moment we land on a new muted step, regardless of
                // the fire-grid boundary. This creates hard dropout gaps.
                // v0.35 — soft-kill via the anti-click fade tail instead
                // of a hard active=false, so consecutive muted steps don't
                // generate an audible click at each boundary. We clamp
                // samplesRemaining down to the 32-sample anti-click window
                // and force fadingIn=false; the existing ramp at the end
                // of the voice loop then fades to zero and sets active=false.
                //
                // v0.36 — ALSO soft-kill when entering a BLANK step (no
                // Act, no locks, no ratchet, no freeze-hold, no ratio).
                // Previous behaviour let a voice fired on a prior active
                // step keep ringing across subsequent blank steps; if
                // that step used MIX 100% + high decay + judder, the dry
                // signal was silenced for the whole tail. The user mental
                // model is "each step is atomic — a blank step plays the
                // dry signal", so we stop the voice (and any in-flight
                // ratchet) on blank-step entry so the dry signal resumes.
                if (wrappedStep != lastWrappedStep)
                {
                    // v0.44 — detect pattern wrap for ratio counter.
                    // Only count wraps that happen due to forward playback
                    // (curSeqStepAbs advancing past a seqLen boundary).
                    // Backward jumps (DAW loop) are ignored so the ratio
                    // counter doesn't double-increment on loop points.
                    if (lastSeqStepAbsForRatio >= 0
                        && curSeqStepAbs > lastSeqStepAbsForRatio
                        && (curSeqStepAbs / seqLen) > (lastSeqStepAbsForRatio / seqLen))
                    {
                        ratioPatternPlayCount++;
                    }
                    lastSeqStepAbsForRatio = curSeqStepAbs;

                    lastWrappedStep = wrappedStep;

                    const bool stepMuted  = stepMute[wrappedStep].load();
                    const bool hasScene   = steps[wrappedStep].load() >= 0;
                    const bool hasLocks   = stepHasAnyLock (wrappedStep);
                    const bool hasRatchet = stepRatchet[wrappedStep].load() > 1;
                    const bool hasFreeze  = stepFreezeHold[wrappedStep].load();
                    const bool hasRatio   = stepRatio[wrappedStep].load() > 0;
                    const bool stepBlank  = ! (hasScene || hasLocks || hasRatchet
                                                || hasFreeze || hasRatio);

                    if (stepMuted)
                    {
                        // v0.44 — muted steps are hard kills: silence
                        // the voice AND cancel any in-flight ratchet.
                        if (voice.active)
                        {
                            voice.samplesRemaining = juce::jmin (voice.samplesRemaining,
                                                                 PlaybackVoice::kAntiClickSamples);
                            voice.fadingIn = false;
                        }
                        pendingRatchet.remaining = 0;
                    }
                    else if (stepBlank)
                    {
                        // v0.44 — blank steps kill the voice but let
                        // any in-flight ratchet from the previous step
                        // play out its remaining sub-fires.
                        if (voice.active && pendingRatchet.remaining <= 0)
                        {
                            voice.samplesRemaining = juce::jmin (voice.samplesRemaining,
                                                                 PlaybackVoice::kAntiClickSamples);
                            voice.fadingIn = false;
                        }
                    }
                }
            }
        }

        // v0.30 — LFO modulation engine. Advance phase using PPQ and
        // compute per-sample offsets for targeted parameters.
        // Only apply LFOs from the currently-playing Act.
        {
            // Clear the offset array each sample.
            std::fill (std::begin (lfoModOffset), std::end (lfoModOffset), 0.0f);

            // Determine the active Act: either the playing Act from the sequencer,
            // or use selectedScene as a fallback (when freehand mode is active).
            int activeActForLfo = activeSceneForFire;
            if (activeActForLfo < 0)
                activeActForLfo = selectedScene.load();
            if (activeActForLfo < 0 || activeActForLfo >= kNumScenes)
                activeActForLfo = 0;

            // v0.34 — sample period in ms, used by envelope progression.
            const double msPerSample = 1000.0 / sampleRate;

            // v0.35 — Advance phases for ALL scenes' LFOs so the MOD-page
            // scope is always alive, regardless of which Act the sequencer
            // is currently playing or which Act the user is editing. We
            // still only apply modulation to audio from the ACTIVE Act —
            // non-active Acts advance their state but don't touch
            // lfoModOffset. This makes LFOs behave as first-class modulators
            // that are "always running" whenever the plugin is live.
            for (int si = 0; si < kNumScenes; ++si)
            {
                const bool applyToAudio = (si == activeActForLfo);

                for (int li = 0; li < kNumLfosPerAct; ++li)
                {
                    auto& lfo = lfos[si][li];
                    const int   target = lfo.targetSlot.load();
                    const float depth  = lfo.depth.load();
                    const int   shape  = lfo.shape.load();
                    const bool  isEnv  = (shape == kLfoEnvAHD);
                    const bool  isFollow = (shape == kLfoEnvFollow);

                    // v0.38 — cross-mod input: any other LFO in this Act
                    // whose crossTargetLfo points at ME contributes to my
                    // effective depth. We read their lastOutput — which is
                    // one sample stale — which breaks feedback loops and
                    // removes any ordering dependency between the 4 LFOs.
                    // At LFO rates a 1-sample delay is musically imperceptible.
                    float depthMod = 0.0f;
                    for (int other = 0; other < kNumLfosPerAct; ++other)
                    {
                        if (other == li) continue;  // no self-mod
                        const auto& oLfo = lfos[si][other];
                        if (oLfo.crossTargetLfo.load() == li)
                            depthMod += oLfo.lastOutput * oLfo.crossDepth.load();
                    }
                    // (1 + depthMod) is the scaling factor. depthMod of 0
                    // means no change; 1 means "double my depth"; -1 means
                    // "invert it". Clamped so runaway accumulation can't
                    // push effective depth outside a sensible range.
                    const float effDepth = juce::jlimit (-1.0f, 1.0f,
                                                         depth * (1.0f + depthMod));

                    // Phase/stage still advances even with zero depth or no
                    // target, so the scope preview keeps scrolling while the
                    // user dials in settings.
                    float val = 0.0f;

                    if (isEnv)
                    {
                        // v0.34 — AHD envelope. Retriggered by fireLfoTriggers
                        // via envPending flag. Monopolar output 0..+1; the
                        // multiply by (possibly negative) depth gives inversion.
                        if (lfo.envPending)
                        {
                            lfo.envStage   = 1;      // -> attack
                            lfo.envPos     = 0.0;
                            lfo.envPending = false;
                        }

                        if (lfo.envStage > 0)
                        {
                            const double aMs = (double) juce::jmax (0.1f, lfo.attackMs.load());
                            const double hMs = (double) juce::jmax (0.0f, lfo.holdMs.load());
                            const double dMs = (double) juce::jmax (0.1f, lfo.decayMs.load());

                            switch (lfo.envStage)
                            {
                                case 1: // attack
                                    lfo.envValue = (float) juce::jlimit (0.0, 1.0, lfo.envPos / aMs);
                                    lfo.envPos += msPerSample;
                                    if (lfo.envPos >= aMs)
                                    {
                                        lfo.envStage = (hMs > 0.0) ? 2 : 3;
                                        lfo.envPos   = 0.0;
                                        lfo.envValue = 1.0f;
                                    }
                                    break;
                                case 2: // hold
                                    lfo.envValue = 1.0f;
                                    lfo.envPos += msPerSample;
                                    if (lfo.envPos >= hMs)
                                    {
                                        lfo.envStage = 3;
                                        lfo.envPos   = 0.0;
                                    }
                                    break;
                                case 3: // decay
                                    lfo.envValue = (float) juce::jlimit (0.0, 1.0, 1.0 - (lfo.envPos / dMs));
                                    lfo.envPos += msPerSample;
                                    if (lfo.envPos >= dMs)
                                    {
                                        lfo.envStage = 0;
                                        lfo.envPos   = 0.0;
                                        lfo.envValue = 0.0f;
                                    }
                                    break;
                                default: break;
                            }
                        }

                        val = lfo.envValue;  // monopolar 0..+1
                    }
                    else if (isFollow)
                    {
                        // v0.5.0 — Envelope follower: tracks input amplitude.
                        // Rate knob repurposes as smoothing time. Higher rate
                        // index = faster response. Uses a simple one-pole
                        // filter with asymmetric attack/release.
                        const int rateIdx = lfo.rateIdx.load();
                        // Map rate index to smoothing time: lower index = more
                        // smoothing. rateIdx 0 = ~200ms, rateIdx max = ~2ms.
                        const float smoothMs = juce::jlimit (1.0f, 200.0f,
                            200.0f / juce::jmax (1.0f, (float) rateIdx));
                        const float attackCoeff = std::exp (-1.0f / (float) (sampleRate * smoothMs * 0.001f * 0.3f));
                        const float releaseCoeff = std::exp (-1.0f / (float) (sampleRate * smoothMs * 0.001f));

                        // v0.6.0 — input gain / sensitivity boost.
                        const float inputLvl = juce::jlimit (0.0f, 1.0f,
                            envFollowInputLevel * lfo.envFollowGain.load());
                        // Asymmetric follower: fast attack, slower release.
                        if (inputLvl > lfo.envFollowValue)
                            lfo.envFollowValue = attackCoeff * lfo.envFollowValue
                                               + (1.0f - attackCoeff) * inputLvl;
                        else
                            lfo.envFollowValue = releaseCoeff * lfo.envFollowValue
                                               + (1.0f - releaseCoeff) * inputLvl;

                        val = juce::jlimit (0.0f, 1.0f, lfo.envFollowValue);
                    }
                    else
                    {
                        // LFO (continuous waveform) path — rate==Off means
                        // we still show a flat line but don't advance phase.
                        const int rateIdx = lfo.rateIdx.load();
                        if (rateIdx > 0)
                        {
                            const bool synced = lfo.sync.load();
                            if (synced)
                            {
                                // v0.37 — SYNC: the rate knob means bar
                                // divisions. Phase derives from host PPQ so
                                // all LFOs at a given rate stay in lockstep
                                // with musical time. Retriggers on loop
                                // wrap — that's the correct behaviour for
                                // tempo-synced modulation.
                                const double lfoQ = lfoDivisionIndexToQuarters (rateIdx);
                                if (lfoQ > 0.0001)
                                {
                                    if (isPlaying)
                                    {
                                        lfo.phase = std::fmod (samplePpq / lfoQ, 1.0);
                                        if (lfo.phase < 0.0) lfo.phase += 1.0;
                                    }
                                    else
                                    {
                                        // Host stopped: keep the LFO turning
                                        // at the musical rate so UI scope
                                        // doesn't freeze. Uses BPM fallback.
                                        const double bpm = (lastKnownBpm.load() > 1.0) ? lastKnownBpm.load() : 120.0;
                                        const double secPerCycle = (lfoQ * 60.0) / bpm;
                                        if (secPerCycle > 1.0e-6)
                                            lfo.phase += (1.0 / sampleRate) / secPerCycle;
                                        if (lfo.phase >= 1.0) lfo.phase -= std::floor (lfo.phase);
                                    }
                                }
                            }
                            else
                            {
                                // v0.37 — FREE: the rate knob means Hz.
                                // Pure sample-clock accumulation, no PPQ,
                                // no BPM, no transport coupling. Survives
                                // loop wraps, stops, starts, tempo changes
                                // without a retrigger. This is the actual
                                // "free" in free-run.
                                const double hz = lfoHzIndexToHz (rateIdx);
                                if (hz > 1.0e-6 && sampleRate > 1.0)
                                    lfo.phase += hz / sampleRate;
                                if (lfo.phase >= 1.0) lfo.phase -= std::floor (lfo.phase);
                            }

                            const double ph = lfo.phase;
                            switch (shape)
                            {
                                default:
                                case kLfoSine:
                                    val = (float) std::sin (ph * juce::MathConstants<double>::twoPi);
                                    break;
                                case kLfoTriangle:
                                    val = (float) (ph < 0.5 ? (4.0 * ph - 1.0) : (3.0 - 4.0 * ph));
                                    break;
                                case kLfoSaw:
                                    val = (float) (1.0 - 2.0 * ph);
                                    break;
                                case kLfoSquare:
                                    val = ph < 0.5f ? 1.0f : -1.0f;
                                    break;
                                case kLfoSandH:
                                {
                                    // v0.9.1 — detect cycle wrap by comparing
                                    // current phase to previous. Latch a fresh
                                    // random value each time the phase resets.
                                    if (ph < lfo.sAndHPrev)
                                        lfo.sAndHValue = rng.nextFloat() * 2.0f - 1.0f;
                                    lfo.sAndHPrev = ph;
                                    val = lfo.sAndHValue;
                                    break;
                                }
                            }
                        }
                    }

                    // v0.38 — stored/applied with cross-mod-adjusted depth
                    // so other LFOs that target me via crossTargetLfo can
                    // actually move the needle. lastOutput feeds the next
                    // sample's cross-mod inputs on other LFOs.
                    lfo.lastOutput = val * effDepth;

                    // Only the active Act's LFOs actually modulate audio;
                    // everyone else just keeps their phase ticking.
                    if (applyToAudio
                        && target >= 0 && target < kNumLockableParams
                        && std::abs (effDepth) >= 0.0001f)
                    {
                        lfoModOffset[target] += val * effDepth;
                    }
                }
            }
        }

        // 2. Compute effective freeze. Any of the three mechanisms can
        //    engage the ring lock:
        //      a) kParamFreeze (button toggle)
        //      b) editor-driven momentaryFreeze (hold-to-engage)
        //      c) per-step freeze-hold flag on the currently playing step
        // v0.30 — per-sample reads for sample-accurate automation.
        const bool knobFrozen = pFreeze && pFreeze->load() >= 0.5f;
        const bool momFrozen  = momentaryFreeze.load();
        const bool stepFrozen = (wrappedStep >= 0) && stepFreezeHold[wrappedStep].load();
        const bool frozen     = knobFrozen || momFrozen || stepFrozen;

        // 3. Capture live input into the circular buffer, unless some
        //    freeze mechanism is engaged — in which case the buffer
        //    holds its last state.
        if (! frozen)
        {
            for (int ch = 0; ch < numChannels; ++ch)
                cb[ch][writePos] = io[ch][i];
        }

        // 4. Input peak ring for the waveform strip. We accumulate the
        //    abs-max over kPeakBucketSamples samples and emit one peak
        //    into the ring per bucket. Reading happens on the editor
        //    timer so we use relaxed atomics.
        {
            float peak = std::abs (io[0][i]);
            if (numChannels > 1)
                peak = juce::jmax (peak, std::abs (io[1][i]));
            if (peak > peakAccum)
                peakAccum = peak;
            if (++peakAccumCount >= kPeakBucketSamples)
            {
                const int wp = peakWritePos.load();
                peakRing[wp].store (peakAccum);
                peakWritePos.store ((wp + 1) % kPeakRingSize);
                peakAccum = 0.0f;
                peakAccumCount = 0;
            }
        }

        // v0.24 — master bypass. Buffer is still recording (so you can
        // un-bypass and immediately chop), but no voices fire and no
        // effects process. The write pointer still advances below.
        if (masterBypass.load (std::memory_order_relaxed))
        {
            if (++writePos >= circularBufferSize)
                writePos = 0;
            samplePpq += quartersPerSample;
            continue;
        }

        // 5. Grid-boundary detection. We advance a floating-point PPQ
        //    counter and watch for it to cross an integer multiple of
        //    the relevant fire-rate. Freehand: Division knob. Sequencer:
        //    the currently-active Act's own Division (Option C).
        if (isPlaying)
        {
            // Effective fire-rate Q. Freehand uses the live Division
            // knob; sequencer uses the active Act's stored Division
            // (or falls back to the knob if the step is empty — it
            // won't fire anyway, but we still need a finite value).
            double fireQ = divisionQ;
            if (seqEnabled && activeSceneForFire >= 0)
                fireQ = divisionIndexToQuarters (scenes[activeSceneForFire].divisionIdx.load());

            if (lastPpq < 0.0)
            {
                // First-sample init after a play start. Put lastPpq one
                // fireQ behind so the very next comparison detects a
                // crossing immediately — sequencer highlights and
                // freehand chops both trigger on sample 0 of play.
                lastPpq = samplePpq - fireQ;
            }

            // Global Swing. Music standard: 50% = straight, 66.67% =
            // triplet, 75% = dotted-eighth. We delay every odd fire by
            // swingOffset * fireQ. swungStep() walks pairs of fires
            // (even, odd) and returns the highest fire index whose
            // start time is <= ppq.
            const float swingKnob = pSwing ? pSwing->load() : 0.5f;
            const double swingOffset = (double) juce::jlimit (0.0f, 0.5f,
                                                              (swingKnob - 0.5f) * 2.0f);

            auto swungStep = [fireQ, swingOffset] (double ppq) -> long long
            {
                const double pairQ = 2.0 * fireQ;
                long long pairIdx = (long long) std::floor (ppq / pairQ);
                const double pairFrac = (ppq - (double) pairIdx * pairQ) / pairQ;
                const double stepInPair = pairFrac * 2.0;
                if (stepInPair < 1.0 + swingOffset)
                    return 2 * pairIdx;
                return 2 * pairIdx + 1;
            };

            const long long prevFireStep = swungStep (lastPpq);
            const long long curFireStep  = swungStep (samplePpq);

            // v0.34 — work out which Act owns the envelopes for any
            // trigger event in this sample. Hoisted here (out of the
            // fire-boundary if-block) so the ratchet drain below can
            // also call it. Sequencer: Act stamped on the step if any,
            // else the selected Act. Freehand: selected Act.
            auto lfoActForTrigger = [&]() -> int
            {
                int a = (activeSceneForFire >= 0) ? activeSceneForFire : selectedScene.load();
                if (a < 0 || a >= kNumScenes) a = 0;
                return a;
            };

            if (curFireStep > prevFireStep)
            {
                // v0.30 — debug: fire timing accuracy. Since we process
                // per-sample, the fire always lands within one sample of
                // the ideal grid boundary. Store the overshoot in µs
                // (samplePpq - lastPpq converted to time).
                {
                    const double overshootPpq = samplePpq - lastPpq;
                    const double overshootSamples = overshootPpq * samplesPerQuarter;
                    const double overshootUs = overshootSamples / sampleRate * 1e6;
                    dbgLastFireTimingUs.store ((float) overshootUs);
                }

                // Crossed at least one fire boundary since the last
                // sample. Same firing-decision tree as before, just
                // driven by fireQ instead of stepQ.

                if (seqEnabled)
                {
                    // v0.9 — a step fires if EITHER:
                    //   * it has an Act stamped on it (activeSceneForFire >= 0), OR
                    //   * it carries any p-locks (lock-only step — uses the
                    //     live knob values as the base and then overlays the
                    //     locks). This matches Steve's mental model: setting
                    //     a lock on an empty step should still produce sound.
                    //
                    // v0.13 — extended: grab offset, ratio trig, ratchet, and
                    // freeze-hold on a step now ALSO make that step fire
                    // (using the live knob values as the base), matching how
                    // p-locks worked since v0.9. The step is still muted if
                    // stepMute is on.
                    const bool hasLocks   = (wrappedStep >= 0) && stepHasAnyLock (wrappedStep);
                    const bool hasRatchet = (wrappedStep >= 0) && stepRatchet[wrappedStep].load() > 1;
                    const bool hasFreeze  = (wrappedStep >= 0) && stepFreezeHold[wrappedStep].load();
                    const bool hasRatio   = (wrappedStep >= 0) && stepRatio[wrappedStep].load() > 0;  // v0.30
                    const bool stepEligible = activeSceneForFire >= 0
                                               || hasLocks
                                               || hasRatchet || hasFreeze
                                               || hasRatio;
                    const bool stepMuted = (wrappedStep >= 0) && stepMute[wrappedStep].load();

                    // v0.34 — grid-tick envelope trigger. Every fireQ
                    // boundary counts as "All Steps" for env retrigger
                    // purposes, including blank steps. Muted steps still
                    // count as a grid tick — they're just silent.
                    fireLfoTriggers (lfoActForTrigger(), kEventAllStep);

                    // v0.39 — ratio/probability bookkeeping now runs on
                    // muted steps too. The dice still roll, the A:B
                    // counter advances, ONCE latches, and
                    // stepLastFireResult[] tracks what the step WOULD
                    // have fired — so IF PREV / !PREV on the next step
                    // stay consistent regardless of mute, and un-muting
                    // a step mid-performance doesn't reset its
                    // probability phase. Actual audio fire is still
                    // gated by !stepMuted, and the mute action itself
                    // (voice kill + ratchet clear) is now ALSO gated
                    // by the probability roll when a ratio is set —
                    // so a 50% muted step lets the sound through half
                    // the time instead of muting unconditionally. If
                    // no ratio is set the mute is absolute (legacy).
                    //
                    // shouldFire is hoisted out of the stepEligible
                    // block so the mute-gate at the bottom can read it.
                    bool shouldFire = false;

                    if (stepEligible)
                    {
                        // v0.34 — "Active Step + P-Locks" envelope trigger.
                        // This step fires (scene OR lock-only OR ratchet/
                        // ratio/freeze-only). Also fire "Active Step" if
                        // the step actually has a scene stamped; p-lock-
                        // only visits don't count as an "active step".
                        // v0.39 — fires regardless of mute: these are
                        // grid-structure events, not audio-fire events.
                        const int _envAct = lfoActForTrigger();
                        fireLfoTriggers (_envAct, kEventActiveStepPlusLock);
                        if (activeSceneForFire >= 0)
                            fireLfoTriggers (_envAct, kEventActiveStep);
                        // Build the ShapingParams base: Act if stamped,
                        // otherwise the live knob values. Then overlay any
                        // per-step parameter locks.
                        // v0.44 — scene-less steps with ratchet re-trigger
                        // the dry audio from the buffer with no FX colouring.
                        // Keep structural params (division, lookback, rate)
                        // from the knobs so timing is sensible, but zero all
                        // effects and set mix to 1 (fully wet = buffer
                        // playback of the captured dry input).
                        const bool scenelessRatchet = (activeSceneForFire < 0)
                            && stepRatchet[wrappedStep].load() > 1;
                        ShapingParams params = (activeSceneForFire >= 0)
                            ? paramsFromScene (activeSceneForFire)
                            : paramsFromKnobs();
                        if (scenelessRatchet)
                        {
                            // v0.44 — clean dry retrigger: grab the most
                            // recent audio (shortest lookback), play at
                            // unity rate, no FX, no pitch, no judder.
                            params.lookbackIdx = 0;   // 1/128 — grab what just came in
                            params.rateIdx     = 3;   // 1× playback speed
                            params.judderCount = 1;   // no stutter
                            params.mix         = 1.0f;
                            params.globalMix   = 1.0f;
                            params.pitchSt     = 0.0f;
                            params.slideSt     = 0.0f;
                            params.reverseChance = 0.0f;
                            params.decay       = 1.0f;
                            params.crunch      = 0.0f;
                            params.crushRate   = 0.0f;
                            params.drive       = 0.0f;
                            params.tone        = 0.0f;
                            params.feedback    = 0.0f;
                            params.tape        = 0.0f;
                            params.ringMod     = 0.0f;
                            params.varispeed   = 0.0f;
                            params.stretch     = 0.0f;
                            params.shimmer     = 0.0f;
                            params.res         = 0.0f;
                            params.fold        = 0.0f;
                            params.smear       = 0.0f;
                            params.stutter     = 0.0f;
                            params.chaos       = 0.0f;
                            params.stereo      = 0.0f;
                        }
                        applyStepLocks (params, wrappedStep);

                        // Decide whether this visit fires. Two paths:
                        //   * Ratio lock (>0): A:B trig condition
                        //   * No ratio: Act/knob chance, possibly p-locked
                        //
                        // v0.12 — ratio semantics changed from visit-count
                        // to pattern-play. A:B now means "fire on the Ath
                        // pattern play out of every B consecutive plays",
                        // That's nicer to reason
                        // about (2:4 really IS different from 1:2 — same
                        // hit rate but different cycle alignment) and it
                        // restores the N:N family as meaningful entries
                        // (3:3 = only every 3rd play). Formula:
                        //     (patternPlay % B) == (A - 1)
                        // where patternPlay counts how many full sequence
                        // wraps have elapsed. curSeqStepAbs is already
                        // monotonic so patternPlay = curSeqStepAbs / seqLen.
                        // ratioLastSeenSeqStep / ratioLastAllowed are kept
                        // to cache the decision across intra-step multi-
                        // fires when Division < Step Rate.
                        // (shouldFire declared above, outside this block.)
                        const int ratioIdx = stepRatio[wrappedStep].load();
                        if (ratioIdx > 0)
                        {
                            int n = 0, m = 0;
                            getRatioNM (ratioIdx, n, m);
                            if (n > 0 && m > 0)
                            {
                                // Classic A:B ratio.
                                // v0.43 — use ratioPatternPlayCount (wraps
                                // independently of DAW PPQ) instead of
                                // curSeqStepAbs / seqLen, which repeats the
                                // same value when the DAW loops a section.
                                if (ratioLastSeenSeqStep != curSeqStepAbs)
                                {
                                    const int playIndex = (int) (((ratioPatternPlayCount % m) + m) % m);
                                    ratioLastAllowed = (playIndex == (n - 1));
                                    ratioLastSeenSeqStep = curSeqStepAbs;
                                }
                                shouldFire = ratioLastAllowed;
                            }
                            else if (n == -10 && m > 0)
                            {
                                // v0.38 — per-step random percentage.
                                // m carries the percent (25/50/75). Rolled
                                // fresh every visit, independent of Act
                                // CHANCE. Debounced against intra-step
                                // multi-fires via ratioLastSeenSeqStep so
                                // a ratchet group doesn't re-roll mid-hit.
                                if (ratioLastSeenSeqStep != curSeqStepAbs)
                                {
                                    const float p = (float) m * 0.01f;
                                    ratioLastAllowed = (rng.nextFloat() < p);
                                    ratioLastSeenSeqStep = curSeqStepAbs;
                                }
                                shouldFire = ratioLastAllowed;
                            }
                            else
                            {
                                // v0.38 — conditional-trigger sentinels.
                                //   n == -1 : PREV
                                //   n == -2 : !PREV
                                //   n == -3 : ONCE (first pass after transport start)
                                if (n == -1 || n == -2)
                                {
                                    const int prevIdx = (wrappedStep - 1 + seqLen) % seqLen;
                                    const bool prevFired = stepLastFireResult[prevIdx];
                                    shouldFire = (n == -1) ? prevFired : ! prevFired;
                                }
                                else if (n == -3)
                                {
                                    // Fire the first time this step is
                                    // visited after transport (re)start,
                                    // then latch off until transport
                                    // restarts. On-beat "one shot" punch-in.
                                    if (! onceStepFired[wrappedStep])
                                    {
                                        shouldFire = true;
                                        onceStepFired[wrappedStep] = true;
                                    }
                                }
                            }
                        }
                        else
                        {
                            // v0.30 — per-sample chance read for automation.
                            const float chance = pChance ? pChance->load() : 1.0f;
                            float rollChance = (activeSceneForFire >= 0)
                                ? scenes[activeSceneForFire].chance.load()
                                : chance;  // knob chance for lock-only steps
                            const int chanceSlot = lockableIndexForId (kParamChance);
                            if (chanceSlot >= 0)
                            {
                                const float lock = stepParamLock[wrappedStep * kNumLockableParams + chanceSlot].load();
                                if (! std::isnan (lock))
                                    rollChance = juce::jlimit (0.0f, 1.0f, lock);
                            }
                            shouldFire = (rng.nextFloat() < rollChance);
                        }

                        // v0.38 — remember this step's decision so the
                        // IF PREV / NOT PREV conditionals at step i+1 have
                        // a "previous-step fired?" bit to look at. Record
                        // the fire decision, not the post-ratchet count —
                        // ratcheting is about subdivision, not presence.
                        stepLastFireResult[wrappedStep] = shouldFire;

                        // v0.39 — mute gates the actual audio fire, but
                        // not the decision. shouldFire was computed above
                        // so stepLastFireResult[] reflects what the step
                        // WOULD have done. fireVoice / ratchet / Slice
                        // env triggers only run when we're actually
                        // making sound.
                        if (shouldFire && ! stepMuted)
                        {
                            // v0.41 — stamp the source Act onto the fire so
                            // the voice bakes that Act's engine character.
                            // v0.44 — if this step has ratchet, shorten
                            // the first fire to match the ratchet interval
                            // so all sub-hits are equal length.
                            const int ratchet = stepRatchet[wrappedStep].load();
                            const double ratchetDurQ = (ratchet > 1) ? (seqStepQ / (double) ratchet) : 0.0;
                            fireVoice (params, samplesPerQuarter, activeSceneForFire, ratchetDurQ);
                            // v0.34 — "Slice" envelope trigger. Every
                            // actual voice fire counts (including the
                            // first of a ratchet group; subsequent ratchet
                            // sub-hits are handled in the ratchet drain
                            // loop below).
                            fireLfoTriggers (lfoActForTrigger(), kEventSlice);

                            // v0.12 — per-step ratchet. If the step is
                            // marked with ratchet N>1, schedule (N-1)
                            // more fires with the SAME params, spaced
                            // evenly across this step's duration. Any
                            // in-flight ratchet is replaced, so landing
                            // on a new ratcheted step while still mid-
                            // ratchet from the previous one restarts
                            // cleanly instead of overlapping.
                            if (ratchet > 1)
                            {
                                pendingRatchet.remaining    = ratchet - 1;
                                pendingRatchet.intervalPpq  = seqStepQ / (double) ratchet;
                                pendingRatchet.nextFirePpq  = samplePpq + pendingRatchet.intervalPpq;
                                pendingRatchet.params       = params;
                                pendingRatchet.samplesPerQ  = samplesPerQuarter;
                                pendingRatchet.sceneIdx     = activeSceneForFire;
                                pendingRatchet.subDurationQ = pendingRatchet.intervalPpq;
                                // v0.44 — capture the slice position from the
                                // first fire so sub-fires retrigger the same chop.
                                pendingRatchet.sliceStartPos = voice.retrigStartPos;
                                pendingRatchet.hasSlicePos   = true;
                            }
                            else
                            {
                                pendingRatchet.remaining = 0;
                            }
                        }
                    }

                    // v0.14 — muted steps act as a hard cut: kill all
                    // playing voices so the output goes silent for the
                    // duration of this step, creating dropout gaps.
                    // v0.35 — route through the anti-click fade so
                    // back-to-back muted steps don't click at each boundary.
                    // v0.39 — the mute action is now gated by the
                    // probability roll when a ratio is set. Without a
                    // ratio, mute is absolute (legacy). With a ratio,
                    // the same shouldFire decision that would have
                    // gated a voice fire now gates the mute: a 50%
                    // muted step mutes half the time and lets the
                    // sound through the other half. This is what
                    // "probability on a muted step" actually means in
                    // practice — the probability tells you whether
                    // or not the step mutes.
                    const bool hasRatioForMute = (wrappedStep >= 0) && stepRatio[wrappedStep].load() > 0;
                    const bool muteApplies = stepMuted && (! hasRatioForMute || shouldFire);
                    if (muteApplies)
                    {
                        if (voice.active)
                        {
                            voice.samplesRemaining = juce::jmin (voice.samplesRemaining,
                                                                 PlaybackVoice::kAntiClickSamples);
                            voice.fadingIn = false;
                        }
                        pendingRatchet.remaining = 0;
                    }
                }
                else
                {
                    // Freehand mode — knob-driven, always rolls the
                    // current global chance value.
                    // v0.30 — per-sample chance read for automation.
                    // v0.34 — Freehand counts as "All Steps" per boundary
                    // and "Slice" when the chance roll succeeds. There
                    // is no step/p-lock concept here, so those modes
                    // simply never retrigger in freehand.
                    fireLfoTriggers (lfoActForTrigger(), kEventAllStep);
                    {
                        const float freehandChance = pChance ? pChance->load() : 1.0f;
                        if (rng.nextFloat() < freehandChance)
                        {
                            // v0.41 — freehand fires inherit the currently-
                            // selected Act's engine character (paramsFromKnobs
                            // already reads it; pass the index for consistency).
                            fireVoice (paramsFromKnobs(), samplesPerQuarter, selectedScene.load());
                            fireLfoTriggers (lfoActForTrigger(), kEventSlice);
                        }
                    }
                }
            }

            // v0.12 — drain pending ratchet fires. We check every
            // sample so sub-step fires land at their precise PPQ,
            // independent of the main fireQ boundary tracker.
            while (pendingRatchet.remaining > 0
                   && samplePpq >= pendingRatchet.nextFirePpq)
            {
                fireVoice (pendingRatchet.params, pendingRatchet.samplesPerQ, pendingRatchet.sceneIdx, pendingRatchet.subDurationQ,
                          pendingRatchet.hasSlicePos ? pendingRatchet.sliceStartPos : -1.0);
                // v0.34 — each ratchet sub-hit is a "Slice" for env
                // retrigger purposes, but does NOT count as an active
                // step (that already fired for the parent step above).
                fireLfoTriggers (lfoActForTrigger(), kEventSlice);

                pendingRatchet.nextFirePpq += pendingRatchet.intervalPpq;
                --pendingRatchet.remaining;
            }

            lastPpq = samplePpq;
        }
        else
        {
            // Transport stopped — clear the playhead so the editor
            // doesn't leave a stale "current step" highlight on.
            // Also drop the ratio debounce so the next play start
            // treats every step as a fresh visit.
            currentPlayingStep.store (-1);
            ratioLastSeenSeqStep = -1;
            // Kill any in-flight ratchet so a re-start doesn't dump the
            // tail of a previous step's sub-fires into the new play.
            pendingRatchet = {};
        }

        // 3. Produce the voice's sample (or silence if idle).
        float sliceL = 0.0f, sliceR = 0.0f;
        const bool voiceActive = voice.active;
        // v0.5.0 — freq split state (declared here so recombine can see them).
        float freqSplitDryL = 0.0f, freqSplitDryR = 0.0f;
        int   splitMode = 0;

        if (voiceActive)
        {
            // v0.25 — STRETCH: granular time-stretch with three
            // overlapping Hann-windowed grains spaced 120° apart.
            // Three grains sum to near-constant energy, eliminating
            // the amplitude ripple that two grains produce at high
            // stretch values. Pitch is preserved; only playback
            // duration changes. When stretch = 0 the branch is
            // skipped and we do a plain single-sample read.
            if (voice.voiceStretch > 0.0001f)
            {
                // v0.5.0 — 4-grain granular stretch with interpolated reads.
                // 4 Hann-windowed grains at 90° phase offsets sum to constant
                // energy (2.0), giving smoother overlap than the old 3-grain
                // approach. Grain length ~120ms reduces audible granularity.
                constexpr int nGrains = PlaybackVoice::kNumStretchGrains;  // 4
                const int gl = voice.stretchGrainLen;
                const int quarterGl = gl / nGrains;
                const double normalRate = voice.rate;
                const int iMode = voice.voiceInterpMode;

                // Source cursor advances slower → time stretches.
                // At 100% stretch, source plays at 25% speed → 4x stretch.
                // v0.5.0 — mode-aware DSP branching based on stretchMode
                double stretchFactor;
                switch (voice.voiceStretchMode)
                {
                    default:
                    case 0:  // Standard — original
                        stretchFactor = 1.0 - (double) voice.voiceStretch * 0.75;
                        break;
                    case 1:  // Paulstretch — massive overlap, frozen texture
                        stretchFactor = 1.0 - (double) voice.voiceStretch * 0.95;  // source barely moves
                        break;
                    case 2:  // Spectral — freeze grain positions when stretch > 0.5
                        if (voice.voiceStretch > 0.5f)
                        {
                            stretchFactor = 0.0;  // source stops completely
                        }
                        else
                        {
                            stretchFactor = 1.0 - (double) voice.voiceStretch * 1.5;
                        }
                        break;
                    case 3:  // Formant — shorter grains for vocal clarity
                        stretchFactor = 1.0 - (double) voice.voiceStretch * 0.75;
                        // Note: grain length adjustment happens at fire time
                        break;
                }
                voice.stretchSourcePos += normalRate * stretchFactor;
                if (voice.stretchSourcePos >= (double) circularBufferSize)
                    voice.stretchSourcePos -= (double) circularBufferSize;
                if (voice.stretchSourcePos < 0.0)
                    voice.stretchSourcePos += (double) circularBufferSize;

                // Advance all grain read heads at normal speed
                for (int g = 0; g < nGrains; ++g)
                {
                    voice.stretchGrainPos[g] += normalRate;
                    if (voice.stretchGrainPos[g] >= (double) circularBufferSize)
                        voice.stretchGrainPos[g] -= (double) circularBufferSize;
                    if (voice.stretchGrainPos[g] < 0.0)
                        voice.stretchGrainPos[g] += (double) circularBufferSize;
                }

                // Advance grain counter; reset each grain at its 1/N interval
                voice.stretchGrainCount++;
                for (int g = 0; g < nGrains; ++g)
                {
                    const int resetPoint = (g == 0) ? 0 : quarterGl * g;
                    if ((g == 0 && voice.stretchGrainCount >= gl)
                        || (g != 0 && voice.stretchGrainCount == resetPoint))
                    {
                        if (g == 0) voice.stretchGrainCount -= gl;
                        voice.stretchGrainPos[g] = voice.stretchSourcePos;
                    }
                }

                // Sum grains with Hann windows at 90° offsets + interpolated reads
                const float twoPiF = juce::MathConstants<float>::twoPi;
                sliceL = 0.0f;
                sliceR = 0.0f;
                for (int g = 0; g < nGrains; ++g)
                {
                    const float phase = (float) ((voice.stretchGrainCount + quarterGl * g) % gl) / (float) gl;
                    const float win = 0.5f * (1.0f - std::cos (phase * twoPiF));
                    const float sL = readBufferInterp (0, voice.stretchGrainPos[g], iMode);
                    sliceL += sL * win;
                    if (numChannels > 1)
                        sliceR += readBufferInterp (1, voice.stretchGrainPos[g], iMode) * win;
                }
                // 4 Hann windows at 90° sum to 2.0; normalise to unity.
                sliceL *= 0.5f;
                sliceR = (numChannels > 1) ? (sliceR * 0.5f) : sliceL;
            }
            else
            {
                // v0.30 — interpolated read eliminates ZOH noise at
                // non-unity pitch rates. Negligible CPU overhead.
                // v0.41 — voice carries its source Act's interp mode so
                // a slice keeps its character through Act changes.
                sliceL = readBufferInterp (0, voice.readPos, voice.voiceInterpMode);
                sliceR = (numChannels > 1) ? readBufferInterp (1, voice.readPos, voice.voiceInterpMode) : sliceL;
            }

            // v0.43 — judder crossfade: equal-power blend old grain tail
            // into new grain start to eliminate clicks at retrigger
            // boundaries. Cosine curve avoids the -6 dB mid-dip that
            // a linear crossfade produces on uncorrelated signals
            // (reversed grains, different pitch, etc.).
            // The old grain uses its pre-decay gain level so the
            // crossfade doesn't introduce a sudden level jump.
            if (voice.judderXfadeRemaining > 0)
            {
                const float phase = (float) voice.judderXfadeRemaining
                                  / (float) PlaybackVoice::kJudderXfadeSamples;
                // phase goes from 1→0. Equal-power: cos/sin pair.
                const float xfOld = std::cos ((1.0f - phase)
                                    * juce::MathConstants<float>::halfPi);
                const float xfNew = std::sin ((1.0f - phase)
                                    * juce::MathConstants<float>::halfPi);
                float oldL = readBufferInterp (0, voice.judderOldReadPos, voice.voiceInterpMode);
                float oldR = (numChannels > 1)
                    ? readBufferInterp (1, voice.judderOldReadPos, voice.voiceInterpMode) : oldL;
                // Compensate for the gain difference: old grain was at
                // judderOldGain, new grain is at currentGain. Scale old
                // samples so the blend doesn't jump in level.
                if (voice.currentGain > 0.0001f)
                {
                    const float gainComp = voice.judderOldGain / voice.currentGain;
                    oldL *= gainComp;
                    oldR *= gainComp;
                }
                sliceL = sliceL * xfNew + oldL * xfOld;
                sliceR = sliceR * xfNew + oldR * xfOld;
                voice.judderOldReadPos += voice.judderOldRate;
                if (voice.judderOldReadPos >= (double) circularBufferSize)
                    voice.judderOldReadPos -= (double) circularBufferSize;
                if (voice.judderOldReadPos < 0.0)
                    voice.judderOldReadPos += (double) circularBufferSize;
                voice.judderXfadeRemaining--;
            }

            // --- v0.6.0 Frequency split (per-Act) ----------------------------
            // When Low Only or High Only is active, a cascaded two-pole
            // (12 dB/oct) crossover splits the signal BEFORE any FX so
            // that crunch, drive, fold, tape, shimmer, stutter, chaos —
            // everything — only processes the chosen band. The excluded
            // band is stashed in freqSplitDryL/R and recombined after
            // all FX, keeping it pristine. This lets you keep kicks
            // clean while mangling everything above the crossover, etc.
            splitMode = voice.voiceFreqSplitMode;
            if (splitMode != 0)
            {
                const float xHz  = voice.voiceFreqSplitHz;
                const float xCoeff = 1.0f - std::exp (-juce::MathConstants<float>::twoPi
                                                       * xHz / (float) sampleRate);
                // First LP stage
                voice.xoverLpL += xCoeff * (sliceL - voice.xoverLpL);
                voice.xoverLpR += xCoeff * (sliceR - voice.xoverLpR);
                // Second LP stage (cascaded → 12 dB/oct)
                voice.xoverHpL += xCoeff * (voice.xoverLpL - voice.xoverHpL);
                voice.xoverHpR += xCoeff * (voice.xoverLpR - voice.xoverHpR);

                if (splitMode == 1) // Low Only — process lows, keep highs clean
                {
                    freqSplitDryL = sliceL - voice.xoverHpL;
                    freqSplitDryR = sliceR - voice.xoverHpR;
                    sliceL = voice.xoverHpL;
                    sliceR = voice.xoverHpR;
                }
                else // High Only — process highs, keep lows clean
                {
                    freqSplitDryL = voice.xoverHpL;
                    freqSplitDryR = voice.xoverHpR;
                    sliceL = sliceL - voice.xoverHpL;
                    sliceR = sliceR - voice.xoverHpR;
                }
            }

            // v0.22 — skip per-voice crush when "All audio" mode is on
            // (the master output path handles it instead).
            // v0.41 — also use voice's baked AA/Mu flags so per-Act crush
            // character holds steady for the slice's full lifetime.
            if (! gfxCrush)
                applyCrunch (sliceL, sliceR, voice.voiceCrunch, voice.voiceCrushRate, voice.crushState,
                             voice.voiceCrushAA, voice.voiceCrushMu);

            // --- FX row --------------------------------------------------
            // DRIVE: multi-mode saturation. At 0 we return the signal untouched.
            // v0.30 — skip per-voice when global toggle is on.
            // v0.5.0 — mode-aware branching based on voiceDriveType.
            if (voice.voiceDrive > 0.0001f && ! gfxDrive)
            {
                const float d = voice.voiceDrive;
                const float g = 1.0f + d * 5.0f;
                const float wet = d;
                float satL, satR;

                switch (voice.voiceDriveType)
                {
                    default:
                    case 0:  // Tape — symmetric soft-clip (tanh)
                        satL = std::tanh (sliceL * g);
                        satR = std::tanh (sliceR * g);
                        break;
                    case 1:  // Tube — asymmetric even harmonics (shifted tanh)
                    {
                        const float bias = 0.2f * d;
                        satL = std::tanh ((sliceL + bias) * g) - std::tanh (bias * g);
                        satR = std::tanh ((sliceR + bias) * g) - std::tanh (bias * g);
                        break;
                    }
                    case 2:  // Diode — hard clip, odd harmonics
                        satL = juce::jlimit (-1.0f, 1.0f, sliceL * g);
                        satR = juce::jlimit (-1.0f, 1.0f, sliceR * g);
                        break;
                    case 3:  // Fuzz — extreme asymmetric (positive crushed, negative clipped)
                    {
                        const float fuzzL = sliceL * g;
                        const float fuzzR = sliceR * g;
                        satL = fuzzL > 0.0f ? std::tanh (fuzzL * 3.0f) : juce::jlimit (-1.0f, 0.0f, fuzzL);
                        satR = fuzzR > 0.0f ? std::tanh (fuzzR * 3.0f) : juce::jlimit (-1.0f, 0.0f, fuzzR);
                        break;
                    }
                }
                sliceL = (1.0f - wet) * sliceL + wet * satL;
                sliceR = (1.0f - wet) * sliceR + wet * satR;
            }

            // TONE + RES filter (v0.24 / v0.5.0 multi-mode).
            //
            // v0.5.0 — four selectable modes per-Act:
            //   Tilt (legacy):  0.5=bypass, <0.5=LP dark, >0.5=HF bright
            //   LP:  Tone 0→1 maps cutoff 20 Hz→20 kHz, Res = resonance
            //   HP:  same range, high-pass output
            //   BP:  same range, bandpass output
            //
            // All modes use the same Chamberlin SVF with soft-clipped
            // feedback; we just pick which output (low/band/high) to use.
            //
            // v0.30 — skip per-voice when global toggle is on.
            if (! gfxTone)
            {
                const int fMode = voice.voiceFilterMode;

                auto softClip = [] (float x) -> float
                {
                    const float x2 = x * x;
                    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
                };

                if (fMode == loopsab::kFilterTilt)
                {
                    // --- legacy Tilt mode (unchanged behaviour) ---
                    if (voice.voiceTone < 0.499f)
                    {
                        const float toneNorm = voice.voiceTone * 2.0f;
                        const double cutoffHz = juce::jlimit (
                            30.0, sampleRate * 0.45,
                            350.0 * std::pow (60.0, (double) toneNorm));
                        const double fRaw = 2.0 * std::sin (juce::MathConstants<double>::pi
                                                             * cutoffHz / sampleRate);
                        const float f = (float) juce::jlimit (0.0, 0.48, fRaw);
                        const float q = juce::jmax (0.005f, 1.0f - voice.voiceRes);

                        {
                            const float bp = softClip (voice.svfBandL);
                            const float high = sliceL - voice.svfLowL - q * bp;
                            voice.svfBandL += f * high;
                            voice.svfLowL  += f * softClip (voice.svfBandL);
                            sliceL = voice.svfLowL;
                        }
                        {
                            const float bp = softClip (voice.svfBandR);
                            const float high = sliceR - voice.svfLowR - q * bp;
                            voice.svfBandR += f * high;
                            voice.svfLowR  += f * softClip (voice.svfBandR);
                            sliceR = voice.svfLowR;
                        }
                    }
                    else if (voice.voiceTone > 0.501f)
                    {
                        const float airAmt = (voice.voiceTone - 0.5f) * 2.0f;
                        const float hpCoeff = 0.7f;
                        voice.svfLowL += hpCoeff * (sliceL - voice.svfLowL);
                        voice.svfLowR += hpCoeff * (sliceR - voice.svfLowR);
                        const float hfL = sliceL - voice.svfLowL;
                        const float hfR = sliceR - voice.svfLowR;
                        sliceL += hfL * airAmt * 3.0f;
                        sliceR += hfR * airAmt * 3.0f;
                    }
                }
                else
                {
                    // --- LP / HP / BP: full-range resonant SVF ---
                    // Tone 0→1 maps cutoff exponentially 20 Hz → Nyquist*0.45
                    const double cutoffHz = juce::jlimit (
                        20.0, sampleRate * 0.45,
                        20.0 * std::pow (1000.0, (double) voice.voiceTone));
                    const double fRaw = 2.0 * std::sin (juce::MathConstants<double>::pi
                                                         * cutoffHz / sampleRate);
                    const float f = (float) juce::jlimit (0.0, 0.48, fRaw);
                    // Res 0→1 maps damping 1→0.005 (higher res = sharper peak)
                    const float q = juce::jmax (0.005f, 1.0f - voice.voiceRes * 0.995f);

                    // Left channel SVF tick
                    {
                        const float bp = softClip (voice.svfBandL);
                        const float high = sliceL - voice.svfLowL - q * bp;
                        voice.svfBandL += f * high;
                        voice.svfLowL  += f * softClip (voice.svfBandL);
                    }
                    // Right channel SVF tick
                    {
                        const float bp = softClip (voice.svfBandR);
                        const float high = sliceR - voice.svfLowR - q * bp;
                        voice.svfBandR += f * high;
                        voice.svfLowR  += f * softClip (voice.svfBandR);
                    }

                    // Pick the output based on mode
                    switch (fMode)
                    {
                        default:
                        case loopsab::kFilterLP:
                            sliceL = voice.svfLowL;
                            sliceR = voice.svfLowR;
                            break;
                        case loopsab::kFilterHP:
                            sliceL = sliceL - voice.svfLowL - q * softClip (voice.svfBandL);
                            sliceR = sliceR - voice.svfLowR - q * softClip (voice.svfBandR);
                            break;
                        case loopsab::kFilterBP:
                            sliceL = voice.svfBandL;
                            sliceR = voice.svfBandR;
                            break;
                    }
                }
            }

            // FOLD: multi-mode wavefolder. At 0 = untouched; at 1 you get
            // metallic harmonics as the signal keeps folding back on
            // itself. Blended with the dry so knob=1 isn't a full wipe.
            // v0.30 — skip per-voice when global toggle is on.
            // v0.5.0 — mode-aware branching based on voiceFoldTopology.
            if (voice.voiceFold > 0.0001f && ! gfxFold)
            {
                const float fd = voice.voiceFold;
                const float drive = 1.0f + fd * 6.0f;
                float foldedL, foldedR;

                switch (voice.voiceFoldTopology)
                {
                    default:
                    case 0:  // Sine fold — smooth
                        foldedL = std::sin (sliceL * drive * juce::MathConstants<float>::halfPi);
                        foldedR = std::sin (sliceR * drive * juce::MathConstants<float>::halfPi);
                        break;
                    case 1:  // Triangle fold — sharper, metallic
                    {
                        auto triFold = [] (float x) {
                            // Map to triangle wave: 4 * |((x/4 - floor(x/4 + 0.5))| - 1
                            const float p = x * 0.25f;
                            return 4.0f * std::abs (p - std::floor (p + 0.5f)) - 1.0f;
                        };
                        foldedL = triFold (sliceL * drive);
                        foldedR = triFold (sliceR * drive);
                        break;
                    }
                    case 2:  // Asymmetric — different fold on +/- half
                    {
                        auto asymFold = [] (float x, float d) {
                            if (x >= 0.0f)
                                return std::sin (x * d * juce::MathConstants<float>::halfPi);
                            else
                                return -std::sin (-x * d * 0.7f * juce::MathConstants<float>::halfPi);
                        };
                        foldedL = asymFold (sliceL, drive);
                        foldedR = asymFold (sliceR, drive);
                        break;
                    }
                }
                sliceL = (1.0f - fd) * sliceL + fd * foldedL;
                sliceR = (1.0f - fd) * sliceR + fd * foldedR;
            }

            // RING MOD: multiply the signal by a sine oscillator for
            // metallic, inharmonic textures. The knob controls both
            // depth and frequency — low values give a subtle tremolo,
            // higher values push into atonal bell/metal territory.
            // Frequency range: 20 Hz (knob low) to 2 kHz (knob high).
            // v0.30 — skip per-voice when global toggle is on.
            if (voice.voiceRingMod > 0.0001f && ! gfxRingMod)
            {
                const float rm = voice.voiceRingMod;
                // Exponential frequency mapping: 20 Hz to 2000 Hz
                double rmFreq = 20.0 * std::pow (100.0, (double) rm);

                // v0.25 — optional quantise to selected scale. Convert
                // the free Hz to the nearest MIDI note in the scale,
                // then back to Hz so the ring mod is tonal.
                // v0.42 — now baked per-voice at fire time.
                if (voice.voiceRingModQuant)
                {
                    const int root     = (int) std::round (pScaleRoot->load());
                    const int scaleIdx = (int) std::round (pScaleType->load());
                    const float midiNote = 69.0f + 12.0f * std::log2f ((float) rmFreq / 440.0f);
                    const float snapped  = snapSemitonesToScale (midiNote, root, scaleIdx);
                    rmFreq = 440.0 * std::pow (2.0, ((double) snapped - 69.0) / 12.0);
                }

                voice.ringModPhase += rmFreq * juce::MathConstants<double>::twoPi / sampleRate;
                if (voice.ringModPhase > juce::MathConstants<double>::twoPi)
                    voice.ringModPhase -= juce::MathConstants<double>::twoPi;
                float mod;
                switch (voice.voiceRingModWave)
                {
                    default:
                    case 0:  // Sine — classic
                        mod = (float) std::sin (voice.ringModPhase);
                        break;
                    case 1:  // Square — octave-down harshness
                        mod = (voice.ringModPhase < juce::MathConstants<double>::pi) ? 1.0f : -1.0f;
                        break;
                    case 2:  // Triangle — mellower
                    {
                        const double norm = voice.ringModPhase / juce::MathConstants<double>::twoPi;
                        mod = (float) (4.0 * std::abs (norm - 0.5) - 1.0);
                        break;
                    }
                    case 3:  // Saw — asymmetric edge
                    {
                        const double norm = voice.ringModPhase / juce::MathConstants<double>::twoPi;
                        mod = (float) (2.0 * norm - 1.0);
                        break;
                    }
                }
                // Wet/dry blend: full knob = fully ring-modded
                sliceL = (1.0f - rm) * sliceL + rm * sliceL * mod;
                sliceR = (1.0f - rm) * sliceR + rm * sliceR * mod;
            }

            // SHIMMER: aliased-residual bit-crush. We hold every other
            // sample to introduce a very specific quant error, then
            // take the difference (hf residual) and sprinkle it back
            // on top of the clean signal, scaled by the knob. Result
            // is a glistening HF sparkle that sits above the original
            // without muting anything the way CRUNCH does.
            // v0.5.0 — mode-aware DSP branching based on voiceShimmerOctave.
            if (voice.voiceShimmer > 0.0001f)
            {
                int period;
                switch (voice.voiceShimmerOctave)
                {
                    default:
                    case 0: period = 2; break;   // +1 Oct (every 2nd sample)
                    case 1: period = 4; break;   // +2 Oct (every 4th sample)
                    case 2: period = 2; break;   // -1 Oct (hold for 2, see below)
                    case 3: period = 3; break;   // +5th (every 3rd sample)
                    case 4: period = 2; break;   // +5th/Oct (dual, handled below)
                }

                if (voice.voiceShimmerOctave == 2)
                {
                    // -1 Oct: hold each sample for 2 ticks (repeat, creating subharmonic)
                    if ((voice.shimmerCounter % 2) == 0)
                    {
                        voice.shimmerHeldL = sliceL;
                        voice.shimmerHeldR = sliceR;
                    }
                    // Use held value directly (replayed = octave-down residual)
                    const float sh = voice.voiceShimmer;
                    const float diffL = voice.shimmerHeldL - sliceL;
                    const float diffR = voice.shimmerHeldR - sliceR;
                    sliceL += diffL * sh * 2.5f;
                    sliceR += diffR * sh * 2.5f;
                }
                else if (voice.voiceShimmerOctave == 4)
                {
                    // +5th/Oct dual: mix of period-3 (fifth) and period-2 (octave) residuals
                    const float sh = voice.voiceShimmer;
                    float aliasL = 0.0f, aliasR = 0.0f;

                    if ((voice.shimmerCounter % 2) == 0)
                    {
                        voice.shimmerHeldL = sliceL;
                        voice.shimmerHeldR = sliceR;
                    }
                    aliasL += (sliceL - voice.shimmerHeldL) * 0.5f;
                    aliasR += (sliceR - voice.shimmerHeldR) * 0.5f;

                    // We need a second held pair for the fifth. Use shimmerHeld2 if it exists,
                    // or reuse shimmerHeld with a different modulo. Check if voice has a second
                    // held pair — if not, just alternate the residual extraction.
                    if ((voice.shimmerCounter % 3) == 0)
                    {
                        aliasL += (sliceL - voice.shimmerHeldL) * 0.5f;
                        aliasR += (sliceR - voice.shimmerHeldR) * 0.5f;
                    }

                    sliceL += aliasL * sh * 2.5f;
                    sliceR += aliasR * sh * 2.5f;
                }
                else
                {
                    // Standard: sample at period, extract aliased residual
                    if ((voice.shimmerCounter % period) == 0)
                    {
                        voice.shimmerHeldL = sliceL;
                        voice.shimmerHeldR = sliceR;
                    }
                    const float sh = voice.voiceShimmer;
                    const float aliasL = sliceL - voice.shimmerHeldL;
                    const float aliasR = sliceR - voice.shimmerHeldR;
                    sliceL += aliasL * sh * 2.5f;
                    sliceR += aliasR * sh * 2.5f;
                }
                voice.shimmerCounter++;
            }

            // SHAPE — bipolar transient control (v0.5.0, was GATE).
            //
            // Negative (Soft): Buchla-style LPG. Attack softening via
            // exponential decay envelope + cascaded LP filter. Produces
            // squishy vactrol feel; darker as amplitude decays.
            //
            // Positive (Snappy): Transient emphasis via one-shot envelope
            // that boosts the first few ms of each slice, then decays to
            // unity. Makes attacks pop and punch through.
            //
            // Zero = bypass (no processing).
            if (voice.voiceGate < -0.0001f)
            {
                // --- Soft (LPG) side: same Buchla 292 behaviour as old Gate ---
                const float depth = -voice.voiceGate;  // 0..1

                const float decayMs = juce::jlimit (8.0f, 300.0f,
                    300.0f * (1.0f - depth * 0.95f));
                const float decayCoeff = std::exp (-1.0f / (float) (sampleRate * decayMs * 0.001f));

                voice.gateEnvelope *= decayCoeff;
                const float env = voice.gateEnvelope;

                const float lpgCoeff = juce::jlimit (0.003f, 1.0f,
                    env * env * env);

                voice.gateLpgL  += lpgCoeff * (sliceL - voice.gateLpgL);
                voice.gateLpgR  += lpgCoeff * (sliceR - voice.gateLpgR);
                voice.gateLpg2L += lpgCoeff * (voice.gateLpgL - voice.gateLpg2L);
                voice.gateLpg2R += lpgCoeff * (voice.gateLpgR - voice.gateLpg2R);

                const float wet = depth;
                const float dry = 1.0f - wet;
                sliceL = dry * sliceL + wet * voice.gateLpg2L * env;
                sliceR = dry * sliceR + wet * voice.gateLpg2R * env;
            }
            else if (voice.voiceGate > 0.0001f)
            {
                // --- Snappy (transient emphasis) side ---
                // One-shot envelope starts at (1 + boost) and decays to 1.0.
                // The boost magnitude scales with knob position: +1 at 50%,
                // +3 at 100%. Attack time is very short (~1ms) so we're
                // essentially boosting the first few samples.
                const float depth = voice.voiceGate;  // 0..1
                const float maxBoost = 1.0f + depth * 3.0f;

                // Envelope decays from maxBoost toward 1.0 with a fast
                // time constant (~3ms at depth=0.5, ~1ms at depth=1).
                const float decayMs = juce::jlimit (0.5f, 8.0f,
                    8.0f * (1.0f - depth * 0.9f));
                const float decayCoeff = std::exp (-1.0f / (float) (sampleRate * decayMs * 0.001f));

                // gateEnvelope starts at 1.0 (reset on each fire/judder).
                // We use it as the "boost envelope" that fades 1→0.
                voice.gateEnvelope *= decayCoeff;
                const float envGain = 1.0f + voice.gateEnvelope * (maxBoost - 1.0f);

                sliceL *= envGain;
                sliceR *= envGain;
            }

            // STUTTER: micro-loop buffer repeat. Captures a tiny window
            // of the processed slice and loops it for a glitch-hop
            // buffer-freeze effect. Higher knob values = smaller loop
            // window = faster, buzzier repeats.
            //
            // v0.23 — gentler curve so the knob stays musical across
            // the full range. 0% ~ 90ms, 50% ~ 20ms, 100% ~ 4ms.
            // A short crossfade at the loop boundary kills the click.
            if (voice.voiceStutter > 0.0001f)
            {
                if (voice.stutterLoopLen == 0)
                {
                    // Quadratic curve: most of the travel is in the
                    // musically useful 10-90ms range.
                    const float t = voice.voiceStutter;
                    const float minLen = (float) sampleRate * 0.004f;  // 4ms floor
                    const float maxLen = (float) juce::jmin (
                        (int) (sampleRate * 0.093),
                        PlaybackVoice::kStutterBufSize - 1);
                    const float range = maxLen - minLen;
                    voice.stutterLoopLen = juce::jmax (
                        (int) minLen,
                        (int) (maxLen - range * t * t));
                    voice.stutterWritePos  = 0;
                    voice.stutterReadPos   = 0;
                    voice.stutterCaptured  = false;
                }

                if (! voice.stutterCaptured)
                {
                    // Capture phase: write incoming audio into the
                    // micro-loop buffer until we've filled the window.
                    voice.stutterBuf[0][voice.stutterWritePos] = sliceL;
                    voice.stutterBuf[1][voice.stutterWritePos] = sliceR;
                    if (++voice.stutterWritePos >= voice.stutterLoopLen)
                        voice.stutterCaptured = true;
                }
                else
                {
                    // Playback phase: loop the captured buffer with a
                    // short crossfade (last 32 samples) to kill clicks
                    // at the loop seam.
                    const int fadeLen = juce::jmin (32, voice.stutterLoopLen / 4);
                    const int distToEnd = voice.stutterLoopLen - voice.stutterReadPos;

                    float sL = voice.stutterBuf[0][voice.stutterReadPos];
                    float sR = voice.stutterBuf[1][voice.stutterReadPos];

                    if (fadeLen > 1 && distToEnd <= fadeLen)
                    {
                        // Crossfade: blend current position with the
                        // start of the loop so the seam is inaudible.
                        const float fade = (float) distToEnd / (float) fadeLen;
                        const int   wrapPos = fadeLen - distToEnd;
                        sL = sL * fade + voice.stutterBuf[0][wrapPos] * (1.0f - fade);
                        sR = sR * fade + voice.stutterBuf[1][wrapPos] * (1.0f - fade);
                    }

                    sliceL = sL;
                    sliceR = sR;
                    if (++voice.stutterReadPos >= voice.stutterLoopLen)
                    {
                        voice.stutterReadPos = 0;

                        // v0.5.0 — stutter window modes
                        if (voice.voiceStutterWindow == 1 && voice.totalSamples > 0)
                        {
                            // Decaying: shrink loop by ~10% each cycle (min 64 samples)
                            voice.stutterLoopLen = juce::jmax (64, (int)(voice.stutterLoopLen * 0.88f));
                        }
                        else if (voice.voiceStutterWindow == 2 && voice.totalSamples > 0)
                        {
                            // Growing: grow loop by ~15% each cycle (max = buffer size - 1)
                            voice.stutterLoopLen = juce::jmin (PlaybackVoice::kStutterBufSize - 1,
                                                               (int)(voice.stutterLoopLen * 1.18f));
                        }
                    }
                }
            }

            // CHAOS: per-sample glitch roll. At low values you get
            // occasional clicks and holds; at high values the slice
            // disintegrates into tracker-broken static. Deliberately
            // destructive — pair with low MIX to sprinkle rather than
            // bury.
            if (voice.voiceChaos > 0.0001f)
            {
                const float chaosAmt = voice.voiceChaos;

                switch (voice.voiceChaosDistribution)
                {
                    default:
                    case 0:  // Uniform — original behaviour
                    {
                        const float rollThresh = chaosAmt * chaosAmt * 0.25f;
                        if (rng.nextFloat() < rollThresh)
                        {
                            const int action = rng.nextInt (4);
                            if (action == 0)
                            {
                                sliceL = voice.chaosHeldL;
                                sliceR = voice.chaosHeldR;
                            }
                            else if (action == 1)
                            {
                                sliceL = 0.0f;
                                sliceR = 0.0f;
                            }
                            else if (action == 2)
                            {
                                sliceL = -sliceL;
                                sliceR = -sliceR;
                            }
                            else
                            {
                                voice.readPos += (double) (rng.nextInt (32) - 16);
                            }
                        }
                        break;
                    }
                    case 1:  // Gaussian — clustered centre, occasional wild spikes
                    {
                        // Box-Muller: generate gaussian from two uniforms
                        const float u1 = juce::jmax (0.0001f, rng.nextFloat());
                        const float u2 = rng.nextFloat();
                        const float gauss = std::sqrt (-2.0f * std::log (u1)) * std::cos (juce::MathConstants<float>::twoPi * u2);
                        const float rollThresh = chaosAmt * chaosAmt * 0.15f;
                        if (std::abs (gauss) * rollThresh > rng.nextFloat())
                        {
                            // Higher gauss values = wilder action
                            if (std::abs (gauss) > 2.0f)
                                voice.readPos += (double) (rng.nextInt (64) - 32);
                            else if (std::abs (gauss) > 1.0f)
                                { sliceL = -sliceL; sliceR = -sliceR; }
                            else
                                { sliceL = voice.chaosHeldL; sliceR = voice.chaosHeldR; }
                        }
                        break;
                    }
                    case 2:  // Drunk Walk — Brownian motion, smooth wandering
                    {
                        const float step = (rng.nextFloat() - 0.5f) * chaosAmt * 0.1f;
                        voice.chaosDrunkValue = juce::jlimit (-1.0f, 1.0f, voice.chaosDrunkValue + step);
                        const float drunk = voice.chaosDrunkValue;
                        // Apply as a smooth pitch/gain modulation rather than glitchy actions
                        const float gainMod = 1.0f + drunk * chaosAmt * 0.5f;
                        sliceL *= gainMod;
                        sliceR *= gainMod;
                        // Subtle read position drift
                        voice.readPos += (double) drunk * (double) chaosAmt * 0.5;
                        break;
                    }
                    case 3:  // Bipolar Snap — hard ±1 only, dramatic on/off glitching
                    {
                        const float rollThresh = chaosAmt * chaosAmt * 0.3f;
                        if (rng.nextFloat() < rollThresh)
                        {
                            // Binary: either full mute or full polarity flip
                            if (rng.nextBool())
                                { sliceL = 0.0f; sliceR = 0.0f; }
                            else
                                { sliceL = -sliceL; sliceR = -sliceR; }
                        }
                        break;
                    }
                }
                voice.chaosHeldL = sliceL;
                voice.chaosHeldR = sliceR;
            }

            // Apply per-repeat decay tail. currentGain only changes on
            // judder retrigger boundaries, so within a single sub-hit
            // the gain is constant — which gives the discrete
            // tape-echo-step feel rather than a smooth envelope.
            //
            // v0.6.0 — Decay curve shapes the overall voice envelope:
            //   Linear (0):      default — flat gain per judder step
            //   Exponential (1): faster initial drop, slower tail
            //   Logarithmic (2): sustains then drops sharply at end
            //   Gate (3):        full volume, instant cut (no fade)
            {
                float envGain = voice.currentGain;
                if (voice.voiceDecayCurve != 0 && voice.totalSamples > 0)
                {
                    const float progress = 1.0f - (float) voice.samplesRemaining
                                                  / (float) voice.totalSamples;
                    switch (voice.voiceDecayCurve)
                    {
                        case 1:  // Exponential — fast drop, slow tail
                        {
                            // Reshape: gain = currentGain * (1 - progress)^2
                            const float curve = (1.0f - progress) * (1.0f - progress);
                            envGain = voice.currentGain * curve;
                            break;
                        }
                        case 2:  // Logarithmic — sustains then drops
                        {
                            // Reshape: gain = currentGain * sqrt(1 - progress)
                            const float curve = std::sqrt (juce::jmax (0.0f, 1.0f - progress));
                            envGain = voice.currentGain * curve;
                            break;
                        }
                        case 3:  // Gate — full volume, no fade
                        {
                            envGain = 1.0f;
                            break;
                        }
                        default:
                            break;
                    }
                }
                sliceL *= envGain;
                sliceR *= envGain;
            }

            // v0.30 — anti-click micro-fade. Ramps 0→1 over
            // kAntiClickSamples at voice start, and 1→0 at voice end,
            // avoiding the hard discontinuity that causes clicks at
            // slice boundaries and on voice-steal.
            {
                constexpr float step = 1.0f / (float) PlaybackVoice::kAntiClickSamples;
                if (voice.fadingIn)
                {
                    voice.fadeGain += step;
                    if (voice.fadeGain >= 1.0f)
                    {
                        voice.fadeGain = 1.0f;
                        voice.fadingIn = false;
                    }
                }
                // Fade-out near the end of the voice's lifetime.
                if (voice.samplesRemaining <= PlaybackVoice::kAntiClickSamples)
                    voice.fadeGain = (float) voice.samplesRemaining * step;

                sliceL *= voice.fadeGain;
                sliceR *= voice.fadeGain;
            }

            // WOW/FLUTTER: accumulate a per-sample rate deviation on
            // top of the baked voice.rate. Phases advance in
            // output-sample time (not source-sample time) so the
            // wobble stays at a musical frequency regardless of how
            // pitched or reversed the voice is.
            // TAPE — combined wow (slow drift) + flutter (fast wobble).
            // v0.24 — range scaled so full knob = ~55% of original max.
            // Low values give gentle wow drift suitable for pads; higher
            // values layer flutter on top. No hiss — that's gone.
            double rateMod = 1.0;
            if (voice.voiceTape > 0.0001f)
            {
                // v0.7.0 — squared curve so low knob values are much more
                // subtle. At 10% knob the deviation is now ~10x gentler than
                // before; full clockwise is unchanged.
                const float t = voice.voiceTape * voice.voiceTape * 0.75f;

                // Mode-aware wow/flutter blend
                float wowAmount = 1.0f;       // multiplier on wow contribution
                float flutterGate = 0.3f;     // knob threshold where flutter kicks in
                float wowDepth = 0.03;        // wow deviation at max
                float flutterDepth = 0.12;    // flutter deviation at max

                switch (voice.voiceTapeMode)
                {
                    default:
                    case 0:  // Classic — default blend
                        break;
                    case 1:  // Wow Only — no flutter
                        flutterGate = 99.0f;  // flutter never kicks in
                        wowDepth = 0.045;     // slightly wider wow range
                        break;
                    case 2:  // Flutter Only — no wow
                        wowAmount = 0.0f;
                        flutterGate = 0.0f;   // flutter from zero
                        flutterDepth = 0.15;
                        break;
                    case 3:  // Extreme — both maxed with wider deviation
                        wowDepth = 0.06;
                        flutterGate = 0.1f;
                        flutterDepth = 0.25;
                        break;
                }

                // Slow drift (wow)
                if (wowAmount > 0.0f)
                {
                    const double wowHz = 0.7;
                    voice.tapeWowPhase += wowHz * juce::MathConstants<double>::twoPi / sampleRate;
                    if (voice.tapeWowPhase > juce::MathConstants<double>::twoPi)
                        voice.tapeWowPhase -= juce::MathConstants<double>::twoPi;
                    rateMod += std::sin (voice.tapeWowPhase) * (double) t * wowDepth * (double) wowAmount;
                }

                // Fast wobble (flutter)
                const float flutterAmt = juce::jmax (0.0f, (t - flutterGate) / 0.25f);
                if (flutterAmt > 0.0001f)
                {
                    const double flutterHz = 7.0;
                    voice.tapeFlutterPhase += flutterHz * juce::MathConstants<double>::twoPi / sampleRate;
                    if (voice.tapeFlutterPhase > juce::MathConstants<double>::twoPi)
                        voice.tapeFlutterPhase -= juce::MathConstants<double>::twoPi;
                    rateMod += std::sin (voice.tapeFlutterPhase) * (double) flutterAmt * flutterDepth;
                }
            }

            // VARISPEED: mode-aware tempo modulation. The slice starts at full
            // speed and varies according to the selected curve. The knob
            // controls the depth: low = subtle slowdown, high = full
            // stop by the end. Negative rate (reversed) is preserved.
            // v0.5.0 — mode-aware branching based on voiceVarispeedCurve.
            if (voice.voiceVarispeed > 0.0001f && voice.totalSamples > 0)
            {
                // Progress through the slice: 0 = start, 1 = end
                const float progress = 1.0f - (float) voice.samplesRemaining
                                              / (float) voice.totalSamples;
                const float vs = voice.voiceVarispeed;
                float curve;

                switch (voice.voiceVarispeedCurve)
                {
                    default:
                    case 0:  // Linear — constant deceleration
                        curve = progress;
                        break;
                    case 1:  // Exponential — DJ brake (fast then crawl)
                        curve = progress * progress;
                        break;
                    case 2:  // Sudden — full speed then drop at end
                        curve = progress < 0.8f ? 0.0f : (progress - 0.8f) * 5.0f;
                        break;
                }
                const float speedMult = 1.0f - vs * curve;
                rateMod *= (double) juce::jmax (0.01f, speedMult);
            }

            // Advance read position, including tape + varispeed deviation.
            // When stretch is active, the grain engine handles its own
            // source position internally — but we still advance readPos
            // so feedback / judder retrig use a sensible location.
            if (voice.voiceStretch > 0.0001f)
            {
                voice.readPos = voice.stretchSourcePos;
            }
            else
            {
                voice.readPos += voice.rate * rateMod;
                if (voice.readPos >= (double) circularBufferSize)
                    voice.readPos -= (double) circularBufferSize;
                if (voice.readPos < 0.0)
                    voice.readPos += (double) circularBufferSize;
            }

            voice.crushState.sampleCounter++;

            // Judder: when the countdown hits zero, jump back to the
            // slice start, apply the per-judder Slide, and refill.
            // v0.43 — crossfade: save old readPos/rate so we can blend
            // old tail → new start over kAntiClickSamples.
            if (voice.juddersRemaining > 0)
            {
                if (--voice.judderCountdown <= 0)
                {
                    voice.judderOldReadPos     = voice.readPos;
                    voice.judderOldRate        = voice.rate;
                    voice.judderOldGain        = voice.currentGain;
                    voice.judderXfadeRemaining = PlaybackVoice::kJudderXfadeSamples;

                    voice.readPos         = voice.retrigStartPos;

                    // SLIDE: mode-aware curve applied on each judder retrig.
                    // v0.5.0 — voiceSlideCurve shapes how the slide pitch bend
                    // evolves across judder hits.
                    if (voice.juddersRemaining > 1)
                    {
                        // Progress through the judder sequence (not the audio):
                        // 0 = first judder, 1 = last judder
                        const int totalJudders = (int) voice.juddersRemaining;  // remaining + 1 for current
                        const int judderIdx = totalJudders - (int) voice.juddersRemaining;
                        const float progress = (totalJudders > 1)
                            ? (float) judderIdx / (float) (totalJudders - 1)
                            : 0.0f;

                        float shaped;
                        switch (voice.voiceSlideCurve)
                        {
                            default:
                            case 0:  // Linear — consistent slide per judder
                                shaped = progress;
                                break;
                            case 1:  // Exponential — accelerating slide
                                shaped = progress * progress;
                                break;
                            case 2:  // Log — decelerating slide (front-loaded)
                                shaped = 1.0f - (1.0f - progress) * (1.0f - progress);
                                break;
                            case 3:  // S-Curve — slow-fast-slow envelope
                                shaped = progress < 0.5f
                                       ? 2.0f * progress * progress
                                       : 1.0f - 2.0f * (1.0f - progress) * (1.0f - progress);
                                break;
                        }
                        // Interpolate between 1.0 (start) and slideFactor (end)
                        const double slideNow = 1.0 + (voice.slideFactor - 1.0) * (double) shaped;
                        voice.rate *= slideNow;
                    }
                    else
                    {
                        // Single judder, apply full slide
                        voice.rate *= voice.slideFactor;
                    }

                    voice.currentGain    *= voice.decayPerJudder;
                    voice.gateEnvelope    = 1.0f;  // v0.22 — LPG re-ping on each judder hit

                    // v0.5.0 — Judder shape determines next interval
                    switch (voice.voiceJudderShape)
                    {
                        default:
                        case 0:  // Even — fixed interval
                            voice.judderCountdown = voice.judderIntervalSamples;
                            break;
                        case 1:  // Accelerating — intervals shrink
                        {
                            const int total = voice.totalSamples;
                            const int remaining = voice.juddersRemaining;
                            // Progressive shortening: each interval is shorter than the last
                            const float ratio = (remaining > 0) ? (float) remaining / (float) (remaining + 1) : 0.5f;
                            voice.judderCountdown = juce::jmax (64, (int) (voice.judderIntervalSamples * ratio * 1.5f));
                            break;
                        }
                        case 2:  // Decelerating — intervals grow (bouncing ball)
                        {
                            const int remaining = voice.juddersRemaining;
                            const float ratio = (remaining > 0) ? (float) (remaining + 2) / (float) (remaining + 1) : 2.0f;
                            voice.judderCountdown = juce::jmax (64, (int) (voice.judderIntervalSamples * ratio * 0.7f));
                            break;
                        }
                        case 3:  // Random — jittered timing
                        {
                            const float jitter = 0.5f + rng.nextFloat();  // 0.5x to 1.5x
                            voice.judderCountdown = juce::jmax (64, (int) (voice.judderIntervalSamples * jitter));
                            break;
                        }
                    }
                    voice.juddersRemaining--;
                }
            }

            if (--voice.samplesRemaining <= 0)
                voice.active = false;
        }

        // 3b. STEREO — mode-dependent spatial processing.
        //     The knob (voiceStereo) controls depth; the mode (cached
        //     at fire time) selects the algorithm.
        if (voiceActive && voice.voiceStereo > 0.0001f && numChannels > 1)
        {
            const float s = voice.voiceStereo;

            switch (voice.stereoModeCache)
            {
                default:
                case kStereoRandom:    // fixed random pan per fire
                case kStereoPingPong:  // alternating L/R per fire
                {
                    const float pan = voice.stereoPan * s;
                    const float gainL = juce::jlimit (0.0f, 1.0f, 1.0f - juce::jmax (0.0f,  pan));
                    const float gainR = juce::jlimit (0.0f, 1.0f, 1.0f - juce::jmax (0.0f, -pan));
                    sliceL *= gainL;
                    sliceR *= gainR;
                    break;
                }
                case kStereoAutoPan:   // LFO sweep during playback
                {
                    const double lfoHz = 0.5 + (double) s * 1.5;
                    voice.stereoLfoPhase += lfoHz * juce::MathConstants<double>::twoPi / sampleRate;
                    if (voice.stereoLfoPhase > juce::MathConstants<double>::twoPi)
                        voice.stereoLfoPhase -= juce::MathConstants<double>::twoPi;
                    const float pan = (float) std::sin (voice.stereoLfoPhase) * s;
                    const float gainL = juce::jlimit (0.0f, 1.0f, 1.0f - juce::jmax (0.0f,  pan));
                    const float gainR = juce::jlimit (0.0f, 1.0f, 1.0f - juce::jmax (0.0f, -pan));
                    sliceL *= gainL;
                    sliceR *= gainR;
                    break;
                }
                case kStereoHaas:      // micro-delay on one channel
                {
                    // v0.43 — rewritten to use a tiny ring buffer instead
                    // of reading raw from the circular buffer. The old
                    // approach jumped on judder retrigger because it read
                    // from voice.readPos which resets abruptly. Now we
                    // delay the already-crossfaded slice signal, which is
                    // click-free.
                    const int delaySamples = voice.stereoHaasSamples;
                    if (delaySamples > 0 && delaySamples < PlaybackVoice::kHaasDelayMax)
                    {
                        // Write current sample into the Haas ring buffer
                        voice.haasBuffer[0][voice.haasWritePos] = sliceL;
                        voice.haasBuffer[1][voice.haasWritePos] = sliceR;
                        int readIdx = voice.haasWritePos - delaySamples;
                        if (readIdx < 0) readIdx += PlaybackVoice::kHaasDelayMax;
                        if (voice.stereoPan > 0.0f)
                            sliceR = voice.haasBuffer[1][readIdx];
                        else
                            sliceL = voice.haasBuffer[0][readIdx];
                        voice.haasWritePos = (voice.haasWritePos + 1) % PlaybackVoice::kHaasDelayMax;
                    }
                    break;
                }
            }
        }

        // v0.5.0 — Recombine frequency split: add the untouched band
        // back so that only one frequency region was processed by FX.
        if (voiceActive && splitMode != 0)
        {
            sliceL += freqSplitDryL;
            sliceR += freqSplitDryR;
        }

        // v0.5.0 — slice output peak ring (voice output post-FX, pre-mix).
        {
            float sPeak = voiceActive ? juce::jmax (std::abs (sliceL), std::abs (sliceR)) : 0.0f;
            if (sPeak > slicePeakAccum)
                slicePeakAccum = sPeak;
            if (++slicePeakAccumCount >= kPeakBucketSamples)
            {
                const int swp = slicePeakWritePos.load();
                slicePeakRing[swp].store (slicePeakAccum);
                slicePeakWritePos.store ((swp + 1) % kSlicePeakRingSize);
                slicePeakAccum = 0.0f;
                slicePeakAccumCount = 0;
            }
        }

        // 4. Combine voice into output + feedback injection.
        //
        //    Two modes depending on whether any global FX are active:
        //
        //    NO global FX: true wet/dry crossfade. io gets replaced with
        //      clean*(1-mix) + slice*mix. Smear is added as its own layer
        //      scaled by mix. MIX 0% = clean dry, MIX 100% = wet only.
        //
        //    GLOBAL FX on: additive combine into io (dry + slice + smear),
        //      then global FX process everything, then the final crossfade
        //      in step 4c blends clean vs the fully-processed combined.
        //      MIX 0% = clean dry, MIX 100% = effects on everything.
        //
        // Feedback always uses the raw slice (not mixed), since it feeds
        // back into the circular buffer for future lookbacks.
        if (voiceActive)
        {
            if (anyGlobalFx)
            {
                // Additive: global FX will process the whole thing.
                io[0][i] += sliceL;
                if (numChannels > 1)
                    io[1][i] += sliceR;
            }
            else
            {
                // v0.44 — equal-power crossfade: cos/sin curves so the
                // midpoint sits at ~-3dB per leg instead of -6dB linear,
                // eliminating the perceived volume dip at mid-mix.
                const float wet = voice.voiceMix;
                const float phase = wet * juce::MathConstants<float>::halfPi;
                const float gDry = std::cos (phase);
                const float gWet = std::sin (phase);
                io[0][i] = io[0][i] * gDry + sliceL * gWet;
                if (numChannels > 1)
                    io[1][i] = io[1][i] * gDry + sliceR * gWet;
            }

            // FEEDBACK: inject the voice output back into the
            // circular buffer at writePos so future lookbacks pick
            // it up. Skipped on freeze (buffer is locked) and when
            // feedback is zero (no-op path). The next fire's
            // lookback window will blend with this residue and the
            // runaway-loop character emerges.
            // v0.5.0 — mode-aware branching based on feedbackCharacter.
            if (! frozen && voice.voiceFeedback > 0.0001f)
            {
                const float fb = voice.voiceFeedback;
                float injectL = sliceL * fb;
                float injectR = sliceR * fb;

                switch (voice.voiceFeedbackCharacter)
                {
                    default:
                    case 0:  // Clean — direct re-injection
                        break;
                    case 1:  // Filtered — LP at ~2 kHz before re-inject
                    {
                        // Simple one-pole LP: y += a * (x - y)
                        // Coefficient for ~2kHz at any sample rate
                        const float cutoff = 2000.0f;
                        const float coeff = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * cutoff / (float) sampleRate);
                        voice.feedbackFilterL += coeff * (injectL - voice.feedbackFilterL);
                        voice.feedbackFilterR += coeff * (injectR - voice.feedbackFilterR);
                        injectL = voice.feedbackFilterL;
                        injectR = voice.feedbackFilterR;
                        break;
                    }
                    case 2:  // Saturated — soft-clip before re-inject (thickens)
                        injectL = std::tanh (injectL * 2.0f) * 0.7f;
                        injectR = std::tanh (injectR * 2.0f) * 0.7f;
                        break;
                    case 3:  // Ducked — sidechain against input level
                    {
                        // Use the current input level to duck the feedback
                        const float inputLevel = juce::jmax (std::abs (cb[0][writePos]),
                                                              numChannels > 1 ? std::abs (cb[1][writePos]) : 0.0f);
                        // Fast attack, slow release envelope follower
                        const float attackCoeff = 0.01f;
                        const float releaseCoeff = 0.0001f;
                        if (inputLevel > voice.feedbackDuckEnv)
                            voice.feedbackDuckEnv += attackCoeff * (inputLevel - voice.feedbackDuckEnv);
                        else
                            voice.feedbackDuckEnv += releaseCoeff * (inputLevel - voice.feedbackDuckEnv);
                        // Duck: reduce feedback when input is loud
                        const float duckGain = juce::jmax (0.05f, 1.0f - voice.feedbackDuckEnv * 3.0f);
                        injectL *= duckGain;
                        injectR *= duckGain;
                        break;
                    }
                }

                cb[0][writePos] = softLimit (cb[0][writePos] + injectL);
                if (numChannels > 1)
                    cb[1][writePos] = softLimit (cb[1][writePos] + injectR);
            }
        }

        // 4a. SMEAR delay line. Always ticks so the tail keeps decaying
        //     after the voice ends. smearFeedAmount is updated at fire
        //     time so each Act's tail keeps its own character.
        // v0.5.0 — mode-aware DSP branching based on smearCharacter.
        {
            auto* const* sb = smearBuffer.getArrayOfWritePointers();
            const float delayedL = sb[0][smearWritePos];
            const float delayedR = sb[1][smearWritePos];

            // Read the current scene's smear character
            const int smearMode = scenes[selectedScene.load()].smearCharacter.load();

            float processedL = delayedL;
            float processedR = delayedR;

            float fb;
            switch (smearMode)
            {
                default:
                case 0:  // Blur — standard delay feedback
                    fb = juce::jmin (0.85f, smearFeedAmount * 0.85f);
                    break;
                case 1:  // Diffuse — allpass in feedback path
                {
                    fb = juce::jmin (0.85f, smearFeedAmount * 0.85f);
                    const float apCoeff = 0.6f;
                    // Simple allpass: out = -g*in + state; state = in + g*out
                    float apOutL = -apCoeff * processedL + smearApStateL;
                    smearApStateL = processedL + apCoeff * apOutL;
                    float apOutR = -apCoeff * processedR + smearApStateR;
                    smearApStateR = processedR + apCoeff * apOutR;
                    processedL = apOutL;
                    processedR = apOutR;
                    break;
                }
                case 2:  // Freeze — infinite hold when smear is high
                {
                    const float freezeThresh = 0.4f;
                    if (smearFeedAmount > freezeThresh)
                    {
                        const float freezeBlend = juce::jmin (1.0f, (smearFeedAmount - freezeThresh) / (1.0f - freezeThresh));
                        fb = 0.85f + freezeBlend * 0.145f;  // ramps toward 0.995
                    }
                    else
                        fb = juce::jmin (0.85f, smearFeedAmount * 0.85f);
                    break;
                }
            }

            const float feedL = (voiceActive ? sliceL : 0.0f);
            const float feedR = (voiceActive ? sliceR : 0.0f);

            sb[0][smearWritePos] = feedL + processedL * fb;
            sb[1][smearWritePos] = feedR + processedR * fb;

            const float sendLevel = smearFeedAmount * 0.7f;
            // Scale smear send by effective mix so MIX 0% = pure dry.
            // When global FX are on, step 4c's crossfade handles this;
            // when they're off, smear was leaking through unscaled.
            const float smearMix = anyGlobalFx ? sendLevel
                : sendLevel * juce::jlimit (0.0f, 1.0f, pMix->load() * pGlobalMix->load());
            io[0][i] += delayedL * smearMix;
            if (numChannels > 1)
                io[1][i] += delayedR * smearMix;

            if (++smearWritePos >= smearBufferSize)
                smearWritePos = 0;
        }

        // 4b. Global FX chain. When any "all audio" toggle is on,
        //     the effect processes the combined signal (dry + slice +
        //     smear tail) using the live knob values. Per-voice
        //     instances of the same effect were skipped above.
        if (anyGlobalFx)
        {
            // DRIVE (global)
            if (gfxDrive)
            {
                const float d = pDrive->load();
                if (d > 0.0001f)
                {
                    const float g = 1.0f + d * 5.0f;
                    const float wet = d;
                    io[0][i] = (1.0f - wet) * io[0][i] + wet * std::tanh (io[0][i] * g);
                    if (numChannels > 1)
                        io[1][i] = (1.0f - wet) * io[1][i] + wet * std::tanh (io[1][i] * g);
                }
            }

            // TONE + RES (global) — multi-mode filter using global SVF state.
            // v0.5.0 — reads the selected Act's filter mode for consistency.
            if (gfxTone)
            {
                const float toneVal = pTone->load();
                const float resVal  = pRes ->load();
                const int gfMode = scenes[selectedScene.load()].filterMode.load();

                auto softClip = [] (float x) -> float
                {
                    const float x2 = x * x;
                    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
                };

                if (gfMode == loopsab::kFilterTilt)
                {
                    // Legacy Tilt behaviour
                    if (toneVal < 0.499f)
                    {
                        const float toneNorm = toneVal * 2.0f;
                        const double cutoffHz = juce::jlimit (
                            30.0, sampleRate * 0.45,
                            350.0 * std::pow (60.0, (double) toneNorm));
                        const double fRaw = 2.0 * std::sin (juce::MathConstants<double>::pi
                                                             * cutoffHz / sampleRate);
                        const float f = (float) juce::jlimit (0.0, 0.48, fRaw);
                        const float q = juce::jmax (0.005f, 1.0f - resVal);
                        {
                            const float bp = softClip (globalSvfBandL);
                            const float high = io[0][i] - globalSvfLowL - q * bp;
                            globalSvfBandL += f * high;
                            globalSvfLowL  += f * softClip (globalSvfBandL);
                            io[0][i] = globalSvfLowL;
                        }
                        if (numChannels > 1)
                        {
                            const float bp = softClip (globalSvfBandR);
                            const float high = io[1][i] - globalSvfLowR - q * bp;
                            globalSvfBandR += f * high;
                            globalSvfLowR  += f * softClip (globalSvfBandR);
                            io[1][i] = globalSvfLowR;
                        }
                    }
                    else if (toneVal > 0.501f)
                    {
                        const float airAmt = (toneVal - 0.5f) * 2.0f;
                        const float hpCoeff = 0.7f;
                        globalSvfLowL += hpCoeff * (io[0][i] - globalSvfLowL);
                        const float hfL = io[0][i] - globalSvfLowL;
                        io[0][i] += hfL * airAmt * 3.0f;
                        if (numChannels > 1)
                        {
                            globalSvfLowR += hpCoeff * (io[1][i] - globalSvfLowR);
                            const float hfR = io[1][i] - globalSvfLowR;
                            io[1][i] += hfR * airAmt * 3.0f;
                        }
                    }
                }
                else
                {
                    // LP / HP / BP: full-range resonant SVF
                    const double cutoffHz = juce::jlimit (
                        20.0, sampleRate * 0.45,
                        20.0 * std::pow (1000.0, (double) toneVal));
                    const double fRaw = 2.0 * std::sin (juce::MathConstants<double>::pi
                                                         * cutoffHz / sampleRate);
                    const float f = (float) juce::jlimit (0.0, 0.48, fRaw);
                    const float q = juce::jmax (0.005f, 1.0f - resVal * 0.995f);

                    // Left
                    {
                        const float bp = softClip (globalSvfBandL);
                        const float high = io[0][i] - globalSvfLowL - q * bp;
                        globalSvfBandL += f * high;
                        globalSvfLowL  += f * softClip (globalSvfBandL);
                    }
                    // Right
                    if (numChannels > 1)
                    {
                        const float bp = softClip (globalSvfBandR);
                        const float high = io[1][i] - globalSvfLowR - q * bp;
                        globalSvfBandR += f * high;
                        globalSvfLowR  += f * softClip (globalSvfBandR);
                    }

                    switch (gfMode)
                    {
                        default:
                        case loopsab::kFilterLP:
                            io[0][i] = globalSvfLowL;
                            if (numChannels > 1) io[1][i] = globalSvfLowR;
                            break;
                        case loopsab::kFilterHP:
                            io[0][i] = io[0][i] - globalSvfLowL - q * softClip (globalSvfBandL);
                            if (numChannels > 1)
                                io[1][i] = io[1][i] - globalSvfLowR - q * softClip (globalSvfBandR);
                            break;
                        case loopsab::kFilterBP:
                            io[0][i] = globalSvfBandL;
                            if (numChannels > 1) io[1][i] = globalSvfBandR;
                            break;
                    }
                }
            }

            // FOLD (global)
            if (gfxFold)
            {
                const float fd = pFold->load();
                if (fd > 0.0001f)
                {
                    const float drive = 1.0f + fd * 6.0f;
                    const float foldedL = std::sin (io[0][i] * drive * juce::MathConstants<float>::halfPi);
                    io[0][i] = (1.0f - fd) * io[0][i] + fd * foldedL;
                    if (numChannels > 1)
                    {
                        const float foldedR = std::sin (io[1][i] * drive * juce::MathConstants<float>::halfPi);
                        io[1][i] = (1.0f - fd) * io[1][i] + fd * foldedR;
                    }
                }
            }

            // RING MOD (global) — continuous oscillator using global phase.
            if (gfxRingMod)
            {
                const float rm = pRingMod->load();
                if (rm > 0.0001f)
                {
                    double rmFreq = 20.0 * std::pow (100.0, (double) rm);
                    // v0.42 — ring mod quantise is resolved per-Act (see
                    // gfxRingModQuant snapshot at block start).
                    if (gfxRingModQuant)
                    {
                        const int root     = (int) std::round (pScaleRoot->load());
                        const int scaleIdx = (int) std::round (pScaleType->load());
                        const float midiNote = 69.0f + 12.0f * std::log2f ((float) rmFreq / 440.0f);
                        const float snapped  = snapSemitonesToScale (midiNote, root, scaleIdx);
                        rmFreq = 440.0 * std::pow (2.0, ((double) snapped - 69.0) / 12.0);
                    }
                    globalRingModPhase += rmFreq * juce::MathConstants<double>::twoPi / sampleRate;
                    if (globalRingModPhase > juce::MathConstants<double>::twoPi)
                        globalRingModPhase -= juce::MathConstants<double>::twoPi;
                    const float mod = (float) std::sin (globalRingModPhase);
                    io[0][i] = (1.0f - rm) * io[0][i] + rm * io[0][i] * mod;
                    if (numChannels > 1)
                        io[1][i] = (1.0f - rm) * io[1][i] + rm * io[1][i] * mod;
                }
            }

            // CRUSH (global)
            if (gfxCrush)
            {
                const float gBits = pCrunch   ->load();
                const float gRate = pCrushRate->load();
                if (numChannels > 1)
                    applyCrunch (io[0][i], io[1][i], gBits, gRate, globalCrushState);
                else
                {
                    float dummy = 0.0f;
                    applyCrunch (io[0][i], dummy, gBits, gRate, globalCrushState);
                }
                globalCrushState.sampleCounter++;
            }
        }

        // 4c. Final wet/dry crossfade — only when global FX are active.
        //     io currently contains the fully processed combined signal
        //     (dry + slice + smear + global FX). Crossfade with the
        //     saved clean input so MIX 0% = untouched source, MIX 100%
        //     = fully processed combined signal.
        //
        //     When no global FX are on, the per-voice crossfade in step 4
        //     already handled the wet/dry blend using voice.voiceMix, so
        //     we skip this to avoid double-mixing.
        if (anyGlobalFx)
        {
            const float gmMod = (globalMixLockSlot >= 0) ? lfoModOffset[globalMixLockSlot] : 0.0f;
            const float gm = juce::jlimit (0.0f, 1.0f, pGlobalMix->load() + gmMod);
            const float mix = pMix->load() * gm;
            if (mix < 0.9999f)
            {
                const float dry = 1.0f - mix;
                io[0][i] = cleanL * dry + io[0][i] * mix;
                if (numChannels > 1)
                    io[1][i] = cleanR * dry + io[1][i] * mix;
            }
        }

        // v0.30 — mute gate with anti-click envelope. Instead of hard-
        // zeroing (which clicks), we ramp muteGateGain toward 0 or 1
        // over ~32 samples (~0.7ms). Fast enough to feel instant, slow
        // enough to be click-free.
        {
            const bool shouldMute = seqEnabled && wrappedStep >= 0
                                    && stepMute[wrappedStep].load();
            constexpr float muteStep = 1.0f / 32.0f;
            if (shouldMute)
                muteGateGain = juce::jmax (0.0f, muteGateGain - muteStep);
            else
                muteGateGain = juce::jmin (1.0f, muteGateGain + muteStep);

            if (muteGateGain < 1.0f)
            {
                io[0][i] *= muteGateGain;
                if (numChannels > 1)
                    io[1][i] *= muteGateGain;
            }
        }

        // v0.13 — final output safety clip. Linear pass-through below
        // -1dBFS, soft-knee compression above, hard-capped at 0dBFS
        // so extreme parameter combinations (high Feedback + Shimmer
        // + Mix + Drive) can never blow speakers or downstream plugins.
        io[0][i] = softLimit (io[0][i]);
        if (numChannels > 1)
            io[1][i] = softLimit (io[1][i]);

        // v0.40 — ENGINE: post-mix Output Stage. Sits after softLimit so
        // the colour processor sees an already-bounded signal, and before
        // the lookahead delay so its character is delayed in lockstep with
        // everything else. In Clean mode this is a near-zero-cost no-op.
        //
        // v0.41 — push the active Act's engine settings into the
        // CrossfadingOutputStage. setTarget is a no-op when nothing changed,
        // and triggers an ~8ms A/B crossfade when it does — which keeps
        // Act switches (and sequencer step boundaries that switch Acts)
        // pop-free at the master bus.
        {
            const int sceneForEngine = (activeSceneForFire >= 0)
                ? activeSceneForFire
                : selectedScene.load();
            if (sceneForEngine >= 0 && sceneForEngine < kNumScenes)
            {
                outputStage.setTarget (scenes[sceneForEngine].engineStage.load(),
                                       scenes[sceneForEngine].engineMix  .load(),
                                       scenes[sceneForEngine].engineIntensity.load());
            }

            float L = io[0][i];
            float R = (numChannels > 1) ? io[1][i] : L;
            outputStage.process (L, R);
            io[0][i] = L;
            if (numChannels > 1) io[1][i] = R;
        }

        // v0.6.0 — NaN/inf safety net. If any DSP path produces garbage
        // (division by near-zero, unclamped sqrt, feedback runaway, etc.)
        // replace the sample with silence rather than sending poison
        // downstream where it could crash the DAW or blow speakers.
        {
            float& sL = io[0][i];
            if (! std::isfinite (sL)) sL = 0.0f;
            if (numChannels > 1)
            {
                float& sR = io[1][i];
                if (! std::isfinite (sR)) sR = 0.0f;
            }
        }

        // 5. Advance the circular write head. Skipped on freeze so the
        //    buffer contents stay locked to the moment of freeze.
        if (! frozen)
        {
            if (++writePos >= circularBufferSize)
                writePos = 0;
        }
    }

    // v0.7.0 — Cage 4'33" easter egg: if any active scene has the
    // cageSilence flag set, zero the entire output buffer so the user
    // hears absolute silence — a faithful reproduction of the piece.
    for (int si = 0; si < kNumScenes; ++si)
    {
        if (scenes[si].cageSilence.load (std::memory_order_relaxed))
        {
            for (int ch = 0; ch < numChannels; ++ch)
                juce::FloatVectorOperations::clear (io[ch], numSamples);
            break;
        }
    }

    // v0.12 — DJ lookahead output delay. Anything we just rendered is
    // pushed through a ring buffer and delayed by lookaheadSamples
    // before it leaves the plugin. The host compensates via the
    // latency we reported from setLookaheadMs, so timeline alignment
    // is preserved. peakRing still captured the live input above, so
    // it's inherently ahead of what the listener hears by exactly the
    // lookahead window — the editor uses that gap to paint the
    // incoming peaks to the right of the playhead.
    {
        const int lh = lookaheadSamples.load();
        if (lh > 0 && lookaheadBufferSize > 0)
        {
            auto* lb0 = lookaheadBuffer.getWritePointer (0);
            auto* lb1 = (lookaheadBuffer.getNumChannels() > 1)
                        ? lookaheadBuffer.getWritePointer (1)
                        : nullptr;

            for (int i = 0; i < numSamples; ++i)
            {
                int readIdx = lookaheadWritePos - lh;
                if (readIdx < 0) readIdx += lookaheadBufferSize;

                const float inL = io[0][i];
                const float inR = (numChannels > 1) ? io[1][i] : inL;

                io[0][i] = lb0[readIdx];
                if (numChannels > 1 && lb1 != nullptr)
                    io[1][i] = lb1[readIdx];

                lb0[lookaheadWritePos] = inL;
                if (lb1 != nullptr)
                    lb1[lookaheadWritePos] = inR;

                if (++lookaheadWritePos >= lookaheadBufferSize)
                    lookaheadWritePos = 0;
            }
        }
    }

    // v0.30 — debug: voice snapshot for the UI monitor.
    {
        auto& s = dbgVoiceSnap;
        s.active           = voice.active;
        s.readPos          = voice.readPos;
        s.samplesRemaining = voice.samplesRemaining;
        s.totalSamples     = voice.totalSamples;
        s.fadeGain         = voice.fadeGain;
        s.juddersRemaining = voice.juddersRemaining;
        // Determine which scene is firing (best effort).
        if (voice.active && isPlaying && seqOn.load())
        {
            const int ws = currentPlayingStep.load();
            s.sceneIdx = (ws >= 0 && ws < kMaxSteps) ? steps[ws].load() : -1;
        }
        else
        {
            s.sceneIdx = -1;
        }
        s.voiceMix = voice.voiceMix;
    }

    // v0.30 — debug: CPU watchdog. Compare wall time to buffer time.
    {
        const auto dbgBlockEnd = std::chrono::high_resolution_clock::now();
        const double elapsedUs = (double) std::chrono::duration_cast<std::chrono::microseconds>(
                                     dbgBlockEnd - dbgBlockStart).count();
        const double bufferUs  = (double) numSamples / sampleRate * 1e6;
        const float  fraction  = (bufferUs > 0.0) ? (float) (elapsedUs / bufferUs) : 0.0f;
        // Store the worst case since last read.
        float prev = dbgCpuWorstCase.load();
        while (fraction > prev)
        {
            if (dbgCpuWorstCase.compare_exchange_weak (prev, fraction))
                break;
        }
    }
}

// ----------------------------------------------------------------------------
//  State
// ----------------------------------------------------------------------------
juce::AudioProcessorEditor* LoopSaboteurProcessor::createEditor()
{
    return new LoopSaboteurEditor (*this);
}

void LoopSaboteurProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Wrap APVTS + sequencer state in one outer element so we can
    // persist scenes and steps alongside the regular automatable
    // parameters without shoving them into the APVTS itself.
    juce::XmlElement root ("LoopSaboteurState");

    if (auto apvtsXml = apvts.copyState().createXml())
        root.addChildElement (apvtsXml.release());

    // Scenes — flat list with all ten ShapingParams fields per scene.
    // v0.42 — bumped Scenes/@version to 2 so setStateInformation can
    // tell a v0.42+ save (per-Act stereo/FX-on-Dry/ring-mod-quant attrs
    // present) from an older save (globals only, must be broadcast to
    // every scene after the Scene blocks load).
    auto* scenesEl = root.createNewChildElement ("Scenes");
    scenesEl->setAttribute ("version", 2);
    for (int i = 0; i < kNumScenes; ++i)
    {
        auto* s = scenesEl->createNewChildElement ("Scene");
        s->setAttribute ("i",         i);
        s->setAttribute ("division",  scenes[i].divisionIdx  .load());
        s->setAttribute ("chance",    (double) scenes[i].chance       .load());
        s->setAttribute ("lookback",  scenes[i].lookbackIdx  .load());
        s->setAttribute ("rate",      scenes[i].rateIdx      .load());
        s->setAttribute ("judder",    scenes[i].judderCount  .load());
        s->setAttribute ("judderDiv", scenes[i].judderDivIdx .load());
        s->setAttribute ("pitch",     (double) scenes[i].pitchSt      .load());
        s->setAttribute ("slide",     (double) scenes[i].slideSt      .load());
        s->setAttribute ("decay",     (double) scenes[i].decay        .load());
        s->setAttribute ("reverse",   (double) scenes[i].reverseChance.load());
        s->setAttribute ("crunch",    (double) scenes[i].crunch       .load());
        s->setAttribute ("crushrate", (double) scenes[i].crushRate    .load());
        s->setAttribute ("mix",       (double) scenes[i].mix          .load());
        s->setAttribute ("drive",     (double) scenes[i].drive        .load());
        s->setAttribute ("tone",      (double) scenes[i].tone         .load());
        s->setAttribute ("feedback",  (double) scenes[i].feedback     .load());
        s->setAttribute ("tape",      (double) scenes[i].tape         .load());
        s->setAttribute ("ringmod",   (double) scenes[i].ringMod      .load());
        s->setAttribute ("varispeed", (double) scenes[i].varispeed    .load());
        s->setAttribute ("stereo",    (double) scenes[i].stereo       .load());
        s->setAttribute ("stretch",   (double) scenes[i].stretch      .load());
        s->setAttribute ("shimmer",   (double) scenes[i].shimmer      .load());
        s->setAttribute ("res",       (double) scenes[i].res          .load());
        s->setAttribute ("fold",      (double) scenes[i].fold         .load());
        s->setAttribute ("gate",      (double) scenes[i].gate         .load());
        s->setAttribute ("smear",     (double) scenes[i].smear        .load());
        s->setAttribute ("stutter",   (double) scenes[i].stutter      .load());
        s->setAttribute ("chaos",     (double) scenes[i].chaos        .load());
        s->setAttribute ("stored",    sceneStored[i].load() ? 1 : 0);
        s->setAttribute ("presetIdx", scenePresetIdx[i].load());  // BUG 1 fix: serialize per-scene preset tracking

        // v0.41 — per-Act ENGINE bundle.
        s->setAttribute ("engineStage",     scenes[i].engineStage.load());
        s->setAttribute ("engineMix",       (double) scenes[i].engineMix.load());
        s->setAttribute ("engineIntensity", (double) scenes[i].engineIntensity.load());
        s->setAttribute ("interpMode",      scenes[i].interpMode .load());
        s->setAttribute ("crushAA",     scenes[i].crushAA    .load() ? 1 : 0);
        s->setAttribute ("crushMu",     scenes[i].crushMu    .load() ? 1 : 0);
        s->setAttribute ("crushAll",    scenes[i].crushAll   .load() ? 1 : 0);

        // v0.5.0 — per-Act filter mode + frequency split.
        s->setAttribute ("filterMode",    scenes[i].filterMode   .load());
        s->setAttribute ("freqSplitMode", scenes[i].freqSplitMode.load());
        s->setAttribute ("freqSplitHz",   (double) scenes[i].freqSplitHz.load());
        s->setAttribute ("driveType",      scenes[i].driveType.load());
        s->setAttribute ("foldTopology",   scenes[i].foldTopology.load());
        s->setAttribute ("shimmerOctave",  scenes[i].shimmerOctave.load());
        s->setAttribute ("smearCharacter", scenes[i].smearCharacter.load());
        s->setAttribute ("stutterWindow",  scenes[i].stutterWindow.load());
        s->setAttribute ("varispeedCurve", scenes[i].varispeedCurve.load());
        s->setAttribute ("slideCurve",     scenes[i].slideCurve.load());
        s->setAttribute ("reverseMode",    scenes[i].reverseMode.load());
        s->setAttribute ("tapeMode",       scenes[i].tapeMode.load());
        s->setAttribute ("ringModWave",    scenes[i].ringModWave.load());
        s->setAttribute ("chaosDistribution", scenes[i].chaosDistribution.load());
        s->setAttribute ("feedbackCharacter", scenes[i].feedbackCharacter.load());
        s->setAttribute ("stretchMode",    scenes[i].stretchMode.load());
        s->setAttribute ("judderShape",    scenes[i].judderShape.load());
        s->setAttribute ("lookbackBehaviour", scenes[i].lookbackBehaviour.load());
        s->setAttribute ("decayCurve",    scenes[i].decayCurve.load());

        // v0.42 — per-Act stereo mode / FX-on-Dry / ring mod quantise.
        s->setAttribute ("stereoMode",     scenes[i].stereoMode    .load());
        s->setAttribute ("fxOnDryDrive",   scenes[i].fxOnDryDrive  .load() ? 1 : 0);
        s->setAttribute ("fxOnDryTone",    scenes[i].fxOnDryTone   .load() ? 1 : 0);
        s->setAttribute ("fxOnDryRingMod", scenes[i].fxOnDryRingMod.load() ? 1 : 0);
        s->setAttribute ("fxOnDryFold",    scenes[i].fxOnDryFold   .load() ? 1 : 0);
        s->setAttribute ("ringModQuant",   scenes[i].ringModQuant  .load() ? 1 : 0);
    }

    // Step assignments + per-step metadata + length. One <Step>
    // per slot carries scene, ratio lock, freeze-hold flag,
    // and any non-NaN parameter locks as <L slot=".." v=".."/> children.
    auto* stepsEl = root.createNewChildElement ("Steps");
    stepsEl->setAttribute ("length", seqLength.load());
    // v0.10 — stamp the ratio-table version so older saves can be
    // migrated through migrateRatioFromV09 / migrateRatioFromV11.
    //   (absent) = v1  (v0.9 17-entry table, visit-counted)
    //   2        = v0.10/v0.11 13-entry table (visit-counted, dropped N:N)
    //   3        = v0.12 16-entry table (pattern-play semantics)
    stepsEl->setAttribute ("ratioV", 3);
    for (int i = 0; i < kMaxSteps; ++i)
    {
        auto* s = stepsEl->createNewChildElement ("Step");
        s->setAttribute ("i",      i);
        s->setAttribute ("scene",  steps[i].load());
        s->setAttribute ("ratio",  stepRatio[i].load());
        s->setAttribute ("freeze", stepFreezeHold[i].load() ? 1 : 0);
        s->setAttribute ("rch",    stepRatchet[i].load());    // v0.12
        s->setAttribute ("mute",   stepMute[i].load() ? 1 : 0);  // v0.13

        for (int k = 0; k < kNumLockableParams; ++k)
        {
            const float v = stepParamLock[i * kNumLockableParams + k].load();
            if (std::isnan (v))
                continue;
            auto* lockEl = s->createNewChildElement ("L");
            lockEl->setAttribute ("slot", k);
            lockEl->setAttribute ("v",    (double) v);
        }
    }

    // Sequencer-level toggles.
    auto* seqEl = root.createNewChildElement ("Seq");
    seqEl->setAttribute ("on",       seqOn.load() ? 1 : 0);
    seqEl->setAttribute ("selected", selectedScene.load());

    // v0.8 — persisted editor preferences. Not APVTS parameters but
    // they're still "this session should look like this next time".
    auto* uiEl = root.createNewChildElement ("Ui");
    uiEl->setAttribute ("scale",     (double) uiScale.load());
    uiEl->setAttribute ("waveform",  waveformVisible.load() ? 1 : 0);
    uiEl->setAttribute ("tooltips",  tooltipsEnabled.load() ? 1 : 0);
    uiEl->setAttribute ("freezeMom", freezeMomentaryMode.load() ? 1 : 0);
    uiEl->setAttribute ("autoLen",   autoFollowLength.load() ? 1 : 0);
    uiEl->setAttribute ("pageFollow", pageFollowsPlayhead.load() ? 1 : 0);
    uiEl->setAttribute ("gridAccent", gridAccentInterval.load());
    uiEl->setAttribute ("seqTransport", seqTransport.load());
    uiEl->setAttribute ("lookaheadMs", lookaheadMs.load());   // v0.12
    uiEl->setAttribute ("crushAA",    crushAntiAlias.load() ? 1 : 0);  // v0.22
    uiEl->setAttribute ("crushMu",    crushMuLaw.load() ? 1 : 0);      // v0.22
    uiEl->setAttribute ("crushAll",   crushAllAudio.load()  ? 1 : 0);  // v0.22
    uiEl->setAttribute ("bypass",     masterBypass.load()   ? 1 : 0);  // v0.24
    uiEl->setAttribute ("rmQuant",   ringModQuantise.load() ? 1 : 0); // v0.25
    uiEl->setAttribute ("stereoMode", stereoMode.load());             // v0.25
    uiEl->setAttribute ("globalDrive",     globalDrive.load()     ? 1 : 0);  // v0.25
    uiEl->setAttribute ("globalTone",      globalTone.load()      ? 1 : 0);
    uiEl->setAttribute ("globalRingMod",   globalRingMod.load()   ? 1 : 0);
    uiEl->setAttribute ("globalFold",      globalFold.load()      ? 1 : 0);
    uiEl->setAttribute ("preserveLfos",    preserveLfosOnClear.load() ? 1 : 0); // v0.38

    // v0.40 — ENGINE block. Bundled character settings.
    // v0.41 — Engine is per-Act now. The legacy <Engine> child is still
    // written for back-compat readers (it captures Act 0's bundle, which
    // matches the behaviour of older sessions where every Act used the
    // same global engine). The authoritative per-Act values are written
    // as attributes on each <Scene> further down.
    auto* engineEl = root.createNewChildElement ("Engine");
    engineEl->setAttribute ("ver",         2);
    engineEl->setAttribute ("interpMode",  scenes[0].interpMode .load());
    engineEl->setAttribute ("outputStage", scenes[0].engineStage.load());
    engineEl->setAttribute ("outputMix",   (double) scenes[0].engineMix.load());
    engineEl->setAttribute ("outputIntensity", (double) scenes[0].engineIntensity.load());

    // v0.30 — LFO modulation settings. Now per-Act: 8 Acts × 4 LFOs = 32 total.
    for (int si = 0; si < kNumScenes; ++si)
    {
        for (int li = 0; li < kNumLfosPerAct; ++li)
        {
            auto* lfoEl = root.createNewChildElement ("Lfo");
            lfoEl->setAttribute ("scene", si);
            lfoEl->setAttribute ("idx",    li);
            lfoEl->setAttribute ("shape",  lfos[si][li].shape.load());
            lfoEl->setAttribute ("rate",   lfos[si][li].rateIdx.load());
            lfoEl->setAttribute ("depth",  (double) lfos[si][li].depth.load());
            lfoEl->setAttribute ("target", lfos[si][li].targetSlot.load());
            // v0.34 — envelope + sync fields. Optional in XML so saves
            // from v0.33 or earlier load without attribute-miss errors.
            lfoEl->setAttribute ("sync",    lfos[si][li].sync.load() ? 1 : 0);
            lfoEl->setAttribute ("attack",  (double) lfos[si][li].attackMs.load());
            lfoEl->setAttribute ("hold",    (double) lfos[si][li].holdMs.load());
            lfoEl->setAttribute ("decay",   (double) lfos[si][li].decayMs.load());
            lfoEl->setAttribute ("trig",    lfos[si][li].trigMode.load());
            // v0.38 — cross-mod wiring. Default -1 / 0 on older saves
            // so absent attributes restore silently to "no cross-mod".
            lfoEl->setAttribute ("xTarget", lfos[si][li].crossTargetLfo.load());
            lfoEl->setAttribute ("xDepth",  (double) lfos[si][li].crossDepth.load());
            // v0.50 — envelope follower sensitivity gain
            lfoEl->setAttribute ("envFollowGain", lfos[si][li].envFollowGain.load());
        }
    }

    copyXmlToBinary (root, destData);
}

void LoopSaboteurProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto root = getXmlFromBinary (data, sizeInBytes);
    if (root == nullptr) return;

    // v0.10 — suppress the auto-mirror listener for the entire restore
    // so the APVTS values we're loading don't stomp on the scene data
    // we're about to restore from XML (via parameterChanged writing
    // the incoming knob values into scenes[selectedScene]).
    suppressAutoMirror.store (true);
    struct RestoreGuard
    {
        std::atomic<bool>& flag;
        ~RestoreGuard() { flag.store (false); }
    } guard { suppressAutoMirror };

    // Back-compat: older sessions saved just the APVTS XML as the
    // outer element. Detect that and load it directly.
    if (root->hasTagName (apvts.state.getType()))
    {
        apvts.replaceState (juce::ValueTree::fromXml (*root));
        return;
    }

    if (! root->hasTagName ("LoopSaboteurState"))
        return;

    // APVTS (automatable params) — restore via the original codepath.
    if (auto* apvtsXml = root->getChildByName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*apvtsXml));

    // v0.42 — tracked so the Ui block below knows whether to broadcast
    // the legacy FX-on-Dry / stereo / ring-mod-quant globals into every
    // Act slot. Scenes/@version=2 means the saved file already carries
    // per-Act attrs and the globals should be treated as obsolete.
    bool scenesCarryPerActFx = false;

    // Scenes.
    if (auto* scenesEl = root->getChildByName ("Scenes"))
    {
        scenesCarryPerActFx = (scenesEl->getIntAttribute ("version", 1) >= 2);
        for (auto* s : scenesEl->getChildIterator())
        {
            const int i = s->getIntAttribute ("i", -1);
            if (i < 0 || i >= kNumScenes) continue;
            scenes[i].lookbackIdx  .store (juce::jlimit (0, 9,  s->getIntAttribute ("lookback",  3)));
            scenes[i].rateIdx      .store (juce::jlimit (0, 5,  s->getIntAttribute ("rate",      3)));
            scenes[i].judderCount  .store (juce::jlimit (1, kMaxJudder, s->getIntAttribute ("judder", 1)));
            scenes[i].judderDivIdx .store (juce::jlimit (0, 11, s->getIntAttribute ("judderDiv", 3)));
            scenes[i].pitchSt      .store ((float) s->getDoubleAttribute ("pitch",   0.0));
            scenes[i].slideSt      .store ((float) s->getDoubleAttribute ("slide",   0.0));
            scenes[i].decay        .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("decay", 0.8)));
            scenes[i].reverseChance.store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("reverse", 0.0)));
            scenes[i].crunch       .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("crunch",  0.0)));
            scenes[i].crushRate    .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("crushrate", 0.0)));
            scenes[i].mix          .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("mix",     0.5)));
            scenes[i].divisionIdx  .store (juce::jlimit (0, 9,  s->getIntAttribute ("division", 2)));
            scenes[i].chance       .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("chance",  1.0)));
            scenes[i].drive        .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("drive",    0.0)));
            scenes[i].tone         .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("tone",     0.5)));
            scenes[i].feedback     .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("feedback", 0.0)));
            scenes[i].tape         .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("tape",     0.0)));
            scenes[i].ringMod      .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("ringmod",  0.0)));
            scenes[i].varispeed    .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("varispeed",0.0)));
            scenes[i].stereo       .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("stereo",   0.0)));
            scenes[i].stretch      .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("stretch",  0.0)));
            scenes[i].shimmer      .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("shimmer",  0.0)));
            scenes[i].res          .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("res",      0.0)));
            scenes[i].fold         .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("fold",     0.0)));
            scenes[i].gate         .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("gate",     0.0)));
            scenes[i].smear        .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("smear",    0.0)));
            scenes[i].stutter      .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("stutter",  0.0)));
            scenes[i].chaos        .store (juce::jlimit (0.0f, 1.0f, (float) s->getDoubleAttribute ("chaos",    0.0)));
            sceneStored[i]         .store (s->getIntAttribute ("stored", 0) != 0);
            scenePresetIdx[i]      .store (s->getIntAttribute ("presetIdx", -1));  // BUG 1 fix: deserialize per-scene preset tracking (-1 = default if absent)

            // v0.41 — per-Act ENGINE bundle. Defaults below match the
            // pre-v0.41 globals so old saves (which carry no per-scene
            // engine attrs) reload identically; the global Engine block
            // above already broadcast its values to all scenes if recall
            // was enabled, and those broadcasts will be the values we read
            // here when the attributes aren't present.
            scenes[i].engineStage.store (juce::jlimit (0, (int) loopsab::kNumOutputStages - 1,
                                                        s->getIntAttribute ("engineStage",
                                                                             scenes[i].engineStage.load())));
            scenes[i].engineMix  .store (juce::jlimit (0.0f, 1.0f,
                                                        (float) s->getDoubleAttribute ("engineMix",
                                                                                        (double) scenes[i].engineMix.load())));
            scenes[i].engineIntensity.store (juce::jlimit (0.0f, 1.0f,
                                                        (float) s->getDoubleAttribute ("engineIntensity", 0.5)));
            scenes[i].interpMode .store (juce::jlimit (0, (int) loopsab::kNumInterpModes - 1,
                                                        s->getIntAttribute ("interpMode",
                                                                             scenes[i].interpMode.load())));
            scenes[i].crushAA    .store (s->getIntAttribute ("crushAA",
                                                              scenes[i].crushAA.load() ? 1 : 0) != 0);
            scenes[i].crushMu    .store (s->getIntAttribute ("crushMu",
                                                              scenes[i].crushMu.load() ? 1 : 0) != 0);
            scenes[i].crushAll   .store (s->getIntAttribute ("crushAll",
                                                              scenes[i].crushAll.load() ? 1 : 0) != 0);

            // v0.5.0 — per-Act filter mode + frequency split.
            scenes[i].filterMode   .store (juce::jlimit (0, (int) loopsab::kNumFilterModes - 1,
                                                          s->getIntAttribute ("filterMode",
                                                                               scenes[i].filterMode.load())));
            scenes[i].freqSplitMode.store (juce::jlimit (0, 2,
                                                          s->getIntAttribute ("freqSplitMode",
                                                                               scenes[i].freqSplitMode.load())));
            scenes[i].freqSplitHz  .store (juce::jlimit (80.0f, 8000.0f,
                                                          (float) s->getDoubleAttribute ("freqSplitHz",
                                                                                          (double) scenes[i].freqSplitHz.load())));
            scenes[i].driveType.store (juce::jlimit (0, (int) loopsab::kNumDriveTypes - 1,
                                                     s->getIntAttribute ("driveType", scenes[i].driveType.load())));
            scenes[i].foldTopology.store (juce::jlimit (0, (int) loopsab::kNumFoldTopologies - 1,
                                                        s->getIntAttribute ("foldTopology", scenes[i].foldTopology.load())));
            scenes[i].shimmerOctave.store (juce::jlimit (0, (int) loopsab::kNumShimmerOctaves - 1,
                                                         s->getIntAttribute ("shimmerOctave", scenes[i].shimmerOctave.load())));
            scenes[i].smearCharacter.store (juce::jlimit (0, (int) loopsab::kNumSmearCharacters - 1,
                                                          s->getIntAttribute ("smearCharacter", scenes[i].smearCharacter.load())));
            scenes[i].stutterWindow.store (juce::jlimit (0, (int) loopsab::kNumStutterWindows - 1,
                                                         s->getIntAttribute ("stutterWindow", scenes[i].stutterWindow.load())));
            scenes[i].varispeedCurve.store (juce::jlimit (0, (int) loopsab::kNumVarispeedCurves - 1,
                                                          s->getIntAttribute ("varispeedCurve", scenes[i].varispeedCurve.load())));
            scenes[i].slideCurve.store (juce::jlimit (0, (int) loopsab::kNumSlideCurves - 1,
                                                      s->getIntAttribute ("slideCurve", scenes[i].slideCurve.load())));
            scenes[i].reverseMode.store (juce::jlimit (0, (int) loopsab::kNumReverseModes - 1,
                                                       s->getIntAttribute ("reverseMode", scenes[i].reverseMode.load())));
            scenes[i].tapeMode.store (juce::jlimit (0, (int) loopsab::kNumTapeModes - 1,
                                                     s->getIntAttribute ("tapeMode", scenes[i].tapeMode.load())));
            scenes[i].ringModWave.store (juce::jlimit (0, (int) loopsab::kNumRingModWaves - 1,
                                                        s->getIntAttribute ("ringModWave", scenes[i].ringModWave.load())));
            scenes[i].chaosDistribution.store (juce::jlimit (0, (int) loopsab::kNumChaosDistributions - 1,
                                                             s->getIntAttribute ("chaosDistribution", scenes[i].chaosDistribution.load())));
            scenes[i].feedbackCharacter.store (juce::jlimit (0, (int) loopsab::kNumFeedbackCharacters - 1,
                                                             s->getIntAttribute ("feedbackCharacter", scenes[i].feedbackCharacter.load())));
            scenes[i].stretchMode.store (juce::jlimit (0, (int) loopsab::kNumStretchModes - 1,
                                                       s->getIntAttribute ("stretchMode", scenes[i].stretchMode.load())));
            scenes[i].judderShape.store (juce::jlimit (0, (int) loopsab::kNumJudderShapes - 1,
                                                       s->getIntAttribute ("judderShape", scenes[i].judderShape.load())));
            scenes[i].lookbackBehaviour.store (juce::jlimit (0, (int) loopsab::kNumLookbackBehaviours - 1,
                                                       s->getIntAttribute ("lookbackBehaviour", scenes[i].lookbackBehaviour.load())));
            scenes[i].decayCurve.store (juce::jlimit (0, (int) loopsab::kNumDecayCurves - 1,
                                                       s->getIntAttribute ("decayCurve", scenes[i].decayCurve.load())));

            // v0.42 — per-Act stereo / FX-on-Dry / ring mod quantise.
            // Defaults fall through to the current slot value, which was
            // populated from the legacy uiEl globals broadcast earlier in
            // this function (see the setStereoMode/setRingModQuantise/etc.
            // calls), so older session files reload identically.
            scenes[i].stereoMode    .store (juce::jlimit (0, 3,
                                                           s->getIntAttribute ("stereoMode",
                                                                                scenes[i].stereoMode.load())));
            scenes[i].fxOnDryDrive  .store (s->getIntAttribute ("fxOnDryDrive",
                                                                 scenes[i].fxOnDryDrive.load() ? 1 : 0) != 0);
            scenes[i].fxOnDryTone   .store (s->getIntAttribute ("fxOnDryTone",
                                                                 scenes[i].fxOnDryTone.load() ? 1 : 0) != 0);
            scenes[i].fxOnDryRingMod.store (s->getIntAttribute ("fxOnDryRingMod",
                                                                 scenes[i].fxOnDryRingMod.load() ? 1 : 0) != 0);
            scenes[i].fxOnDryFold   .store (s->getIntAttribute ("fxOnDryFold",
                                                                 scenes[i].fxOnDryFold.load() ? 1 : 0) != 0);
            scenes[i].ringModQuant  .store (s->getIntAttribute ("ringModQuant",
                                                                 scenes[i].ringModQuant.load() ? 1 : 0) != 0);
        }
    }

    // Steps.
    if (auto* stepsEl = root->getChildByName ("Steps"))
    {
        seqLength.store (juce::jlimit (2, kMaxSteps,
                                        stepsEl->getIntAttribute ("length", kDefaultSteps)));

        // v0.10 — ratio table version. Missing attr means "v0.9 17-entry
        // table" and every step's ratio index needs to be remapped.
        const int ratioV = stepsEl->getIntAttribute ("ratioV", 1);

        // Start from a clean slate — we'll only overwrite the slots the
        // XML mentions, but any slot the XML skips should be blank.
        for (int i = 0; i < kMaxSteps; ++i)
        {
            stepRatio[i]      .store (0);
            stepFreezeHold[i] .store (false);
            stepRatchet[i]    .store (1);  // v0.12 default = no ratchet
            stepMute[i]       .store (false);  // v0.13 default = unmuted
            clearStepLocks (i);
        }

        for (auto* s : stepsEl->getChildIterator())
        {
            const int i = s->getIntAttribute ("i", -1);
            if (i < 0 || i >= kMaxSteps) continue;
            const int sceneIdx = s->getIntAttribute ("scene", -1);
            steps[i].store (sceneIdx >= -1 && sceneIdx < kNumScenes ? sceneIdx : -1);

            // v0.7 attributes. Chain the migrations so saves from any
            // prior version end up on the current v0.12+ table.
            //   ratioV absent/1 -> v0.9 17-entry visit-count table
            //   ratioV == 2     -> v0.10/v0.11 13-entry visit-count table
            //   ratioV >= 3     -> v0.12 16-entry pattern-play table
            int rawRatio = s->getIntAttribute ("ratio", 0);
            if (ratioV < 2)
                rawRatio = migrateRatioFromV09 (rawRatio);  // now returns v0.12 indices
            else if (ratioV < 3)
                rawRatio = migrateRatioFromV11 (rawRatio);
            stepRatio[i]     .store (juce::jlimit (0, kNumRatios - 1, rawRatio));
            stepFreezeHold[i].store (s->getIntAttribute ("freeze", 0) != 0);
            // v0.12 — per-step ratchet count. Absent attr = 1 (no
            // ratchet), which is what older saves should round-trip to.
            stepRatchet[i]   .store (clampRatchet (
                                        s->getIntAttribute ("rch", 1)));
            // v0.13 — per-step mute. Absent attr = unmuted (older saves).
            stepMute[i]      .store (s->getIntAttribute ("mute", 0) != 0);

            // v0.6 back-compat: sessions saved a float "chance" in 0..1
            // with -1 as the sentinel. Map to the closest ratio. Only
            // honour it if no v0.7 "ratio" attribute was present.
            if (! s->hasAttribute ("ratio") && s->hasAttribute ("chance"))
            {
                const float old = (float) s->getDoubleAttribute ("chance", -1.0);
                if (old >= 0.0f)
                {
                    // Pick the ratio whose (n/m) is closest to the old chance.
                    int bestIdx = 1;                   // default 1:1
                    float bestDist = 1.0e6f;
                    for (int r = 1; r < kNumRatios; ++r)
                    {
                        int n = 0, m = 0;
                        getRatioNM (r, n, m);
                        if (m == 0) continue;
                        const float ratioF = (float) n / (float) m;
                        const float d = std::abs (ratioF - old);
                        if (d < bestDist) { bestDist = d; bestIdx = r; }
                    }
                    stepRatio[i].store (bestIdx);
                }
            }

            // v0.7 p-lock children.
            for (auto* lockEl : s->getChildIterator())
            {
                if (! lockEl->hasTagName ("L")) continue;
                const int slot = lockEl->getIntAttribute ("slot", -1);
                if (slot < 0 || slot >= kNumLockableParams) continue;
                const float v = (float) lockEl->getDoubleAttribute ("v", 0.0);
                stepParamLock[i * kNumLockableParams + slot].store (v);
            }
        }
    }

    // Sequencer toggles.
    if (auto* seqEl = root->getChildByName ("Seq"))
    {
        seqOn        .store (seqEl->getIntAttribute ("on", 0) != 0);
        selectedScene.store (juce::jlimit (0, kNumScenes - 1,
                                            seqEl->getIntAttribute ("selected", 0)));
    }

    // v0.8 — editor-only prefs. Missing element means "session from
    // before v0.8"; defaults stay in place.
    if (auto* uiEl = root->getChildByName ("Ui"))
    {
        uiScale            .store ((float) juce::jlimit (0.5, 2.0,
                                                          uiEl->getDoubleAttribute ("scale", 1.0)));
        waveformVisible    .store (uiEl->getIntAttribute ("waveform",  1) != 0);
        tooltipsEnabled    .store (uiEl->getIntAttribute ("tooltips",  1) != 0);
        freezeMomentaryMode.store (uiEl->getIntAttribute ("freezeMom", 0) != 0);
        // v0.9 — autoFollowLength is restored LAST on purpose. If we
        // turned it on earlier in this function, the step-restore calls
        // would have flapped seqLength away from the saved value.
        autoFollowLength   .store (uiEl->getIntAttribute ("autoLen", 0) != 0);
        pageFollowsPlayhead.store (uiEl->getIntAttribute ("pageFollow", 0) != 0);
        gridAccentInterval .store (juce::jlimit (0, 32,
                                                  uiEl->getIntAttribute ("gridAccent", 4)));
        seqTransport       .store (juce::jlimit (0, 3,
                                                  uiEl->getIntAttribute ("seqTransport", 0)));
        // v0.12 — restore lookahead through the setter so setLatencySamples
        // and the sample-count recompute happen at the right sample rate.
        setLookaheadMs (uiEl->getIntAttribute ("lookaheadMs", 0));
        crushAntiAlias .store (uiEl->getIntAttribute ("crushAA", 1) != 0);  // v0.22
        crushMuLaw     .store (uiEl->getIntAttribute ("crushMu", 1) != 0);  // v0.22
        crushAllAudio  .store (uiEl->getIntAttribute ("crushAll", 0) != 0); // v0.22
        masterBypass   .store (uiEl->getIntAttribute ("bypass",   0) != 0); // v0.24
        ringModQuantise.store (uiEl->getIntAttribute ("rmQuant",  0) != 0); // v0.25
        stereoMode     .store (juce::jlimit (0, 3, uiEl->getIntAttribute ("stereoMode", 0)));
        globalDrive    .store (uiEl->getIntAttribute ("globalDrive",     0) != 0);  // v0.25
        globalTone     .store (uiEl->getIntAttribute ("globalTone",      0) != 0);
        globalRingMod  .store (uiEl->getIntAttribute ("globalRingMod",   0) != 0);
        globalFold     .store (uiEl->getIntAttribute ("globalFold",      0) != 0);
        preserveLfosOnClear.store (uiEl->getIntAttribute ("preserveLfos", 0) != 0); // v0.38

        // v0.42 — migration: if the Scenes block didn't carry per-Act
        // stereo/FX-on-Dry/ringModQuant attrs (pre-v0.42 save), broadcast
        // the legacy globals we just loaded into every scene so behaviour
        // matches the old plugin. v0.42+ saves set Scenes/@version=2 and
        // skip this step, preserving the per-Act values loaded above.
        if (! scenesCarryPerActFx)
        {
            const int   legacyStereo      = stereoMode.load();
            const bool  legacyFxDrive     = globalDrive.load();
            const bool  legacyFxTone      = globalTone.load();
            const bool  legacyFxRingMod   = globalRingMod.load();
            const bool  legacyFxFold      = globalFold.load();
            const bool  legacyRingQuant   = ringModQuantise.load();
            for (auto& sc : scenes)
            {
                sc.stereoMode    .store (legacyStereo);
                sc.fxOnDryDrive  .store (legacyFxDrive);
                sc.fxOnDryTone   .store (legacyFxTone);
                sc.fxOnDryRingMod.store (legacyFxRingMod);
                sc.fxOnDryFold   .store (legacyFxFold);
                sc.ringModQuant  .store (legacyRingQuant);
            }
        }
    }

    // v0.40 — ENGINE block. If absent, the state is from before v0.40 and
    // we keep current Engine settings unchanged (no migration noise).
    if (auto* engineEl = root->getChildByName ("Engine"))
    {
        const int   wantedInterp = juce::jlimit (0, (int) loopsab::kNumInterpModes - 1,
                                                  engineEl->getIntAttribute ("interpMode",
                                                                              loopsab::kInterpLinear));
        const int   wantedStage  = juce::jlimit (0, (int) loopsab::kNumOutputStages - 1,
                                                  engineEl->getIntAttribute ("outputStage",
                                                                              loopsab::kOutClean));
        const float wantedMix    = juce::jlimit (0.0f, 1.0f,
                                                  (float) engineEl->getDoubleAttribute ("outputMix", 1.0));
        const float wantedIntensity = juce::jlimit (0.0f, 1.0f,
                                                  (float) engineEl->getDoubleAttribute ("outputIntensity", 0.5));
        // The presetRecallEngine flag itself is always restored — it's a
        // user preference, not an Engine value. On session restore we
        // always apply the Engine values: they represent the user's own
        // saved state, not an incoming preset. The "Designed for X" badge
        // is only populated on explicit preset loads (see applyUserPreset).
        // v0.42 — previously we stashed designed-for here whenever
        // v0.5.0 — presetRecallEngine removed; presets always include
        // Engine+Character settings now.

        // v0.41 — broadcast the legacy single-engine bundle to every Act so
        // older (pre-v0.41) sessions reload identically. Per-Act values
        // written by v0.41+ saves overwrite these below when the Scene
        // blocks are read, so this is effectively a migration fallback.
        interpMode.store (wantedInterp);
        for (auto& sc : scenes)
        {
            sc.interpMode    .store (wantedInterp);
            sc.engineStage   .store (wantedStage);
            sc.engineMix     .store (wantedMix);
            sc.engineIntensity.store (wantedIntensity);
        }
        // Clear designed-for — no preset has been loaded in this session
        // restore, so no "Designed for X" banner should be shown.
        designedForInterp.store (-1);
        designedForStage .store (-1);
        designedForMix   .store (-1.0f);
    }

    // v0.30 — LFO modulation settings. Now per-Act: 8 Acts × 4 LFOs = 32 total.
    // Back-compat: old saves with global 2-LFO system get migrated to scene=0.
    for (auto* lfoEl : root->getChildIterator())
    {
        if (! lfoEl->hasTagName ("Lfo")) continue;
        const int si = lfoEl->getIntAttribute ("scene", 0);  // Back-compat: default to scene 0
        const int li = lfoEl->getIntAttribute ("idx", -1);
        if (si < 0 || si >= kNumScenes || li < 0 || li >= kNumLfosPerAct) continue;
        lfos[si][li].shape     .store (juce::jlimit (0, kNumLfoShapes - 1, lfoEl->getIntAttribute ("shape", 0)));
        lfos[si][li].rateIdx   .store (juce::jlimit (0, 20, lfoEl->getIntAttribute ("rate", 4)));
        // v0.34 — depth is now bipolar. Old saves used 0..1; a clamp to
        // [-1, +1] keeps them valid and interpreted as positive polarity.
        lfos[si][li].depth     .store (juce::jlimit (-1.0f, 1.0f, (float) lfoEl->getDoubleAttribute ("depth", 0.0)));
        lfos[si][li].targetSlot.store (juce::jlimit (-1, kNumLockableParams - 1, lfoEl->getIntAttribute ("target", -1)));
        // v0.34 — optional envelope + sync fields. Defaults match the
        // in-memory struct defaults so older saves load cleanly.
        lfos[si][li].sync      .store (lfoEl->getIntAttribute ("sync", 1) != 0);
        lfos[si][li].attackMs  .store (juce::jlimit (0.1f, 2000.0f, (float) lfoEl->getDoubleAttribute ("attack", 5.0)));
        lfos[si][li].holdMs    .store (juce::jlimit (0.0f, 2000.0f, (float) lfoEl->getDoubleAttribute ("hold", 0.0)));
        lfos[si][li].decayMs   .store (juce::jlimit (0.1f, 2000.0f, (float) lfoEl->getDoubleAttribute ("decay", 200.0)));
        lfos[si][li].trigMode  .store (juce::jlimit (0, kNumTrigModes - 1, lfoEl->getIntAttribute ("trig", (int) kTrigActiveSteps)));
        // v0.38 — cross-mod. Default -1 / 0.0 so saves without these
        // attributes (anything pre-v0.38) restore to "no cross-mod".
        lfos[si][li].crossTargetLfo.store (juce::jlimit (-1, kNumLfosPerAct - 1,
                                                         lfoEl->getIntAttribute ("xTarget", -1)));
        lfos[si][li].crossDepth    .store (juce::jlimit (-1.0f, 1.0f,
                                                         (float) lfoEl->getDoubleAttribute ("xDepth", 1.0)));
        // v0.50 — envelope follower sensitivity gain
        lfos[si][li].envFollowGain .store (juce::jlimit (1.0f, 10.0f, (float) lfoEl->getDoubleAttribute ("envFollowGain", 1.0)));
    }
}

// ----------------------------------------------------------------------------
//  Act import / export — hand-editable XML snapshots of a single Act or
//  the whole 8-slot bank. Deliberately small and forward-compatible so
//  you can paste them into bug reports or version-control them.
// ----------------------------------------------------------------------------
std::unique_ptr<juce::XmlElement>
LoopSaboteurProcessor::serializeAct (int sceneIdx) const
{
    auto out = std::make_unique<juce::XmlElement> ("LpsbAct");
    // v0.41 — bumped to 2 to advertise the per-Act ENGINE attributes
    // (engineStage, engineMix, interpMode, crushAA, crushMu, crushAll).
    // v0.42 — bumped to 3 to advertise per-Act stereo mode, FX-on-Dry
    // flags (drive/tone/ringMod/fold) and ring mod quantise. Older
    // readers ignore unknown attrs so this stays forward-compatible;
    // v2 files lacking these attrs fall back to the loading Act's
    // current atomic value, matching the Engine-bundle migration policy.
    out->setAttribute ("version", 3);

    if (sceneIdx < 0 || sceneIdx >= kNumScenes)
        return out;

    const auto& s = scenes[sceneIdx];
    out->setAttribute ("division",  s.divisionIdx .load());
    out->setAttribute ("lookback",  s.lookbackIdx .load());
    out->setAttribute ("rate",      s.rateIdx     .load());
    out->setAttribute ("judder",    s.judderCount .load());
    out->setAttribute ("judderDiv", s.judderDivIdx.load());
    out->setAttribute ("pitch",    (double) s.pitchSt      .load());
    out->setAttribute ("slide",    (double) s.slideSt      .load());
    out->setAttribute ("decay",    (double) s.decay        .load());
    out->setAttribute ("reverse",  (double) s.reverseChance.load());
    out->setAttribute ("crunch",   (double) s.crunch       .load());
    out->setAttribute ("crushrate",(double) s.crushRate    .load());
    out->setAttribute ("mix",      (double) s.mix          .load());
    out->setAttribute ("chance",   (double) s.chance       .load());
    out->setAttribute ("drive",    (double) s.drive        .load());
    out->setAttribute ("tone",     (double) s.tone         .load());
    out->setAttribute ("feedback", (double) s.feedback     .load());
    out->setAttribute ("tape",     (double) s.tape         .load());
    out->setAttribute ("ringmod",  (double) s.ringMod      .load());
    out->setAttribute ("varispeed",(double) s.varispeed    .load());
    out->setAttribute ("stereo",   (double) s.stereo       .load());
    out->setAttribute ("stretch",  (double) s.stretch      .load());
    out->setAttribute ("shimmer",  (double) s.shimmer      .load());
    out->setAttribute ("res",      (double) s.res          .load());
    out->setAttribute ("fold",     (double) s.fold         .load());
    out->setAttribute ("gate",     (double) s.gate         .load());
    out->setAttribute ("smear",    (double) s.smear        .load());
    out->setAttribute ("stutter",  (double) s.stutter      .load());
    out->setAttribute ("chaos",    (double) s.chaos        .load());

    // v0.41 — per-Act ENGINE bundle.
    out->setAttribute ("engineStage",     s.engineStage.load());
    out->setAttribute ("engineMix",       (double) s.engineMix.load());
    out->setAttribute ("engineIntensity", (double) s.engineIntensity.load());
    out->setAttribute ("interpMode",      s.interpMode .load());
    out->setAttribute ("crushAA",     s.crushAA    .load() ? 1 : 0);
    out->setAttribute ("crushMu",     s.crushMu    .load() ? 1 : 0);
    out->setAttribute ("crushAll",    s.crushAll   .load() ? 1 : 0);

    // v0.5.0 — per-Act filter mode + frequency split.
    out->setAttribute ("filterMode",    s.filterMode   .load());
    out->setAttribute ("freqSplitMode", s.freqSplitMode.load());
    out->setAttribute ("freqSplitHz",   (double) s.freqSplitHz.load());
    out->setAttribute ("driveType",      s.driveType.load());
    out->setAttribute ("foldTopology",   s.foldTopology.load());
    out->setAttribute ("shimmerOctave",  s.shimmerOctave.load());
    out->setAttribute ("smearCharacter", s.smearCharacter.load());
    out->setAttribute ("stutterWindow",  s.stutterWindow.load());
    out->setAttribute ("varispeedCurve", s.varispeedCurve.load());
    out->setAttribute ("slideCurve",     s.slideCurve.load());
    out->setAttribute ("reverseMode",    s.reverseMode.load());
    out->setAttribute ("tapeMode",       s.tapeMode.load());
    out->setAttribute ("ringModWave",    s.ringModWave.load());
    out->setAttribute ("chaosDistribution", s.chaosDistribution.load());
    out->setAttribute ("feedbackCharacter", s.feedbackCharacter.load());
    out->setAttribute ("stretchMode",    s.stretchMode.load());
    out->setAttribute ("judderShape",    s.judderShape.load());
    out->setAttribute ("lookbackBehaviour", s.lookbackBehaviour.load());
    out->setAttribute ("decayCurve",    s.decayCurve.load());

    // v0.42 — per-Act stereo mode + FX-on-Dry + ring mod quantise.
    out->setAttribute ("stereoMode",     s.stereoMode    .load());
    out->setAttribute ("fxOnDryDrive",   s.fxOnDryDrive  .load() ? 1 : 0);
    out->setAttribute ("fxOnDryTone",    s.fxOnDryTone   .load() ? 1 : 0);
    out->setAttribute ("fxOnDryRingMod", s.fxOnDryRingMod.load() ? 1 : 0);
    out->setAttribute ("fxOnDryFold",    s.fxOnDryFold   .load() ? 1 : 0);
    out->setAttribute ("ringModQuant",   s.ringModQuant  .load() ? 1 : 0);

    // v0.38 — serialise the 4 MODs attached to this Act as child
    // elements. A MOD with rateIdx == 0 (Off) AND targetSlot < 0 is
    // skipped so the file stays tidy for acts that have no modulation.
    for (int li = 0; li < kNumLfosPerAct; ++li)
    {
        const auto& lfo = lfos[sceneIdx][li];
        const int rateIdx    = lfo.rateIdx.load();
        const int targetSlot = lfo.targetSlot.load();
        const int crossTgt   = lfo.crossTargetLfo.load();
        if (rateIdx == 0 && targetSlot < 0 && crossTgt < 0)
            continue;   // nothing to save
        auto* e = out->createNewChildElement ("Lfo");
        e->setAttribute ("idx",       li);
        e->setAttribute ("shape",     lfo.shape.load());
        e->setAttribute ("rate",      rateIdx);
        e->setAttribute ("depth",     (double) lfo.depth.load());
        e->setAttribute ("target",    targetSlot);
        e->setAttribute ("sync",      lfo.sync.load() ? 1 : 0);
        e->setAttribute ("attackMs",  (double) lfo.attackMs.load());
        e->setAttribute ("holdMs",    (double) lfo.holdMs.load());
        e->setAttribute ("decayMs",   (double) lfo.decayMs.load());
        e->setAttribute ("trigMode",  lfo.trigMode.load());
        e->setAttribute ("xTarget",   crossTgt);
        e->setAttribute ("xDepth",    (double) lfo.crossDepth.load());
        e->setAttribute ("envFollowGain", (double) lfo.envFollowGain.load());
    }
    return out;
}

bool LoopSaboteurProcessor::loadActFromXml (const juce::XmlElement& el, int targetIdx)
{
    if (! el.hasTagName ("LpsbAct"))                           return false;
    if (targetIdx < 0 || targetIdx >= kNumScenes)              return false;

    auto& s = scenes[targetIdx];
    s.divisionIdx  .store (juce::jlimit (0, 9,  el.getIntAttribute ("division",  2)));
    s.lookbackIdx  .store (juce::jlimit (0, 9,  el.getIntAttribute ("lookback",  3)));
    s.rateIdx      .store (juce::jlimit (0, 5,  el.getIntAttribute ("rate",      3)));
    s.judderCount  .store (juce::jlimit (1, kMaxJudder,
                                          el.getIntAttribute ("judder", 1)));
    s.judderDivIdx .store (juce::jlimit (0, 11, el.getIntAttribute ("judderDiv", 3)));
    s.pitchSt      .store ((float) el.getDoubleAttribute ("pitch",    0.0));
    s.slideSt      .store ((float) el.getDoubleAttribute ("slide",    0.0));
    s.decay        .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("decay",    0.8)));
    s.reverseChance.store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("reverse",  0.0)));
    s.crunch       .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("crunch",   0.0)));
    s.crushRate    .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("crushrate", 0.0)));
    s.mix          .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("mix",      0.5)));
    s.chance       .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("chance",   1.0)));
    s.drive        .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("drive",    0.0)));
    s.tone         .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("tone",     0.5)));
    s.feedback     .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("feedback", 0.0)));
    s.tape         .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("tape",     0.0)));
    s.ringMod      .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("ringmod",  0.0)));
    s.varispeed    .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("varispeed",0.0)));
    s.stereo       .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("stereo",   0.0)));
    s.stretch      .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("stretch",  0.0)));
    s.shimmer      .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("shimmer",  0.0)));
    s.res          .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("res",      0.0)));
    s.fold         .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("fold",     0.0)));
    s.gate         .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("gate",     0.0)));
    s.smear        .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("smear",    0.0)));
    s.stutter      .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("stutter",  0.0)));
    s.chaos        .store (juce::jlimit (0.0f, 1.0f, (float) el.getDoubleAttribute ("chaos",    0.0)));

    // v0.41 — per-Act ENGINE bundle. Defaults below preserve whatever
    // engine the Act is currently running so older .lpsbact files (which
    // never carried an engine bundle) load as a pure DSP-knob change
    // without disrupting the global character.
    s.engineStage.store (juce::jlimit (0, (int) loopsab::kNumOutputStages - 1,
                                        el.getIntAttribute ("engineStage", s.engineStage.load())));
    s.engineMix  .store (juce::jlimit (0.0f, 1.0f,
                                        (float) el.getDoubleAttribute ("engineMix",
                                                                        (double) s.engineMix.load())));
    s.engineIntensity.store (juce::jlimit (0.0f, 1.0f,
                                        (float) el.getDoubleAttribute ("engineIntensity", 0.5)));
    s.interpMode .store (juce::jlimit (0, (int) loopsab::kNumInterpModes - 1,
                                        el.getIntAttribute ("interpMode", s.interpMode.load())));
    s.crushAA    .store (el.getIntAttribute ("crushAA", s.crushAA.load() ? 1 : 0) != 0);
    s.crushMu    .store (el.getIntAttribute ("crushMu", s.crushMu.load() ? 1 : 0) != 0);
    s.crushAll   .store (el.getIntAttribute ("crushAll", s.crushAll.load() ? 1 : 0) != 0);

    // v0.5.0 — per-Act filter mode + frequency split.
    s.filterMode   .store (juce::jlimit (0, (int) loopsab::kNumFilterModes - 1,
                                          el.getIntAttribute ("filterMode", s.filterMode.load())));
    s.freqSplitMode.store (juce::jlimit (0, 2,
                                          el.getIntAttribute ("freqSplitMode", s.freqSplitMode.load())));
    s.freqSplitHz  .store (juce::jlimit (80.0f, 8000.0f,
                                          (float) el.getDoubleAttribute ("freqSplitHz",
                                                                          (double) s.freqSplitHz.load())));
    s.driveType.store (juce::jlimit (0, (int) loopsab::kNumDriveTypes - 1,
                                     el.getIntAttribute ("driveType", s.driveType.load())));
    s.foldTopology.store (juce::jlimit (0, (int) loopsab::kNumFoldTopologies - 1,
                                        el.getIntAttribute ("foldTopology", s.foldTopology.load())));
    s.shimmerOctave.store (juce::jlimit (0, (int) loopsab::kNumShimmerOctaves - 1,
                                         el.getIntAttribute ("shimmerOctave", s.shimmerOctave.load())));
    s.smearCharacter.store (juce::jlimit (0, (int) loopsab::kNumSmearCharacters - 1,
                                          el.getIntAttribute ("smearCharacter", s.smearCharacter.load())));
    s.stutterWindow.store (juce::jlimit (0, (int) loopsab::kNumStutterWindows - 1,
                                         el.getIntAttribute ("stutterWindow", s.stutterWindow.load())));
    s.varispeedCurve.store (juce::jlimit (0, (int) loopsab::kNumVarispeedCurves - 1,
                                          el.getIntAttribute ("varispeedCurve", s.varispeedCurve.load())));
    s.slideCurve.store (juce::jlimit (0, (int) loopsab::kNumSlideCurves - 1,
                                      el.getIntAttribute ("slideCurve", s.slideCurve.load())));
    s.reverseMode.store (juce::jlimit (0, (int) loopsab::kNumReverseModes - 1,
                                       el.getIntAttribute ("reverseMode", s.reverseMode.load())));
    s.tapeMode.store (juce::jlimit (0, (int) loopsab::kNumTapeModes - 1,
                                     el.getIntAttribute ("tapeMode", s.tapeMode.load())));
    s.ringModWave.store (juce::jlimit (0, (int) loopsab::kNumRingModWaves - 1,
                                        el.getIntAttribute ("ringModWave", s.ringModWave.load())));
    s.chaosDistribution.store (juce::jlimit (0, (int) loopsab::kNumChaosDistributions - 1,
                                             el.getIntAttribute ("chaosDistribution", s.chaosDistribution.load())));
    s.feedbackCharacter.store (juce::jlimit (0, (int) loopsab::kNumFeedbackCharacters - 1,
                                             el.getIntAttribute ("feedbackCharacter", s.feedbackCharacter.load())));
    s.stretchMode.store (juce::jlimit (0, (int) loopsab::kNumStretchModes - 1,
                                       el.getIntAttribute ("stretchMode", s.stretchMode.load())));
    s.judderShape.store (juce::jlimit (0, (int) loopsab::kNumJudderShapes - 1,
                                       el.getIntAttribute ("judderShape", s.judderShape.load())));
    s.lookbackBehaviour.store (juce::jlimit (0, (int) loopsab::kNumLookbackBehaviours - 1,
                                       el.getIntAttribute ("lookbackBehaviour", s.lookbackBehaviour.load())));
    s.decayCurve.store (juce::jlimit (0, (int) loopsab::kNumDecayCurves - 1,
                                       el.getIntAttribute ("decayCurve", s.decayCurve.load())));

    // v0.42 — per-Act stereo mode / FX-on-Dry flags / ring mod quantise.
    // Defaults preserve the target slot's current values so v2 (and v1)
    // .lpsbact files load cleanly without disturbing these new per-Act
    // settings; only the DSP-knob values get overwritten.
    s.stereoMode    .store (juce::jlimit (0, 3,
                                          el.getIntAttribute ("stereoMode", s.stereoMode.load())));
    s.fxOnDryDrive  .store (el.getIntAttribute ("fxOnDryDrive",   s.fxOnDryDrive  .load() ? 1 : 0) != 0);
    s.fxOnDryTone   .store (el.getIntAttribute ("fxOnDryTone",    s.fxOnDryTone   .load() ? 1 : 0) != 0);
    s.fxOnDryRingMod.store (el.getIntAttribute ("fxOnDryRingMod", s.fxOnDryRingMod.load() ? 1 : 0) != 0);
    s.fxOnDryFold   .store (el.getIntAttribute ("fxOnDryFold",    s.fxOnDryFold   .load() ? 1 : 0) != 0);
    s.ringModQuant  .store (el.getIntAttribute ("ringModQuant",   s.ringModQuant  .load() ? 1 : 0) != 0);

    // v0.38 — MODs. Reset all 4 to defaults first so missing MOD
    // elements behave as "no modulation" even if the Act previously
    // had MODs wired up. Then apply whatever the file supplies.
    resetLfosForScene (targetIdx);
    for (auto* lfoEl : el.getChildIterator())
    {
        if (! lfoEl->hasTagName ("Lfo")) continue;
        const int li = lfoEl->getIntAttribute ("idx", -1);
        if (li < 0 || li >= kNumLfosPerAct) continue;
        auto& lfo = lfos[targetIdx][li];
        lfo.shape         .store (juce::jlimit (0, kNumLfoShapes - 1,
                                                lfoEl->getIntAttribute ("shape", 0)));
        lfo.rateIdx       .store (juce::jlimit (0, 20,
                                                lfoEl->getIntAttribute ("rate", 0)));
        lfo.depth         .store (juce::jlimit (-1.0f, 1.0f,
                                                (float) lfoEl->getDoubleAttribute ("depth", 1.0)));
        lfo.targetSlot    .store (juce::jlimit (-1, kNumLockableParams - 1,
                                                lfoEl->getIntAttribute ("target", -1)));
        lfo.sync          .store (lfoEl->getIntAttribute ("sync", 1) != 0);
        lfo.attackMs      .store ((float) lfoEl->getDoubleAttribute ("attackMs", 5.0));
        lfo.holdMs        .store ((float) lfoEl->getDoubleAttribute ("holdMs",   0.0));
        lfo.decayMs       .store ((float) lfoEl->getDoubleAttribute ("decayMs", 200.0));
        lfo.trigMode      .store (juce::jlimit (0, (int) kNumTrigModes - 1,
                                                lfoEl->getIntAttribute ("trigMode", 0)));
        lfo.crossTargetLfo.store (juce::jlimit (-1, kNumLfosPerAct - 1,
                                                lfoEl->getIntAttribute ("xTarget", -1)));
        lfo.crossDepth    .store (juce::jlimit (-1.0f, 1.0f,
                                                (float) lfoEl->getDoubleAttribute ("xDepth", 1.0)));
        lfo.envFollowGain .store (juce::jlimit (1.0f, 10.0f, (float) lfoEl->getDoubleAttribute ("envFollowGain", 1.0)));
    }

    sceneStored[targetIdx].store (true);
    scenePresetIdx[targetIdx].store (-1);  // BUG 1 fix: loaded from file, not a factory preset
    return true;
}

std::unique_ptr<juce::XmlElement>
LoopSaboteurProcessor::serializeAllActs() const
{
    auto out = std::make_unique<juce::XmlElement> ("LpsbActBank");
    out->setAttribute ("version", 1);
    for (int i = 0; i < kNumScenes; ++i)
    {
        if (auto child = serializeAct (i))
        {
            child->setAttribute ("slot", i);
            child->setAttribute ("stored", sceneStored[i].load() ? 1 : 0);
            out->addChildElement (child.release());
        }
    }
    return out;
}

bool LoopSaboteurProcessor::loadAllActsFromXml (const juce::XmlElement& el)
{
    if (! el.hasTagName ("LpsbActBank")) return false;
    int loaded = 0;
    for (auto* child : el.getChildIterator())
    {
        if (! child->hasTagName ("LpsbAct")) continue;
        const int slot = child->getIntAttribute ("slot", -1);
        if (slot < 0 || slot >= kNumScenes) continue;
        if (loadActFromXml (*child, slot))
        {
            sceneStored[slot].store (child->getIntAttribute ("stored", 1) != 0);
            ++loaded;
        }
    }
    return loaded > 0;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LoopSaboteurProcessor();
}
