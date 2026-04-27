// =============================================================================
//  SettingsPage.h
//
//  Full-screen settings page that replaces the old PopupMenu. Toggled by
//  clicking the SETTINGS button in the title strip, exactly like the MOD
//  button toggles the modulation page. Built header-only so CMakeLists
//  doesn't need to grow another source file.
//
//  Design layout (1068 x 602 inside the page):
//    Header row (50 high)  -  "SETTINGS"
//    Three columns:
//      Col 1 (wide)  -  ENGINE (Output Stage / Mix / Interp / Crunch / Recall)
//      Col 2         -  GRID ACCENT, TRANSPORT, FREEZE, SCALE QUANTISE, DISPLAY
//      Col 3         -  STEREO, GLOBAL FX, RING MOD, PITCH/SLIDE, UI SCALE
//
//  Threading: every change writes through getter/setter on the processor (which
//  uses atomics) or fires a Hooks closure that the editor wires into its own
//  state. Nothing here touches the audio thread directly.
// =============================================================================
#pragma once

#include <JuceHeader.h>
#include <functional>
#include <array>
#include <vector>
#include <memory>
#include "PluginProcessor.h"
#include "OutputStage.h"
#include "FontHelper.h"

namespace loopsab {

// -----------------------------------------------------------------------------
//  Hooks the editor passes in so the settings page can reach editor-local
//  state (fineMode, freezeMomentary, tooltip window, UI scale) without us
//  having to friend-class the editor or reach into private members.
// -----------------------------------------------------------------------------
struct SettingsHooks
{
    std::function<bool()>      getFineMode;
    std::function<void(bool)>  setFineMode;            // applies pitch/slide step
    std::function<bool()>      getFreezeMomentary;
    std::function<void(bool)>  setFreezeMomentary;     // applies attachment swap
    std::function<void()>      onAccentChanged;        // refreshStepCells + repaint
    std::function<void()>      onWaveformVisChanged;   // setWaveformVisible + relayout
    std::function<void(bool)>  applyTooltipsEnabled;   // create/destroy tooltipWindow
    std::function<void()>      resetUiScale;           // applyUiScale (1.0)
};

class SettingsPage : public juce::Component
{
public:
    SettingsPage (LoopSaboteurProcessor* p, SettingsHooks h)
        : proc (p), hooks (std::move (h)) {}

    ~SettingsPage() override
    {
        applyStencilLookAndFeel (nullptr);
    }

    void buildControls();
    void applyStencilLookAndFeel (juce::LookAndFeel* laf);
    void updateFromProcessor();

    // v0.41.2 — set which Act's per-Act bundle this page is editing.
    // Mirrors ModPage::setCurrentActTab. The Engine controls re-bind to
    // getActEngineX/setActEngineX for this Act.
    void setCurrentActTab (int actIdx);

    void paint   (juce::Graphics&) override;
    void resized() override;

private:
    LoopSaboteurProcessor* proc = nullptr;
    SettingsHooks          hooks;
    juce::LookAndFeel*     stencilLaf = nullptr;

    // v0.41.2 — which Act the per-Act sections (Engine, etc.) are editing.
    int currentActIdx = 0;

    // v0.42 — two-view mode. true → GLOBAL view (Grid Accent / Transport /
    // Freeze / Display / UI Scale / Scale Quantise / Pitch Fine / Preset
    // Recall). false → PER-ACT view (Engine / Stereo / FX on Dry / Ring
    // Mod). The tab strip has GLOBAL as the first tab and A..H after.
    bool showGlobalView = false;

    // ---------- panel rectangles (recomputed in resized()) ----------------
    juce::Rectangle<int> headerR;
    juce::Rectangle<int> actTabStripR;     // GLOBAL + A..H selector strip
    juce::Rectangle<int> enginePanelR;
    juce::Rectangle<int> accentPanelR, transportPanelR, freezePanelR,
                         scalePanelR, displayPanelR;
    juce::Rectangle<int> stereoPanelR, fxPanelR, ringModPanelR,
                         pitchPanelR, uiPanelR;
    // v0.5.0 — CHARACTER panel (replaces enginePanelR when CHARACTER sub-tab active)
    juce::Rectangle<int> characterPanelR;

    // ---------- Tab selector: GLOBAL + A..H -------------------------------
    juce::TextButton globalTab { "GLOBAL" };
    std::array<juce::TextButton, 8> actTabs;            // A B C D E F G H

    // Apply setVisible to every control in allInteractive based on the
    // current view mode. Called from setCurrentActTab() and buildControls().
    void applyViewModeVisibility();

    // v0.5.0 — Show/hide ENGINE and CHARACTER sub-tab controls.
    void applySubTabVisibility();

    // ---------- ENGINE controls -------------------------------------------
    juce::ComboBox stageBox;
    juce::Slider   outputMixSlider;                     // 0..100% (continuous)
    juce::Slider   engineIntensitySlider;               // 0..100% (continuous)
    std::array<juce::TextButton, 3> interpButtons;      // Linear / Drop / Cubic

    // v0.42.1 — Crunch got restyled: each sub-toggle is now an explicit
    // ON/OFF paired radio for glanceability. Anti-alias uses radio group
    // 906, μ-law (S950-style) uses 907 — disjoint from transport (903),
    // interp (901), recall (902), freeze (904), stereo (905). That matters
    // because JUCE's radio-group semantics are global to the parent, so
    // reusing an id across unrelated controls used to silently untoggle
    // the wrong button whenever updateFromProcessor() re-synced state.
    juce::TextButton crunchAAOn    { "ON"  };
    juce::TextButton crunchAAOff   { "OFF" };
    juce::TextButton crunchMuOn    { "ON"  };
    juce::TextButton crunchMuOff   { "OFF" };

    // v0.5.0 — filter mode radio buttons (Tilt/LP/HP/BP) and frequency split.
    std::array<juce::TextButton, 4> filterButtons;          // Tilt / LP / HP / BP
    std::array<juce::TextButton, 3> freqSplitButtons;       // Full / Low / High
    juce::Slider   freqSplitSlider;                          // crossover Hz

    // v0.5.0 — ENGINE / CHARACTER sub-tab within the per-Act view.
    bool showCharacterSubTab = false;
    juce::TextButton engineSubTab    { "ENGINE" };
    juce::TextButton characterSubTab { "CHARACTER" };

    // ---------- CHARACTER controls (per-Act knob modes) -----------------
    std::array<juce::TextButton, 4> driveTypeButtons;       // Tape/Tube/Diode/Fuzz
    std::array<juce::TextButton, 3> foldTopologyButtons;    // Sine/Triangle/Asymmetric
    std::array<juce::TextButton, 5> shimmerOctaveButtons;   // +1Oct/+2Oct/-1Oct/+5th/+5th+Oct
    std::array<juce::TextButton, 3> smearCharacterButtons;  // Blur/Diffuse/Freeze
    std::array<juce::TextButton, 3> stutterWindowButtons;   // Fixed/Decaying/Growing
    std::array<juce::TextButton, 3> varispeedCurveButtons;  // Linear/Exponential/Sudden
    std::array<juce::TextButton, 4> slideCurveButtons;      // Linear/Exponential/Log/S-Curve
    std::array<juce::TextButton, 4> reverseModeButtons;     // Random/Alternate/Palindrome/PingPong
    std::array<juce::TextButton, 4> tapeModeButtons;            // Classic/WowOnly/FlutterOnly/Extreme
    std::array<juce::TextButton, 4> ringModWaveButtons;         // Sine/Square/Triangle/Saw
    std::array<juce::TextButton, 4> chaosDistributionButtons;   // Uniform/Gaussian/DrunkWalk/BipolarSnap
    std::array<juce::TextButton, 4> feedbackCharacterButtons;   // Clean/Filtered/Saturated/Ducked
    std::array<juce::TextButton, 4> stretchModeButtons;         // Standard/Paulstretch/Spectral/Formant
    std::array<juce::TextButton, 4> judderShapeButtons;         // Even/Accelerating/Decelerating/Random
    std::array<juce::TextButton, 3> lookbackBehaviourButtons;  // Fixed/Jittered/Quantised
    std::array<juce::TextButton, 4> decayCurveButtons;         // Linear/Exponential/Logarithmic/Gate

    // ---------- GRID ACCENT -----------------------------------------------
    juce::ComboBox accentBox;                            // Off + 2..12

    // ---------- TRANSPORT -------------------------------------------------
    std::array<juce::TextButton, 4> transportButtons;   // Forward / Reverse / PingPong / Random

    // ---------- FREEZE MODE -----------------------------------------------
    std::array<juce::TextButton, 2> freezeButtons;      // Toggle / Momentary

    // ---------- SCALE QUANTISE --------------------------------------------
    juce::ComboBox scaleRootBox, scaleTypeBox;

    // ---------- DISPLAY ---------------------------------------------------
    juce::TextButton displayWaveform { "Show waveform strip" };
    juce::TextButton displayTooltips { "Show tooltips" };
    juce::TextButton displayPageFollow { "Page follows playhead" };
    juce::TextButton setDefaultsBtn { "Set as default" };

    // ---------- STEREO ----------------------------------------------------
    std::array<juce::TextButton, 4> stereoButtons;      // Random / PingPong / AutoPan / Haas

    // ---------- GLOBAL FX (FX on dry) -------------------------------------
    juce::TextButton fxAll    { "All" };
    juce::TextButton fxDrive  { "Drive" };
    juce::TextButton fxTone   { "Tone" };
    juce::TextButton fxRing   { "Ring Mod" };
    juce::TextButton fxFold   { "Fold" };
    // v0.42.1 — the old "apply crunch to all audio" paired radio that used
    // to sit in the Crunch section now lives here instead, because that's
    // exactly what it does: bleed Crunch (bits + rate) into the dry
    // signal. Backed by the same per-Act flag (getActCrushAll).
    juce::TextButton fxCrunch { "Crunch (bits / rate)" };

    // ---------- RING MOD --------------------------------------------------
    juce::TextButton ringQuant { "Quantise to scale" };

    // ---------- PITCH/SLIDE -----------------------------------------------
    juce::TextButton fineToggle { "Fine-tune mode (cents)" };

    // ---------- UI SCALE --------------------------------------------------
    juce::TextButton uiResetButton { "Reset UI to 100%" };

    // Track all components for L&F application + a cheap setVisible loop.
    std::vector<juce::Component*> allInteractive;

    // ---------- helpers ---------------------------------------------------
    void styleSection  (juce::TextButton& b, bool radio = false);
    void layoutPanel   (juce::Rectangle<int>& bounds, juce::Rectangle<int>& slot,
                        int panelHeight) const;

