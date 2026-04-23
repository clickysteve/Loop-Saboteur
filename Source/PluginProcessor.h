#pragma once

#include <JuceHeader.h>
#include <limits>
#include "OutputStage.h"

// ============================================================================
//  LoopSaboteurProcessor
//
//  Grid-locked breakbeat chopper with an Act-based sequencer.
//
//  FREEHAND MODE (SEQ off): on every DIVISION boundary of the host
//  transport we roll against CHANCE. If it fires, a monophonic voice
//  grabs a slice of length DIVISION from the rolling buffer, starting
//  exactly LOOKBACK bars behind the current write head. Division here
//  drives both the trigger grid AND the slice length.
//
//  SEQUENCER MODE (SEQ on): the sequencer advances one step per global
//  SEQ RATE boundary (1/32..1 bar). At each step we look up which Act
//  (scene slot A..H) is parked there and, if any, fire that Act's
//  stored ShapingParams — crucially including ITS OWN Division. So the
//  sequencer ticks at one global pace but each Act chops at whatever
//  slice size it was stored with, giving you 1/8 steps where A hits a
//  1/4 slice and D hits a 1/16 slice in the same bar.
//
//  JUDDER retriggers the slice at the JUDDER DIV rate for a rhythmic
//  stutter. PITCH and SLIDE speed-shift the slice and apply per-judder
//  drift. MIX controls how loud the chop layers on top of the dry.
//
//  v0.7 additions:
//    * 64-step sequencer with two-page (A/B) paging
//    * N:M ratio probability locks per step
//    * Per-step parameter locks on every knob (shift+click a step to edit)
//    * Per-step freeze marker (cmd+click) + momentary/toggle freeze mode
//    * Scale-quantise on PITCH (root + scale type, global)
//    * Input peak strip feeding a waveform display above the sequencer
//
//  v0.8 changes:
//    * Sequencer bumped to 128 steps (four pages: A/B/C/D, 32 each)
//    * Ratio table trimmed to 17 entries (dropped the no-op {1,1})
//    * Ratio counter only advances on step-change, not per-fire, so
//      Division < Step Rate no longer burns through N:M counts too fast
//    * Persisted editor preferences (UI scale, waveform visibility,
//      freeze momentary mode) serialised alongside APVTS state
// ============================================================================
class LoopSaboteurProcessor : public juce::AudioProcessor,
                              public juce::AudioProcessorValueTreeState::Listener
{
public:
    LoopSaboteurProcessor();
    ~LoopSaboteurProcessor() override;

    // v0.10 — APVTS listener. Used to mirror any knob change into the
    // currently-selected Act's stored values, so the explicit STORE
    // button is no longer needed. Marks sceneStored[selectedScene].
    void parameterChanged (const juce::String& paramId, float newValue) override;

    // --- AudioProcessor plumbing ----------------------------------------
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

   #ifndef JucePlugin_PreferredChannelConfigurations
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
   #endif

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                        { return true; }

    const juce::String getName() const override            { return JucePlugin_Name; }

    bool acceptsMidi() const override                      { return false; }
    bool producesMidi() const override                     { return false; }
    bool isMidiEffect() const override                     { return false; }
    // v0.9.1 — infinite tail so the DAW never stops calling processBlock
    // during silence. Loop Saboteur's circular buffer, sequencer, and LFOs
    // must keep running even when input is quiet — otherwise everything
    // freezes after a few bars of silence.
    double getTailLengthSeconds() const override           { return std::numeric_limits<double>::infinity(); }

    int getNumPrograms() override                          { return 1; }
    int getCurrentProgram() override                       { return 0; }
    void setCurrentProgram (int) override                  {}
    const juce::String getProgramName (int) override       { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    // --- sequencer capacity (public so the editor can size its grid) ---
    static constexpr int kNumScenes    = 8;   // act slots A..H
    static constexpr int kMaxSteps     = 64;  // four pages of 16 (A/B/C/D)
    static constexpr int kStepsPerPage = 16;  // one editor page
    static constexpr int kNumPages     = kMaxSteps / kStepsPerPage;
    static constexpr int kDefaultSteps = 16;  // power-on default
    static constexpr int kMaxJudder    = 32;  // pushed upper bound — wilder tails

    // ShapingParams is the set of per-fire values a voice needs. A
    // fire can come from either the current knobs (freehand) or a
    // stored scene (sequencer). Keeping it as a plain POD means
    // fireVoice has one code path and no branching on source type.
    struct ShapingParams
    {
        int   divisionIdx   = 2;   // 1/16 — per-Act
        int   lookbackIdx   = 3;   // 1/16 (index shifted after prepending 1/128)
        int   rateIdx       = 3;   // 1x
        int   judderCount   = 1;
        int   judderDivIdx  = 3;   // 1/32 (after adding 1/128 + triplets)
        float pitchSt       = 0.0f;
        float slideSt       = 0.0f;
        float decay         = 0.8f;
        float reverseChance = 0.0f;
        float crunch        = 0.0f;   // bit-depth reduction (0 = 16-bit, 1 = ~2-bit)
        float crushRate     = 0.0f;   // v0.21 — S&H decimation (0 = off, 1 = extreme)
        float mix           = 0.5f;
        float globalMix     = 0.5f;   // v0.42.6 — session-wide scaler (not per-Act)

        // Tape/sampler FX row — all per-Act, all bake into scenes.
        float drive         = 0.0f;  // pre-voice tanh saturation
        float tone          = 0.5f;  // bipolar: 0=dark, 0.5=bypass, 1=bright
        float feedback      = 0.0f;  // voice output re-injected into circular buffer
        float tape          = 0.0f;  // v0.24 — combined wow+flutter tape degradation
        float ringMod       = 0.0f;  // v0.24 — sine ring mod depth/freq
        float varispeed     = 0.0f;  // v0.24 — DJ brake/spinup amount
        float stereo        = 0.0f;  // v0.24 — stereo spread per-slice
        float stretch       = 0.0f;  // v0.25 — granular time-stretch

        // Mangle/destruction row — second FX row (v0.6).
        float shimmer       = 0.0f;  // aliased-residual HF sparkle bit-crush
        float res           = 0.0f;  // SVF resonance, pairs with TONE
        float fold          = 0.0f;  // sine wavefolder
        float gate          = 0.0f;  // Shape: -1=Soft(LPG), 0=bypass, +1=Snappy
        float smear         = 0.0f;  // short delay blur feedback amount
        float stutter       = 0.0f;  // v0.22 — micro-loop size (0 = off, 1 = tiny)
        float chaos         = 0.0f;  // probabilistic sample glitching

        // v0.41 — per-Act ENGINE bundle. Output stage colour, output
        // wet/dry, slice playback interpolation, and crush-mode flags
        // are now baked per-Act so each preset carries its own character.
        // engineValid=false means "this fire used live knobs" / fall back
        // to global/processor-level engine settings (kept for the master
        // bus only — voice bake always uses these values).
        int   engineStage      = 0;     // OutputStage::Mode
        float engineMix        = 0.0f;  // 0..1
        float engineIntensity  = 0.5f;  // 0..1, v0.7.x ENGINE colour intensity
        int   interpMode    = 0;     // loopsab::InterpMode
        bool  crushAA       = true;  // post-crunch anti-alias LP
        bool  crushMu       = true;  // mu-law companding
        bool  crushAll      = false; // crush master output, not just slices

        // v0.5.0 — per-Act filter mode and frequency split.
        int   filterMode    = 0;     // loopsab::FilterMode (Tilt default)
        int   freqSplitMode = 0;     // 0=Full, 1=Low Only, 2=High Only
        float freqSplitHz   = 500.0f;

        // v0.42 — per-Act stereo mode + FX-on-Dry flags + ring-mod quantise.
        // These used to be global processor atomics (`stereoMode`, `globalDrive`
        // etc.) but the user reasonably expected every sound-shaping choice
        // that affects the character of an Act to travel with the Act. The
        // globals still exist as a back-compat mirror so legacy session XMLs
        // keep loading; the Settings page now drives each Act's slot directly.
        int   stereoMode     = 0;     // StereoMode enum (0..3)
        bool  fxOnDryDrive   = false; // apply DRIVE to whole stream
        bool  fxOnDryTone    = false; // apply TONE  to whole stream
        bool  fxOnDryRingMod = false; // apply RING MOD to whole stream
        bool  fxOnDryFold    = false; // apply FOLD  to whole stream
        bool  ringModQuant   = false; // quantise ring mod to scale

        // v0.5.0 — per-Act CHARACTER options (right-click knob modes).
        int   driveType      = 0;     // loopsab::DriveType (Tape default)
        int   foldTopology   = 0;     // loopsab::FoldTopology (Sine default)
        int   shimmerOctave  = 0;     // loopsab::ShimmerOctave (+1 Oct default)
        int   smearCharacter = 0;     // loopsab::SmearCharacter (Blur default)
        int   stutterWindow  = 0;     // loopsab::StutterWindow (Fixed default)
        int   varispeedCurve = 0;     // loopsab::VarispeedCurve (Linear default)
        int   slideCurve     = 0;     // loopsab::SlideCurve (Linear default)
        int   reverseMode    = 0;     // loopsab::ReverseMode (Random default)

        // v0.5.0 wave 2 — more CHARACTER options.
        int   tapeMode          = 0;  // loopsab::TapeMode (Classic default)
        int   ringModWave       = 0;  // loopsab::RingModWave (Sine default)
        int   chaosDistribution = 0;  // loopsab::ChaosDistribution (Uniform default)
        int   feedbackCharacter = 0;  // loopsab::FeedbackCharacter (Clean default)
        int   stretchMode       = 0;  // loopsab::StretchMode (Standard default)
        int   judderShape       = 0;  // loopsab::JudderShape (Even default)
        int   lookbackBehaviour = 0;  // loopsab::LookbackBehaviour (Fixed default)
        int   decayCurve        = 0;  // loopsab::DecayCurve (Linear default)
    };

    // One scene = one stored ShapingParams snapshot, but held as
    // atomics so the audio thread can read it lock-free.
    struct SceneData
    {
        std::atomic<int>   divisionIdx   { 2 };   // per-Act Division
        std::atomic<int>   lookbackIdx   { 3 };   // 1/16 after 1/128 prepend
        std::atomic<int>   rateIdx       { 3 };
        std::atomic<int>   judderCount   { 1 };
        std::atomic<int>   judderDivIdx  { 3 };   // 1/32 in the extended list
        std::atomic<float> pitchSt       { 0.0f };
        std::atomic<float> slideSt       { 0.0f };
        std::atomic<float> decay         { 0.8f };
        std::atomic<float> reverseChance { 0.0f };
        std::atomic<float> crunch        { 0.0f };
        std::atomic<float> crushRate     { 0.0f };   // v0.21
        std::atomic<float> mix           { 0.5f };
        std::atomic<float> chance        { 1.0f }; // per-Act trigger probability

        // FX row — per-Act saturation, tone, feedback, pitch wobble, hiss.
        std::atomic<float> drive         { 0.0f };
        std::atomic<float> tone          { 0.5f };
        std::atomic<float> feedback      { 0.0f };
        std::atomic<float> tape          { 0.0f };  // v0.24 — combined wow+flutter
        std::atomic<float> ringMod       { 0.0f };  // v0.24
        std::atomic<float> varispeed     { 0.0f };  // v0.24
        std::atomic<float> stereo        { 0.0f };  // v0.24
        std::atomic<float> stretch       { 0.0f };  // v0.25

        // Mangle row — per-Act shimmer/res/fold/gate/smear/chaos.
        std::atomic<float> shimmer       { 0.0f };
        std::atomic<float> res           { 0.0f };
        std::atomic<float> fold          { 0.0f };
        std::atomic<float> gate          { 0.0f };
        std::atomic<float> smear         { 0.0f };
        std::atomic<float> stutter       { 0.0f };   // v0.22
        std::atomic<float> chaos         { 0.0f };

        // v0.41 — per-Act ENGINE bundle. See ShapingParams above for
        // semantics. Defaults match the previous global defaults so an
        // un-configured Act behaves like the legacy single-engine plugin.
        std::atomic<int>   engineStage      { 0 };       // OutputStage::Mode (Off)
        std::atomic<float> engineMix        { 0.0f };    // dry by default
        std::atomic<float> engineIntensity  { 0.5f };    // colour intensity
        std::atomic<int>   interpMode    { 0 };       // kInterpLinear
        std::atomic<bool>  crushAA       { true };
        std::atomic<bool>  crushMu       { true };
        std::atomic<bool>  crushAll      { false };

        // v0.5.0 — per-Act filter mode (Tilt/LP/HP/BP) and frequency split.
        std::atomic<int>   filterMode    { 0 };       // loopsab::kFilterTilt
        std::atomic<int>   freqSplitMode { 0 };       // 0=Full, 1=Low Only, 2=High Only
        std::atomic<float> freqSplitHz   { 500.0f };  // crossover frequency

        // v0.42 — per-Act stereo mode + FX-on-Dry flags + ring mod quantise.
        // See ShapingParams above for rationale. Defaults match the previous
        // global defaults so an un-configured Act behaves like legacy.
        std::atomic<int>   stereoMode     { 0 };      // kStereoRandom
        std::atomic<bool>  fxOnDryDrive   { false };
        std::atomic<bool>  fxOnDryTone    { false };
        std::atomic<bool>  fxOnDryRingMod { false };
        std::atomic<bool>  fxOnDryFold    { false };
        std::atomic<bool>  ringModQuant   { false };

        // v0.5.0 — per-Act CHARACTER options (right-click knob modes).
        std::atomic<int>   driveType      { 0 };      // loopsab::kDriveTape
        std::atomic<int>   foldTopology   { 0 };      // loopsab::kFoldSine
        std::atomic<int>   shimmerOctave  { 0 };      // loopsab::kShimmerUp1
        std::atomic<int>   smearCharacter { 0 };      // loopsab::kSmearBlur
        std::atomic<int>   stutterWindow  { 0 };      // loopsab::kStutterFixed
        std::atomic<int>   varispeedCurve { 0 };      // loopsab::kVariLinear
        std::atomic<int>   slideCurve     { 0 };      // loopsab::kSlideLinear
        std::atomic<int>   reverseMode    { 0 };      // loopsab::kReverseRandom

        // v0.5.0 wave 2 — more CHARACTER options.
        std::atomic<int>   tapeMode          { 0 };  // loopsab::kTapeClassic
        std::atomic<int>   ringModWave       { 0 };  // loopsab::kRingSine
        std::atomic<int>   chaosDistribution { 0 };  // loopsab::kChaosUniform
        std::atomic<int>   feedbackCharacter { 0 };  // loopsab::kFeedbackClean
        std::atomic<int>   stretchMode       { 0 };  // loopsab::kStretchStandard
        std::atomic<int>   judderShape       { 0 };  // loopsab::kJudderEven
        std::atomic<int>   lookbackBehaviour { 0 };  // loopsab::kLookbackFixed
        std::atomic<int>   decayCurve        { 0 };  // loopsab::kDecayLinear

        // v0.7.0 — Cage 4'33" easter egg: when true, output is zeroed.
        std::atomic<bool>  cageSilence       { false };
    };

    // Editor-thread API for scene + grid management. These are
    // called from the message thread when the user clicks stuff;
    // they write atomics that the audio thread reads.
    void storeKnobsAsScene (int sceneIdx);       // snapshot current APVTS into scene i
    void loadSceneToKnobs  (int sceneIdx);       // push scene i values into APVTS
    // v0.10 — reset an Act back to the "pass-through" defaults (chance
    // and mix at 0 so the Act makes no audible change, plus every FX
    // knob at its neutral position). Also clears the "stored" flag so
    // the Act button stops showing the populated dot.
    void resetSceneToDefaults (int sceneIdx);
    // v0.11 — wipe every Act back to defaults. Used by the ACT I/O menu.
    void resetAllScenesToDefaults();

    // v0.38 — reset just the 4 LFOs (MODs) for one Act back to defaults:
    // rate=Off, depth=1.0, target=none, shape=Sine, sync=true, AHD times
    // at the factory values, no cross-mod. Called by resetSceneToDefaults
    // when preserveLfosOnClear is false (the default).
    void resetLfosForScene (int sceneIdx);

    // v0.38 — user preference: when clearing an Act (either via the per-
    // Act Clear button or "Clear all Acts"), should we also wipe the 4
    // MODs attached to it? Default = false ⇒ MODs are cleared alongside
    // the Act, which is the intuitive behaviour. Set true to keep your
    // modulation rigs around while you iterate on the Act knobs below.
    void setPreserveLfosOnClear (bool preserve) noexcept { preserveLfosOnClear.store (preserve); }
    bool getPreserveLfosOnClear () const noexcept        { return preserveLfosOnClear.load(); }

    // v0.13 — factory presets for single-Act slots. A handful of named
    // starting points that give a known character. Callers select by
    // index into the kActPresets table; pass -1 for "count" mode.
    //
    // v0.38 — presets now carry their 4 MOD configurations too. Existing
    // aggregate-initialised entries get the default (all-Off) lfos array
    // filled in by the compiler, so none of them need touching. Only
    // presets that actually use modulation (like "Pretty Machine") need
    // to spell out non-default values for any of the four slots.
    struct LfoPresetEntry
    {
        int   shape          = 0;      // kLfoSine
        int   rateIdx        = 0;      // 0 = Off (no modulation)
        float depth          = 1.0f;   // bipolar -1..+1
        int   targetSlot     = -1;     // -1 = none
        bool  sync           = true;
        float attackMs       = 5.0f;
        float holdMs         = 0.0f;
        float decayMs        = 200.0f;
        int   trigMode       = 0;      // kTrigSlice
        int   crossTargetLfo = -1;     // no cross-mod
        float crossDepth     = 1.0f;
        float envFollowGain  = 1.0f;   // 1.0..10.0 sensitivity
    };

    struct ActPreset
    {
        const char* category;             // folder label in the menu
        const char* name;                 // menu label
        int   divisionIdx, lookbackIdx, rateIdx, judderCount, judderDivIdx;
        float pitchSt, slideSt, decay, reverseChance, crunch, crushRate, mix, chance;
        float drive, tone, feedback, tape, ringMod, varispeed, stereo, stretch;
        float shimmer, res, fold, gate, smear, stutter, chaos;
        // v0.43 — optional FX-on-dry flags. Defaults to all off so
        // existing aggregate-init entries compile unchanged.
        bool crushAll      = false;
        bool fxOnDryDrive  = false;
        bool fxOnDryTone   = false;
        bool fxOnDryRingMod= false;
        bool fxOnDryFold   = false;
        // v0.38 — optional MOD section. Defaults leave all 4 LFOs Off
        // so old aggregate-init entries continue to compile unchanged
        // and behave identically (no modulation applied).
        LfoPresetEntry lfos[4] = {};

        // v0.7.0 — ENGINE settings. Defaults match SceneData so old
        // aggregate-init entries compile unchanged and behave identically.
        int   engineStage      = 0;
        float engineMix        = 0.0f;
        float engineIntensity  = 0.5f;
        int   interpMode    = 0;
        bool  presetCrushAA = true;    // "preset" prefix avoids shadowing
        bool  presetCrushMu = true;
        int   filterMode    = 0;
        int   freqSplitMode = 0;
        float freqSplitHz   = 500.0f;

        // v0.7.0 — CHARACTER settings (right-click knob modes).
        int   stereoMode        = 0;
        bool  ringModQuant      = false;
        int   driveType         = 0;
        int   foldTopology      = 0;
        int   shimmerOctave     = 0;
        int   smearCharacter    = 0;
        int   stutterWindow     = 0;
        int   varispeedCurve    = 0;
        int   slideCurve        = 0;
        int   reverseMode       = 0;
        int   tapeMode          = 0;
        int   ringModWave       = 0;
        int   chaosDistribution = 0;
        int   feedbackCharacter = 0;
        int   stretchMode       = 0;
        int   judderShape       = 0;
        int   lookbackBehaviour = 0;
        int   decayCurve        = 0;
    };
    static int              getNumActPresets() noexcept;
    static const ActPreset& getActPreset (int idx) noexcept;
    void applyActPreset (int sceneIdx, int presetIdx);

    // v0.38 — user-saved presets. These are .lpsbact files that live in
    // a per-user folder; on startup we scan the folder and merge them
    // into the preset menu under a "User" category. Saving prompts for
    // a name and writes the file; the menu auto-refreshes.
    //
    // getUserPresetsDir  — the folder we read/write to.
    // saveCurrentActAsUserPreset — snapshot the selected Act and write
    //   it to the user folder with the given display name.
    // rescanUserPresets  — reload the file list; call after save or on
    //   startup. Thread-safe to call from the message thread.
    // getNumUserPresets / getUserPresetName / getUserPresetFile —
    //   accessors the editor uses to build the preset menu.
    // applyUserPreset    — load the nth user preset into the given Act.
    // deleteUserPreset   — remove the file from disk, then rescan.
    static juce::File getUserPresetsDir();
    // v0.7.0 — optional category puts the file in a sub-folder.
    bool  saveCurrentActAsUserPreset (int sceneIdx, const juce::String& displayName,
                                      const juce::String& category = {});
    void  rescanUserPresets();
    int   getNumUserPresets() const noexcept;
    juce::String getUserPresetName (int idx) const;
    juce::String getUserPresetCategory (int idx) const;
    juce::File   getUserPresetFile (int idx) const;
    bool  applyUserPreset (int sceneIdx, int userIdx);
    bool  deleteUserPreset (int userIdx);
    juce::StringArray getUserPresetCategories() const;
    // v0.7.0 — category management: rename moves the sub-folder,
    // delete moves all presets in that sub-folder to the root.
    bool  renameUserPresetCategory (const juce::String& oldName, const juce::String& newName);
    bool  deleteUserPresetCategory (const juce::String& categoryName);

    // v0.7.0 — persist UI preferences so new instances start with the
    // user's preferred settings (tooltips, page-follow, scale, etc.).
    void  saveGlobalDefaults();
    void  loadGlobalDefaults();

    // v0.11 — randomization helpers. Drive the "RANDOMIZE" button in
    // the sequencer header.
    //
    // randomizeStepPlacements: for every active step cell, roll a
    //   coin-flip (or 3-in-8, or always) to stamp one of the currently-
    //   populated Acts onto it. `density` is the probability [0,1]. A
    //   cell that loses the roll is cleared.
    //
    // randomizeActValues: roll a new value for every knob of every
    //   populated Act. `intensity` ramps how far from the current value
    //   we're allowed to stray. 1.0 = full-range random, 0.0 = no-op.
    //
    // randomizeEverything: both of the above, plus a roll of per-step
    //   ratio locks. Maximum chaos.
    void randomizeStepPlacements (float density);
    // v0.13 — forceAllActs=true fills every A-H slot (used by
    // randomizeEverything). Default false still auto-populates the
    // currently selected Act so the user always sees the knobs move.
    void randomizeActValues      (float intensity, bool forceAllActs = false);
    void randomizeEverything     ();

    // v0.12 — musical step placement generators. These replace the old
    // "sparse/busy" density rolls with rhythms that actually land on a
    // beat grid. Each clears the pattern first, then stamps random
    // populated Acts onto the chosen positions.
    //
    // placeFourOnTheFloor — steps on every accent beat (default 1,5,9,13
    //   for accent=4). Accent interval is taken from gridAccentInterval.
    // placeOffbeats       — steps on the half-accent positions
    //   (3,7,11,15 for accent=4), the classic "and" beats.
    // placeEuclidean      — Bjorklund/Bresenham E(k,n) distribution with
    //   `pulses` hits spread across the current seqLength.
    void placeFourOnTheFloor ();
    void placeOffbeats       ();
    void placeEuclidean      (int pulses);
    // v0.13 — extra musical patterns, all populating the Patterns
    // submenu under the RANDOM button. Each one shares the same "pick
    // a random Act per hit" idiom as the originals above.
    void placeDownbeats      ();    // step 0 of every other accent group
    void placeBackbeat       ();    // "2 and 4" — accent-phase 1 & 3
    void placeSyncopated     ();    // e and a sixteenths
    void placeSaturated      ();    // every active step fires
    void placeSparse         ();    // 25% density scatter
    void placeBusy           ();    // 75% density scatter
    void placeClave          ();    // 3-3-2 son clave
    void setStepScene      (int stepIdx, int sceneIdx); // -1 for empty
    int  getStepScene      (int stepIdx) const noexcept;

    // --- per-step trig condition lock (A:B) ---------------------------
    // Index 0 = "no lock, inherit Act chance". Index 1..kNumRatios-1
    // selects an A:B trig condition from the table in the .cpp.
    //
    // v0.12 — semantics changed to pattern-play based. Earlier
    // versions were visit-counted ("fire N of every M visits") which
    // broke down visually (2:4 looked the same as 1:2 even though the
    // visit pattern differed). New rule: the step fires on the Ath
    // pattern play out of every B consecutive pattern plays, so 1:2
    // fires on the 1st play then the 3rd play then the 5th play, and
    // 3:3 fires only on the 3rd, 6th, 9th... play. Formula:
    //     (patternPlayCount % B) == (A - 1)
    // The table is {1:2, 1:3, 2:3, 3:3, 1:4, 2:4, 3:4, 4:4, 1:5, 2:5,
    // 3:5, 4:5, 5:5, 1:6, 1:7}, giving 16 entries total (0 = unlocked).
    // v0.38 — 17 A:B ratio entries (0 = unlocked, 1..16 = A:B) plus
    // trig-condition entries appended at the end:
    //   17 = PREV, 18 = !PREV, 19 = ONCE,
    //   20 = 25%, 21 = 50%, 22 = 75%.
    static constexpr int kNumRatios = 23;

    // Resolve a ratio index into (A, B). Index 0 is a no-op and
    // returns (0, 0). Out-of-range indices clamp to 0.
    static void getRatioNM (int ratioIdx, int& n, int& m) noexcept;
    static juce::String ratioLabel (int ratioIdx);          // "1:2"
    static juce::String ratioShortLabel (int ratioIdx);     // "1:2"  — cell text
    // v0.10 — one-way migration from the 17-entry v0.9 table.
    static int  migrateRatioFromV09 (int oldIdx) noexcept;
    // v0.12 — one-way migration from the 13-entry v0.10/v0.11 table.
    static int  migrateRatioFromV11 (int oldIdx) noexcept;

    void setStepRatio (int stepIdx, int ratioIdx);
    int  getStepRatio (int stepIdx) const noexcept;

    // --- per-step freeze hold ------------------------------------------
    // When marked, the buffer write head locks in place while this step
    // is the currently-playing step. Useful for punching "this slice is
    // a held sample" moments into an otherwise-live pattern.
    void setStepFreeze (int stepIdx, bool held);
    bool getStepFreeze (int stepIdx) const noexcept;

    // --- per-step ratchet (v0.12) --------------------------------------
    // 1 = no ratchet, 2/3/4/6/8 = subdivide the step duration into that
    // many equal slices and fire once per slice. Other values are
    // clamped to the supported set on write.
    static constexpr int kMaxRatchet = 8;
    static bool isValidRatchet (int r) noexcept;
    void setStepRatchet (int stepIdx, int ratchet);
    int  getStepRatchet (int stepIdx) const noexcept;

    // --- per-step mute (v0.13) ----------------------------------------
    // When true, the step is silenced regardless of Act stamp, p-locks,
    // ratio, ratchet etc. Gives Steve a "manual stutter" gap that still
    // preserves everything else on the step so you can un-mute it later.
    void setStepMute (int stepIdx, bool muted);
    bool getStepMute (int stepIdx) const noexcept;

    // --- per-step parameter locks ("p-locks") ---------------------------
    // Each step can override any number of the 24 knob parameters with
    // its own value. NaN = not locked (inherit from the Act). These are
    // overlaid on top of the Act's baked ShapingParams at fire time.
    static constexpr int kNumLockableParams = 29;  // v0.42.6 — +globalMix

    // Map an APVTS parameter ID to a lock-slot index [0..kNumLockableParams-1].
    // Returns -1 if the ID isn't lockable. Used by both the editor and
    // the serializer so the slot numbering is stable across versions.
    static int lockableIndexForId (const juce::String& id);
    static const juce::String& lockableIdForIndex (int slot);

    void  setStepLock   (int stepIdx, int lockSlot, float value); // NaN clears
    float getStepLock   (int stepIdx, int lockSlot) const noexcept;
    void  clearStepLocks (int stepIdx);
    bool  stepHasAnyLock (int stepIdx) const noexcept;

    // --- bulk sequencer ops -------------------------------------------
    // Wipes every step (acts, ratio locks, freeze holds,
    // parameter locks). Used by the Clear button.
    void clearAllSteps();

    // v0.30 — debug helper: serialises the full plugin state, immediately
    // deserialises it into this instance, then compares every field
    // against a pre-save snapshot. Returns an empty string on success or
    // a newline-separated list of mismatches. Intended for manual QA:
    // call from a settings-menu action or from a unit test.
    juce::String validateStateRoundTrip();

    void setSeqLength      (int newLength);
    int  getSeqLength      () const noexcept     { return seqLength.load(); }
    void setSeqOn          (bool on);
    bool isSeqOn           () const noexcept     { return seqOn.load(); }
    void setMasterBypass   (bool b) noexcept     { masterBypass.store (b); }
    bool isMasterBypassed  () const noexcept     { return masterBypass.load(); }
    void setSelectedScene  (int sceneIdx);
    int  getSelectedScene  () const noexcept     { return selectedScene.load(); }
    int  getCurrentPlayingStep () const noexcept { return currentPlayingStep.load(); }
    bool isSceneStored     (int sceneIdx) const noexcept;
    float getSceneMix      (int sceneIdx) const noexcept
    {
        if (sceneIdx < 0 || sceneIdx >= kNumScenes) return 1.0f;
        return scenes[sceneIdx].mix.load();
    }

    // --- per-scene preset tracking (BUG 1 fix) ---
    int  getScenePresetIdx (int sceneIdx) const noexcept;
    void setScenePresetIdx (int sceneIdx, int presetIdx) noexcept;

    // --- single-level undo for step grid ---------------------------------
    // v0.30 — captures the entire sequencer grid state (steps, ratios,
    // freeze holds, ratchets, mutes, p-locks, length) in one POD
    // snapshot. Call pushUndoSnapshot() before any destructive bulk
    // operation (clear all, randomize, etc.) and popUndo() to restore.
    struct StepSnapshot
    {
        int   steps[kMaxSteps]        {};
        int   ratio[kMaxSteps]        {};
        bool  freeze[kMaxSteps]       {};
        int   ratchet[kMaxSteps]      {};
        bool  mute[kMaxSteps]         {};
        float locks[kMaxSteps * kNumLockableParams] {};
        int   seqLen                  = kDefaultSteps;
    };
    void pushUndoSnapshot();   // call BEFORE the destructive action
    bool popUndo();            // returns true if restored, false if nothing to undo
    bool hasUndo() const noexcept  { return undoAvailable.load(); }

    // --- debug instrumentation (v0.30) ----------------------------------
    // All read-only from the message thread; written by the audio thread.

    // CPU watchdog: worst-case processBlock time as a fraction of the
    // available buffer time (0..1+). Resets each time it's read.
    float readAndResetCpuWorstCase() noexcept
    {
        return dbgCpuWorstCase.exchange (0.0f);
    }

    // Voice state snapshot — copied from the audio thread's voice struct
    // at the end of each processBlock so the UI can display it.
    struct VoiceSnapshot
    {
        bool   active           = false;
        double readPos          = 0.0;
        int    samplesRemaining = 0;
        int    totalSamples     = 0;
        float  fadeGain         = 0.0f;
        int    juddersRemaining = 0;
        int    sceneIdx         = -1;
        float  voiceMix        = 0.0f;
    };
    VoiceSnapshot getVoiceSnapshot() const noexcept;

    // Fire timing accuracy: delta (in microseconds) between the ideal
    // grid boundary and the actual sample where the last fire landed.
    // Positive = late, negative = early.
    float getLastFireTimingUs() const noexcept { return dbgLastFireTimingUs.load(); }

    // Force-fire: immediately fires a voice with the current knob values.
    // Safe to call from the message thread — sets a flag that processBlock
    // picks up on the very next block.
    void debugForceFire() noexcept { dbgForceFire.store (true); }

    // Dump the circular buffer to a WAV file. Returns true on success.
    bool debugDumpBufferToWav (const juce::File& destFile) const;

    // Step grid dump: returns a compact human-readable string.
    juce::String debugDumpStepGrid() const;

    // P-lock inspector: returns a multi-line string of all non-NaN locks.
    juce::String debugDumpAllLocks() const;

    // --- momentary freeze (editor-driven) ------------------------------
    // The FREEZE button supports both click-to-toggle and hold-to-
    // engage. Toggle mode drives kParamFreeze directly via an
    // APVTS attachment. Momentary mode holds an editor-only flag here
    // that's OR'd with the main freeze in processBlock.
    void setMomentaryFreeze (bool held) noexcept { momentaryFreeze.store (held); }
    bool getMomentaryFreeze () const noexcept    { return momentaryFreeze.load(); }

    // --- persisted editor preferences ----------------------------------
    // Not APVTS params — these are UI modes that we still want to
    // restore on re-open. Serialised alongside APVTS state in
    // get/setStateInformation so they survive DAW save/load cycles.
    float getUiScale       () const noexcept { return uiScale.load(); }
    void  setUiScale       (float s) noexcept { uiScale.store (s); }
    bool  getWaveformVisible () const noexcept { return waveformVisible.load(); }
    void  setWaveformVisible (bool v) noexcept { waveformVisible.store (v); }
    bool  getTooltipsEnabled () const noexcept { return tooltipsEnabled.load(); }
    void  setTooltipsEnabled (bool v) noexcept { tooltipsEnabled.store (v); }

    // v0.12 — DJ-style lookahead. When ms > 0 the plugin delays its
    // OUTPUT by that much (via a post-processing delay line) and
    // reports the delay via setLatencySamples so the host can
    // compensate. The peak ring is still written from the live input,
    // so it "runs ahead" of what the user is hearing by exactly the
    // lookahead window — the editor uses that gap to paint the
    // incoming peaks to the right of the playhead on the waveform
    // strip, giving the "about to drop" cue.
    static constexpr int kMaxLookaheadMs = 1000;
    int   getLookaheadMs      () const noexcept { return lookaheadMs.load(); }
    int   getLookaheadSamples () const noexcept { return lookaheadSamples.load(); }
    void  setLookaheadMs      (int ms);
    bool  getFreezeMomentaryMode () const noexcept { return freezeMomentaryMode.load(); }
    void  setFreezeMomentaryMode (bool m) noexcept { freezeMomentaryMode.store (m); }
    bool  getAutoFollowLength () const noexcept { return autoFollowLength.load(); }
    void  setAutoFollowLength (bool v) noexcept { autoFollowLength.store (v); }
    bool  getPageFollowsPlayhead () const noexcept { return pageFollowsPlayhead.load(); }
    void  setPageFollowsPlayhead (bool v) noexcept { pageFollowsPlayhead.store (v); }

    // v0.21 — crush DSP mode flags. v0.41: per-Act. The legacy global atomics
    // are kept as a fallback when no Act is selected (live-knobs mode), and as
    // the source-of-truth for "newly created Act defaults". Setters below are
    // backward-compatible "broadcast to all Acts" shims so older UI/automation
    // continues to work; new per-Act setters live further down.
    bool  getCrushAntiAlias () const noexcept { return crushAntiAlias.load(); }
    void  setCrushAntiAlias (bool v) noexcept
    {
        crushAntiAlias.store (v);
        for (auto& s : scenes) s.crushAA.store (v);
    }
    bool  getCrushMuLaw     () const noexcept { return crushMuLaw.load(); }
    void  setCrushMuLaw     (bool v) noexcept
    {
        crushMuLaw.store (v);
        for (auto& s : scenes) s.crushMu.store (v);
    }
    bool  getCrushAllAudio () const noexcept { return crushAllAudio.load(); }
    void  setCrushAllAudio (bool v) noexcept
    {
        crushAllAudio.store (v);
        for (auto& s : scenes) s.crushAll.store (v);
    }

    // v0.40 — ENGINE: per-Act character settings recallable as a bundle.
    //   * Output Stage = post-mix coloration (see OutputStage.h)
    //   * Output Mix   = wet/dry on the Output Stage
    //   * Interp Mode  = how slice playback reads the buffer
    // The two existing crush toggles (AntiAlias, MuLaw) are also part of
    // the Engine bundle for preset-recall purposes.
    //
    // v0.41 — these "global" getters/setters operate on the *currently selected*
    // Act so legacy callers (APVTS, designed-for badge) keep working. Setters
    // also broadcast to all 8 Acts when no Act is in focus, preserving the
    // pre-v0.41 single-engine behaviour for fresh sessions.
    int   getOutputStageMode () const noexcept
    {
        const int idx = selectedScene.load();
        if (idx >= 0 && idx < kNumScenes) return scenes[idx].engineStage.load();
        return outputStage.getMode();
    }
    void  setOutputStageMode (int m) noexcept
    {
        m = juce::jlimit (0, (int) loopsab::kNumOutputStages - 1, m);
        for (auto& s : scenes) s.engineStage.store (m);
    }
    float getOutputStageMix  () const noexcept
    {
        const int idx = selectedScene.load();
        if (idx >= 0 && idx < kNumScenes) return scenes[idx].engineMix.load();
        return outputStage.getMix();
    }
    void  setOutputStageMix  (float v) noexcept
    {
        v = juce::jlimit (0.0f, 1.0f, v);
        for (auto& s : scenes) s.engineMix.store (v);
    }
    int   getInterpMode      () const noexcept
    {
        const int idx = selectedScene.load();
        if (idx >= 0 && idx < kNumScenes) return scenes[idx].interpMode.load();
        return interpMode.load();
    }
    void  setInterpMode      (int m) noexcept
    {
        m = juce::jlimit (0, (int) loopsab::kNumInterpModes - 1, m);
        interpMode.store (m);
        for (auto& s : scenes) s.interpMode.store (m);
    }

    // v0.41 — per-Act Engine accessors (clamped & atomic). These let the
    // SettingsPage edit a specific Act's bundle without disturbing others.
    int   getActEngineStage (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].engineStage.load();
    }
    void  setActEngineStage (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].engineStage.store (juce::jlimit (0, (int) loopsab::kNumOutputStages - 1, m));
    }
    float getActEngineMix (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0.0f;
        return scenes[act].engineMix.load();
    }
    void  setActEngineMix (int act, float v) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].engineMix.store (juce::jlimit (0.0f, 1.0f, v));
    }
    float getActEngineIntensity (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0.5f;
        return scenes[act].engineIntensity.load();
    }
    void  setActEngineIntensity (int act, float v) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].engineIntensity.store (juce::jlimit (0.0f, 1.0f, v));
    }
    int   getActInterpMode (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].interpMode.load();
    }
    void  setActInterpMode (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].interpMode.store (juce::jlimit (0, (int) loopsab::kNumInterpModes - 1, m));
    }
    bool  getActCrushAA (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return true;
        return scenes[act].crushAA.load();
    }
    void  setActCrushAA (int act, bool v) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].crushAA.store (v);
    }
    bool  getActCrushMu (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return true;
        return scenes[act].crushMu.load();
    }
    void  setActCrushMu (int act, bool v) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].crushMu.store (v);
    }
    bool  getActCrushAll (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return false;
        return scenes[act].crushAll.load();
    }
    void  setActCrushAll (int act, bool v) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].crushAll.store (v);
    }

    // v0.5.0 — per-Act filter mode (Tilt/LP/HP/BP).
    int   getActFilterMode (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].filterMode.load();
    }
    void  setActFilterMode (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].filterMode.store (juce::jlimit (0, (int) loopsab::kNumFilterModes - 1, m));
    }

    // v0.5.0 — per-Act frequency split (Full/Low Only/High Only).
    int   getActFreqSplitMode (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].freqSplitMode.load();
    }
    void  setActFreqSplitMode (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].freqSplitMode.store (juce::jlimit (0, 2, m));
    }
    float getActFreqSplitHz (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 500.0f;
        return scenes[act].freqSplitHz.load();
    }
    void  setActFreqSplitHz (int act, float hz) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].freqSplitHz.store (juce::jlimit (80.0f, 8000.0f, hz));
    }

    // v0.5.0 — per-Act CHARACTER options (right-click knob modes).
    int   getActDriveType (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].driveType.load();
    }
    void  setActDriveType (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].driveType.store (juce::jlimit (0, (int) loopsab::kNumDriveTypes - 1, m));
    }
    int   getActFoldTopology (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].foldTopology.load();
    }
    void  setActFoldTopology (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].foldTopology.store (juce::jlimit (0, (int) loopsab::kNumFoldTopologies - 1, m));
    }
    int   getActShimmerOctave (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].shimmerOctave.load();
    }
    void  setActShimmerOctave (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].shimmerOctave.store (juce::jlimit (0, (int) loopsab::kNumShimmerOctaves - 1, m));
    }
    int   getActSmearCharacter (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].smearCharacter.load();
    }
    void  setActSmearCharacter (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].smearCharacter.store (juce::jlimit (0, (int) loopsab::kNumSmearCharacters - 1, m));
    }
    int   getActStutterWindow (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].stutterWindow.load();
    }
    void  setActStutterWindow (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].stutterWindow.store (juce::jlimit (0, (int) loopsab::kNumStutterWindows - 1, m));
    }
    int   getActVarispeedCurve (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].varispeedCurve.load();
    }
    void  setActVarispeedCurve (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].varispeedCurve.store (juce::jlimit (0, (int) loopsab::kNumVarispeedCurves - 1, m));
    }
    int   getActSlideCurve (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].slideCurve.load();
    }
    void  setActSlideCurve (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].slideCurve.store (juce::jlimit (0, (int) loopsab::kNumSlideCurves - 1, m));
    }
    int   getActReverseMode (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].reverseMode.load();
    }
    void  setActReverseMode (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].reverseMode.store (juce::jlimit (0, (int) loopsab::kNumReverseModes - 1, m));
    }

    // v0.5.0 wave 2 — more CHARACTER getters/setters.
    int   getActTapeMode (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].tapeMode.load();
    }
    void  setActTapeMode (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].tapeMode.store (juce::jlimit (0, (int) loopsab::kNumTapeModes - 1, m));
    }
    int   getActRingModWave (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].ringModWave.load();
    }
    void  setActRingModWave (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].ringModWave.store (juce::jlimit (0, (int) loopsab::kNumRingModWaves - 1, m));
    }
    int   getActChaosDistribution (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].chaosDistribution.load();
    }
    void  setActChaosDistribution (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].chaosDistribution.store (juce::jlimit (0, (int) loopsab::kNumChaosDistributions - 1, m));
    }
    int   getActFeedbackCharacter (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].feedbackCharacter.load();
    }
    void  setActFeedbackCharacter (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].feedbackCharacter.store (juce::jlimit (0, (int) loopsab::kNumFeedbackCharacters - 1, m));
    }
    int   getActStretchMode (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].stretchMode.load();
    }
    void  setActStretchMode (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].stretchMode.store (juce::jlimit (0, (int) loopsab::kNumStretchModes - 1, m));
    }
    int   getActJudderShape (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].judderShape.load();
    }
    void  setActJudderShape (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].judderShape.store (juce::jlimit (0, (int) loopsab::kNumJudderShapes - 1, m));
    }
    int   getActLookbackBehaviour (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].lookbackBehaviour.load();
    }
    void  setActLookbackBehaviour (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].lookbackBehaviour.store (juce::jlimit (0, (int) loopsab::kNumLookbackBehaviours - 1, m));
    }
    int   getActDecayCurve (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].decayCurve.load();
    }
    void  setActDecayCurve (int act, int m) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].decayCurve.store (juce::jlimit (0, (int) loopsab::kNumDecayCurves - 1, m));
    }

    // v0.25 / v0.42 — ring mod quantise to scale.
    // Became per-Act in v0.42. The global accessors keep their old public
    // signature but now read the currently-selected Act (with a legacy-
    // global fallback for the no-scene case) and the setters broadcast to
    // every Act so external callers that don't know about per-Act state
    // still see consistent behaviour.
    bool  getRingModQuantise () const noexcept
    {
        const int idx = selectedScene.load();
        if (idx >= 0 && idx < kNumScenes) return scenes[idx].ringModQuant.load();
        return ringModQuantise.load();
    }
    void  setRingModQuantise (bool v) noexcept
    {
        ringModQuantise.store (v);
        for (auto& s : scenes) s.ringModQuant.store (v);
    }

    // v0.25 / v0.42 — FX-on-Dry toggles: when on, effects process the
    // entire output, not just the wet slices. Per-effect flags so the user
    // can choose which ones bleed into the full mix. Became per-Act in
    // v0.42; see ringModQuantise above for the back-compat pattern.
    bool  getGlobalDrive     () const noexcept
    {
        const int idx = selectedScene.load();
        if (idx >= 0 && idx < kNumScenes) return scenes[idx].fxOnDryDrive.load();
        return globalDrive.load();
    }
    void  setGlobalDrive     (bool v) noexcept
    {
        globalDrive.store (v);
        for (auto& s : scenes) s.fxOnDryDrive.store (v);
    }
    bool  getGlobalTone      () const noexcept
    {
        const int idx = selectedScene.load();
        if (idx >= 0 && idx < kNumScenes) return scenes[idx].fxOnDryTone.load();
        return globalTone.load();
    }
    void  setGlobalTone      (bool v) noexcept
    {
        globalTone.store (v);
        for (auto& s : scenes) s.fxOnDryTone.store (v);
    }
    bool  getGlobalRingMod   () const noexcept
    {
        const int idx = selectedScene.load();
        if (idx >= 0 && idx < kNumScenes) return scenes[idx].fxOnDryRingMod.load();
        return globalRingMod.load();
    }
    void  setGlobalRingMod   (bool v) noexcept
    {
        globalRingMod.store (v);
        for (auto& s : scenes) s.fxOnDryRingMod.store (v);
    }
    bool  getGlobalFold      () const noexcept
    {
        const int idx = selectedScene.load();
        if (idx >= 0 && idx < kNumScenes) return scenes[idx].fxOnDryFold.load();
        return globalFold.load();
    }
    void  setGlobalFold      (bool v) noexcept
    {
        globalFold.store (v);
        for (auto& s : scenes) s.fxOnDryFold.store (v);
    }

    // v0.42 — per-Act accessors. Settings page uses these directly so the
    // user can tune each Act's FX-on-Dry / Ring Mod Quant / Stereo Mode
    // independently without disturbing the other 7 slots.
    bool  getActFxOnDryDrive   (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return false;
        return scenes[act].fxOnDryDrive.load();
    }
    void  setActFxOnDryDrive   (int act, bool v) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].fxOnDryDrive.store (v);
    }
    bool  getActFxOnDryTone    (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return false;
        return scenes[act].fxOnDryTone.load();
    }
    void  setActFxOnDryTone    (int act, bool v) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].fxOnDryTone.store (v);
    }
    bool  getActFxOnDryRingMod (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return false;
        return scenes[act].fxOnDryRingMod.load();
    }
    void  setActFxOnDryRingMod (int act, bool v) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].fxOnDryRingMod.store (v);
    }
    bool  getActFxOnDryFold    (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return false;
        return scenes[act].fxOnDryFold.load();
    }
    void  setActFxOnDryFold    (int act, bool v) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].fxOnDryFold.store (v);
    }
    bool  getActRingModQuant   (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return false;
        return scenes[act].ringModQuant.load();
    }
    void  setActRingModQuant   (int act, bool v) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].ringModQuant.store (v);
    }

    // v0.25 — stereo spread modes. The STEREO knob controls depth;
    // the mode determines the algorithm used to spread slices.
    enum StereoMode
    {
        kStereoRandom   = 0,   // random pan per fire (default, original)
        kStereoPingPong = 1,   // alternate L/R on successive fires
        kStereoAutoPan  = 2,   // LFO sweep during slice playback
        kStereoHaas     = 3    // micro-delay on one channel for width
    };
    // v0.25 / v0.42 — stereo mode. Became per-Act in v0.42; legacy
    // accessors preserved with selectedScene-resolve + broadcast semantics
    // (see getRingModQuantise above).
    int   getStereoMode () const noexcept
    {
        const int idx = selectedScene.load();
        if (idx >= 0 && idx < kNumScenes) return scenes[idx].stereoMode.load();
        return stereoMode.load();
    }
    void  setStereoMode (int v) noexcept
    {
        v = juce::jlimit (0, 3, v);
        stereoMode.store (v);
        for (auto& s : scenes) s.stereoMode.store (v);
    }
    // v0.42 — per-Act stereo mode accessors.
    int   getActStereoMode (int act) const noexcept
    {
        if (act < 0 || act >= kNumScenes) return 0;
        return scenes[act].stereoMode.load();
    }
    void  setActStereoMode (int act, int v) noexcept
    {
        if (act < 0 || act >= kNumScenes) return;
        scenes[act].stereoMode.store (juce::jlimit (0, 3, v));
    }

    // v0.30 — LFO modulation. Per-Act LFOs (4 per Act, 32 total across 8 Acts).
    // Each tempo-synced LFO can target any lockable parameter.
    // Phase is driven by host PPQ for bar-locked motion.
    //
    // v0.34 — extended to include envelope shapes (monopolar, step-triggered)
    // and a sync/free mode toggle. Envelopes share the slot, depth, and
    // target system with LFOs; they simply output a retriggered AHD curve
    // (0..+1) instead of a continuous waveform.
    static constexpr int kNumLfosPerAct = 4;
    enum LfoShape
    {
        kLfoSine = 0,
        kLfoTriangle,
        kLfoSaw,
        kLfoSquare,
        kLfoSandH,
        kLfoEnvAHD,       // v0.34 — attack/hold/decay envelope, monopolar
        kLfoEnvFollow,    // v0.5.0 — envelope follower, tracks input amplitude
        kNumLfoShapes
    };

    // v0.34 — when shape is an envelope, this decides what retriggers it.
    // Slice: every slice audio trigger (ratchets retrigger each sub-hit).
    // ActiveSteps: once per sequencer step that actually fires a scene
    //              (ratchets do NOT retrigger the envelope).
    // ActiveStepsPlusLocks: ActiveSteps + p-lock-only steps.
    // AllSteps: every grid position regardless of content, including blanks.
    enum LfoTrigMode
    {
        kTrigSlice = 0,
        kTrigActiveSteps,
        kTrigActiveStepsPlusLocks,
        kTrigAllSteps,
        kNumTrigModes
    };

    struct LfoState
    {
        std::atomic<int>   shape     { kLfoSine };
        std::atomic<int>   rateIdx   { 4 };       // default ~1/8 (LFO modes only)
        std::atomic<float> depth     { 1.0f };    // bipolar: -1..+1 (0 = silent)
        std::atomic<int>   targetSlot { -1 };     // -1 = none

        // v0.38 — cross-mod target within the same Act. -1 = no cross-
        // mod, 0..3 = modulate that other LFO's depth at audio rate. The
        // amount is the signed magnitude of crossDepth (unit: "multiply
        // target depth by (1 + crossDepth * sourceValue)"). Implementation
        // uses the target LFO's cached lastOutput to avoid creating an
        // ordering dependency between the 4 LFOs in one sample.
        std::atomic<int>   crossTargetLfo { -1 };
        std::atomic<float> crossDepth     { 1.0f };

        // v0.34 — sync mode. true = tempo/PPQ-locked, false = free-running
        // sample clock (keeps moving even when the host is stopped).
        std::atomic<bool>  sync      { true };

        // v0.34 — envelope configuration (only consulted when shape == kLfoEnvAHD).
        std::atomic<float> attackMs  { 5.0f };    // 1..2000 ms
        std::atomic<float> holdMs    { 0.0f };    // 0..2000 ms (0 = AD-style)
        std::atomic<float> decayMs   { 200.0f };  // 1..2000 ms
        std::atomic<int>   trigMode  { kTrigActiveSteps };

        // Audio-thread-only running state (not atomic, only touched in processBlock).
        double phase        = 0.0;
        float  sAndHValue  = 0.0f;  // held value for S&H shape
        double sAndHPrev   = 0.0;   // previous phase for wrap detection
        float  lastOutput  = 0.0f;  // current output * depth (bipolar)

        // v0.34 — envelope runtime. envStage: 0=idle, 1=attack, 2=hold, 3=decay.
        // envPos is elapsed ms within the current stage. envValue is 0..+1.
        int    envStage   = 0;
        double envPos     = 0.0;
        float  envValue   = 0.0f;
        bool   envPending = false;  // set by trigger hook, consumed next sample

        // v0.5.0 — envelope follower state (shape == kLfoEnvFollow).
        // Smoothed absolute amplitude of the input signal. Rate knob
        // repurposes as smoothing time when this shape is selected.
        float  envFollowValue = 0.0f;

        // v0.6.0 — envelope follower input gain / sensitivity.
        // Multiplied onto the raw input level before tracking. 1.0 = unity,
        // up to 10.0 = +20 dB boost for quiet signals.
        std::atomic<float> envFollowGain { 1.0f };
    };

    // Accessors for LFO state. sceneIdx = Act (0..7), lfoIdx = LFO within Act (0..3).
    void  setLfoShape      (int sceneIdx, int lfoIdx, int shape);
    int   getLfoShape      (int sceneIdx, int lfoIdx) const noexcept;
    void  setLfoRateIdx    (int sceneIdx, int lfoIdx, int rateIdx);
    int   getLfoRateIdx    (int sceneIdx, int lfoIdx) const noexcept;
    void  setLfoDepth      (int sceneIdx, int lfoIdx, float depth);
    float getLfoDepth      (int sceneIdx, int lfoIdx) const noexcept;
    void  setLfoTargetSlot (int sceneIdx, int lfoIdx, int slot);
    int   getLfoTargetSlot (int sceneIdx, int lfoIdx) const noexcept;
    // v0.34 — sync/free toggle.
    void  setLfoSync       (int sceneIdx, int lfoIdx, bool sync);
    bool  getLfoSync       (int sceneIdx, int lfoIdx) const noexcept;
    // v0.34 — envelope AHD times (ms) + trigger mode.
    void  setLfoAttackMs   (int sceneIdx, int lfoIdx, float ms);
    float getLfoAttackMs   (int sceneIdx, int lfoIdx) const noexcept;
    void  setLfoHoldMs     (int sceneIdx, int lfoIdx, float ms);
    float getLfoHoldMs     (int sceneIdx, int lfoIdx) const noexcept;
    void  setLfoDecayMs    (int sceneIdx, int lfoIdx, float ms);
    float getLfoDecayMs    (int sceneIdx, int lfoIdx) const noexcept;
    void  setLfoTrigMode   (int sceneIdx, int lfoIdx, int mode);
    int   getLfoTrigMode   (int sceneIdx, int lfoIdx) const noexcept;
    void  setLfoEnvFollowGain (int sceneIdx, int lfoIdx, float g);
    float getLfoEnvFollowGain (int sceneIdx, int lfoIdx) const noexcept;
    // v0.38 — cross-mod wiring. crossTargetLfo = another LFO (0..3) in
    // the same Act whose depth this LFO should modulate, or -1 to
    // disable. crossDepth is the amount (-1..+1). Signed: negative
    // values invert the sense, which makes for nice "squash" behaviour.
    void  setLfoCrossTarget (int sceneIdx, int lfoIdx, int targetLfoIdx);
    int   getLfoCrossTarget (int sceneIdx, int lfoIdx) const noexcept;
    void  setLfoCrossDepth  (int sceneIdx, int lfoIdx, float depth);
    float getLfoCrossDepth  (int sceneIdx, int lfoIdx) const noexcept;
    // For UI visualization: read audio-thread-only phase and lastOutput (approximate reads from message thread OK).
    double getLfoPhase     (int sceneIdx, int lfoIdx) const noexcept;
    float  getLfoOutput    (int sceneIdx, int lfoIdx) const noexcept;
    float  getLfoLastOutput (int sceneIdx, int lfoIdx) const noexcept;

    // v0.33 — ask whether ANY LFO on a given Act currently targets a
    // lockable slot. Used by the editor to paint the cyan "LFO-targeted"
    // dot on knobs whose param is being modulated. "Active" means
    // rateIdx != 0 (not Off) AND depth > 0.
    bool anyLfoTargetsSlot (int sceneIdx, int slot) const noexcept;

    // v0.34 — envelope trigger hook. Called from the sequencer scheduler
    // at each of the four possible trigger points. For each env-shaped
    // LFO on the currently active Act whose trigMode matches this event
    // type, we flag the envelope to restart at the next sample.
    // Safe to call from the audio thread only.
    enum LfoTrigEvent
    {
        kEventSlice = 0,             // a slice audio trigger (including ratchet sub-hits)
        kEventActiveStep,            // sequencer step that fired a scene (NOT ratchets)
        kEventActiveStepPlusLock,    // active step OR p-lock-only step
        kEventAllStep                // every grid tick, blank included
    };
    void fireLfoTriggers (int sceneIdx, int eventKind) noexcept;

    // v0.33 — last known host BPM and play state. Written by the audio
    // thread each processBlock, read by the UI for the LFO scope preview
    // so the scroll speed matches the real rate at the current tempo.
    double getLastKnownBpm () const noexcept { return lastKnownBpm.load(); }
    bool   getIsHostPlaying () const noexcept { return isHostPlaying.load(); }

    // v0.11 — grid accent interval for the step strip. 0 = off (no
    // visual accents), N = every Nth step gets a brighter background
    // so the beat structure is obvious. Default 4 matches the old
    // implicit "every-four-is-darker" behaviour. OctaMed-style: allow
    // 2/3/4/6/8.
    int   getGridAccentInterval () const noexcept { return gridAccentInterval.load(); }
    void  setGridAccentInterval (int n) noexcept  { gridAccentInterval.store (juce::jlimit (0, 32, n)); }

    // v0.11 — sequencer transport mode. 0 = forward (default), 1 =
    // reverse, 2 = ping-pong, 3 = random. Audio thread reads this on
    // every sample while computing wrappedStep from curSeqStepAbs.
    enum SeqTransport { kTransportForward = 0, kTransportReverse = 1,
                        kTransportPingPong = 2, kTransportRandom = 3 };
    int   getSeqTransport () const noexcept { return seqTransport.load(); }
    void  setSeqTransport (int v) noexcept  { seqTransport.store (juce::jlimit (0, 3, v)); }

    // v0.9 — AUTO-follow helper. Returns the smallest multiple of
    // kStepsPerPage (32) in [kStepsPerPage..kMaxSteps] that contains
    // every populated step (where "populated" means: an Act stamped,
    // a ratio lock, a per-step freeze hold, a non-zero grab offset, or
    // any p-lock slot populated). Returns kStepsPerPage for an empty
    // pattern. Called from setStepScene, clearStep, clearAllSteps,
    // setStepRatio, etc. whenever autoFollowLength is true.
    int   computeAutoFollowLength () const noexcept;
    void  applyAutoFollowIfEnabled () noexcept;

    // --- input peak ring (for the waveform strip above the sequencer) --
    // Audio thread writes one peak-per-bucket into the ring; the editor
    // timer reads it to paint a rolling waveform. All lock-free.
    static constexpr int kPeakBucketSamples = 128;  // ~2.9 ms at 44.1k (v0.8: halved)
    static constexpr int kPeakRingSize      = 2048; // ~6 s of history at 44.1k

    float getPeak (int idx) const noexcept;
    int   getPeakWritePos () const noexcept { return peakWritePos.load(); }

    // v0.5.0 — slice output peak ring for slice preview waveform.
    // Same bucket size as the input ring. Written from the voice output
    // (post-FX, pre-mix) so the user sees the shaped slice waveform.
    static constexpr int kSlicePeakRingSize = 2048;
    float getSlicePeak (int idx) const noexcept;
    int   getSlicePeakWritePos () const noexcept { return slicePeakWritePos.load(); }

    // v0.30 — expose buffer geometry for slice-position visualisation.
    int   getCircularWritePos  () const noexcept { return writePos; }
    int   getCircularBufferSize() const noexcept { return circularBufferSize; }

    // Act (scene) import/export. XML-based so you can hand-edit files,
    // version-control them, or paste them into chat. Single-act files
    // carry one scene; bank files carry all eight. Callers run on the
    // message thread; these allocate freely.
    std::unique_ptr<juce::XmlElement> serializeAct       (int sceneIdx) const;
    std::unique_ptr<juce::XmlElement> serializeAllActs   () const;
    bool loadActFromXml      (const juce::XmlElement& el, int targetIdx);
    bool loadAllActsFromXml  (const juce::XmlElement& el);

    // --- parameters ------------------------------------------------------
    juce::AudioProcessorValueTreeState apvts;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Parameter IDs — kept as constexpr strings so the editor and
    // processor agree without magic literals floating around.
    static constexpr const char* kParamChance    = "chance";
    static constexpr const char* kParamDivision  = "division";
    static constexpr const char* kParamLookback  = "lookback";
    static constexpr const char* kParamJudder    = "judder";
    static constexpr const char* kParamJudderDiv = "judderdiv";
    static constexpr const char* kParamPitch     = "pitch";
    static constexpr const char* kParamSlide     = "slide";
    static constexpr const char* kParamDecay     = "decay";
    static constexpr const char* kParamReverse   = "reverse";
    static constexpr const char* kParamRate      = "rate";
    static constexpr const char* kParamCrunch    = "crunch";    // bit-depth (was combined crush)
    static constexpr const char* kParamCrushRate = "crushrate"; // v0.21 — S&H decimation rate
    static constexpr const char* kParamMix       = "mix";
    static constexpr const char* kParamGlobalMix = "globalmix";
    static constexpr const char* kParamFreeze    = "freeze";
    static constexpr const char* kParamSeqRate   = "seqrate";

    // FX row param IDs (tape/sampler character).
    static constexpr const char* kParamDrive     = "drive";
    static constexpr const char* kParamTone      = "tone";
    static constexpr const char* kParamFeedback  = "feedback";
    static constexpr const char* kParamTape      = "tape";      // v0.24 — replaces wow+flutter
    static constexpr const char* kParamRingMod   = "ringmod";   // v0.24 — sine ring modulator
    static constexpr const char* kParamVarispeed = "varispeed"; // v0.24 — DJ brake/spinup
    static constexpr const char* kParamStereo    = "stereo";    // v0.24 — stereo width/pan spread
    static constexpr const char* kParamStretch   = "stretch";   // v0.25 — granular time-stretch

    // Mangle row param IDs (v0.6 — shimmer + res + fold + gate + smear + chaos).
    static constexpr const char* kParamShimmer   = "shimmer";
    static constexpr const char* kParamRes       = "res";
    static constexpr const char* kParamFold      = "fold";
    static constexpr const char* kParamGate      = "gate";
    static constexpr const char* kParamSmear     = "smear";
    static constexpr const char* kParamStutter   = "stutter";  // v0.22
    static constexpr const char* kParamChaos     = "chaos";

    // v0.9.1 — human-readable display names for MOD target dropdown.
    static juce::String displayNameForParam (const juce::String& id) noexcept
    {
        if (id == kParamDivision)  return "Division";
        if (id == kParamLookback)  return "Lookback";
        if (id == kParamRate)      return "Rate";
        if (id == kParamJudder)    return "Judder";
        if (id == kParamJudderDiv) return "Judder Div";
        if (id == kParamPitch)     return "Pitch";
        if (id == kParamSlide)     return "Slide";
        if (id == kParamDecay)     return "Decay";
        if (id == kParamReverse)   return "Reverse";
        if (id == kParamCrunch)    return "Crunch (bits)";
        if (id == kParamCrushRate) return "Crunch (rate)";
        if (id == kParamMix)       return "Mix";
        if (id == kParamDrive)     return "Drive";
        if (id == kParamTone)      return "Tone";
        if (id == kParamFeedback)  return "Feedback";
        if (id == kParamTape)      return "Tape";
        if (id == kParamRingMod)   return "Ring Mod";
        if (id == kParamVarispeed) return "Varispeed";
        if (id == kParamStereo)    return "Stereo";
        if (id == kParamStretch)   return "Stretch";
        if (id == kParamShimmer)   return "Shimmer";
        if (id == kParamRes)       return "Resonance";
        if (id == kParamFold)      return "Fold";
        if (id == kParamGate)      return "Shape";
        if (id == kParamSmear)     return "Smear";
        if (id == kParamStutter)   return "Stutter";
        if (id == kParamChaos)     return "Chaos";
        if (id == kParamChance)    return "Chance";
        if (id == kParamGlobalMix) return "Global Mix";
        return id;  // fallback: raw ID
    }

    // v0.7 — global scale quantise for PITCH. Not per-Act, not baked
    // into scenes: the same root+scale applies across the whole plugin.
    static constexpr const char* kParamScaleRoot = "scaleroot";
    static constexpr const char* kParamScaleType = "scaletype";

    // Global Swing. Range [0.5, 0.75] — drum-machine convention where
    // 0.5 = straight, 0.667 = triplet feel, 0.75 = dotted-eighth.
    static constexpr const char* kParamSwing    = "swing";


