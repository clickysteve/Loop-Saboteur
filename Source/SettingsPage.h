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
    // v0.42 — Preset Recall lifted out of the Engine panel into its own
    // panel so it can live in the GLOBAL view alongside the other globals.
    juce::Rectangle<int> recallPanelR;

    // ---------- Tab selector: GLOBAL + A..H -------------------------------
    juce::TextButton globalTab { "GLOBAL" };
    std::array<juce::TextButton, 8> actTabs;            // A B C D E F G H

    // Apply setVisible to every control in allInteractive based on the
    // current view mode. Called from setCurrentActTab() and buildControls().
    void applyViewModeVisibility();

    // ---------- ENGINE controls -------------------------------------------
    juce::ComboBox stageBox;
    juce::Slider   outputMixSlider;                     // 0..100% (continuous)
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
    juce::TextButton recallYes     { "YES" };
    juce::TextButton recallNo      { "NO"  };

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
        // v0.42 — OUTPUT MIX is meaningless for the CLEAN stage (there's
        // no processed signal to mix against), so disable it there.
        outputMixSlider.setEnabled (stage != (int) kOutClean);
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

    // ---------- ENGINE: Preset recall YES / NO (global flag) --------------
    // Paired radio — makes the state more glanceable than a single toggle.
    wireRadioBtn (recallYes,
                  [this] { return   proc->getPresetRecallEngine(); },
                  [this] { proc->setPresetRecallEngine (true); },
                  902);
    wireRadioBtn (recallNo,
                  [this] { return ! proc->getPresetRecallEngine(); },
                  [this] { proc->setPresetRecallEngine (false); },
                  902);

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
            for (int i = 0; i < rootParam->choices.size(); ++i)
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
            for (int i = 0; i < scaleParam->choices.size(); ++i)
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
    // v0.42 — Output Mix is greyed for CLEAN (no processed signal to mix).
    outputMixSlider.setEnabled (proc->getActEngineStage (currentActIdx) != (int) kOutClean);

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

    recallYes.setToggleState (  proc->getPresetRecallEngine(), juce::dontSendNotification);
    recallNo .setToggleState (! proc->getPresetRecallEngine(), juce::dontSendNotification);

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