    static juce::String stageMenuLabel (int idx);
    void  paintPanelHeader (juce::Graphics& g, juce::Rectangle<int> r,
                            const juce::String& title) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsPage)
};

// =============================================================================
//  Implementation (header-only).
// =============================================================================

inline juce::String SettingsPage::stageMenuLabel (int idx)
{
    juce::String s;
    s << outputStageShortName (idx);
    if (idx != kOutClean)
        s << " — " << outputStageDescription (idx);
    return s;
}

inline void SettingsPage::styleSection (juce::TextButton& b, bool /*radio*/)
{
    // Toggle-style appearance: dark fill when off, accent fill when on.
    b.setClickingTogglesState (true);
    b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff111111));
    b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0x33ff5a3c));
    b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff999999));
    b.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xffff5a3c));
}

inline void SettingsPage::buildControls()
{
    if (! proc) return;

    auto wireToggleBtn = [this] (juce::TextButton& b,
                                  std::function<bool()> getter,
                                  std::function<void(bool)> setter)
    {
        styleSection (b);
        b.setToggleState (getter(), juce::dontSendNotification);
        b.onClick = [&b, getter, setter] {
            const bool now = ! getter();
            setter (now);
            b.setToggleState (now, juce::dontSendNotification);
        };
        addAndMakeVisible (b);
        allInteractive.push_back (&b);
    };

    auto wireRadioBtn = [this] (juce::TextButton& b,
                                 std::function<bool()> isCurrent,
                                 std::function<void()> activate,
                                 int radioGroupId)
    {
        styleSection (b);
        b.setRadioGroupId (radioGroupId);
        b.setToggleState (isCurrent(), juce::dontSendNotification);
        b.onClick = [activate] { activate(); };
        addAndMakeVisible (b);
        allInteractive.push_back (&b);
    };

    // ---------- Tab selector: GLOBAL + A..H -------------------------------
    // v0.42 — leading GLOBAL tab lets users see/edit only the global
    // panels (everything that applies across all Acts). Default lands on
    // the currently-selected Act so opening the page lands on the Act you
    // were playing, PER-ACT view.
    {
        static const char* actNames[8] = { "A", "B", "C", "D", "E", "F", "G", "H" };
        currentActIdx   = juce::jlimit (0, 7, proc->getSelectedScene());
        showGlobalView  = false;

        globalTab.setClickingTogglesState (true);
        globalTab.setRadioGroupId (990);
        globalTab.setColour (juce::TextButton::buttonColourId,   juce::Colour (0x00000000));
        globalTab.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff999999));
        globalTab.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0x33ff5a3c));
        globalTab.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xffff5a3c));
        globalTab.setToggleState (showGlobalView, juce::dontSendNotification);
        globalTab.onClick = [this] { setCurrentActTab (-1); };
        addAndMakeVisible (globalTab);
        allInteractive.push_back (&globalTab);

        for (int i = 0; i < 8; ++i)
        {
            actTabs[i].setButtonText (actNames[i]);
            actTabs[i].setClickingTogglesState (true);
            actTabs[i].setRadioGroupId (990);
            actTabs[i].setColour (juce::TextButton::buttonColourId,   juce::Colour (0x00000000));
            actTabs[i].setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff999999));
            actTabs[i].setColour (juce::TextButton::buttonOnColourId, juce::Colour (0x33ff5a3c));
            actTabs[i].setColour (juce::TextButton::textColourOnId,   juce::Colour (0xffff5a3c));
            actTabs[i].setToggleState (! showGlobalView && i == currentActIdx, juce::dontSendNotification);
            actTabs[i].onClick = [this, i] { setCurrentActTab (i); };
            addAndMakeVisible (actTabs[i]);
            allInteractive.push_back (&actTabs[i]);
        }
    }

    // ---------- ENGINE: Output Stage combo (per-Act) ----------------------
    stageBox.clear (juce::dontSendNotification);
    for (int i = 0; i < (int) kNumOutputStages; ++i)
        stageBox.addItem (stageMenuLabel (i), i + 1);
    stageBox.setSelectedId (proc->getActEngineStage (currentActIdx) + 1, juce::dontSendNotification);
    stageBox.onChange = [this] {
        const int stage = stageBox.getSelectedId() - 1;
        proc->setActEngineStage (currentActIdx, stage);
        // v0.42 — OUTPUT MIX and INTENSITY are meaningless for CLEAN.
        const bool active = (stage != (int) kOutClean);
        outputMixSlider.setEnabled (active);
        engineIntensitySlider.setEnabled (active);
    };
    addAndMakeVisible (stageBox);
    allInteractive.push_back (&stageBox);

    // ---------- ENGINE: Output Mix slider (0..100 %) ----------------------
    outputMixSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    outputMixSlider.setRange (0.0, 100.0, 1.0);
    outputMixSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);
    outputMixSlider.setTextValueSuffix (" %");
    outputMixSlider.setValue (proc->getActEngineMix (currentActIdx) * 100.0, juce::dontSendNotification);
    outputMixSlider.onValueChange = [this] {
        proc->setActEngineMix (currentActIdx, (float) (outputMixSlider.getValue() * 0.01));
    };
    // v0.42 — match initial enabled state to the current stage selection.
    outputMixSlider.setEnabled (proc->getActEngineStage (currentActIdx) != (int) kOutClean);
    addAndMakeVisible (outputMixSlider);
    allInteractive.push_back (&outputMixSlider);

    // ---------- ENGINE: Intensity slider (0..100 %) -------------------------
    engineIntensitySlider.setSliderStyle (juce::Slider::LinearHorizontal);
    engineIntensitySlider.setRange (0.0, 100.0, 1.0);
    engineIntensitySlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);
    engineIntensitySlider.setTextValueSuffix (" %");
    engineIntensitySlider.setValue (proc->getActEngineIntensity (currentActIdx) * 100.0, juce::dontSendNotification);
    engineIntensitySlider.onValueChange = [this] {
        proc->setActEngineIntensity (currentActIdx, (float) (engineIntensitySlider.getValue() * 0.01));
    };
    // v0.7.0 — match initial enabled state to the current stage selection.
    engineIntensitySlider.setEnabled (proc->getActEngineStage (currentActIdx) != (int) kOutClean);
    addAndMakeVisible (engineIntensitySlider);
    allInteractive.push_back (&engineIntensitySlider);

    // ---------- ENGINE: Interpolation radio (3, per-Act) ------------------
    for (int i = 0; i < 3; ++i)
    {
        interpButtons[i].setButtonText (interpShortName (i));
        wireRadioBtn (interpButtons[i],
                      [this, i] { return proc->getActInterpMode (currentActIdx) == i; },
                      [this, i] { proc->setActInterpMode (currentActIdx, i); },
                      901);
    }

    // ---------- ENGINE: Crunch toggles (per-Act) --------------------------
    // v0.42.1 — Anti-alias and μ-law are each now an ON/OFF paired radio
    // rather than a single lit-when-on toggle. Radio groups 906/907 are
    // deliberately unique to crunch so transport/stereo/etc. can't clash.
    wireRadioBtn (crunchAAOn,
                  [this] { return   proc->getActCrushAA (currentActIdx); },
                  [this] { proc->setActCrushAA (currentActIdx, true);  },
                  906);
    wireRadioBtn (crunchAAOff,
                  [this] { return ! proc->getActCrushAA (currentActIdx); },
                  [this] { proc->setActCrushAA (currentActIdx, false); },
                  906);
    wireRadioBtn (crunchMuOn,
                  [this] { return   proc->getActCrushMu (currentActIdx); },
                  [this] { proc->setActCrushMu (currentActIdx, true);  },
                  907);
    wireRadioBtn (crunchMuOff,
                  [this] { return ! proc->getActCrushMu (currentActIdx); },
                  [this] { proc->setActCrushMu (currentActIdx, false); },
                  907);

    // ---------- ENGINE: Filter mode radio (Tilt/LP/HP/BP, per-Act) --------
    {
        static const char* fNames[] = { "Tilt", "LP", "HP", "BP" };
        for (int i = 0; i < 4; ++i)
        {
            filterButtons[i].setButtonText (fNames[i]);
            wireRadioBtn (filterButtons[i],
                          [this, i] { return proc->getActFilterMode (currentActIdx) == i; },
                          [this, i] { proc->setActFilterMode (currentActIdx, i); },
                          908);
        }
    }

    // ---------- ENGINE: Frequency split radio (Full/Low/High, per-Act) ---
    {
        static const char* sNames[] = { "Full Range", "Low Only", "High Only" };
        for (int i = 0; i < 3; ++i)
        {
            freqSplitButtons[i].setButtonText (sNames[i]);
            wireRadioBtn (freqSplitButtons[i],
                          [this, i] { return proc->getActFreqSplitMode (currentActIdx) == i; },
                          [this, i] {
                              proc->setActFreqSplitMode (currentActIdx, i);
                              freqSplitSlider.setVisible (i != 0);
                          },
                          909);
        }
    }

    // ---------- ENGINE: Frequency split crossover Hz slider (per-Act) ----
    freqSplitSlider.setRange (80.0, 8000.0, 1.0);
    freqSplitSlider.setSkewFactorFromMidPoint (500.0);
    freqSplitSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 72, 22);
    freqSplitSlider.setTextValueSuffix (" Hz");
    freqSplitSlider.setValue (proc->getActFreqSplitHz (currentActIdx), juce::dontSendNotification);
    freqSplitSlider.onValueChange = [this] {
        proc->setActFreqSplitHz (currentActIdx, (float) freqSplitSlider.getValue());
    };
    freqSplitSlider.setVisible (proc->getActFreqSplitMode (currentActIdx) != 0);
    addAndMakeVisible (freqSplitSlider);
    allInteractive.push_back (&freqSplitSlider);

    // v0.5.0 — ENGINE/CHARACTER sub-tab buttons within per-Act view.
    {
        auto wireSubTab = [this] (juce::TextButton& b, bool isCharTab) {
            styleSection (b);
            b.setRadioGroupId (918);
            b.setToggleState (isCharTab == showCharacterSubTab, juce::dontSendNotification);
            b.onClick = [this, isCharTab] {
                showCharacterSubTab = isCharTab;
                applySubTabVisibility();
                resized();
                repaint();
            };
            addAndMakeVisible (b);
            allInteractive.push_back (&b);
        };
        wireSubTab (engineSubTab,    false);
        wireSubTab (characterSubTab, true);
    }

    // ---------- CHARACTER: Drive Type (Tape/Tube/Diode/Fuzz) per-Act ------
    {
        for (int i = 0; i < (int) loopsab::kNumDriveTypes; ++i)
        {
            driveTypeButtons[(size_t)i].setButtonText (loopsab::driveShortName (i));
            wireRadioBtn (driveTypeButtons[(size_t)i],
                          [this, i] { return proc->getActDriveType (currentActIdx) == i; },
                          [this, i] { proc->setActDriveType (currentActIdx, i); },
                          910);
        }
    }
    // ---------- CHARACTER: Fold Topology (Sine/Triangle/Asymmetric) -------
    {
        for (int i = 0; i < (int) loopsab::kNumFoldTopologies; ++i)
        {
            foldTopologyButtons[(size_t)i].setButtonText (loopsab::foldShortName (i));
            wireRadioBtn (foldTopologyButtons[(size_t)i],
                          [this, i] { return proc->getActFoldTopology (currentActIdx) == i; },
                          [this, i] { proc->setActFoldTopology (currentActIdx, i); },
                          911);
        }
    }
    // ---------- CHARACTER: Shimmer Octave (+1Oct/+2Oct/-1Oct/+5th/+5th+Oct)
    {
        for (int i = 0; i < (int) loopsab::kNumShimmerOctaves; ++i)
        {
            shimmerOctaveButtons[(size_t)i].setButtonText (loopsab::shimmerShortName (i));
            wireRadioBtn (shimmerOctaveButtons[(size_t)i],
                          [this, i] { return proc->getActShimmerOctave (currentActIdx) == i; },
                          [this, i] { proc->setActShimmerOctave (currentActIdx, i); },
                          912);
        }
    }
    // ---------- CHARACTER: Smear Character (Blur/Diffuse/Freeze) ----------
    {
        for (int i = 0; i < (int) loopsab::kNumSmearCharacters; ++i)
        {
            smearCharacterButtons[(size_t)i].setButtonText (loopsab::smearShortName (i));
            wireRadioBtn (smearCharacterButtons[(size_t)i],
                          [this, i] { return proc->getActSmearCharacter (currentActIdx) == i; },
                          [this, i] { proc->setActSmearCharacter (currentActIdx, i); },
                          913);
        }
    }
    // ---------- CHARACTER: Stutter Window (Fixed/Decaying/Growing) --------
    {
        for (int i = 0; i < (int) loopsab::kNumStutterWindows; ++i)
        {
            stutterWindowButtons[(size_t)i].setButtonText (loopsab::stutterShortName (i));
            wireRadioBtn (stutterWindowButtons[(size_t)i],
                          [this, i] { return proc->getActStutterWindow (currentActIdx) == i; },
                          [this, i] { proc->setActStutterWindow (currentActIdx, i); },
                          914);
        }
    }
    // ---------- CHARACTER: Varispeed Curve (Linear/Exponential/Sudden) ----
    {
        for (int i = 0; i < (int) loopsab::kNumVarispeedCurves; ++i)
        {
            varispeedCurveButtons[(size_t)i].setButtonText (loopsab::varispeedShortName (i));
            wireRadioBtn (varispeedCurveButtons[(size_t)i],
                          [this, i] { return proc->getActVarispeedCurve (currentActIdx) == i; },
                          [this, i] { proc->setActVarispeedCurve (currentActIdx, i); },
                          915);
        }
    }
    // ---------- CHARACTER: Slide Curve (Linear/Exponential/Log/S-Curve) ---
    {
        for (int i = 0; i < (int) loopsab::kNumSlideCurves; ++i)
        {
            slideCurveButtons[(size_t)i].setButtonText (loopsab::slideShortName (i));
            wireRadioBtn (slideCurveButtons[(size_t)i],
                          [this, i] { return proc->getActSlideCurve (currentActIdx) == i; },
                          [this, i] { proc->setActSlideCurve (currentActIdx, i); },
                          916);
        }
    }
    // ---------- CHARACTER: Reverse Mode (Random/Alternate/Palindrome/PingPong)
    {
        for (int i = 0; i < (int) loopsab::kNumReverseModes; ++i)
        {
            reverseModeButtons[(size_t)i].setButtonText (loopsab::reverseShortName (i));
            wireRadioBtn (reverseModeButtons[(size_t)i],
                          [this, i] { return proc->getActReverseMode (currentActIdx) == i; },
                          [this, i] { proc->setActReverseMode (currentActIdx, i); },
                          917);
        }
    }
    // ---------- CHARACTER: Tape Mode (Classic/WowOnly/FlutterOnly/Extreme) --
    {
        for (int i = 0; i < (int) loopsab::kNumTapeModes; ++i)
        {
            tapeModeButtons[(size_t)i].setButtonText (loopsab::tapeShortName (i));
            wireRadioBtn (tapeModeButtons[(size_t)i],
                          [this, i] { return proc->getActTapeMode (currentActIdx) == i; },
                          [this, i] { proc->setActTapeMode (currentActIdx, i); },
                          919);
        }
    }
    // ---------- CHARACTER: Ring Mod Waveform (Sine/Square/Triangle/Saw) -----
    {
        for (int i = 0; i < (int) loopsab::kNumRingModWaves; ++i)
        {
            ringModWaveButtons[(size_t)i].setButtonText (loopsab::ringModWaveShortName (i));
            wireRadioBtn (ringModWaveButtons[(size_t)i],
                          [this, i] { return proc->getActRingModWave (currentActIdx) == i; },
                          [this, i] { proc->setActRingModWave (currentActIdx, i); },
                          920);
        }
    }
    // ---------- CHARACTER: Chaos Distribution (Uniform/Gaussian/DrunkWalk/BipolarSnap)
    {
        for (int i = 0; i < (int) loopsab::kNumChaosDistributions; ++i)
        {
            chaosDistributionButtons[(size_t)i].setButtonText (loopsab::chaosShortName (i));
            wireRadioBtn (chaosDistributionButtons[(size_t)i],
                          [this, i] { return proc->getActChaosDistribution (currentActIdx) == i; },
                          [this, i] { proc->setActChaosDistribution (currentActIdx, i); },
                          921);
        }
    }
    // ---------- CHARACTER: Feedback Character (Clean/Filtered/Saturated/Ducked)
    {
        for (int i = 0; i < (int) loopsab::kNumFeedbackCharacters; ++i)
        {
            feedbackCharacterButtons[(size_t)i].setButtonText (loopsab::feedbackShortName (i));
            wireRadioBtn (feedbackCharacterButtons[(size_t)i],
                          [this, i] { return proc->getActFeedbackCharacter (currentActIdx) == i; },
                          [this, i] { proc->setActFeedbackCharacter (currentActIdx, i); },
                          922);
        }
    }
    // ---------- CHARACTER: Stretch Mode (Standard/Paulstretch/Spectral/Formant)
    {
        for (int i = 0; i < (int) loopsab::kNumStretchModes; ++i)
        {
            stretchModeButtons[(size_t)i].setButtonText (loopsab::stretchShortName (i));
            wireRadioBtn (stretchModeButtons[(size_t)i],
                          [this, i] { return proc->getActStretchMode (currentActIdx) == i; },
                          [this, i] { proc->setActStretchMode (currentActIdx, i); },
                          923);
        }
    }
    // ---------- CHARACTER: Judder Shape (Even/Accelerating/Decelerating/Random)
    {
        for (int i = 0; i < (int) loopsab::kNumJudderShapes; ++i)
        {
            judderShapeButtons[(size_t)i].setButtonText (loopsab::judderShortName (i));
            wireRadioBtn (judderShapeButtons[(size_t)i],
                          [this, i] { return proc->getActJudderShape (currentActIdx) == i; },
                          [this, i] { proc->setActJudderShape (currentActIdx, i); },
                          924);
        }
    }
    // ---------- CHARACTER: Lookback Behaviour (Fixed/Jittered/Quantised)
    {
        for (int i = 0; i < (int) loopsab::kNumLookbackBehaviours; ++i)
        {
            lookbackBehaviourButtons[(size_t)i].setButtonText (loopsab::lookbackShortName (i));
            wireRadioBtn (lookbackBehaviourButtons[(size_t)i],
                          [this, i] { return proc->getActLookbackBehaviour (currentActIdx) == i; },
                          [this, i] { proc->setActLookbackBehaviour (currentActIdx, i); },
                          925);
        }
    }
    // ---------- CHARACTER: Decay Curve (Linear/Exponential/Logarithmic/Gate)
    {
        for (int i = 0; i < (int) loopsab::kNumDecayCurves; ++i)
        {
            decayCurveButtons[(size_t)i].setButtonText (loopsab::decayShortName (i));
            wireRadioBtn (decayCurveButtons[(size_t)i],
                          [this, i] { return proc->getActDecayCurve (currentActIdx) == i; },
                          [this, i] { proc->setActDecayCurve (currentActIdx, i); },
                          926);
        }
    }

    // ---------- GRID ACCENT combo (Off + 2..12) ---------------------------
    accentBox.clear (juce::dontSendNotification);
    accentBox.addItem ("Off", 1);             // id 1  = value 0
    for (int n = 2; n <= 12; ++n)
        accentBox.addItem (juce::String (n), n);   // id n = value n  (2..12)
    {
        const int cur = proc->getGridAccentInterval();
        accentBox.setSelectedId (cur <= 0 ? 1 : juce::jlimit (2, 12, cur),
                                  juce::dontSendNotification);
    }
    accentBox.onChange = [this] {
        const int id = accentBox.getSelectedId();
        const int v  = (id == 1) ? 0 : id;            // map id -> accent value
        proc->setGridAccentInterval (v);
        if (hooks.onAccentChanged) hooks.onAccentChanged();
    };
    addAndMakeVisible (accentBox);
    allInteractive.push_back (&accentBox);

    // ---------- TRANSPORT radio -------------------------------------------
    static const char* trLabels[4] = { "Forward", "Reverse", "Ping-Pong", "Random" };
    static const int   trValues[4] = {
        LoopSaboteurProcessor::kTransportForward,
        LoopSaboteurProcessor::kTransportReverse,
        LoopSaboteurProcessor::kTransportPingPong,
        LoopSaboteurProcessor::kTransportRandom };
    for (int i = 0; i < 4; ++i)
    {
        transportButtons[i].setButtonText (trLabels[i]);
        const int v = trValues[i];
        wireRadioBtn (transportButtons[i],
                      [this, v] { return proc->getSeqTransport() == v; },
                      [this, v] { proc->setSeqTransport (v); },
                      903);
    }

    // ---------- FREEZE MODE radio -----------------------------------------
    freezeButtons[0].setButtonText ("Toggle (latching)");
    freezeButtons[1].setButtonText ("Momentary (hold)");
    wireRadioBtn (freezeButtons[0],
                  [this] { return ! (hooks.getFreezeMomentary && hooks.getFreezeMomentary()); },
                  [this] { if (hooks.setFreezeMomentary) hooks.setFreezeMomentary (false); },
                  904);
    wireRadioBtn (freezeButtons[1],
                  [this] { return   (hooks.getFreezeMomentary && hooks.getFreezeMomentary()); },
                  [this] { if (hooks.setFreezeMomentary) hooks.setFreezeMomentary (true); },
                  904);

    // ---------- SCALE QUANTISE: Root + Scale combos -----------------------
    {
        auto* rootParam = dynamic_cast<juce::AudioParameterChoice*> (
            proc->apvts.getParameter (LoopSaboteurProcessor::kParamScaleRoot));
        scaleRootBox.clear (juce::dontSendNotification);
        if (rootParam)
        {
            for (int i = 0; i < (int) rootParam->choices.size(); ++i)
                scaleRootBox.addItem (rootParam->choices[i], i + 1);
            scaleRootBox.setSelectedId (rootParam->getIndex() + 1, juce::dontSendNotification);
            scaleRootBox.onChange = [this, rootParam] {
                const int idx = scaleRootBox.getSelectedId() - 1;
                rootParam->beginChangeGesture();
                rootParam->setValueNotifyingHost (
                    (float) idx / (float) (rootParam->choices.size() - 1));
                rootParam->endChangeGesture();
            };
        }
        addAndMakeVisible (scaleRootBox);
        allInteractive.push_back (&scaleRootBox);

        auto* scaleParam = dynamic_cast<juce::AudioParameterChoice*> (
            proc->apvts.getParameter (LoopSaboteurProcessor::kParamScaleType));
        scaleTypeBox.clear (juce::dontSendNotification);
        if (scaleParam)
        {
            for (int i = 0; i < (int) scaleParam->choices.size(); ++i)
                scaleTypeBox.addItem (scaleParam->choices[i], i + 1);
            scaleTypeBox.setSelectedId (scaleParam->getIndex() + 1, juce::dontSendNotification);
            scaleTypeBox.onChange = [this, scaleParam] {
                const int idx = scaleTypeBox.getSelectedId() - 1;
                scaleParam->beginChangeGesture();
                scaleParam->setValueNotifyingHost (
                    (float) idx / (float) (scaleParam->choices.size() - 1));
                scaleParam->endChangeGesture();
            };
        }
        addAndMakeVisible (scaleTypeBox);
        allInteractive.push_back (&scaleTypeBox);
    }

    // ---------- DISPLAY toggles -------------------------------------------
    wireToggleBtn (displayWaveform,
                   [this] { return proc->getWaveformVisible(); },
                   [this] (bool v) {
                       proc->setWaveformVisible (v);
                       if (hooks.onWaveformVisChanged) hooks.onWaveformVisChanged();
                   });
    wireToggleBtn (displayTooltips,
                   [this] { return proc->getTooltipsEnabled(); },
                   [this] (bool v) {
                       proc->setTooltipsEnabled (v);
                       if (hooks.applyTooltipsEnabled) hooks.applyTooltipsEnabled (v);
                   });
    wireToggleBtn (displayPageFollow,
                   [this] { return proc->getPageFollowsPlayhead(); },
                   [this] (bool v) { proc->setPageFollowsPlayhead (v); });

    // v0.7.0 — "Set as default" saves current UI prefs to disk.
    setDefaultsBtn.setClickingTogglesState (false);
    setDefaultsBtn.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff1a1a1a));
    setDefaultsBtn.setColour (juce::TextButton::textColourOffId, juce::Colour (0xffbbbbbb));
    setDefaultsBtn.onClick = [this]
    {
        proc->saveGlobalDefaults();
        // Brief visual feedback: flash the button text.
        // Use a SafePointer so the timer callback is a no-op if
        // the SettingsPage is destroyed before it fires.
        setDefaultsBtn.setButtonText ("Saved!");
        auto safeThis = juce::Component::SafePointer<SettingsPage> (this);
        juce::Timer::callAfterDelay (800, [safeThis]
        {
            if (safeThis != nullptr)
                safeThis->setDefaultsBtn.setButtonText ("Set as default");
        });
    };
    addAndMakeVisible (setDefaultsBtn);

    // ---------- STEREO radio ----------------------------------------------
    static const char* stLabels[4] = { "Random", "Ping-Pong", "Auto-Pan", "Haas" };
    static const int   stValues[4] = {
        LoopSaboteurProcessor::kStereoRandom,
        LoopSaboteurProcessor::kStereoPingPong,
        LoopSaboteurProcessor::kStereoAutoPan,
        LoopSaboteurProcessor::kStereoHaas };
    // v0.42 — STEREO MODE is now per-Act. Each radio button writes to the
    // currently-selected Settings-page Act tab (currentActIdx) and the
    // read-back predicate checks that same slot so switching tabs updates
    // the visual immediately via updateFromProcessor().
    for (int i = 0; i < 4; ++i)
    {
        stereoButtons[i].setButtonText (stLabels[i]);
        const int v = stValues[i];
        wireRadioBtn (stereoButtons[i],
                      [this, v] { return proc->getActStereoMode (currentActIdx) == v; },
                      [this, v] { proc->setActStereoMode (currentActIdx, v); },
                      905);
    }

    // ---------- FX-ON-DRY toggles (per-Act, v0.42) -----------------------
    // Were previously global; now every Act carries its own five flags:
    // Drive / Tone / Ring Mod / Fold + Crunch (bits / rate).
    //
    // v0.42.2 — Crunch is now part of the "All" interlock. It's still a
    // distinct flag under the hood (getActCrushAll, not a sibling of
    // fxOnDry*) but from a user's perspective everything in this panel is
    // "bleed this FX into the dry signal", so toggling All flips all five.
    auto allFxOnDryBits = [this]
    {
        return proc->getActFxOnDryDrive   (currentActIdx)
            && proc->getActFxOnDryTone    (currentActIdx)
            && proc->getActFxOnDryRingMod (currentActIdx)
            && proc->getActFxOnDryFold    (currentActIdx)
            && proc->getActCrushAll       (currentActIdx);
    };
    auto refreshFxAll = [this, allFxOnDryBits]
    {
        fxAll.setToggleState (allFxOnDryBits(), juce::dontSendNotification);
    };

    wireToggleBtn (fxDrive,
                   [this] { return proc->getActFxOnDryDrive (currentActIdx); },
                   [this, refreshFxAll] (bool v) {
                       proc->setActFxOnDryDrive (currentActIdx, v);
                       refreshFxAll();
                   });
    wireToggleBtn (fxTone,
                   [this] { return proc->getActFxOnDryTone (currentActIdx); },
                   [this, refreshFxAll] (bool v) {
                       proc->setActFxOnDryTone (currentActIdx, v);
                       refreshFxAll();
                   });
    wireToggleBtn (fxRing,
                   [this] { return proc->getActFxOnDryRingMod (currentActIdx); },
                   [this, refreshFxAll] (bool v) {
                       proc->setActFxOnDryRingMod (currentActIdx, v);
                       refreshFxAll();
                   });
    wireToggleBtn (fxFold,
                   [this] { return proc->getActFxOnDryFold (currentActIdx); },
                   [this, refreshFxAll] (bool v) {
                       proc->setActFxOnDryFold (currentActIdx, v);
                       refreshFxAll();
                   });
    wireToggleBtn (fxCrunch,
                   [this] { return proc->getActCrushAll (currentActIdx); },
                   [this, refreshFxAll] (bool v) {
                       proc->setActCrushAll (currentActIdx, v);
                       refreshFxAll();
                   });

    // "All" must come last so the lambdas above can reference it.
    wireToggleBtn (fxAll,
                   [allFxOnDryBits] { return allFxOnDryBits(); },
                   [this] (bool v) {
                       proc->setActFxOnDryDrive   (currentActIdx, v);
                       proc->setActFxOnDryTone    (currentActIdx, v);
                       proc->setActFxOnDryRingMod (currentActIdx, v);
                       proc->setActFxOnDryFold    (currentActIdx, v);
                       proc->setActCrushAll       (currentActIdx, v);
                       fxDrive .setToggleState (v, juce::dontSendNotification);
                       fxTone  .setToggleState (v, juce::dontSendNotification);
                       fxRing  .setToggleState (v, juce::dontSendNotification);
                       fxFold  .setToggleState (v, juce::dontSendNotification);
                       fxCrunch.setToggleState (v, juce::dontSendNotification);
                   });

    // ---------- RING MOD (per-Act, v0.42) --------------------------------
    wireToggleBtn (ringQuant,
                   [this] { return proc->getActRingModQuant (currentActIdx); },
                   [this] (bool v) { proc->setActRingModQuant (currentActIdx, v); });

    // ---------- PITCH/SLIDE Fine -----------------------------------------
    wireToggleBtn (fineToggle,
                   [this] { return hooks.getFineMode && hooks.getFineMode(); },
                   [this] (bool v) { if (hooks.setFineMode) hooks.setFineMode (v); });

    // ---------- UI SCALE: Reset button -----------------------------------
    uiResetButton.setClickingTogglesState (false);
    uiResetButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff111111));
    uiResetButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff999999));
    uiResetButton.onClick = [this] { if (hooks.resetUiScale) hooks.resetUiScale(); };
    addAndMakeVisible (uiResetButton);
    allInteractive.push_back (&uiResetButton);

    // v0.42 — apply the initial view-mode visibility so panels not in the
    // starting view (PER-ACT by default) are hidden from the get-go.
    applyViewModeVisibility();
}