private:
    // ------- tuning constants --------------------------------------------

    // 32 s of rolling input holds 4 bars comfortably even at 30 BPM,
    // with headroom for the longest slice to play out.
    static constexpr double kBufferSeconds = 32.0;

    // ------- cached raw parameter pointers -------------------------------
    // Populated in the constructor. Audio-thread reads them as atomic
    // floats — no lock, no string lookup in processBlock.
    std::atomic<float>* pChance    = nullptr;
    std::atomic<float>* pDivision  = nullptr;
    std::atomic<float>* pLookback  = nullptr;
    std::atomic<float>* pJudder    = nullptr;
    std::atomic<float>* pJudderDiv = nullptr;
    std::atomic<float>* pPitch     = nullptr;
    std::atomic<float>* pSlide     = nullptr;
    std::atomic<float>* pDecay     = nullptr;
    std::atomic<float>* pReverse   = nullptr;
    std::atomic<float>* pRate      = nullptr;
    std::atomic<float>* pCrunch    = nullptr;
    std::atomic<float>* pCrushRate = nullptr;
    std::atomic<float>* pMix       = nullptr;
    std::atomic<float>* pGlobalMix = nullptr;
    int                 globalMixLockSlot = -1;  // cached lockableIndexForId result
    std::atomic<float>* pFreeze    = nullptr;
    std::atomic<float>* pSeqRate   = nullptr;
    std::atomic<float>* pDrive     = nullptr;
    std::atomic<float>* pTone      = nullptr;
    std::atomic<float>* pFeedback  = nullptr;
    std::atomic<float>* pTape      = nullptr;   // v0.24
    std::atomic<float>* pRingMod   = nullptr;   // v0.24
    std::atomic<float>* pVarispeed = nullptr;   // v0.24
    std::atomic<float>* pStereo    = nullptr;   // v0.24
    std::atomic<float>* pStretch   = nullptr;   // v0.25
    std::atomic<float>* pShimmer   = nullptr;
    std::atomic<float>* pRes       = nullptr;
    std::atomic<float>* pFold      = nullptr;
    std::atomic<float>* pGate      = nullptr;
    std::atomic<float>* pSmear     = nullptr;
    std::atomic<float>* pStutter   = nullptr;   // v0.22
    std::atomic<float>* pChaos     = nullptr;
    std::atomic<float>* pScaleRoot = nullptr;
    std::atomic<float>* pScaleType = nullptr;
    std::atomic<float>* pSwing     = nullptr;

    // ------- audio-thread state ------------------------------------------
    double sampleRate = 44100.0;
    int    circularBufferSize = 0;
    juce::AudioBuffer<float> circularBuffer;
    int    writePos = 0;

    // Tracks the PPQ we last evaluated so we can detect grid-boundary
    // crossings sample-by-sample. -1 means "transport hasn't played yet".
    double lastPpq = -1.0;
    bool   wasPlaying = false;  // v0.29 — detect play-start transitions

    // v0.17 — integer step offset captured on first play sample so the
    // sequencer always starts from step 0 regardless of DAW playhead
    // position. Integer domain avoids floating-point drift entirely.
    long long seqOriginStep    = 0;
    bool      seqOriginSet   = false;
    long long lastExpectedStep = 0;  // v0.29 — detect forward jumps in PPQ
    long long seqSampleCounter = 0;  // v0.29 — sample-driven step counter

    // v0.14 — track the last sequencer step so muted-step cuts trigger
    // on step entry, not just on fire-grid boundaries.
    int lastWrappedStep = -1;
    long long lastSeqStepAbsForRatio = -1;  // v0.44 — forward-only wrap detection

    long long fireCounter = 0;  // v0.5.0 — monotonic fire count for Reverse Alternate/PingPong modes

    juce::Random rng;

    // ------- sequencer state ---------------------------------------------
    // Scene storage. All atomics so the audio thread can read without
    // locks while the UI writes from the message thread.
    SceneData scenes[kNumScenes];

    // Step assignments: -1 = empty, 0..kNumScenes-1 = scene index.
    std::atomic<int> steps[kMaxSteps];

    // Per-step ratio lock. 0 = no lock (use Act's own chance). 1+ =
    // index into the N:M ratio table. See getRatioNM.
    std::atomic<int> stepRatio[kMaxSteps];

    // Audio-thread-only counter tracking how many times the step has
    // been visited since its ratio window started. Used to decide
    // whether the current visit is a "hit" within the current N:M pass.
    // Not atomic: only the audio thread reads and writes it.
    int stepRatioVisit[kMaxSteps] = {};

    // Audio-thread-only debounce for the ratio counter. Keyed on the
    // MONOTONIC sequencer step count, not wrappedStep — otherwise
    // revisits to the same wrapped index after one pattern loop would
    // read as "same step" and skip the counter advance, leaving the
    // cached result true forever. Using the absolute count means the
    // cache only protects multi-fire bursts inside a single visit.
    long long ratioLastSeenSeqStep = -1;
    bool      ratioLastAllowed     = true;
    // v0.43 — independent pattern-play counter for ratio A:B. Increments
    // each time the sequencer wraps, independent of DAW PPQ. Fixes ratios
    // being stuck when the DAW loops a section (PPQ repeats identically).
    long long ratioPatternPlayCount = 0;

    // v0.38 — per-step bookkeeping for conditional-trigger kinds.
    //   stepLastFireResult[i] — remembered from the most recent visit
    //     to step i (this pass or the previous one if we haven't reached
    //     it yet this pass). Used by IF PREV / NOT PREV at step j, which
    //     read stepLastFireResult[(j-1+seqLen) % seqLen].
    //   onceStepFired[i] — latched true once step i fires under an ONCE
    //     condition, preventing further fires until transport restarts.
    //   Both are reset on transport play-start (justStartedPlaying
    //   branch in processBlock).
    // Audio-thread-only; not atomic.
    bool stepLastFireResult[kMaxSteps] = {};
    bool onceStepFired[kMaxSteps]      = {};

    // Per-step freeze-hold flag. While this step is the current playing
    // step the buffer write head holds still, which turns the step into
    // a "play back what was here" moment regardless of what the host
    // audio is doing live.
    std::atomic<bool> stepFreezeHold[kMaxSteps];

    // v0.12 — per-step ratchet. 1 = no ratchet (single fire, the
    // default), 2/3/4/6/8 = sub-divide the step duration into that
    // many equal slices and fire once per slice. Carried as a plain
    // int because only small positive values are legal; clamped on
    // read. The audio thread uses a small "pending ratchet" state
    // struct below to stagger the extra fires over the step's window.
    std::atomic<int> stepRatchet[kMaxSteps];

    // v0.13 — per-step mute. Audio thread skips the fire entirely when
    // true, even if the step has an Act stamp, locks, ratio, ratchet,
    // or other per-step features. Lives alongside the other per-step atomics so
    // serialization and clearAllSteps stay in lockstep.
    std::atomic<bool> stepMute[kMaxSteps];

    // Ratchet run state. When a step fires with ratchet > 1 the audio
    // thread records the ShapingParams it used and schedules the
    // remaining fires at equal PPQ intervals. The sample loop checks
    // this every sample and drops the voices as the intervals cross.
    // Only touched from the audio thread.
    struct PendingRatchet
    {
        int            remaining     = 0;     // fires still to go
        double         nextFirePpq   = 0.0;   // next PPQ to cross
        double         intervalPpq   = 0.0;   // spacing between ratchet fires
        ShapingParams  params;                // cached — same voice for every sub-fire
        double         samplesPerQ   = 0.0;   // snapshot for fireVoice
        int            sceneIdx      = -1;    // v0.41 — Act stamp for engine bake
        double         subDurationQ  = 0.0;   // v0.44 — ratchet sub-fire duration (quarters)
        double         sliceStartPos = 0.0;   // v0.44 — captured buffer position from first fire
        bool           hasSlicePos   = false;  // v0.44 — true when sliceStartPos is valid
    };
    PendingRatchet pendingRatchet;

    // Per-step parameter locks. Flat [step * kNumLockableParams + slot]
    // array of atomic floats. NaN means "no lock for this (step, slot)".
    std::atomic<float> stepParamLock[kMaxSteps * kNumLockableParams];

    // v0.30 — single-level undo snapshot for the step grid.
    StepSnapshot      undoSnapshot;
    std::atomic<bool> undoAvailable { false };

    // v0.30 — debug instrumentation (private atomics).
    std::atomic<float> dbgCpuWorstCase  { 0.0f };   // worst processBlock CPU fraction
    std::atomic<float> dbgLastFireTimingUs { 0.0f }; // last fire timing error in µs
    std::atomic<bool>  dbgForceFire     { false };   // message-thread → audio-thread flag

    // Voice snapshot — written at the end of each processBlock.
    // Not atomic per-field; we accept occasional tearing since it's
    // only for the debug display.
    VoiceSnapshot dbgVoiceSnap;

    // Which scene slots have had a STORE committed to them. Used by the
    // editor to colour the STORE buttons so the user can see at a glance
    // which acts are populated.
    std::atomic<bool> sceneStored[kNumScenes];

    // Per-scene preset tracking (BUG 1 fix): -1 = none/modified, >=0 = factory preset idx
    std::atomic<int> scenePresetIdx[kNumScenes];
    bool applyingPreset = false;  // Flag to skip preset-idx reset during programmatic apply

    // v0.38 — cached list of user presets (scanned from disk). Pairs of
    // (display name, file path). Rebuilt by rescanUserPresets. All
    // access is on the message thread; we hold a lock so the editor can
    // safely query during menu construction while a save is happening.
    // v0.7.0 — category holds the sub-folder name (empty = root).
    struct UserPresetEntry { juce::String name; juce::String category; juce::File file; };
    mutable juce::CriticalSection userPresetsLock;
    std::vector<UserPresetEntry> userPresets;

    std::atomic<int>  seqLength          { kDefaultSteps };
    std::atomic<bool> seqOn              { false };
    std::atomic<bool> masterBypass       { false };  // v0.24 — full plugin bypass
    std::atomic<int>  selectedScene      { 0 };
    std::atomic<int>  currentPlayingStep { -1 };  // editor reads this to highlight the grid

    // Editor-driven momentary-freeze latch. OR'd with the APVTS Freeze
    // bool inside processBlock so both mechanisms can drive the ring
    // lock. The editor sets this true on mouseDown and false on mouseUp
    // when freeze mode = momentary.
    std::atomic<bool> momentaryFreeze { false };

    // Persisted editor preferences. Default values match the v0.7
    // "everything visible, 100% scale, toggle freeze" baseline.
    std::atomic<float> uiScale            { 1.0f };
    std::atomic<bool>  waveformVisible    { true };
    std::atomic<bool>  tooltipsEnabled    { true };

    // v0.12 — lookahead. Both values are written from the message
    // thread (setLookaheadMs) and read on the audio thread every
    // sample. The delay buffer is sized to the maximum lookahead at
    // prepareToPlay so we never have to reallocate during runtime.
    std::atomic<int>   lookaheadMs       { 0 };
    std::atomic<int>   lookaheadSamples  { 0 };
    juce::AudioBuffer<float> lookaheadBuffer;
    int                lookaheadBufferSize = 0;
    int                lookaheadWritePos   = 0;
    std::atomic<bool>  freezeMomentaryMode { false };
    std::atomic<bool>  autoFollowLength    { false };
    // v0.10 — when true, the APVTS listener skips mirroring knob
    // changes into the selected Act. Used during loadSceneToKnobs so
    // loading Act B doesn't just round-trip B's own values back into B.
    std::atomic<bool>  suppressAutoMirror  { false };
    // v0.10 — pageFollowsPlayhead: editor polls the currently playing
    // step and flips its visible page (A/B/C/D) to whichever page owns
    // that step. Persisted alongside the other UI prefs.
    std::atomic<bool>  pageFollowsPlayhead { false };

    // v0.21 — crush DSP mode flags.
    std::atomic<bool>  crushAntiAlias { true };   // post-S&H LP filter at Nyquist
    std::atomic<bool>  crushMuLaw     { true };   // mu-law companding (S950-style)
    std::atomic<bool>  crushAllAudio  { false };  // v0.22 — crush master output, not just slices

    // v0.40 — Engine: global character bundle.
    loopsab::CrossfadingOutputStage outputStage;  // post-mix colour, A/B crossfading
    std::atomic<int>     interpMode { loopsab::kInterpLinear };  // slice read interp (legacy global, kept as fallback)

    // v0.40 — "Designed for X" stash. Populated on preset load when the
    // recallEngine flag is OFF — the editor reads these to render the
    // badge, and clears them when the user clicks "switch" or dismisses.
    std::atomic<int>     designedForInterp { -1 };  // -1 = no badge active
    std::atomic<int>     designedForStage  { -1 };
    std::atomic<float>   designedForMix    { -1.0f };
    std::atomic<juce::int64> designedForLoadedAtMs { 0 };