inline void SettingsPage::applyViewModeVisibility()
{
    // Global-scope panels' interactive children.
    juce::Component* globalControls[] = {
        &accentBox,
        &transportButtons[0], &transportButtons[1], &transportButtons[2], &transportButtons[3],
        &freezeButtons[0], &freezeButtons[1],
        &displayWaveform, &displayTooltips, &displayPageFollow,
        &scaleRootBox, &scaleTypeBox,
        &fineToggle,
        &uiResetButton,
        &recallYes, &recallNo
    };
    // Per-Act-scope panels' interactive children.
    juce::Component* perActControls[] = {
        &stageBox, &outputMixSlider,
        &interpButtons[0], &interpButtons[1], &interpButtons[2],
        &crunchAAOn, &crunchAAOff, &crunchMuOn, &crunchMuOff,
        &stereoButtons[0], &stereoButtons[1], &stereoButtons[2], &stereoButtons[3],
        &fxAll, &fxDrive, &fxTone, &fxRing, &fxFold, &fxCrunch,
        &ringQuant
    };

    for (auto* c : globalControls)  if (c) c->setVisible (  showGlobalView);
    for (auto* c : perActControls)  if (c) c->setVisible (! showGlobalView);
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
    auto font = juce::Font (juce::FontOptions ("Impact", 22.0f, juce::Font::plain));
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
    auto font = juce::Font (juce::FontOptions ("Impact", 28.0f, juce::Font::plain));
    font.setExtraKerningFactor (0.15f);
    g.setFont (font);
    const float anchorY = titleR.getCentreY();
    g.saveState();
    g.addTransform (juce::AffineTransform::translation (0.0f, -anchorY)
                        .sheared (-0.10f, 0.0f)
                        .translated (0.0f, anchorY));
    g.drawText ("SETTINGS", titleR.reduced (16, 0), juce::Justification::centredLeft);
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
        auto hintFont = juce::Font (juce::FontOptions ("Impact", 17.0f, juce::Font::plain));
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
              + "   " + dash + "   PER-ACT SETTINGS";
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
        paintPanelHeader (g, recallPanelR,    "Presets Recall Engine");
    }
    else
    {
        paintPanelHeader (g, enginePanelR,    "Engine");
        paintPanelHeader (g, stereoPanelR,    "Stereo");
        paintPanelHeader (g, fxPanelR,        "FX on Dry Signal");
        paintPanelHeader (g, ringModPanelR,   "Ring Mod");

        // Sub-headers inside the Engine panel — only meaningful in PER-ACT view.
        // v0.42.2 — bumped to 16pt so sub-sections (OUTPUT STAGE,
        // INTERPOLATION, CRUNCH, ANTI-ALIAS FILTER, μ-LAW) read clearly
        // against the new 22pt panel headers. Darker text colour too so
        // the hierarchy stays visible: headers = accent orange, sub-labels
        // = light grey, footnotes = dim grey.
        auto subFont = juce::Font (juce::FontOptions ("Impact", 16.0f, juce::Font::plain));
        subFont.setExtraKerningFactor (0.14f);
        g.setFont (subFont);
        g.setColour (juce::Colour (0xffbbbbbb));

        auto eng = enginePanelR.reduced (12, 10).withTrimmedTop (36);
        auto label = [&g] (juce::Rectangle<int> rr, const juce::String& txt)
        {
            g.drawText (txt, rr, juce::Justification::centredLeft);
        };
        eng.removeFromTop (4);
        label (eng.removeFromTop (20), "OUTPUT STAGE");
        eng.removeFromTop (30);   // combo height
        eng.removeFromTop (8);
        label (eng.removeFromTop (20), "OUTPUT MIX   (WET / DRY OF THE STAGE)");
        eng.removeFromTop (30);   // slider
        eng.removeFromTop (8);
        label (eng.removeFromTop (20), "INTERPOLATION");
        eng.removeFromTop (30);   // radio row
        eng.removeFromTop (8);
        // v0.42.1 — Crunch section rewritten: label + 2 labelled rows
        // (ANTI-ALIAS FILTER, μ-LAW (S950-STYLE)), each as an explicit
        // ON/OFF paired radio. "APPLY TO …" moved into the FX on Dry
        // panel to sit alongside the other dry-signal routings.
        label (eng.removeFromTop (20), "CRUNCH");
        eng.removeFromTop (4);
        label (eng.removeFromTop (18), "ANTI-ALIAS FILTER");
        eng.removeFromTop (30);   // AA ON/OFF row
        eng.removeFromTop (4);
        label (eng.removeFromTop (18),
               juce::String::fromUTF8 ("\xce\xbc-LAW   (S950-STYLE COMPANDING)"));
        eng.removeFromTop (30);   // μ-law ON/OFF row
        // v0.42 — Preset Recall moved out to its own panel in GLOBAL view.
    }

    // v0.42.1 — Ring Mod panel carries a small footer hint telling the
    // user that Scale Quantise is configured in GLOBAL settings, so when
    // they enable "Quantise to scale" here they know where to change it.
    // v0.42.2 — bumped from 11pt → 14pt and moved into its own reserved
    // 22pt-high footer slot so it no longer overlaps the toggle above it.
    if (! showGlobalView && ! ringModPanelR.isEmpty())
    {
        auto hintFont = juce::Font (juce::FontOptions ("Impact", 14.0f, juce::Font::plain));
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
                     { 70, 98, 98, 138 },
                     { &accentPanelR, &transportPanelR, &freezePanelR,
                       &displayPanelR });
        layoutStack (colB,
                     { 116, 74, 98, 98 },
                     { &scalePanelR, &pitchPanelR, &uiPanelR, &recallPanelR });
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
        recallPanelR    = {};

        const int perActW  = bounds.getWidth();
        const int engineW  = juce::jmax (280, (perActW - gap) * 60 / 100);
        auto colEngine = bounds.removeFromLeft (engineW);
        bounds.removeFromLeft (gap);
        auto colMisc   = bounds;

        enginePanelR = colEngine;

        // ---------- Engine panel (per-Act) ----------------------------
        // Geometry mirrors sub-label positions in paint() — keep in sync.
        // v0.42 — Preset Recall was lifted out into GLOBAL view, so the
        // Engine panel no longer carries a recall row here.
        // v0.42.2 — sub-labels bumped 16→20 (main) and 14→18 (crunch
        // sub-sections); header trim 28→36; vertical reduce 8→10 — must
        // match the font sizes set in paint() or controls overlap the
        // labels above them.
        {
            auto eng = enginePanelR.reduced (12, 10).withTrimmedTop (36);
            eng.removeFromTop (4);
            eng.removeFromTop (20);                              // "OUTPUT STAGE" label
            stageBox.setBounds (eng.removeFromTop (30).reduced (0, 2));

            eng.removeFromTop (8);
            eng.removeFromTop (20);                              // "OUTPUT MIX" label
            outputMixSlider.setBounds (eng.removeFromTop (30).reduced (0, 2));

            eng.removeFromTop (8);
            eng.removeFromTop (20);                              // "INTERPOLATION" label
            {
                auto row = eng.removeFromTop (30);
                const int btnW = row.getWidth() / 3;
                for (int i = 0; i < 3; ++i)
                    interpButtons[i].setBounds (
                        row.removeFromLeft (i == 2 ? row.getWidth() : btnW).reduced (2, 2));
            }

            eng.removeFromTop (8);
            eng.removeFromTop (20);                              // "CRUNCH" section header
            // v0.42.1 — paired ON/OFF radio per sub-setting.
            // v0.42.2 — sub-label 14→18 to match paint().
            eng.removeFromTop (4);
            eng.removeFromTop (18);                              // "ANTI-ALIAS FILTER"
            {
                auto row = eng.removeFromTop (30);
                const int btnW = row.getWidth() / 2;
                crunchAAOn .setBounds (row.removeFromLeft (btnW).reduced (2, 2));
                crunchAAOff.setBounds (row.reduced (2, 2));
            }
            eng.removeFromTop (4);
            eng.removeFromTop (18);                              // "μ-law ..." sub-label
            {
                auto row = eng.removeFromTop (30);
                const int btnW = row.getWidth() / 2;
                crunchMuOn .setBounds (row.removeFromLeft (btnW).reduced (2, 2));
                crunchMuOff.setBounds (row.reduced (2, 2));
            }
        }

        // ---------- Per-Act misc column -------------------------------
        // v0.42.1 — FX panel grew to fit a sixth toggle (Crunch bits/rate,
        // migrated out of the Engine/Crunch section). Ring Mod grew a
        // little to carry a "scale set in global" footer hint.
        // v0.42.2 — panel heights bumped for taller headers + taller
        // footer band on Ring Mod. FX panel in particular gets +12 to
        // keep its six toggles clear of the new 36pt header.
        layoutStack (colMisc,
                     { 98, 256, 98 },
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
    // v0.42 — PRESET RECALL ENGINE (now its own panel in GLOBAL view).
    {
        auto inner = recallPanelR.reduced (10, 40).withHeight (34);
        const int btnW = inner.getWidth() / 2;
        recallYes.setBounds (inner.removeFromLeft (btnW).reduced (2, 2));
        recallNo .setBounds (inner.reduced (2, 2));
    }
}

} // namespace loopsab