inline void SettingsPage::applyStencilLookAndFeel (juce::LookAndFeel* laf)
{
    stencilLaf = laf;
    for (auto* c : allInteractive)
        if (c != nullptr)
            c->setLookAndFeel (laf);
}

inline void SettingsPage::updateFromProcessor()
{
    if (! proc) return;

    // Engine: read through per-Act accessors.
    stageBox.setSelectedId (proc->getActEngineStage (currentActIdx) + 1, juce::dontSendNotification);
    outputMixSlider.setValue (proc->getActEngineMix (currentActIdx) * 100.0, juce::dontSendNotification);
    engineIntensitySlider.setValue (proc->getActEngineIntensity (currentActIdx) * 100.0, juce::dontSendNotification);
    // v0.42 — Output Mix is greyed for CLEAN (no processed signal to mix).
    // v0.7.0 — Intensity is also greyed for CLEAN.
    outputMixSlider.setEnabled (proc->getActEngineStage (currentActIdx) != (int) kOutClean);
    engineIntensitySlider.setEnabled (proc->getActEngineStage (currentActIdx) != (int) kOutClean);

    for (int i = 0; i < 3; ++i)
        interpButtons[i].setToggleState (proc->getActInterpMode (currentActIdx) == i,
                                          juce::dontSendNotification);

    {
        const bool aa = proc->getActCrushAA (currentActIdx);
        const bool mu = proc->getActCrushMu (currentActIdx);
        crunchAAOn .setToggleState (  aa, juce::dontSendNotification);
        crunchAAOff.setToggleState (! aa, juce::dontSendNotification);
        crunchMuOn .setToggleState (  mu, juce::dontSendNotification);
        crunchMuOff.setToggleState (! mu, juce::dontSendNotification);
    }

    // v0.5.0 — filter mode + frequency split sync.
    for (int i = 0; i < 4; ++i)
        filterButtons[i].setToggleState (proc->getActFilterMode (currentActIdx) == i,
                                          juce::dontSendNotification);
    for (int i = 0; i < 3; ++i)
        freqSplitButtons[i].setToggleState (proc->getActFreqSplitMode (currentActIdx) == i,
                                             juce::dontSendNotification);
    freqSplitSlider.setValue (proc->getActFreqSplitHz (currentActIdx), juce::dontSendNotification);
    // Only touch visibility in per-Act view — GLOBAL hides all per-Act controls.
    if (! showGlobalView)
        freqSplitSlider.setVisible (! showCharacterSubTab
                                     && proc->getActFreqSplitMode (currentActIdx) != 0);

    // v0.5.0 — CHARACTER sub-tab sync.
    for (int i = 0; i < (int) loopsab::kNumDriveTypes; ++i)
        driveTypeButtons[(size_t)i].setToggleState (proc->getActDriveType (currentActIdx) == i, juce::dontSendNotification);
    for (int i = 0; i < (int) loopsab::kNumFoldTopologies; ++i)
        foldTopologyButtons[(size_t)i].setToggleState (proc->getActFoldTopology (currentActIdx) == i, juce::dontSendNotification);
    for (int i = 0; i < (int) loopsab::kNumShimmerOctaves; ++i)
        shimmerOctaveButtons[(size_t)i].setToggleState (proc->getActShimmerOctave (currentActIdx) == i, juce::dontSendNotification);
    for (int i = 0; i < (int) loopsab::kNumSmearCharacters; ++i)
        smearCharacterButtons[(size_t)i].setToggleState (proc->getActSmearCharacter (currentActIdx) == i, juce::dontSendNotification);
    for (int i = 0; i < (int) loopsab::kNumStutterWindows; ++i)
        stutterWindowButtons[(size_t)i].setToggleState (proc->getActStutterWindow (currentActIdx) == i, juce::dontSendNotification);
    for (int i = 0; i < (int) loopsab::kNumVarispeedCurves; ++i)
        varispeedCurveButtons[(size_t)i].setToggleState (proc->getActVarispeedCurve (currentActIdx) == i, juce::dontSendNotification);
    for (int i = 0; i < (int) loopsab::kNumSlideCurves; ++i)
        slideCurveButtons[(size_t)i].setToggleState (proc->getActSlideCurve (currentActIdx) == i, juce::dontSendNotification);
    for (int i = 0; i < (int) loopsab::kNumReverseModes; ++i)
        reverseModeButtons[(size_t)i].setToggleState (proc->getActReverseMode (currentActIdx) == i, juce::dontSendNotification);
    for (int i = 0; i < (int) loopsab::kNumTapeModes; ++i)
        tapeModeButtons[(size_t)i].setToggleState (proc->getActTapeMode (currentActIdx) == i, juce::dontSendNotification);
    for (int i = 0; i < (int) loopsab::kNumRingModWaves; ++i)
        ringModWaveButtons[(size_t)i].setToggleState (proc->getActRingModWave (currentActIdx) == i, juce::dontSendNotification);
    for (int i = 0; i < (int) loopsab::kNumChaosDistributions; ++i)
        chaosDistributionButtons[(size_t)i].setToggleState (proc->getActChaosDistribution (currentActIdx) == i, juce::dontSendNotification);
    for (int i = 0; i < (int) loopsab::kNumFeedbackCharacters; ++i)
        feedbackCharacterButtons[(size_t)i].setToggleState (proc->getActFeedbackCharacter (currentActIdx) == i, juce::dontSendNotification);
    for (int i = 0; i < (int) loopsab::kNumStretchModes; ++i)
        stretchModeButtons[(size_t)i].setToggleState (proc->getActStretchMode (currentActIdx) == i, juce::dontSendNotification);
    for (int i = 0; i < (int) loopsab::kNumJudderShapes; ++i)
        judderShapeButtons[(size_t)i].setToggleState (proc->getActJudderShape (currentActIdx) == i, juce::dontSendNotification);
    for (int i = 0; i < (int) loopsab::kNumLookbackBehaviours; ++i)
        lookbackBehaviourButtons[(size_t)i].setToggleState (proc->getActLookbackBehaviour (currentActIdx) == i, juce::dontSendNotification);
    for (int i = 0; i < (int) loopsab::kNumDecayCurves; ++i)
        decayCurveButtons[(size_t)i].setToggleState (proc->getActDecayCurve (currentActIdx) == i, juce::dontSendNotification);
    applySubTabVisibility();

    // v0.42.1 — also sync globalTab. Previously only actTabs were synced
    // here, which meant that after clicking GLOBAL the act-tab re-sync
    // would flip actTabs[currentActIdx] back ON and radio-group 990 would
    // untoggle GLOBAL, leaving the tab un-highlighted even though the
    // GLOBAL view was the active mode.
    globalTab.setToggleState (showGlobalView, juce::dontSendNotification);
    for (int i = 0; i < 8; ++i)
        actTabs[i].setToggleState (! showGlobalView && i == currentActIdx,
                                     juce::dontSendNotification);

    // Grid accent combo: id 1 = Off, id n = value n for 2..12.
    {
        const int v = proc->getGridAccentInterval();
        accentBox.setSelectedId (v <= 0 ? 1 : juce::jlimit (2, 12, v),
                                  juce::dontSendNotification);
    }

    static const int trValues[4] = {
        LoopSaboteurProcessor::kTransportForward,
        LoopSaboteurProcessor::kTransportReverse,
        LoopSaboteurProcessor::kTransportPingPong,
        LoopSaboteurProcessor::kTransportRandom };
    for (int i = 0; i < 4; ++i)
        transportButtons[i].setToggleState (
            proc->getSeqTransport() == trValues[i], juce::dontSendNotification);

    const bool mom = (hooks.getFreezeMomentary && hooks.getFreezeMomentary());
    freezeButtons[0].setToggleState (! mom, juce::dontSendNotification);
    freezeButtons[1].setToggleState (  mom, juce::dontSendNotification);

    if (auto* rootParam = dynamic_cast<juce::AudioParameterChoice*> (
            proc->apvts.getParameter (LoopSaboteurProcessor::kParamScaleRoot)))
        scaleRootBox.setSelectedId (rootParam->getIndex() + 1, juce::dontSendNotification);
    if (auto* scaleParam = dynamic_cast<juce::AudioParameterChoice*> (
            proc->apvts.getParameter (LoopSaboteurProcessor::kParamScaleType)))
        scaleTypeBox.setSelectedId (scaleParam->getIndex() + 1, juce::dontSendNotification);

    displayWaveform  .setToggleState (proc->getWaveformVisible(),    juce::dontSendNotification);
    displayTooltips  .setToggleState (proc->getTooltipsEnabled(),    juce::dontSendNotification);
    displayPageFollow.setToggleState (proc->getPageFollowsPlayhead(), juce::dontSendNotification);

    static const int stValues[4] = {
        LoopSaboteurProcessor::kStereoRandom,
        LoopSaboteurProcessor::kStereoPingPong,
        LoopSaboteurProcessor::kStereoAutoPan,
        LoopSaboteurProcessor::kStereoHaas };
    // v0.42 — read Stereo mode / FX-on-Dry / Ring mod quant from the
    // currently-selected Act tab, not the global atomics, so tab
    // switching shows the right slot's values.
    for (int i = 0; i < 4; ++i)
        stereoButtons[i].setToggleState (
            proc->getActStereoMode (currentActIdx) == stValues[i],
            juce::dontSendNotification);

    fxDrive .setToggleState (proc->getActFxOnDryDrive   (currentActIdx), juce::dontSendNotification);
    fxTone  .setToggleState (proc->getActFxOnDryTone    (currentActIdx), juce::dontSendNotification);
    fxRing  .setToggleState (proc->getActFxOnDryRingMod (currentActIdx), juce::dontSendNotification);
    fxFold  .setToggleState (proc->getActFxOnDryFold    (currentActIdx), juce::dontSendNotification);
    fxCrunch.setToggleState (proc->getActCrushAll       (currentActIdx), juce::dontSendNotification);
    // v0.42.2 — "All" now includes Crunch (bits / rate).
    fxAll   .setToggleState (proc->getActFxOnDryDrive   (currentActIdx)
                          && proc->getActFxOnDryTone    (currentActIdx)
                          && proc->getActFxOnDryRingMod (currentActIdx)
                          && proc->getActFxOnDryFold    (currentActIdx)
                          && proc->getActCrushAll       (currentActIdx),
                             juce::dontSendNotification);

    ringQuant .setToggleState (proc->getActRingModQuant (currentActIdx), juce::dontSendNotification);
    fineToggle.setToggleState (hooks.getFineMode && hooks.getFineMode(), juce::dontSendNotification);
}