public:
    // Public accessors so the editor can read/clear the badge state.
    int   getDesignedForInterp() const noexcept { return designedForInterp.load(); }
    int   getDesignedForStage()  const noexcept { return designedForStage .load(); }
    float getDesignedForMix()    const noexcept { return designedForMix   .load(); }
    juce::int64 getDesignedForLoadedAtMs() const noexcept { return designedForLoadedAtMs.load(); }
    void  clearDesignedFor() noexcept
    {
        designedForInterp.store (-1);
        designedForStage .store (-1);
        designedForMix   .store (-1.0f);
    }
    void  applyDesignedFor() noexcept
    {
        const int   i = designedForInterp.load();
        const int   s = designedForStage .load();
        const float m = designedForMix   .load();
        // Broadcast to all 8 Acts so the legacy "designed-for" snapshot acts
        // as a session-wide engine bundle. The CrossfadingOutputStage will
        // pick up the change on the next processBlock via the active scene.
        if (i >= 0)
        {
            interpMode.store (i);
            for (auto& sc : scenes) sc.interpMode.store (i);
        }
        if (s >= 0)      for (auto& sc : scenes) sc.engineStage.store (s);
        if (m >= 0.0f)   for (auto& sc : scenes) sc.engineMix  .store (m);
        clearDesignedFor();
    }