inline void SettingsPage::setCurrentActTab (int actIdx)
{
    if (! proc) return;

    // v0.42 — idx == -1 switches to GLOBAL view; 0..7 selects an Act and
    // switches to (or stays in) PER-ACT view.
    const bool wantGlobal = (actIdx < 0);
    if (! wantGlobal)
        actIdx = juce::jlimit (0, 7, actIdx);

    const bool modeChanged = (wantGlobal != showGlobalView);
    const bool actChanged  = (! wantGlobal) && (actIdx != currentActIdx);

    showGlobalView = wantGlobal;
    if (! wantGlobal)
        currentActIdx = actIdx;

    // Keep the radio group tab visuals consistent — GLOBAL lit in GLOBAL
    // view; the current Act lit in PER-ACT view; everything else off.
    globalTab.setToggleState (showGlobalView, juce::dontSendNotification);
    for (int i = 0; i < 8; ++i)
        actTabs[i].setToggleState (! showGlobalView && i == currentActIdx,
                                    juce::dontSendNotification);

    if (modeChanged)
    {
        applyViewModeVisibility();
        resized();   // different view → different panel layout
        repaint();   // hint text + panel chrome change too
    }

    if (modeChanged || actChanged)
        updateFromProcessor();
}

inline void SettingsPage::applySubTabVisibility()
{
    if (showGlobalView) return;  // sub-tabs only matter in per-Act view

    // ENGINE sub-controls: visible when showCharacterSubTab == false
    const bool engVis = ! showCharacterSubTab;
    stageBox.setVisible (engVis);
    outputMixSlider.setVisible (engVis);
    engineIntensitySlider.setVisible (engVis);
    for (auto& b : interpButtons) b.setVisible (engVis);
    crunchAAOn.setVisible (engVis);  crunchAAOff.setVisible (engVis);
    crunchMuOn.setVisible (engVis);  crunchMuOff.setVisible (engVis);
    for (auto& b : filterButtons) b.setVisible (engVis);
    for (auto& b : freqSplitButtons) b.setVisible (engVis);
    freqSplitSlider.setVisible (engVis && proc && proc->getActFreqSplitMode (currentActIdx) != 0);

    // CHARACTER sub-controls: visible when showCharacterSubTab == true
    const bool charVis = showCharacterSubTab;
    for (auto& b : driveTypeButtons) b.setVisible (charVis);
    for (auto& b : foldTopologyButtons) b.setVisible (charVis);
    for (auto& b : shimmerOctaveButtons) b.setVisible (charVis);
    for (auto& b : smearCharacterButtons) b.setVisible (charVis);
    for (auto& b : stutterWindowButtons) b.setVisible (charVis);
    for (auto& b : varispeedCurveButtons) b.setVisible (charVis);
    for (auto& b : slideCurveButtons) b.setVisible (charVis);
    for (auto& b : reverseModeButtons) b.setVisible (charVis);
    for (auto& b : tapeModeButtons) b.setVisible (charVis);
    for (auto& b : ringModWaveButtons) b.setVisible (charVis);
    for (auto& b : chaosDistributionButtons) b.setVisible (charVis);
    for (auto& b : feedbackCharacterButtons) b.setVisible (charVis);
    for (auto& b : stretchModeButtons) b.setVisible (charVis);
    for (auto& b : judderShapeButtons) b.setVisible (charVis);
    for (auto& b : lookbackBehaviourButtons) b.setVisible (charVis);
    for (auto& b : decayCurveButtons) b.setVisible (charVis);

    // Sub-tab buttons themselves are always visible in per-Act mode
    engineSubTab.setToggleState (! showCharacterSubTab, juce::dontSendNotification);
    characterSubTab.setToggleState (showCharacterSubTab, juce::dontSendNotification);
}

inline void SettingsPage::applyViewModeVisibility()
{
    // Global-scope panels' interactive children.
    juce::Component* globalControls[] = {
        &accentBox,
        &transportButtons[0], &transportButtons[1], &transportButtons[2], &transportButtons[3],
        &freezeButtons[0], &freezeButtons[1],
        &displayWaveform, &displayTooltips, &displayPageFollow, &setDefaultsBtn,
        &scaleRootBox, &scaleTypeBox,
        &fineToggle,
        &uiResetButton
    };
    // Per-Act-scope panels' interactive children.
    juce::Component* perActControls[] = {
        &stageBox, &outputMixSlider, &engineIntensitySlider,
        &interpButtons[0], &interpButtons[1], &interpButtons[2],
        &crunchAAOn, &crunchAAOff, &crunchMuOn, &crunchMuOff,
        &filterButtons[0], &filterButtons[1], &filterButtons[2], &filterButtons[3],
        &freqSplitButtons[0], &freqSplitButtons[1], &freqSplitButtons[2],
        &freqSplitSlider,
        &engineSubTab, &characterSubTab,
        &driveTypeButtons[0], &driveTypeButtons[1], &driveTypeButtons[2], &driveTypeButtons[3],
        &foldTopologyButtons[0], &foldTopologyButtons[1], &foldTopologyButtons[2],
        &shimmerOctaveButtons[0], &shimmerOctaveButtons[1], &shimmerOctaveButtons[2], &shimmerOctaveButtons[3], &shimmerOctaveButtons[4],
        &smearCharacterButtons[0], &smearCharacterButtons[1], &smearCharacterButtons[2],
        &stutterWindowButtons[0], &stutterWindowButtons[1], &stutterWindowButtons[2],
        &varispeedCurveButtons[0], &varispeedCurveButtons[1], &varispeedCurveButtons[2],
        &slideCurveButtons[0], &slideCurveButtons[1], &slideCurveButtons[2], &slideCurveButtons[3],
        &reverseModeButtons[0], &reverseModeButtons[1], &reverseModeButtons[2], &reverseModeButtons[3],
        &tapeModeButtons[0], &tapeModeButtons[1], &tapeModeButtons[2], &tapeModeButtons[3],
        &ringModWaveButtons[0], &ringModWaveButtons[1], &ringModWaveButtons[2], &ringModWaveButtons[3],
        &chaosDistributionButtons[0], &chaosDistributionButtons[1], &chaosDistributionButtons[2], &chaosDistributionButtons[3],
        &feedbackCharacterButtons[0], &feedbackCharacterButtons[1], &feedbackCharacterButtons[2], &feedbackCharacterButtons[3],
        &stretchModeButtons[0], &stretchModeButtons[1], &stretchModeButtons[2], &stretchModeButtons[3],
        &judderShapeButtons[0], &judderShapeButtons[1], &judderShapeButtons[2], &judderShapeButtons[3],
        &lookbackBehaviourButtons[0], &lookbackBehaviourButtons[1], &lookbackBehaviourButtons[2],
        &decayCurveButtons[0], &decayCurveButtons[1], &decayCurveButtons[2], &decayCurveButtons[3],
        &stereoButtons[0], &stereoButtons[1], &stereoButtons[2], &stereoButtons[3],
        &fxAll, &fxDrive, &fxTone, &fxRing, &fxFold, &fxCrunch,
        &ringQuant
    };

    for (auto* c : globalControls)  if (c) c->setVisible (  showGlobalView);
    for (auto* c : perActControls)  if (c) c->setVisible (! showGlobalView);

    applySubTabVisibility();
}