private:
    std::atomic<bool>  ringModQuantise { false };  // v0.25 — snap ring mod freq to scale
    std::atomic<bool>  globalDrive     { false };  // v0.25 — apply drive to all audio
    std::atomic<bool>  globalTone      { false };  // v0.25 — apply tone to all audio
    std::atomic<bool>  globalRingMod   { false };  // v0.25 — apply ring mod to all audio
    std::atomic<bool>  globalFold      { false };  // v0.25 — apply fold to all audio
    // Note: Varispeed and Stretch are inherently per-voice (they depend on
    // slice progress and grain state) so they have no "all audio" mode.
    std::atomic<int>   stereoMode      { 0 };      // v0.25 — StereoMode enum
    int                stereoPingPongSide = 0;     // alternates 0/1 per fire for ping-pong

    // v0.11 — grid accent interval (default 4 = quarter-beat accents).
    std::atomic<int>   gridAccentInterval { 4 };

    // v0.11 — sequencer transport mode (see SeqTransport enum above).
    std::atomic<int>   seqTransport { 0 };

    // v0.38 — user preference for "clear Act" behaviour. false (default)
    // means an Act clear also wipes its 4 MODs back to defaults. true
    // keeps the MOD rigs intact so you can rebuild the Act below them.
    std::atomic<bool>  preserveLfosOnClear { false };

    // v0.30 — LFO modulation state and per-sample offsets.
    // Stored per-Act (out of SceneData to avoid atomic copy issues).
    // Index as lfos[sceneIdx][lfoIdx] where sceneIdx=0..7, lfoIdx=0..3.
    LfoState lfos[kNumScenes][kNumLfosPerAct];
    float lfoModOffset[kNumLockableParams] = {};

    // v0.33 — last transport snapshot pushed by processBlock so the UI
    // (LFO scope preview) can draw at the real rate, not a guess.
    std::atomic<double> lastKnownBpm { 120.0 };
    std::atomic<bool>   isHostPlaying { false };

    // Input peak ring for the waveform strip. Audio thread accumulates
    // abs-max over kPeakBucketSamples into peakAccum, then writes to
    // the ring and advances peakWritePos.
    std::atomic<float> peakRing[kPeakRingSize];
    std::atomic<int>   peakWritePos { 0 };
    float              peakAccum     = 0.0f;
    int                peakAccumCount = 0;

    // v0.5.0 — slice output peak ring (mirrors the input ring).
    std::atomic<float> slicePeakRing[kSlicePeakRingSize];
    std::atomic<int>   slicePeakWritePos { 0 };
    float              slicePeakAccum     = 0.0f;
    int                slicePeakAccumCount = 0;

    // --- crush DSP state (reusable for per-voice and global paths) ------
    struct CrushState
    {
        int    sampleCounter = 0;
        float  held[2]       = { 0.0f, 0.0f };
        float  aaL           = 0.0f;
        float  aaR           = 0.0f;

        void reset() { sampleCounter = 0; held[0] = held[1] = aaL = aaR = 0.0f; }
    };

    // --- monophonic playback voice ---------------------------------------
    // POD struct — no virtuals, no allocation. Total state for one
    // in-flight slice including judder + slide + crunch.
    struct PlaybackVoice
    {
        bool   active             = false;

        double readPos            = 0.0;   // fractional sample index into the circular buffer
        double rate               = 1.0;   // signed: positive plays forward, negative plays backward.
                                           // Magnitude encodes pitch+rate (2^(st/12) * rateMult).
        int    samplesRemaining   = 0;     // total output samples left in this slice
        int    totalSamples       = 0;     // v0.24 — initial count, for varispeed progress

        // Judder state.
        double retrigStartPos     = 0.0;   // readPos to reset to on each sub-trigger.
                                           // For forward slices this is the buffer-side oldest
                                           // sample; for reversed slices it's the end of the
                                           // source span so playback walks backward to the start.
        double slideFactor        = 1.0;   // rate-magnitude multiplier applied on each sub-trigger
                                           // (slide preserves direction: negative rate stays negative)
        int    judderIntervalSamples = 0;  // output-sample span between sub-triggers
        int    judderCountdown    = 0;     // counts down to the next sub-trigger
        int    juddersRemaining   = 0;     // how many sub-triggers still to fire

        // Per-repeat amplitude decay — gives the pitched-delay tail feel.
        // currentGain starts at 1 and is multiplied by decayPerJudder at
        // every sub-trigger, so later repeats are progressively quieter.
        float  currentGain        = 1.0f;
        float  decayPerJudder     = 1.0f;

        // Per-voice Mix and Crunch, captured at fire time. This lets
        // different scenes have different dry/wet and grit settings
        // that apply for the full lifetime of the voice even if the
        // global knobs are automated elsewhere mid-chop.
        float  voiceMix           = 0.7f;
        float  voiceCrunch        = 0.0f;
        float  voiceCrushRate     = 0.0f;   // v0.21 — separate S&H rate

        // Crunch state — sample-and-hold decimation + bit reduction.
        CrushState crushState;

        // --- FX row (all baked at fire time, per-Act) ---------------
        // These shape the voice output for the lifetime of the fire
        // so moving the knob live after firing doesn't mid-flight
        // rewrite an in-progress slice.
        float  voiceDrive         = 0.0f;
        float  voiceTone          = 0.5f;   // 0.5 = bypass (bipolar)
        float  voiceFeedback      = 0.0f;   // 0..0.95 sent back into buffer
        float  voiceTape          = 0.0f;   // v0.24 — combined wow+flutter
        float  voiceRingMod      = 0.0f;   // v0.24 — ring mod depth
        float  voiceVarispeed    = 0.0f;   // v0.24 — brake/spinup amount
        float  voiceStereo       = 0.0f;   // v0.24 — stereo spread
        float  voiceStretch      = 0.0f;   // v0.25 — granular time-stretch

        // Ring mod oscillator phase (advanced per sample)
        double ringModPhase      = 0.0;

        // Stereo: pan position for this fire. Meaning depends on mode:
        //   Random:    fixed random value in [-1,+1], set at fire
        //   Ping-Pong: -1 or +1, alternated per fire
        //   Auto-Pan:  not used (LFO drives pan live)
        //   Haas:      -1 or +1, picks which channel gets the delay
        float  stereoPan         = 0.0f;
        double stereoLfoPhase    = 0.0;    // auto-pan LFO phase
        int    stereoHaasSamples = 0;      // haas delay in samples (0..~30ms)
        int    stereoModeCache   = 0;      // cached mode at fire time
        // v0.43 — Haas micro-delay ring buffer. Delays the already-
        // crossfaded slice signal instead of reading raw from the
        // circular buffer, avoiding clicks on judder retrigger.
        static constexpr int kHaasDelayMax = 1536; // ~35ms @ 44.1k
        float  haasBuffer[2][kHaasDelayMax] = {};
        int    haasWritePos       = 0;

        // v0.25 — granular time-stretch state. Overlapping grains read at
        // normal speed (preserving pitch) while a source cursor advances
        // at a slower rate (stretching time).
        // v0.5.0 — upgraded to 4 grains at 90° spacing with interpolated
        // reads and ~120ms grain length for smoother sound.
        static constexpr int kNumStretchGrains = 4;
        double stretchSourcePos   = 0.0;   // slow-advancing logical position
        double stretchGrainPos[kNumStretchGrains] = {};  // per-grain read heads
        int    stretchGrainLen    = 2048;  // grain length in samples
        int    stretchGrainCount  = 0;     // sample counter within grain cycle

        // v0.6 mangle row — baked at fire time, per-Act.
        float  voiceShimmer       = 0.0f;
        float  voiceRes           = 0.0f;
        float  voiceFold          = 0.0f;
        float  voiceGate          = 0.0f;
        float  voiceSmear         = 0.0f;
        float  voiceStutter       = 0.0f;   // v0.22 — micro-loop amount
        float  voiceChaos         = 0.0f;

        // v0.41 — per-voice ENGINE bake. Slice playback uses the source
        // Act's interpolation mode + crunch flags so a fire that started
        // under Act A keeps its character even if the user flips to Act B
        // mid-flight. The output-stage settings live on the master bus
        // (CrossfadingOutputStage) and aren't baked here.
        int    voiceInterpMode    = 0;     // loopsab::kInterpLinear
        bool   voiceCrushAA       = true;
        bool   voiceCrushMu       = true;
        // v0.5.0 — filter mode + frequency split bake.
        int    voiceFilterMode    = 0;     // loopsab::kFilterTilt
        int    voiceFreqSplitMode = 0;     // 0=Full, 1=Low Only, 2=High Only
        float  voiceFreqSplitHz   = 500.0f;
        // Linkwitz-Riley crossover state (two cascaded butterworth per channel).
        float  xoverLpL = 0.0f, xoverLpR = 0.0f;   // first-order LP state
        float  xoverHpL = 0.0f, xoverHpR = 0.0f;   // first-order HP state
        // v0.42 — per-voice ring-mod quantise bake. The quantise flag used
        // to be read live from a global atomic; now it's snapshotted at
        // fire time from the source Act so the voice keeps its tonality
        // regardless of later Act flips.
        bool   voiceRingModQuant  = false;

        // v0.5.0 — CHARACTER option bakes (right-click knob modes).
        int    voiceDriveType      = 0;   // loopsab::kDriveTape
        int    voiceFoldTopology   = 0;   // loopsab::kFoldSine
        int    voiceShimmerOctave  = 0;   // loopsab::kShimmerUp1
        int    voiceSmearCharacter = 0;   // loopsab::kSmearBlur
        int    voiceStutterWindow  = 0;   // loopsab::kStutterFixed
        int    voiceVarispeedCurve = 0;   // loopsab::kVariLinear
        int    voiceSlideCurve     = 0;   // loopsab::kSlideLinear
        int    voiceReverseMode    = 0;   // loopsab::kReverseRandom

        // v0.5.0 wave 2 — more CHARACTER bakes.
        int    voiceTapeMode          = 0;   // loopsab::kTapeClassic
        int    voiceRingModWave       = 0;   // loopsab::kRingSine
        int    voiceChaosDistribution = 0;   // loopsab::kChaosUniform
        int    voiceFeedbackCharacter = 0;   // loopsab::kFeedbackClean
        int    voiceStretchMode       = 0;   // loopsab::kStretchStandard
        int    voiceJudderShape       = 0;   // loopsab::kJudderEven
        int    voiceLookbackBehaviour = 0;   // loopsab::kLookbackFixed
        int    voiceDecayCurve        = 0;   // loopsab::kDecayLinear

        // Drunk-walk chaos state (Brownian motion accumulator).
        float  chaosDrunkValue        = 0.0f;

        // v0.5.0 — mode-aware feedback character state
        float  feedbackFilterL        = 0.0f;  // one-pole LP state for Filtered feedback mode
        float  feedbackFilterR        = 0.0f;
        float  feedbackDuckEnv        = 0.0f;  // envelope follower for Ducked feedback mode

        long long fireIndex        = 0;   // v0.5.0 — monotonic fire counter for Reverse mode branching

        int    sceneIdx           = -1;    // source Act index (-1 = live knobs)

        // v0.22 — stutter micro-loop state. A tiny circular buffer that
        // captures a window of audio and plays it back in a loop.
        static constexpr int kStutterBufSize = 4096; // ~93ms at 44.1k — enough for 1/64 at most tempos
        float  stutterBuf[2][kStutterBufSize] = {};
        int    stutterWritePos    = 0;
        int    stutterReadPos     = 0;
        int    stutterLoopLen     = 0;    // 0 = not stuttering
        bool   stutterCaptured    = false;

        // Chamberlin state-variable filter state (per channel). Replaces
        // the old one-pole LP so RES can pump a resonance peak at TONE's
        // cutoff. TONE drives f, RES drives damping (1 - res → squishy).
        float  svfLowL            = 0.0f;
        float  svfBandL           = 0.0f;
        float  svfLowR            = 0.0f;
        float  svfBandR           = 0.0f;

        // Shimmer sample-and-hold state. Alternates every sample to build
        // the aliased HF residual we sprinkle back on top of the clean
        // signal for the glistening bit-crush character.
        float  shimmerHeldL       = 0.0f;
        float  shimmerHeldR       = 0.0f;
        int    shimmerCounter     = 0;

        // GATE — Buchla LPG envelope + two-pole filter state.
        // v0.22 — exponential decay envelope replaces the old sine LFO.
        // Resets to 1.0 on each judder sub-trigger for percussive feel.
        float  gateEnvelope       = 1.0f;
        float  gateLpgL           = 0.0f;   // first pole L
        float  gateLpgR           = 0.0f;   // first pole R
        float  gateLpg2L          = 0.0f;   // second pole L (cascade)
        float  gateLpg2R          = 0.0f;   // second pole R (cascade)

        // CHAOS held samples — when a glitch event says "hold", we
        // freeze on the most recent in-sample and repeat it until the
        // next roll. Per-channel.
        float  chaosHeldL         = 0.0f;
        float  chaosHeldR         = 0.0f;

        // TAPE LFO phases — slow drift (wow) and fast wobble (flutter)
        // blended by a single knob. Advanced per output sample.
        double tapeWowPhase       = 0.0;
        double tapeFlutterPhase   = 0.0;

        // v0.30 — anti-click micro-fade on voice start/end. fadeGain
        // ramps 0→1 over kAntiClickSamples at the start of each fire
        // and 1→0 at the end, avoiding discontinuities at slice edges.
        static constexpr int kAntiClickSamples = 512;   // ~12ms @ 44.1k
        float  fadeGain           = 0.0f;
        bool   fadingIn           = true;

        // v0.43 — judder crossfade. On retrigger, we save the old
        // readPos and crossfade from old→new over kJudderXfadeSamples
        // so pads don't click at grain boundaries. Longer than the
        // voice start/end fade because low-frequency content needs
        // more time to blend without audible artefacts.
        static constexpr int kJudderXfadeSamples = 512; // ~12ms @ 44.1k
        int    judderXfadeRemaining = 0;
        double judderOldReadPos     = 0.0;
        double judderOldRate        = 1.0;
        float  judderOldGain        = 1.0f;  // currentGain before decay step

    };

    PlaybackVoice voice;

    // SMEAR delay line — short stereo buffer that the voice output
    // bleeds into with feedback, producing a blurred/washy haze
    // around each chop. Allocated in prepareToPlay and lives for
    // the lifetime of the processor so it accumulates smear across
    // successive fires (it doesn't reset per-voice).
    juce::AudioBuffer<float> smearBuffer;
    int smearBufferSize = 0;
    int smearWritePos   = 0;
    // Last fired voice's SMEAR amount — lives at processor scope so the
    // delay tail keeps decaying even after the voice that set it has
    // ended. Reset to 0 in prepareToPlay.
    float smearFeedAmount = 0.0f;

    // v0.5.0 — allpass filter state for Diffuse smear mode
    float smearApStateL = 0.0f;
    float smearApStateR = 0.0f;

    // v0.30 — mute gate envelope. Smoothly ramps 0↔1 instead of hard-
    // zeroing the output, eliminating clicks at mute step boundaries.
    float muteGateGain = 1.0f;

    // ------- helpers ------------------------------------------------------
    int   wrappedBufferIndex (double pos) const noexcept;
    float readBufferInterp (int channel, double pos) const noexcept;             // legacy: uses global interpMode
    float readBufferInterp (int channel, double pos, int interp) const noexcept; // v0.41: per-voice
    void fireVoice (const ShapingParams& params, double samplesPerQuarter, int sourceSceneIdx = -1, double durationOverrideQ = 0.0, double slicePosOverride = -1.0);
    void applyCrunch (float& l, float& r, float bitAmount, float rateAmount, CrushState& cs);              // legacy: globals
    void applyCrunch (float& l, float& r, float bitAmount, float rateAmount, CrushState& cs,
                      bool useAA, bool useMu);                                                              // v0.41

    // v0.22 — global crush state for "all audio" mode.
    CrushState globalCrushState;

    // v0.30 — global FX state for "all audio" toggles. These hold the
    // persistent filter/oscillator state needed when effects run on the
    // combined signal rather than per-voice slices.
    // v0.5.0 — input level for envelope follower MOD shape.
    float  envFollowInputLevel = 0.0f;  // peak abs of current sample (audio thread only)

    float  globalSvfLowL   = 0.0f;   // Tone SVF
    float  globalSvfBandL  = 0.0f;
    float  globalSvfLowR   = 0.0f;
    float  globalSvfBandR  = 0.0f;
    double globalRingModPhase = 0.0;  // Ring Mod oscillator

    ShapingParams paramsFromKnobs()       const;
    ShapingParams paramsFromScene (int i) const;

    // Overlay the step's parameter locks on top of a ShapingParams. No-op
    // on steps with no locks set. Called by fireVoice when in sequencer
    // mode so the step's custom values take priority over the Act's.
    void applyStepLocks (ShapingParams& p, int stepIdx) const;

    // Apply global scale quantise (kParamScaleRoot + kParamScaleType) to
    // a pitch-semitone value. "Off" / "Chromatic" is a no-op pass-through.
    float quantiseToScale (float semitones) const noexcept;

    void initSceneDefaults();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoopSaboteurProcessor)
};