inline void SettingsPage::paintPanelHeader (juce::Graphics& g, juce::Rectangle<int> r,
                                            const juce::String& title) const
{
    // Panel chrome: rounded rect outlined in dim accent.
    g.setColour (juce::Colour (0xff141414));
    g.fillRoundedRectangle (r.toFloat(), 5.0f);
    g.setColour (juce::Colour (0xffff5a3c).withAlpha (0.18f));
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 5.0f, 1.0f);

    // v0.42.2 — header band enlarged to 36pt so the panel titles carry
    // visible weight (Engine / Stereo / FX / Ring Mod were reading as
    // squashed at 17pt against the now-bigger hint + controls below).
    auto headerBand = r.removeFromTop (36).reduced (12, 6);
    g.setColour (juce::Colour (0xffff5a3c));
    auto font = FontHelper::impact (22.0f, juce::Font::plain);
    font.setExtraKerningFactor (0.18f);
    g.setFont (font);
    g.drawText (title.toUpperCase(), headerBand, juce::Justification::centredLeft);
}

inline void SettingsPage::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();

    // Outer frame (matches MOD page styling).
    g.setColour (juce::Colour (0xff1a1a1a));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (juce::Colour (0xffff5a3c).withAlpha (0.2f));
    g.drawRoundedRectangle (r, 6.0f, 1.0f);

    // Title header in big sheared Impact, same vibe as MOD.
    auto titleR = headerR.toFloat();
    g.setColour (juce::Colour (0xffff5a3c));
    auto font = FontHelper::impact (28.0f, juce::Font::plain);
    font.setExtraKerningFactor (0.15f);
    g.setFont (font);
    const float anchorY = titleR.getCentreY();
    g.saveState();
    g.addTransform (juce::AffineTransform::translation (0.0f, -anchorY)
                        .sheared (-0.10f, 0.0f)
                        .translated (0.0f, anchorY));
    g.drawText ("TOOLS", titleR.reduced (16, 0), juce::Justification::centredLeft);
    g.restoreState();

    // v0.42 — hint line reflects the current view mode. PER-ACT mode
    // shows "EDITING ACT X — PER-ACT SETTINGS"; GLOBAL mode shows a
    // matching line that signals the panels apply across all Acts.
    // Uses juce::String::fromUTF8 on the em-dash to avoid JUCE
    // interpreting the \xe2\x80\x94 bytes as Latin-1 and rendering "â€"
    // mojibake.
    {
        // v0.42.1 — hint font bumped 12pt → 17pt so the mode indicator is
        // legible at a glance. Width still capped to the 30% strip we
        // reserved in resized() so it won't overrun the tab buttons.
        auto hintFont = FontHelper::impact (17.0f, juce::Font::plain);
        hintFont.setExtraKerningFactor (0.14f);
        g.setFont (hintFont);
        g.setColour (juce::Colour (0xffcccccc));
        const int hintW = actTabStripR.getWidth() * 30 / 100 - 6;
        auto hintR = actTabStripR.withWidth (hintW);
        const juce::String dash = juce::String::fromUTF8 ("\xe2\x80\x94");
        const juce::String hintTxt = showGlobalView
            ? juce::String ("GLOBAL   ") + dash + "   APPLIES TO ALL ACTS"
            : juce::String ("EDITING ACT ")
              + juce::String::charToString ((juce_wchar) ('A' + currentActIdx))
              + "   " + dash + "   PER-ACT TOOLS";
        g.drawText (hintTxt, hintR, juce::Justification::centredLeft);
    }

    // Panel chrome + headers — only paint the ones that are visible in
    // the current view mode.
    if (showGlobalView)
    {
        paintPanelHeader (g, accentPanelR,    "Grid Accent");
        paintPanelHeader (g, transportPanelR, "Transport");
        paintPanelHeader (g, freezePanelR,    "Freeze Mode");
        paintPanelHeader (g, displayPanelR,   "Display");
        paintPanelHeader (g, scalePanelR,     "Scale Quantise");
        paintPanelHeader (g, pitchPanelR,     "Pitch / Slide");
        paintPanelHeader (g, uiPanelR,        "UI Scale");
    }
    else
    {
        paintPanelHeader (g, enginePanelR,    "Engine");
        paintPanelHeader (g, stereoPanelR,    "Stereo");
        paintPanelHeader (g, fxPanelR,        "FX on Dry Signal");
        paintPanelHeader (g, ringModPanelR,   "Ring Mod");

        // v0.5.0 — CHARACTER panel header.
        paintPanelHeader (g, characterPanelR, "Character");

        // Sub-headers inside the Engine panel — only meaningful in PER-ACT view.
        // v0.42.2 — bumped to 16pt so sub-sections (OUTPUT STAGE,
        // INTERPOLATION, CRUNCH, ANTI-ALIAS FILTER, μ-LAW) read clearly
        // against the new 22pt panel headers. Darker text colour too so
        // the hierarchy stays visible: headers = accent orange, sub-labels
        // = light grey, footnotes = dim grey.
        auto subFont = FontHelper::impact (16.0f, juce::Font::plain);
        subFont.setExtraKerningFactor (0.14f);
        g.setFont (subFont);
        g.setColour (juce::Colour (0xffbbbbbb));

        // v0.7.0 — compacted to match resized() spacing.
        auto eng = enginePanelR.reduced (10, 6).withTrimmedTop (36);
        auto label = [&g] (juce::Rectangle<int> rr, const juce::String& txt)
        {
            g.drawText (txt, rr, juce::Justification::centredLeft);
        };
        label (eng.removeFromTop (14), "OUTPUT STAGE");
        eng.removeFromTop (22);   // combo
        eng.removeFromTop (2);
        label (eng.removeFromTop (14), "OUTPUT MIX   (WET / DRY OF THE STAGE)");
        eng.removeFromTop (22);   // slider
        eng.removeFromTop (2);
        label (eng.removeFromTop (14), "INTENSITY   (EFFECT DEPTH)");
        eng.removeFromTop (22);   // slider
        eng.removeFromTop (3);
        label (eng.removeFromTop (14), "INTERPOLATION");
        eng.removeFromTop (22);   // radio row
        eng.removeFromTop (6);
        label (eng.removeFromTop (14), "CRUNCH");
        eng.removeFromTop (2);
        label (eng.removeFromTop (12), "ANTI-ALIAS FILTER");
        eng.removeFromTop (22);   // AA ON/OFF row
        eng.removeFromTop (2);
        label (eng.removeFromTop (12),
               juce::String::fromUTF8 ("\xce\xbc-LAW   (S950-STYLE COMPANDING)"));
        eng.removeFromTop (22);   // μ-law ON/OFF row
        eng.removeFromTop (3);
        label (eng.removeFromTop (14), "FILTER MODE");
        eng.removeFromTop (22);   // filter radio row
        eng.removeFromTop (3);
        label (eng.removeFromTop (14), "FREQUENCY SPLIT");
        eng.removeFromTop (22);   // freq split radio row
        label (eng.removeFromTop (12), "CROSSOVER");
        eng.removeFromTop (24);   // crossover slider
        // v0.42 — Preset Recall moved out to its own panel in GLOBAL view.

        // v0.5.0 / v0.6.0 — CHARACTER panel labels (two-column layout).
        if (! characterPanelR.isEmpty())
        {
            // Use a slightly smaller font for the narrower columns.
            auto charFont = FontHelper::impact (14.0f, juce::Font::plain);
            charFont.setExtraKerningFactor (0.12f);
            g.setFont (charFont);

            auto area = characterPanelR.reduced (12, 10).withTrimmedTop (36);
            const int colGap = 8;
            const int colW   = (area.getWidth() - colGap) / 2;
            auto colL = area.removeFromLeft (colW);
            area.removeFromLeft (colGap);
            auto colR = area;

            auto labelCol = [&g] (juce::Rectangle<int>& col, const juce::String& txt,
                                   bool first = false) {
                col.removeFromTop (first ? 4 : 8);
                g.drawText (txt, col.removeFromTop (16), juce::Justification::centredLeft);
                col.removeFromTop (24);
            };

            // Left column (8 items)
            labelCol (colL, "DRIVE TYPE",         true);
            labelCol (colL, "FOLD SHAPE");
            labelCol (colL, "SHIMMER INTERVAL");
            labelCol (colL, "SMEAR CHARACTER");
            labelCol (colL, "STUTTER WINDOW");
            labelCol (colL, "BRAKE CURVE");
            labelCol (colL, "SLIDE CURVE");
            labelCol (colL, "REVERSE MODE");

            // Right column (8 items)
            labelCol (colR, "TAPE MODE",           true);
            labelCol (colR, "RING MOD WAVEFORM");
            labelCol (colR, "CHAOS DISTRIBUTION");
            labelCol (colR, "FEEDBACK CHARACTER");
            labelCol (colR, "STRETCH MODE");
            labelCol (colR, "JUDDER SHAPE");
            labelCol (colR, "LOOKBACK BEHAVIOUR");
            labelCol (colR, "DECAY CURVE");
        }
    }

    // v0.42.1 — Ring Mod panel carries a small footer hint telling the
    // user that Scale Quantise is configured in GLOBAL settings, so when
    // they enable "Quantise to scale" here they know where to change it.
    // v0.42.2 — bumped from 11pt → 14pt and moved into its own reserved
    // 22pt-high footer slot so it no longer overlaps the toggle above it.
    if (! showGlobalView && ! ringModPanelR.isEmpty())
    {
        auto hintFont = FontHelper::impact (14.0f, juce::Font::plain);
        hintFont.setExtraKerningFactor (0.13f);
        g.setFont (hintFont);
        g.setColour (juce::Colour (0xff999999));
        auto footer = ringModPanelR.reduced (12, 10);
        auto hintR  = footer.removeFromBottom (22);
        g.drawText (juce::String::fromUTF8 ("SCALE IS SET IN GLOBAL \xe2\x86\x92 SCALE QUANTISE"),
                    hintR, juce::Justification::centredLeft);
    }
}

inline void SettingsPage::resized()
{
    // v0.42 — two-view layout. The tab strip along the top carries
    // GLOBAL + A..H. The body underneath then lays out either the
    // global panels (GLOBAL view) or the per-Act panels (A..H view).
    auto bounds = getLocalBounds().reduced (10);
    headerR = bounds.removeFromTop (50);
    bounds.removeFromTop (4);

    const int gap = 8;

    // Tab strip spans the full width so GLOBAL and A..H share one row.
    actTabStripR = bounds.removeFromTop (40);
    bounds.removeFromTop (6);

    // ---------- Tab strip bounds ------------------------------------------
    // Left 30% = "EDITING ACT X" / "GLOBAL" hint drawn in paint().
    // Right 70% = GLOBAL tab (wider) + 8 Act tabs.
    {
        auto tabs = actTabStripR;
        tabs.removeFromLeft (tabs.getWidth() * 30 / 100);   // hint space
        // GLOBAL gets roughly 2× the width of an Act tab so the word fits.
        const int totalSlots  = 10;    // 2 for GLOBAL + 1 each A..H
        const int slotW       = tabs.getWidth() / totalSlots;
        globalTab.setBounds (tabs.removeFromLeft (slotW * 2).reduced (3, 5));
        for (int i = 0; i < 8; ++i)
        {
            auto slot = tabs.removeFromLeft (i == 7 ? tabs.getWidth() : slotW);
            actTabs[i].setBounds (slot.reduced (3, 5));
        }
    }

    // ---------- Stack helper (used by both view layouts) -----------------
    auto layoutStack = [] (juce::Rectangle<int>& col,
                            std::initializer_list<int> heights,
                            std::initializer_list<juce::Rectangle<int>*> outRects)
    {
        auto h = heights.begin();
        auto r = outRects.begin();
        for (; h != heights.end() && r != outRects.end(); ++h, ++r)
        {
            **r = col.removeFromTop (*h);
            col.removeFromTop (6);
        }
    };

    if (showGlobalView)
    {
        // ---------- GLOBAL view: 2 equal columns -------------------------
        // Left:  Grid Accent | Transport | Freeze | Display
        // Right: Scale Quantise | Pitch/Slide | UI Scale | Preset Recall
        // Any per-Act panel rects are zeroed so paint()/layout never
        // references stale geometry from a previous mode.
        enginePanelR   = {};
        characterPanelR = {};
        stereoPanelR   = {};
        fxPanelR       = {};
        ringModPanelR  = {};

        const int colW = (bounds.getWidth() - gap) / 2;
        auto colA = bounds.removeFromLeft (colW);
        bounds.removeFromLeft (gap);
        auto colB = bounds;

        // v0.42.2 — panel heights bumped by +8 to absorb the taller
        // (36pt) header band introduced this version. Without this the
        // first control row sits under the accent-orange title strip.
        layoutStack (colA,
                     { 70, 88, 88, 168 },
                     { &accentPanelR, &transportPanelR, &freezePanelR,
                       &displayPanelR });
        layoutStack (colB,
                     { 116, 74, 98 },
                     { &scalePanelR, &pitchPanelR, &uiPanelR });
    }
    else
    {
        // ---------- PER-ACT view: Engine column + misc column ------------
        // Zero the global rects so they don't leak into paint().
        accentPanelR    = {};
        transportPanelR = {};
        freezePanelR    = {};
        displayPanelR   = {};
        scalePanelR     = {};
        pitchPanelR     = {};
        uiPanelR        = {};

        const int perActW  = bounds.getWidth();
        const int engineW  = juce::jmax (280, (perActW - gap) * 60 / 100);
        auto colEngine = bounds.removeFromLeft (engineW);
        bounds.removeFromLeft (gap);
        auto colMisc   = bounds;

        // v0.5.0 — ENGINE / CHARACTER sub-tab buttons at top of left column.
        {
            auto subTabRow = colEngine.removeFromTop (34);
            const int btnW = subTabRow.getWidth() / 2;
            engineSubTab.setBounds (subTabRow.removeFromLeft (btnW).reduced (3, 3));
            characterSubTab.setBounds (subTabRow.reduced (3, 3));
        }
        colEngine.removeFromTop (4);

        if (! showCharacterSubTab)
        {
            enginePanelR = colEngine;
            characterPanelR = {};
        }
        else
        {
            characterPanelR = colEngine;
            enginePanelR = {};
        }

        // ---------- Engine panel (per-Act) ----------------------------
        // v0.7.0 — only lay out engine controls when the panel is live.
        // v0.7.0 — compacted spacing (gaps 2-3, labels 14, controls 22)
        // to fit the new Intensity slider without overflowing the panel.
        if (! enginePanelR.isEmpty())
        {
            auto eng = enginePanelR.reduced (10, 6).withTrimmedTop (36);
            eng.removeFromTop (14);                              // "OUTPUT STAGE"
            stageBox.setBounds (eng.removeFromTop (22).reduced (0, 1));
            eng.removeFromTop (2);
            eng.removeFromTop (14);                              // "OUTPUT MIX"
            outputMixSlider.setBounds (eng.removeFromTop (22).reduced (0, 1));
            eng.removeFromTop (2);
            eng.removeFromTop (14);                              // "INTENSITY"
            engineIntensitySlider.setBounds (eng.removeFromTop (22).reduced (0, 1));
            eng.removeFromTop (3);
            eng.removeFromTop (14);                              // "INTERPOLATION"
            {
                auto row = eng.removeFromTop (22);
                const int btnW = row.getWidth() / 3;
                for (int i = 0; i < 3; ++i)
                    interpButtons[i].setBounds (
                        row.removeFromLeft (i == 2 ? row.getWidth() : btnW).reduced (2, 1));
            }
            eng.removeFromTop (6);
            eng.removeFromTop (14);                              // "CRUNCH" header
            eng.removeFromTop (2);
            eng.removeFromTop (12);                              // "ANTI-ALIAS FILTER"
            {
                auto row = eng.removeFromTop (22);
                const int btnW = row.getWidth() / 2;
                crunchAAOn .setBounds (row.removeFromLeft (btnW).reduced (2, 1));
                crunchAAOff.setBounds (row.reduced (2, 1));
            }
            eng.removeFromTop (2);
            eng.removeFromTop (12);                              // "μ-law ..."
            {
                auto row = eng.removeFromTop (22);
                const int btnW = row.getWidth() / 2;
                crunchMuOn .setBounds (row.removeFromLeft (btnW).reduced (2, 1));
                crunchMuOff.setBounds (row.reduced (2, 1));
            }
            eng.removeFromTop (3);
            eng.removeFromTop (14);                              // "FILTER MODE"
            {
                auto row = eng.removeFromTop (22);
                const int btnW = row.getWidth() / 4;
                for (int i = 0; i < 4; ++i)
                    filterButtons[i].setBounds (
                        row.removeFromLeft (i == 3 ? row.getWidth() : btnW).reduced (2, 1));
            }
            eng.removeFromTop (3);
            eng.removeFromTop (14);                              // "FREQUENCY SPLIT"
            {
                auto row = eng.removeFromTop (22);
                const int btnW = row.getWidth() / 3;
                for (int i = 0; i < 3; ++i)
                    freqSplitButtons[i].setBounds (
                        row.removeFromLeft (i == 2 ? row.getWidth() : btnW).reduced (2, 1));
            }
            eng.removeFromTop (12);                              // "CROSSOVER"
            freqSplitSlider.setBounds (eng.removeFromTop (24).reduced (0, 1));
        }

        // v0.5.0 / v0.6.0 — CHARACTER panel layout (two-column, replaces
        // Engine when sub-tab active). 16 options split into 2 cols of 8.
        if (! characterPanelR.isEmpty())
        {
            auto area = characterPanelR.reduced (12, 10).withTrimmedTop (36);
            const int colGap = 8;
            const int colW   = (area.getWidth() - colGap) / 2;
            auto colL = area.removeFromLeft (colW);
            area.removeFromLeft (colGap);
            auto colR = area;

            auto layoutRadioRow = [] (juce::Rectangle<int>& col, auto& buttons,
                                       int count, bool first = false) {
                col.removeFromTop (first ? 4 : 8);
                col.removeFromTop (16);   // label space (painted by paint())
                auto row = col.removeFromTop (24);
                const int btnW = row.getWidth() / count;
                for (int i = 0; i < count; ++i)
                    buttons[(size_t)i].setBounds (
                        row.removeFromLeft (i == count - 1 ? row.getWidth() : btnW).reduced (1, 1));
            };

            // Left column (8 items)
            layoutRadioRow (colL, driveTypeButtons,        (int) loopsab::kNumDriveTypes,        true);
            layoutRadioRow (colL, foldTopologyButtons,     (int) loopsab::kNumFoldTopologies);
            layoutRadioRow (colL, shimmerOctaveButtons,    (int) loopsab::kNumShimmerOctaves);
            layoutRadioRow (colL, smearCharacterButtons,   (int) loopsab::kNumSmearCharacters);
            layoutRadioRow (colL, stutterWindowButtons,    (int) loopsab::kNumStutterWindows);
            layoutRadioRow (colL, varispeedCurveButtons,   (int) loopsab::kNumVarispeedCurves);
            layoutRadioRow (colL, slideCurveButtons,       (int) loopsab::kNumSlideCurves);
            layoutRadioRow (colL, reverseModeButtons,      (int) loopsab::kNumReverseModes);

            // Right column (8 items)
            layoutRadioRow (colR, tapeModeButtons,         (int) loopsab::kNumTapeModes,         true);
            layoutRadioRow (colR, ringModWaveButtons,      (int) loopsab::kNumRingModWaves);
            layoutRadioRow (colR, chaosDistributionButtons,(int) loopsab::kNumChaosDistributions);
            layoutRadioRow (colR, feedbackCharacterButtons,(int) loopsab::kNumFeedbackCharacters);
            layoutRadioRow (colR, stretchModeButtons,      (int) loopsab::kNumStretchModes);
            layoutRadioRow (colR, judderShapeButtons,      (int) loopsab::kNumJudderShapes);
            layoutRadioRow (colR, lookbackBehaviourButtons,(int) loopsab::kNumLookbackBehaviours);
            layoutRadioRow (colR, decayCurveButtons,       (int) loopsab::kNumDecayCurves);
        }

        // ---------- Per-Act misc column -------------------------------
        // v0.42.1 — FX panel grew to fit a sixth toggle (Crunch bits/rate,
        // migrated out of the Engine/Crunch section). Ring Mod grew a
        // little to carry a "scale set in global" footer hint.
        // v0.42.2 — panel heights bumped for taller headers + taller
        // footer band on Ring Mod. FX panel in particular gets +12 to
        // keep its six toggles clear of the new 36pt header.
        layoutStack (colMisc,
                     { 88, 256, 108 },
                     { &stereoPanelR, &fxPanelR, &ringModPanelR });
    }

    // v0.42.2 — header band grew from 28→36 so every "withTrimmedTop(28)"
    // and vertical-reduce(…,28/32) below has been bumped accordingly,
    // otherwise controls slide up under the new taller title bar.
    // GRID ACCENT combo
    {
        auto inner = accentPanelR.reduced (10, 36).withHeight (30);
        accentBox.setBounds (inner.reduced (2, 2));
    }
    // TRANSPORT row
    {
        auto inner = transportPanelR.reduced (10, 40).withHeight (34);
        const int btnW = inner.getWidth() / 4;
        for (int i = 0; i < 4; ++i)
            transportButtons[i].setBounds (
                inner.removeFromLeft (i == 3 ? inner.getWidth() : btnW).reduced (2, 2));
    }
    // FREEZE MODE
    {
        auto inner = freezePanelR.reduced (10, 40).withHeight (34);
        const int btnW = inner.getWidth() / 2;
        freezeButtons[0].setBounds (inner.removeFromLeft (btnW).reduced (2, 2));
        freezeButtons[1].setBounds (inner.reduced (2, 2));
    }
    // DISPLAY
    {
        auto inner = displayPanelR.reduced (10, 8).withTrimmedTop (36);
        displayWaveform .setBounds (inner.removeFromTop (30).reduced (2, 2));
        displayTooltips .setBounds (inner.removeFromTop (30).reduced (2, 2));
        displayPageFollow.setBounds (inner.removeFromTop (30).reduced (2, 2));
        inner.removeFromTop (6);
        setDefaultsBtn.setBounds (inner.removeFromTop (26).reduced (2, 2));
    }
    // UI SCALE
    {
        auto inner = uiPanelR.reduced (10, 40).withHeight (34);
        uiResetButton.setBounds (inner.reduced (2, 2));
    }
    // SCALE QUANTISE
    {
        auto inner = scalePanelR.reduced (10, 8).withTrimmedTop (36);
        scaleRootBox.setBounds (inner.removeFromTop (30).reduced (0, 4));
        inner.removeFromTop (4);
        scaleTypeBox.setBounds (inner.removeFromTop (30).reduced (0, 4));
    }
    // RING MOD
    {
        // v0.42.1 — toggle sits slightly higher so the "scale is set in
        // global" footer hint (painted by paint()) has breathing room at
        // the bottom of the panel.
        // v0.42.2 — header 28→36, footer now reserves 22pt, so the toggle
        // lives in the middle band between them.
        auto inner = ringModPanelR.reduced (10, 10)
                                   .withTrimmedTop (36)
                                   .withTrimmedBottom (24);
        ringQuant.setBounds (inner.removeFromTop (30).reduced (2, 2));
    }
    // STEREO
    {
        auto inner = stereoPanelR.reduced (10, 40).withHeight (34);
        const int btnW = inner.getWidth() / 4;
        for (int i = 0; i < 4; ++i)
            stereoButtons[i].setBounds (
                inner.removeFromLeft (i == 3 ? inner.getWidth() : btnW).reduced (2, 2));
    }
    // GLOBAL FX (FX on dry)
    {
        auto inner = fxPanelR.reduced (10, 8).withTrimmedTop (36);
        fxAll   .setBounds (inner.removeFromTop (30).reduced (2, 2));
        inner.removeFromTop (4);
        fxDrive .setBounds (inner.removeFromTop (30).reduced (2, 2));
        fxTone  .setBounds (inner.removeFromTop (30).reduced (2, 2));
        fxRing  .setBounds (inner.removeFromTop (30).reduced (2, 2));
        fxFold  .setBounds (inner.removeFromTop (30).reduced (2, 2));
        // v0.42.2 — Crunch (bits/rate) routing sits here as the 5th
        // bit-reduction toggle. It's now part of "All" so Drive/Tone/
        // Ring/Fold/Crunch all flip together when "All" is toggled.
        inner.removeFromTop (4);
        fxCrunch.setBounds (inner.removeFromTop (30).reduced (2, 2));
    }
    // PITCH / SLIDE
    {
        auto inner = pitchPanelR.reduced (10, 40).withHeight (30);
        fineToggle.setBounds (inner.reduced (2, 2));
    }
}

} // namespace loopsab
