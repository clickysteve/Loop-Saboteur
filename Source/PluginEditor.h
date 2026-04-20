#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "SettingsPage.h"

// ============================================================================
//  LoopSaboteurEditor — v0.8
//
//  Panels (top to bottom):
//    1. Title strip — logo + help (?) + FINE + FREEZE + SEQ + settings (gear)
//    2. 7x4 knob grid — all per-Act state (26 knobs)
//    3. Act bank — 8 Act slots (A..H) with a STORE button each
//    4. Waveform strip — scrolling peak history with a playhead marker
//       (hide-able via the settings menu)
//    5. Sequencer strip — header row (RANDOM / Clear / PAGE / LENGTH /
//       RATE / SWING) and a step row holding 32 cells (one of the
//       four 32-step pages)
//    6. Footer — tagline
//
//  New in v0.8:
//    * 128 steps addressed as four pages of 32 (A/B/C/D). PAGE button
//      lives in the length cluster so it reads next to the step count.
//    * Right-click on a step cell opens a full context menu —
//      discoverable alternative to the modifier-click shortcuts.
//    * Grab offset rendered on each cell (top-right, hidden when 0).
//    * Lock Edit mode colours the *knob label* for any slot that holds
//      a non-NaN lock, so the user can see which knobs already override.
//    * Settings (gear) popup — freeze mode Toggle/Momentary, waveform
//      visibility, UI scale preset (75/100/125/150%).
//    * Help (?) button — opens a small crib-sheet CallOutBox next to
//      the plugin title.
//    * 1:1 ratio removed (was a no-op relative to "inherit chance").
//    * Editor preferences are persisted inside the processor state.
//
//  Click semantics on a step cell:
//    * plain left         — stamp the currently selected Act
//    * right / ctrl-click — open full context menu (stamp / clear /
//                           ratio / grab / freeze hold / edit locks)
//    * shift-click        — enter / exit Lock Edit mode on this step
//    * cmd-click          — toggle freeze hold on this step
//    * alt-click          — cycle ratio probability
// ============================================================================
class LoopSaboteurEditor : public juce::AudioProcessorEditor,
                           private juce::Timer
{
public:
    explicit LoopSaboteurEditor (LoopSaboteurProcessor&);
    ~LoopSaboteurEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;
    void mouseDown (const juce::MouseEvent& e) override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttachment  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    // ----- nested: slider that snaps to the nearest integer on
    //       double-click. Useful for pitch/slide ("I want exactly -2
    //       semitones" is otherwise a steady-hand exercise). For the
    //       choice-param knobs the underlying step is already 1 so
    //       rounding is a no-op; for 0..1 percentages it snaps to 0/1.
    //
    // v0.12 — PITCH and SLIDE also support a "snap-to-whole" drag mode
    // used when FINE is OFF. Earlier versions tried to achieve this by
    // calling setRange(..., 1.0) after the APVTS SliderAttachment was
    // built, but the attachment's NormalisableRange kept asserting
    // itself and the drag remained continuous — Steve hit this as
    // "FINE mode broken, always on fine". Overriding snapValue() is
    // the robust fix: Slider calls it for every drag position and we
    // just round to the nearest integer when snapWhole is true.
    class SnapSlider : public juce::Slider
    {
    public:
        bool snapWhole = false;

        // v0.22 — optional hard override for the value→text display.
        // JUCE's SliderParameterAttachment can silently re-set
        // textFromValueFunction, so we bypass it entirely by overriding
        // getTextFromValue() which the attachment can't touch.
        std::function<juce::String(double)> displayOverride;

        // v0.38 — right-click hook. If set, right-clicks (or ctrl-click
        // on macOS) invoke this callback instead of bubbling up to
        // juce::Slider's default drag handling. Used by the editor to
        // pop a MOD-assign menu on the clicked knob.
        std::function<void()> onRightClick;

        juce::String getTextFromValue (double v) override
        {
            if (displayOverride)
                return displayOverride (v);
            return juce::Slider::getTextFromValue (v);
        }

        void mouseDoubleClick (const juce::MouseEvent&) override
        {
            setValue (std::round (getValue()),
                      juce::sendNotificationSync);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            // isPopupMenu() covers right-click AND ctrl-click on macOS,
            // which is the platform convention we want anyway.
            if (onRightClick && e.mods.isPopupMenu())
            {
                onRightClick();
                return;  // swallow so JUCE's drag-start logic doesn't engage
            }
            juce::Slider::mouseDown (e);
        }

        double snapValue (double attemptedValue, DragMode dragMode) override
        {
            if (snapWhole && dragMode != notDragging)
                return std::round (attemptedValue);
            return juce::Slider::snapValue (attemptedValue, dragMode);
        }
    };

    // ----- nested: one labelled rotary knob bundle --------------------
    // lockDirty is set while Lock Edit mode is active on a step whose
    // slot for THIS knob is non-NaN. It's purely cosmetic — the label
    // turns the accent colour so the user can see at a glance which
    // knobs already carry an override for the step they're editing.
    struct LabeledKnob
    {
        SnapSlider   slider;
        juce::Label  label;
        std::unique_ptr<SliderAttachment> attachment;
        bool         lockDirty = false;
        // v0.38 — we remember the APVTS paramId at knob-build time so
        // the right-click MOD-assign menu can translate this knob to a
        // lockable slot index at click time. Empty string means the
        // knob has no MOD route (shouldn't happen for real knobs but
        // keeps debug builds safe).
        juce::String paramId;
    };

    // ----- nested: stencil look-and-feel --------------------------------
    // v0.28 — Matches the K5 mockup CSS exactly:
    //   • Buttons: transparent bg, 1px border, rounded 4px, 10px font
    //   • Rotary sliders: 50px dial, 3px bright outline, accent arc
    //   • Labels: 10px bold
    struct StencilLF : public juce::LookAndFeel_V4
    {
        // --- buttons (outlined, matching mockup: transparent + border) ---
        void drawButtonBackground (juce::Graphics& g,
                                   juce::Button& btn,
                                   const juce::Colour& backgroundColour,
                                   bool shouldDrawButtonAsHighlighted,
                                   bool shouldDrawButtonAsDown) override
        {
            auto r = btn.getLocalBounds().toFloat().reduced (0.5f);
            auto c = backgroundColour;
            if (shouldDrawButtonAsHighlighted) c = c.brighter (0.1f);
            if (shouldDrawButtonAsDown)        c = c.darker (0.1f);

            // Fill (typically transparent or very faint tint).
            g.setColour (c);
            g.fillRoundedRectangle (r, 4.0f);

            // Border colour = button's current text colour (matches mockup:
            // grey border off, coloured border on).
            auto* tb = dynamic_cast<juce::TextButton*> (&btn);
            auto borderCol = tb
                ? tb->findColour (btn.getToggleState()
                                  ? juce::TextButton::textColourOnId
                                  : juce::TextButton::textColourOffId)
                                  .withAlpha (0.7f)
                : juce::Colour (0xff555555);
            g.setColour (borderCol);
            g.drawRoundedRectangle (r, 4.0f, 1.0f);
        }

        juce::Font getTextButtonFont (juce::TextButton&, int) override
        {
            return juce::Font (juce::FontOptions (13.0f, juce::Font::bold));
        }

        // v0.36 — custom text draw:
        //   * "hoverAccent" property → button text lights up in that
        //     colour on mouse-over (e.g. MOD glows cyan on hover).
        //   * "segmentedToggle" property (string "A|B") → renders a
        //     two-segment label where the ACTIVE half is painted in the
        //     "on" text colour and the inactive half in the "off" text
        //     colour. Removes the state-vs-action ambiguity for toggles
        //     like SYNC / FREE.
        void drawButtonText (juce::Graphics& g, juce::TextButton& btn,
                             bool shouldDrawButtonAsHighlighted,
                             bool shouldDrawButtonAsDown) override
        {
            const auto font = getTextButtonFont (btn, btn.getHeight());
            g.setFont (font);

            const juce::String segmented = btn.getProperties()
                .getWithDefault ("segmentedToggle", juce::String()).toString();

            const int yIndent = juce::jmin (4, btn.proportionOfHeight (0.3f));
            const int bh = btn.getHeight() - yIndent * 2;

            if (segmented.contains ("|"))
            {
                // Split into two halves and paint each with its own
                // colour based on the toggle state.
                auto parts = juce::StringArray::fromTokens (segmented, "|", "");
                parts.removeEmptyStrings();
                if (parts.size() == 2)
                {
                    const juce::Colour onCol  = btn.findColour (juce::TextButton::textColourOnId);
                    const juce::Colour offCol = btn.findColour (juce::TextButton::textColourOffId);
                    const bool isOn = btn.getToggleState();

                    auto paintHalf = [&] (juce::Rectangle<int> r, const juce::String& txt, bool active)
                    {
                        juce::Colour c = active ? onCol : offCol;
                        if (shouldDrawButtonAsHighlighted && ! active) c = c.brighter (0.2f);
                        if (shouldDrawButtonAsDown)                    c = c.darker   (0.1f);
                        g.setColour (c);
                        g.drawFittedText (txt, r, juce::Justification::centred, 1);
                    };

                    const int w = btn.getWidth();
                    const int halfW = w / 2;
                    paintHalf ({ 0,     yIndent, halfW,       bh }, parts[0],  isOn);
                    // Dim separator pip between the halves.
                    g.setColour (juce::Colour (0x55ffffff));
                    g.fillRect (halfW - 1, yIndent + 2, 1, bh - 4);
                    paintHalf ({ halfW, yIndent, w - halfW,   bh }, parts[1], !isOn);
                    return;
                }
            }

            juce::Colour col = btn.findColour (btn.getToggleState()
                                               ? juce::TextButton::textColourOnId
                                               : juce::TextButton::textColourOffId);

            // Hover accent: when NOT toggled on, highlight lifts the text
            // to the tagged accent colour. When already on, hover just
            // brightens slightly.
            if (shouldDrawButtonAsHighlighted)
            {
                const int accentARGB = (int) btn.getProperties().getWithDefault ("hoverAccent", 0);
                if (accentARGB != 0 && ! btn.getToggleState())
                    col = juce::Colour ((juce::uint32) accentARGB);
                else
                    col = col.brighter (0.2f);
            }
            if (shouldDrawButtonAsDown)
                col = col.darker (0.1f);

            g.setColour (col);
            const int cornerSize = juce::jmin (btn.getHeight(), btn.getWidth()) / 2;
            const int fontHeight = juce::roundToInt (font.getHeight() * 0.6f);
            const int leftIndent  = juce::jmin (fontHeight, 2 + cornerSize / (btn.isConnectedOnLeft()  ? 4 : 2));
            const int rightIndent = juce::jmin (fontHeight, 2 + cornerSize / (btn.isConnectedOnRight() ? 4 : 2));
            const int textWidth = btn.getWidth() - leftIndent - rightIndent;
            if (textWidth > 0)
                g.drawFittedText (btn.getButtonText(),
                                  leftIndent, yIndent, textWidth,
                                  bh,
                                  juce::Justification::centred, 2);
        }

        // --- rotary sliders: two styles depending on outline colour ---
        // Grid knobs (outline = textBright): white filled circle, black pointer,
        //   orange value arc drawn OUTSIDE the white circle.
        // Flanking knobs (outline = transparent): dark filled circle, white pointer,
        //   no outline ring.
        void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                               float sliderPosProportional, float rotaryStartAngle,
                               float rotaryEndAngle, juce::Slider& slider) override
        {
            const float maxDia   = 56.0f;
            const float rawDia   = (float) juce::jmin (width, height);
            const float diameter = juce::jmin (rawDia, maxDia);
            const float radius   = diameter * 0.5f - 2.0f;
            const float cx       = (float) x + (float) width  * 0.5f;
            const float cy       = (float) y + (float) height * 0.5f;
            const float angle    = rotaryStartAngle
                                 + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

            auto outlineCol = slider.findColour (juce::Slider::rotarySliderOutlineColourId);
            const bool isFlanking = (outlineCol.getAlpha() < 10);

            auto arcCol = slider.findColour (juce::Slider::rotarySliderFillColourId);

            // v0.33 — when this knob is targeted by an active LFO we
            // change its look. Two styles exist and can be toggled here:
            //   kModBodyCyanStyle = true  → v0.38 style: the knob BODY
            //       is painted cyan and the pointer line goes black.
            //       More visible at a glance — better for knob-dense
            //       pages where a thin coloured line is easy to miss.
            //   kModBodyCyanStyle = false → original v0.33 style: the
            //       pointer line is drawn cyan, body stays dark. Kept
            //       as a trivial revert if the new look feels wrong.
            static constexpr bool kModBodyCyanStyle = true;

            const bool lfoTargeted =
                (bool) slider.getProperties().getWithDefault ("lfoTargeted", false);
            const juce::Colour lfoCyan (0xff4fd1e0);

            if (isFlanking)
            {
                // === FLANKING STYLE (CHANCE / MIX) ===
                const float arcRadius = radius + 1.0f;

                // Value arc ONLY — no background track ring.
                if (sliderPosProportional > 0.005f)
                {
                    juce::Path arc;
                    arc.addCentredArc (cx, cy, arcRadius, arcRadius, 0.0f,
                                       rotaryStartAngle, angle, true);
                    g.setColour (arcCol);
                    g.strokePath (arc, juce::PathStrokeType (4.0f));
                }

                // Knob body fill — cyan when modulated (new style),
                // otherwise the usual near-black.
                const juce::Colour flankBody (0xff1a1a1a);
                const juce::Colour bodyFill = (lfoTargeted && kModBodyCyanStyle)
                                                ? lfoCyan : flankBody;
                g.setColour (bodyFill);
                g.fillEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);

                // Pointer line. In body-cyan mode the pointer flips to
                // black so it stays legible against the cyan fill.
                const float innerR = radius * 0.2f;
                const float outerR = radius * 0.7f;
                juce::Colour pointerCol;
                if (lfoTargeted)
                    pointerCol = kModBodyCyanStyle ? juce::Colours::black : lfoCyan;
                else
                    pointerCol = juce::Colour (0xffeeeeee);
                g.setColour (pointerCol);
                g.drawLine (cx + innerR * std::sin (angle),
                            cy - innerR * std::cos (angle),
                            cx + outerR * std::sin (angle),
                            cy - outerR * std::cos (angle), 2.5f);
            }
            else
            {
                // === GRID STYLE ===
                const float arcRadius = radius + 1.0f;

                // Dim background track (full sweep).
                {
                    juce::Path track;
                    track.addCentredArc (cx, cy, arcRadius, arcRadius, 0.0f,
                                         rotaryStartAngle, rotaryEndAngle, true);
                    g.setColour (juce::Colour (0xff333333));
                    g.strokePath (track, juce::PathStrokeType (4.0f));
                }

                // Bold value arc (orange, or yellow when p-locked).
                if (sliderPosProportional > 0.001f)
                {
                    juce::Path arc;
                    arc.addCentredArc (cx, cy, arcRadius, arcRadius, 0.0f,
                                       rotaryStartAngle, angle, true);
                    g.setColour (arcCol);
                    g.strokePath (arc, juce::PathStrokeType (4.0f));
                }

                // White outlined circle (the knob body).
                const float knobR = radius - 3.0f;
                g.setColour (outlineCol);  // textBright = white
                g.drawEllipse (cx - knobR, cy - knobR, knobR * 2.0f, knobR * 2.0f, 3.0f);

                // Fill inside — cyan when modulated (new style), else dark.
                const juce::Colour gridBody (0xff222222);
                const juce::Colour bodyFill = (lfoTargeted && kModBodyCyanStyle)
                                                ? lfoCyan : gridBody;
                g.setColour (bodyFill);
                g.fillEllipse (cx - knobR + 1.5f, cy - knobR + 1.5f,
                               (knobR - 1.5f) * 2.0f, (knobR - 1.5f) * 2.0f);

                // Pointer line. Matches the arc colour by default; in
                // body-cyan mode we flip to black so it reads against
                // the cyan fill.
                const float innerR = radius * 0.15f;
                const float outerR = radius * 0.6f;
                juce::Colour pointerCol;
                if (lfoTargeted)
                    pointerCol = kModBodyCyanStyle ? juce::Colours::black : lfoCyan;
                else
                    pointerCol = arcCol;
                g.setColour (pointerCol);
                g.drawLine (cx + innerR * std::sin (angle),
                            cy - innerR * std::cos (angle),
                            cx + outerR * std::sin (angle),
                            cy - outerR * std::cos (angle), 2.5f);
            }

            // === VALUE TEXT — painted below the knob ===
            {
                auto valueText = slider.getTextFromValue (slider.getValue());
                if (valueText.isNotEmpty())
                {
                    auto textCol = isFlanking
                        ? slider.findColour (juce::Slider::textBoxTextColourId)
                        : arcCol;  // grid: match arc colour (orange / yellow when p-locked)
                    g.setColour (textCol);
                    // v0.36 — value text bumped 12pt → 13.5pt bold to
                    // match the chunkier caption weight above the knob.
                    g.setFont (juce::Font (juce::FontOptions (13.5f, juce::Font::bold)));
                    // Centred below the knob dial.
                    auto valRect = juce::Rectangle<float> (
                        cx - 44.0f, cy + radius + 5.0f, 88.0f, 16.0f);
                    g.drawText (valueText, valRect, juce::Justification::centred);
                }
            }

        }

        // --- labels: respect the font explicitly set on each Label ---
        // v0.36: previously hardcoded 12pt bold, which SILENTLY overrode
        // every setFont() call in configureKnob (and elsewhere). That's
        // why bumping the knob label size in configureKnob appeared to
        // do nothing. Returning label.getFont() restores JUCE's default
        // behaviour so setFont() actually takes effect. Labels that
        // haven't explicitly set a font still get juce::Label's default,
        // which is sized appropriately for its bounds.
        juce::Font getLabelFont (juce::Label& label) override
        {
            return label.getFont();
        }

        // --- Tooltip: orange bg, white text, wraps to multiple lines ---
        // v0.32: previously used 12pt plain for measurement and 13pt bold
        // for rendering (wider per char) with drawFittedText at 1 line,
        // so longer tooltips clipped. Now we use a shared font spec and
        // AttributedString::draw with word-wrap so the box sized by
        // getTooltipBounds below always fits the text.
        void drawTooltip (juce::Graphics& g, const juce::String& text,
                          int width, int height) override
        {
            g.fillAll (juce::Colour (0xffff5a3c));   // accent orange
            juce::Font f (juce::FontOptions (16.0f, juce::Font::bold));
            auto textArea = juce::Rectangle<int> (10, 6, width - 20, height - 12).toFloat();
            juce::AttributedString attStr;
            attStr.setJustification (juce::Justification::topLeft);
            attStr.setWordWrap (juce::AttributedString::byWord);
            attStr.append (text, f, juce::Colours::white);
            attStr.draw (g, textArea);
        }

        // --- PopupMenu (Settings) — K5 stencil style ---
        void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
        {
            g.fillAll (juce::Colour (0xff1a1a1a));
            // Orange border (2px).
            g.setColour (juce::Colour (0xffff5a3c));
            g.drawRect (0, 0, width, height, 2);
        }

        void drawPopupMenuSectionHeader (juce::Graphics& g,
                                         const juce::Rectangle<int>& area,
                                         const juce::String& sectionName) override
        {
            // Stencil-style section header: Impact font, orange, skewed,
            // with a 2px orange underline with gap below text.
            auto r = area.reduced (16, 0);
            auto textR = r.withTrimmedBottom (6);  // gap before underline
            float anchorY = (float) textR.getCentreY();
            g.saveState();
            g.addTransform (juce::AffineTransform::translation (0.0f, -anchorY)
                                .sheared (-0.10f, 0.0f)
                                .translated (0.0f, anchorY));
            g.setColour (juce::Colour (0xffff5a3c));
            auto hdrFont = juce::Font (juce::FontOptions ("Impact", 16.0f, juce::Font::plain));
            hdrFont.setExtraKerningFactor (0.18f);
            g.setFont (hdrFont);
            g.drawText (sectionName, textR, juce::Justification::bottomLeft);
            g.restoreState();

            // Orange underline with gap.
            g.setColour (juce::Colour (0xffff5a3c));
            g.fillRect (r.getX(), r.getBottom() - 2, r.getWidth(), 2);
        }

        void drawPopupMenuItem (juce::Graphics& g,
                                const juce::Rectangle<int>& area,
                                bool isSeparator,
                                bool isActive,
                                bool isHighlighted,
                                bool isTicked,
                                bool hasSubMenu,
                                const juce::String& text,
                                const juce::String& shortcutKeyText,
                                const juce::Drawable* icon,
                                const juce::Colour* textColour) override
        {
            if (isSeparator)
            {
                auto sepArea = area.reduced (16, 0);
                g.setColour (juce::Colour (0xff333333));
                g.fillRect (sepArea.getX(), sepArea.getCentreY(), sepArea.getWidth(), 1);
                return;
            }

            auto r = area.reduced (6, 1);

            // v0.42.5 — Two genuinely different hues instead of two
            // shades of the same grey. Ticked items live in the ORANGE
            // family (bold 4pt left stripe + heavy orange tint fill +
            // warm text + orange check). Hovered-but-not-ticked items
            // get a clean medium-grey fill — no orange anywhere. The
            // eye reads "orange = selected, grey = cursor here" at a
            // glance without having to compare shade-levels of the
            // same colour. When a row is BOTH ticked and hovered we
            // brighten the orange fill rather than stacking a grey
            // wash on top of it, so the orange identity is preserved.
            const juce::Colour accentOrange (0xffff5a3c);

            if (isTicked)
            {
                // Heavy orange wash so it's impossible to miss. The
                // alpha is bumped when also hovered so the row
                // visibly responds without losing its orange identity.
                const float tint = (isHighlighted && isActive) ? 0.45f : 0.28f;
                g.setColour (accentOrange.withAlpha (tint));
                g.fillRect (r);

                // Bold left indicator strip — 4pt wide, solid orange.
                g.setColour (accentOrange);
                g.fillRect (r.getX(), r.getY(), 4, r.getHeight());
            }
            else if (isHighlighted && isActive)
            {
                // Solid mid-grey — clearly a "cursor is here" colour,
                // no colour hue that could be confused with ticked.
                g.setColour (juce::Colour (0xff3d3d3d));
                g.fillRect (r);
            }

            // Tick mark — orange checkmark.
            if (isTicked)
            {
                g.setColour (juce::Colour (0xffffffff));   // white on the orange fill reads louder
                auto tickArea = r.removeFromLeft (28);
                g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
                g.drawText (juce::String::fromUTF8 ("\xe2\x9c\x93"), tickArea,
                            juce::Justification::centred);
            }
            else
            {
                r.removeFromLeft (28);
            }

            // Text — ticked rows go to near-white so they POP against
            // the orange fill (the fill carries the colour identity,
            // not the text). Hovered rows go pure white against grey.
            // Idle rows stay dimmer grey so the active one stands out.
            juce::Colour textCol;
            if (! isActive)             textCol = juce::Colour (0xff555555);
            else if (isTicked)          textCol = juce::Colour (0xfffff4ee);
            else if (isHighlighted)     textCol = juce::Colour (0xffffffff);
            else                        textCol = juce::Colour (0xffbbbbbb);
            g.setColour (textCol);
            g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
            g.drawText (text, r.reduced (6, 0), juce::Justification::centredLeft);

            // Submenu arrow.
            if (hasSubMenu)
            {
                auto arrowArea = r.removeFromRight (24);
                g.setColour (juce::Colour (0xff666666));
                g.setFont (juce::Font (juce::FontOptions (14.0f)));
                g.drawText (juce::String::fromUTF8 ("\xe2\x96\xb6"), arrowArea,
                            juce::Justification::centred);
            }
        }

        juce::Font getPopupMenuFont() override
        {
            return juce::Font (juce::FontOptions (18.0f, juce::Font::bold));
        }

        // Comfortable item sizes — readable but not wasteful.
        void getIdealPopupMenuItemSize (const juce::String& text,
                                        bool isSeparator,
                                        int standardMenuItemHeight,
                                        int& idealWidth,
                                        int& idealHeight) override
        {
            if (isSeparator)
            {
                idealWidth = 50;
                idealHeight = 10;
                return;
            }
            auto font = getPopupMenuFont();
            idealWidth = (int) font.getStringWidthFloat (text) + 80;
            idealHeight = 32;
        }

        int getPopupMenuBorderSize() override { return 2; }

        // --- Tooltip bounds (drawTooltip is defined above) ---
        // v0.32/v0.34: must use the SAME font spec AND the same paddings
        // that drawTooltip renders with, or measurements under-report
        // width/height and text clips. drawTooltip renders inside a
        // Rectangle (10, 6, width - 20, height - 12) at 16pt bold, so we
        // bake 20px horizontal / 12px vertical padding into the bounds
        // calculation here and measure at the exact same 16pt font. Uses
        // std::ceil on both axes so fractional pixels round up instead of
        // chopping the last character or the last wrapped line. Cap width
        // is lifted to 420 so two-line tooltips don't hit the ceiling for
        // knob descriptions like TAPE / FOLD / RING MOD.
        juce::Rectangle<int> getTooltipBounds (const juce::String& tipText,
                                                juce::Point<int> screenPos,
                                                juce::Rectangle<int> parentArea) override
        {
            juce::Font f (juce::FontOptions (16.0f, juce::Font::bold));
            const int maxW         = 420;
            const int horizPad     = 20;   // matches drawTooltip textArea inset
            const int vertPad      = 12;
            juce::AttributedString attStr;
            attStr.setWordWrap (juce::AttributedString::byWord);
            attStr.append (tipText, f);
            juce::TextLayout layout;
            layout.createLayout (attStr, (float) (maxW - horizPad));
            const int w = juce::jmin (maxW, (int) std::ceil (layout.getWidth()) + horizPad + 4);
            const int h = (int) std::ceil (layout.getHeight()) + vertPad + 4;
            return juce::Rectangle<int> (screenPos.x > parentArea.getCentreX() ? screenPos.x - w : screenPos.x + 12,
                                          screenPos.y + 20, w, h)
                    .constrainedWithin (parentArea);
        }

        // --- ComboBox (LFO page dropdowns) — stencil style ---
        // Dark panel, orange border, orange arrow. Matches the
        // outlined-button aesthetic so the MOD page dropdowns don't
        // look like factory JUCE.
        void drawComboBox (juce::Graphics& g, int width, int height,
                           bool /*isButtonDown*/,
                           int /*buttonX*/, int /*buttonY*/,
                           int /*buttonW*/, int /*buttonH*/,
                           juce::ComboBox& box) override
        {
            auto r = juce::Rectangle<float> (0.5f, 0.5f, (float) width - 1.0f, (float) height - 1.0f);
            g.setColour (juce::Colour (0xff1a1a1a));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (juce::Colour (0xffff5a3c).withAlpha (0.7f));
            g.drawRoundedRectangle (r, 3.0f, 1.0f);

            // Arrow ▼ on the right
            auto arrowArea = juce::Rectangle<float> ((float) width - 18.0f, 0.0f, 18.0f, (float) height);
            g.setColour (box.isEnabled() ? juce::Colour (0xffff5a3c) : juce::Colour (0xff555555));
            juce::Path tri;
            const float ax = arrowArea.getCentreX();
            const float ay = arrowArea.getCentreY();
            tri.addTriangle (ax - 4.0f, ay - 2.0f,
                             ax + 4.0f, ay - 2.0f,
                             ax,        ay + 3.0f);
            g.fillPath (tri);
        }

        juce::Font getComboBoxFont (juce::ComboBox&) override
        {
            return juce::Font (juce::FontOptions (13.0f, juce::Font::bold));
        }

        void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
        {
            label.setBounds (6, 1, box.getWidth() - 22, box.getHeight() - 2);
            label.setFont (getComboBoxFont (box));
            label.setColour (juce::Label::textColourId, juce::Colour (0xffebebf0));
        }

        // --- Linear slider (LFO depth slider) — stencil style ---
        // Dark track, orange fill from min to thumb position, orange
        // thumb bar. Horizontal only (which matches LFO depth usage).
        void drawLinearSlider (juce::Graphics& g,
                               int x, int y, int width, int height,
                               float sliderPos,
                               float minSliderPos, float maxSliderPos,
                               const juce::Slider::SliderStyle style,
                               juce::Slider& slider) override
        {
            if (style != juce::Slider::LinearHorizontal)
            {
                // Fall back to base class for styles we don't handle.
                juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height,
                                                       sliderPos,
                                                       minSliderPos, maxSliderPos,
                                                       style, slider);
                return;
            }

            const float trackH = 6.0f;
            const float trackY = (float) y + (float) height * 0.5f - trackH * 0.5f;
            juce::Rectangle<float> track ((float) x + 2.0f, trackY,
                                          (float) width - 4.0f, trackH);

            // Background track
            g.setColour (juce::Colour (0xff2a2a2a));
            g.fillRoundedRectangle (track, 2.0f);
            g.setColour (juce::Colour (0xff555555));
            g.drawRoundedRectangle (track, 2.0f, 1.0f);

            // Fill from left edge to thumb
            const float filledW = juce::jmax (0.0f, sliderPos - track.getX());
            if (filledW > 0.5f)
            {
                g.setColour (juce::Colour (0xffff5a3c));
                g.fillRoundedRectangle (juce::Rectangle<float> (track.getX(), trackY,
                                                                filledW, trackH), 2.0f);
            }

            // Vertical thumb bar
            const float thumbW = 3.0f;
            const float thumbH = (float) height - 6.0f;
            juce::Rectangle<float> thumb (sliderPos - thumbW * 0.5f,
                                          (float) y + 3.0f,
                                          thumbW, thumbH);
            g.setColour (juce::Colour (0xffebebf0));
            g.fillRect (thumb);

            juce::ignoreUnused (slider);
        }
    };
    StencilLF stencilLF;

    // ----- nested: TextButton that can operate in momentary mode ------
    // In momentary mode the button fires onMomentary(true) on mouseDown
    // and onMomentary(false) on mouseUp, without toggling its own
    // persistent state. Used for the momentary-freeze option: we
    // bypass the ButtonAttachment entirely while momentary is active.
    class MomentaryButton : public juce::TextButton
    {
    public:
        MomentaryButton() = default;
        std::function<void(bool)> onMomentary;
        bool momentaryMode = false;

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (momentaryMode)
            {
                setToggleState (true, juce::dontSendNotification);
                if (onMomentary) onMomentary (true);
                return;
            }
            juce::TextButton::mouseDown (e);
        }
        void mouseUp (const juce::MouseEvent& e) override
        {
            if (momentaryMode)
            {
                setToggleState (false, juce::dontSendNotification);
                if (onMomentary) onMomentary (false);
                return;
            }
            juce::TextButton::mouseUp (e);
        }
    };

    // ----- nested: TextButton that exposes right-click as a callback --
    // juce::Button::onClick deliberately ignores right-mouse clicks.
    // This tiny subclass routes them to onRightClick instead so we can
    // wire secondary actions (clear state, open context menu, etc.)
    // without having to subclass per-use-site.
    class RightClickButton : public juce::TextButton
    {
    public:
        RightClickButton() = default;
        std::function<void()> onRightClick;

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isRightButtonDown() || e.mods.isCtrlDown())
            {
                if (onRightClick) onRightClick();
                return;
            }
            juce::TextButton::mouseDown (e);
        }
    };

    // ----- nested: Act slot button ------------------------------------
    // v0.11 — the dot idea was overkill. An Act button now carries all
    // its state in its fill colour: default (grey panel), populated
    // (tinted variant), selected (bright accent orange). paintButton is
    // overridden so the three tiers are applied independently of the
    // single-colour-per-state limit on the base class.
    class ActButton : public juce::TextButton
    {
    public:
        ActButton() = default;
        bool populated = false;
        std::function<void()> onContextMenu;

        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isRightButtonDown() || e.mods.isCtrlDown())
            {
                if (onContextMenu) onContextMenu();
                return;
            }
            juce::TextButton::mouseDown (e);
        }

        void paintButton (juce::Graphics& g,
                          bool shouldDrawButtonAsHighlighted,
                          bool shouldDrawButtonAsDown) override
        {
            // Three-tier colour picker:
            //   - selected           → full accent (bright reddy-orange)
            //   - populated (not sel)→ muted accent (darker tint)
            //   - default            → plain panel grey
            juce::Colour fill;
            juce::Colour txt;
            if (getToggleState())
            {
                fill = findColour (juce::TextButton::buttonOnColourId);
                txt  = juce::Colour (0xff121216);   // near-black
            }
            else if (populated)
            {
                fill = findColour (juce::TextButton::buttonOnColourId).darker (0.55f);
                txt  = juce::Colour (0xffebebf0);   // textBright
            }
            else
            {
                fill = juce::Colour (0xff1c1c22);   // Col::panel
                txt  = juce::Colour (0xff8c8c96);   // textDim
            }
            if (shouldDrawButtonAsHighlighted)
                fill = fill.brighter (0.08f);
            if (shouldDrawButtonAsDown)
                fill = fill.darker (0.08f);

            // v0.28 — mockup-matched: bordered rounded rect.
            auto r = getLocalBounds().toFloat().reduced (1.0f);

            if (getToggleState())
            {
                // Active: filled accent bg, dark text.
                g.setColour (fill);
                g.fillRoundedRectangle (r, 4.0f);
            }
            else
            {
                // Inactive: dark fill + 2px border.
                g.setColour (juce::Colour (0xff1a1a1a));
                g.fillRoundedRectangle (r, 4.0f);
                auto borderCol = populated
                               ? findColour (juce::TextButton::buttonOnColourId).darker (0.4f)
                               : juce::Colour (0xff444444);
                g.setColour (borderCol);
                g.drawRoundedRectangle (r, 4.0f, 2.0f);
            }

            g.setColour (txt);
            g.setFont (juce::Font (juce::FontOptions ("Impact", 22.0f, juce::Font::plain)));
            g.drawText (getButtonText(), getLocalBounds(),
                        juce::Justification::centred, false);
        }
    };

    // ----- nested: one step cell in the sequencer grid ----------------
    // Routes click modifiers to callbacks and paints the per-step state
    // (scene letter, ratio label, freeze dot, lock dot, edit ring,
    // grab-offset number). v0.8: right/ctrl-click now opens a popup
    // context menu via onContextMenu instead of wiping the step, so
    // users can discover the full action set without knowing modifiers.
    class StepCell : public juce::Component,
                     public juce::TooltipClient
    {
    public:
        juce::String getTooltip() override;

        std::function<void()> onSet;          // plain left click: stamp Act
        std::function<void()> onContextMenu;  // right or ctrl-click: popup menu
        std::function<void()> onCycleRatio;   // alt-click: cycle N:M ratio
        std::function<void()> onToggleFreeze; // cmd-click: per-step hold
        std::function<void()> onShiftClick;   // shift-click: lock-edit mode
        std::function<void()> onSelectRange;  // ctrl+shift+click: range selection

        void setSceneIndex (int i)  { scene = i;      repaint(); }
        void setActive     (bool a) { active = a;     repaint(); }
        void setPlaying    (bool p) { if (p != playing) { playing = p; repaint(); } }
        void setRatioIndex (int r)  { ratio = r;      repaint(); }
        void setFreezeHold (bool f) { freezeHold = f; repaint(); }
        void setHasLocks   (bool h) { hasLocks = h;   repaint(); }
        void setEditingLock(bool e) { editing = e;    repaint(); }
        void setRatchet    (int r)  { if (r != ratchet) { ratchet = r; repaint(); } }
        void setMuted      (bool m) { if (m != muted)   { muted = m;     repaint(); } }
        void setIndex      (int i)  { stepIndex = i; }
        // v0.11 — true when this cell should paint with the brighter
        // accent-beat background (driven by the settings-menu
        // "Grid accent" choice).
        void setAccentBeat (bool a) { if (a != accentBeat) { accentBeat = a; repaint(); } }
        // v0.31 — selection highlight for multi-step copy/paste
        void setSelected   (bool s) { if (s != selected) { selected = s; repaint(); } }

        int  getSceneIndex()  const noexcept { return scene; }
        int  getRatioIndex()  const noexcept { return ratio; }
        bool getFreezeHold()  const noexcept { return freezeHold; }
        int  getRatchet()     const noexcept { return ratchet; }

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

        // v0.41 — birthday egg force overrides. When the corresponding
        // timestamp is in the future, step 1 paints its ring regardless of
        // the system date. Set by the editor's debug-menu test toggles.
        static juce::int64 force303UntilMs;
        static juce::int64 force606UntilMs;
        static juce::int64 force808UntilMs;

    private:
        int   stepIndex  = 0;
        int   scene      = -1;     // -1 = empty, 0..kNumScenes-1 = A..H
        bool  active     = true;   // false when index >= seqLength
        bool  playing    = false;
        int   ratio      = 0;      // 0 = inherit Act chance, 1..kNumRatios-1 = N:M
        bool  freezeHold = false;  // per-step freeze hold
        bool  hasLocks   = false;  // any p-lock slot populated
        bool  editing    = false;  // currently selected for lock editing
        bool  accentBeat = false;  // v0.11: brighter background when on the accent grid
        int   ratchet    = 1;      // v0.12: sub-step fire count (1 = none)
        bool  muted      = false;  // v0.13: per-step mute (manual stutter)
        bool  selected   = false;  // v0.31: selection highlight for multi-step operations
    };

    // ----- nested: waveform strip component ---------------------------
    // Reads the processor's peak ring and paints a scrolling bar
    // history with a static playhead on the right edge. Polled by the
    // editor's 30 Hz timer — we don't need sample-accurate display.
    class WaveformStrip : public juce::Component
    {
    public:
        void setProcessor (LoopSaboteurProcessor* p) { proc = p; }
        void paint (juce::Graphics&) override;
    private:
        LoopSaboteurProcessor* proc = nullptr;
    };

    // ----- nested: MOD page for LFO configuration ----------------------
    // Dedicated UI page for LFO modulation sources with waveform
    // previews, shape/rate/depth/target controls, and a header label.
    class ModPage : public juce::Component, public juce::Timer
    {
    public:
        explicit ModPage (LoopSaboteurProcessor* p) : proc (p) {}
        ~ModPage() override;

        void setProcessor (LoopSaboteurProcessor* p) { proc = p; }
        void paint (juce::Graphics&) override;
        void resized() override;
        void timerCallback() override;

        void setupLfoControls();
        void updateFromProcessor();
        void applyStencilLookAndFeel (juce::LookAndFeel* laf);
        // v0.37 — promoted to public so the outer editor can jump the
        // MOD page to the current Act when the user clicks the MOD
        // header button. Safe to call with any 0..7 index.
        void setCurrentActTab (int actIdx);

    private:
        // Helper: animated oscilloscope-style preview for each LFO.
        // Scrolls the waveform from right-to-left at a speed tied to
        // the selected rate (assumes 120 BPM for preview). When rate
        // is Off (rateIdx == 0) the wave is dimmed and doesn't move.
        class WaveformPreview : public juce::Component
        {
        public:
            int sceneIdx = 0;
            int lfoIdx = 0;
            int shape = 0;     // 0=Sine, 1=Triangle, 2=Saw, 3=Square, 4=S&H, 5=Env AHD
            int rateIdx = 0;   // 0 = Off; 1..12 map to lfoDivisionIndexToQuarters
            LoopSaboteurProcessor* proc = nullptr;
            double previewPhase = 0.0;  // advances on timer ticks for scrolling
            void paint (juce::Graphics&) override;
            // Advance the scrolling phase by dtSeconds based on current rate.
            void advancePreview (double dtSeconds) noexcept;
        };

        LoopSaboteurProcessor* proc = nullptr;
        int currentActTab = 0;    // 0..7, tracks which Act's LFOs are being displayed

        // Act tab buttons: A, B, C, D, E, F, G, H
        std::array<juce::TextButton, 8> actTabButtons;

        // 4 LFO panels (2x2 grid for the current Act)
        std::array<WaveformPreview, 4> previews;
        std::array<juce::ComboBox, 4> shapeBoxes;
        std::array<juce::Label, 4> shapeLabels;
        std::array<juce::ComboBox, 4> rateBoxes;
        std::array<juce::Label, 4> rateLabels;
        std::array<juce::TextButton, 4> syncButtons;  // v0.34 — SYNC / FREE toggle (LFO only)
        std::array<juce::Slider, 4> depthSliders;
        std::array<juce::Label, 4> depthLabels;
        std::array<juce::ComboBox, 4> targetBoxes;
        std::array<juce::Label, 4> targetLabels;

        // v0.34 — envelope AHD controls (only visible when shape == Env AHD).
        std::array<juce::Slider, 4> attackSliders;
        std::array<juce::Label,  4> attackLabels;
        std::array<juce::Slider, 4> holdSliders;
        std::array<juce::Label,  4> holdLabels;
        std::array<juce::Slider, 4> decaySliders;
        std::array<juce::Label,  4> decayLabels;
        std::array<juce::ComboBox, 4> trigBoxes;
        // v0.42.2 — explicit "Retrigger Mode:" label above trigBoxes so
        // users don't have to guess what "Slice / Active Steps / All
        // Steps" is referring to. Only drawn in Env AHD mode, same as
        // the combo.
        std::array<juce::Label,    4> trigLabels;

        // v0.38 — cross-mod controls. Target combo: None / MOD 1..4
        // (self-entry is present but disabled — self-mod can feedback).
        // Depth slider: -1..+1 signed amount; multiplies the receiver's
        // depth. Both only have an audible effect on the currently
        // selected Act's MODs, same rule as targetSlot.
        std::array<juce::ComboBox, 4> crossTargetBoxes;
        std::array<juce::Label,    4> crossTargetLabels;
        std::array<juce::Slider,   4> crossDepthSliders;

        // v0.34 — swap visibility of LFO-mode vs Env-mode controls when
        // the shape dropdown changes. Called by the shape onChange and
        // also from updateFromProcessor() when state is loaded.
        void applyShapeVisibility (int lfoIdx);
        // v0.37 — rebuild the rate combo items to show bar-divisions
        // (when synced) or Hz values (when free). Preserves the
        // selected index across the swap. Called on initial setup,
        // on sync toggle, and from updateFromProcessor().
        void repopulateRateBox (int lfoIdx, bool synced);
    };

    // ----- nested: category panel grouping ----------------------------
    // v0.25 — knob grid is now organized into 8 category groups, each
    // with a rounded-rect background panel and a label. This struct holds
    // the bounds and visual attributes for painting and layout.
    struct CategoryPanel
    {
        juce::Rectangle<int> bounds;
        juce::Colour tint;
        juce::String label;
        // v0.38 — hover text describing what the grouping has in common.
        // Empty tooltip = skip (e.g. for the empty slots past the 5
        // named categories). Shown when the mouse hovers over the
        // stencil label/panel area between knobs.
        juce::String tooltip;
    };

    // ----- nested: inner content container ---------------------------
    // v0.13 — all of the editor's children live on this container, not
    // on the editor itself. The container's internal size is fixed at
    // (kDesignW, kDesignH) and it gets an AffineTransform::scale applied
    // by the editor's resized() so the visible area matches the host
    // window. Keeping the layout in a single logical coordinate space
    // means resized() no longer has to re-enter itself (the v0.12
    // "setSize back to design size" trick) — which was causing the
    // drag-resize jitter Steve reported.
    class InnerContent : public juce::Component,
                          public juce::TooltipClient
    {
    public:
        std::function<void(juce::Graphics&)> onPaint;
        std::function<void(const juce::MouseEvent&)> onMouseDown;
        // v0.38 — editor wires this to return a tooltip string based on
        // current mouse position over category panels. Returning "" lets
        // the TooltipWindow walk up the parent chain normally.
        std::function<juce::String()> onGetTooltip;
        void paint (juce::Graphics& g) override { if (onPaint) onPaint (g); }
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (onMouseDown) onMouseDown (e);
            juce::Component::mouseDown (e);
        }
        juce::String getTooltip() override
        {
            return onGetTooltip ? onGetTooltip() : juce::String();
        }
    };

    // Category panels for v0.25 knob grid grouping
    std::array<CategoryPanel, 8> categoryPanels;

    // v0.28 — K5 flanking column bounds (painted in paintContent).
    juce::Rectangle<int> chanceColumnBounds;
    juce::Rectangle<int> mixColumnBounds;

    LoopSaboteurProcessor& processorRef;
    InnerContent           innerContent;

    // v0.10 — one TooltipWindow per editor instance so any child that
    // calls setTooltip() or implements TooltipClient actually pops a
    // tip. Earlier versions called setTooltip in several places but
    // there was no window to render them — this hooks them all up.
    std::unique_ptr<juce::TooltipWindow> tooltipWindow;

    // Per-Act knobs laid out as rows (trigger/shape/tape/mangle),
    // plus the global SEQ RATE combo in the sequencer strip. Rows:
    //   1 - trigger/slicing: chance, lookback, rate, judder, judderDiv, division
    //   2 - shaping:          pitch, slide, decay, reverse, crunch, crushRate
    //   3 - tape/sampler FX:  drive, tone, feedback, tape, ringMod, varispeed, stereo
    //   4 - mangle row:       shimmer, res, fold, gate, smear, stutter, chaos
    //   + mix at bottom-right
    LabeledKnob chance, division, lookback, rate, judder, judderDiv,
                pitch, slide, decay, reverse, crunch, crushRate, mix,
                drive, tone, feedback, tape, ringMod, varispeed, stereo, stretch,
                shimmer, res, fold, gate, smear, stutter, chaos;
    // v0.14 — SEQ RATE is a combo box in the sequencer header row
    // (alongside Swing slider), freeing the controls strip so cells
    // can be full-width.
    juce::ComboBox seqRateCombo;
    juce::Label    seqRateLabel;
    std::unique_ptr<ComboAttachment> seqRateAttachment;

    juce::Slider swingSlider;
    juce::Label  swingLabel;
    std::unique_ptr<SliderAttachment> swingAttachment;

    // FINE toggle — when on, PITCH and SLIDE drag with 0.01 st
    // resolution; otherwise they snap to whole semitones. Lives as
    // editor-local state (not an APVTS param) because it's a UX mode,
    // not a musical value. v0.11 — no dedicated button any more; the
    // setting lives inside the settings popup as a checkbox.
    bool             fineMode = false;
    void             applyPitchSlideFineMode();

    // Global MIX — horizontal slider above the Acts strip. Scales
    // every Act's wet/dry mix so you can pull back the entire effect
    // without touching individual per-Act knobs.
    juce::Slider globalMixSlider;
    juce::Label  globalMixLabel;
    std::unique_ptr<SliderAttachment> globalMixAttachment;
    bool         globalMixLockDirty = false;   // p-lock active on this step?
    void         refreshGlobalMixHighlight();   // update track colour for mod/plock state

    // Act I/O — one button that opens a popup menu with
    // Export Current/All + Import Current/All. FileChooser is held as
    // a member so its async callback doesn't fire into a dead object.
    juce::TextButton ioButton { "ACTS ACTS" };
    juce::TextButton presetPrevButton { "<" };
    juce::TextButton presetNextButton { ">" };
    juce::Label      presetNameLabel;
    // BUG 1 fix: removed lastAppliedPresetIdx; now read directly from processorRef via getScenePresetIdx()
    std::unique_ptr<juce::FileChooser> activeChooser;
    void showActIoMenu();
    void applyPresetDelta (int delta);  // -1 = prev, +1 = next
    void updatePresetNameLabel();  // updates label text based on processorRef.getScenePresetIdx()
    void exportCurrentAct();
    void exportAllActs();
    void importToCurrentAct();
    void importAllActs();

    // FREEZE button + its mode toggle (Toggle vs Momentary). In
    // Momentary mode we detach the APVTS attachment and let
    // MomentaryButton drive processorRef.setMomentaryFreeze on
    // mouseDown/mouseUp; in Toggle mode it's a plain APVTS-backed
    // latching button. v0.8 — the FRZ MODE button is gone from the
    // title strip; freeze mode now lives in the settings popup.
    MomentaryButton  freezeButton;
    std::unique_ptr<ButtonAttachment> freezeAttachment;
    bool             freezeMomentary = false;
    void             applyFreezeMode();

    // v0.12 — "SEQ" on its own was ambiguous (sounded like an effect
    // name). Re-labelled to "SEQ ON" so the toggle action is obvious.
    juce::TextButton seqOnButton  { "SEQ ON" };

    // v0.8 — settings (gear) + help (?) buttons in the title strip.
    // Settings opens a PopupMenu with freeze mode / waveform visibility /
    // UI scale. Help launches a CallOutBox with a crib sheet of all the
    // modifier click shortcuts so the user can learn them without
    // guessing.
    juce::TextButton bypassButton   { "BYPASS" };  // v0.24 — master bypass
    juce::TextButton modButton      { "MOD" };     // LFO modulation page toggle
    juce::TextButton settingsButton { "SET" };
    juce::TextButton helpButton     { "?" };
    bool             modPageVisible = false;
    std::unique_ptr<ModPage> modPage;
    // v0.41 — Settings is now a full-screen page (not a popup) so it
    // matches the MOD pattern. SETTINGS button toggles visibility; Alt-
    // click still opens the hidden debug menu via showDebugMenu().
    bool             settingsPageVisible = false;
    std::unique_ptr<loopsab::SettingsPage> settingsPage;
    void             toggleSettingsPage();
    void             showSettingsMenu();   // legacy popup, kept around for now
    void             showDebugMenu();
    void             showHelpCallout();

    // v0.8 — UI scale + waveform visibility. Both mirror persisted
    // state on the processor. applyUiScale resizes the component and
    // installs an AffineTransform::scale so the editor draws at 1:1
    // resolution while the outer window picks a larger size.
    // v0.12 — the editor is now freely resizable (drag the bottom-
    // right corner). The fixed-preset menu is gone; UI scale lives
    // only as the persisted float, derived from the current size.
    float uiScale         = 1.0f;
    bool  waveformVisible = true;
    void  applyUiScale (float s);

    // v0.12 — design size used by the layout code. The editor always
    // thinks in these coordinates; an AffineTransform scales them up
    // to whatever the host window currently is. Constrainer locks the
    // aspect ratio so the user can drag freely without squashing.
    static constexpr int kDesignW = 1100;
    static constexpr int kDesignH = 876;
    juce::ComponentBoundsConstrainer resizeConstrainer;
    // v0.13 — the content container owns the AffineTransform so we no
    // longer need a reentrantResize guard; editor::resized() just
    // positions innerContent and calls layoutContent() once.
    void paintContent  (juce::Graphics& g);
    void layoutContent ();

    // v0.8 — helper for the per-step right-click context menu. Shows
    // stamp/clear/ratio submenu/grab submenu/freeze toggle/edit locks.
    void showStepCellMenu (int stepIdx);

    // v0.30 — step clipboard for copy/paste. Stores all per-step data
    // (scene, ratio, freeze, ratchet, mute, p-locks) from one step.
    struct StepClipboard
    {
        bool  valid      = false;
        int   scene      = -1;
        int   ratio      = 0;
        bool  freeze     = false;
        int   ratchet    = 1;
        bool  mute       = false;
        float locks[LoopSaboteurProcessor::kNumLockableParams];

        StepClipboard() { std::fill (std::begin (locks), std::end (locks), std::numeric_limits<float>::quiet_NaN()); }
    };
    StepClipboard stepClipboard;
    void copyStep  (int stepIdx);
    void pasteStep (int stepIdx);

    // v0.31 — multi-step selection and clipboard for range copy/paste
    std::vector<StepClipboard> multiStepClipboard;  // stores clipboard data for multiple steps
    int  selectionStart = -1;  // first selected step index (-1 = no selection)
    int  selectionEnd   = -1;  // last selected step index (inclusive)

    bool hasSelection() const { return selectionStart >= 0 && selectionEnd >= 0; }
    void clearSelection();
    void updateSelectionHighlight();
    void copySelection();
    void pasteSelection (int startStep);
    void copyPage();
    void pastePage();

    // Scene slots — one ActButton per scene. v0.10: the separate STORE
    // button has been removed; knob changes auto-mirror to the selected
    // Act, and the slot button paints a dot to show populated state.
    std::array<ActButton, LoopSaboteurProcessor::kNumScenes> sceneButtons;

    // v0.10 — right-click on an Act slot. Opens a menu with "Reset to
    // defaults" (+ load-to-knobs shortcut if not already selected).
    void showActContextMenu (int sceneIdx);

    // Sequencer-header global buttons. PAGE now lives in the length
    // cluster (not the header row) and cycles through kNumPages =
    // A/B/C/D so the user can see it next to the step count.
    juce::TextButton  clearAllButton { "CLEAR" };
    juce::TextButton  undoButton     { "UNDO" };
    RightClickButton  randomizeButton;
    // v0.14 — four page buttons replace the single cycling button.
    // v0.31 — changed to RightClickButton to support page copy/paste menu.
    juce::Label pageLabel;
    std::array<RightClickButton, 4> pageButtons;
    int               currentPage = 0;
    void              showPageContextMenu();
    void              showRandomizeMenu();

    // Scale quantise dropdowns (wired to APVTS choice params).
    juce::ComboBox   scaleRootCombo;
    juce::ComboBox   scaleTypeCombo;
    juce::Label      scaleLabel;
    std::unique_ptr<ComboAttachment> scaleRootAttachment;
    std::unique_ptr<ComboAttachment> scaleTypeAttachment;

    // Step grid — allocate all kMaxSteps cells once and show only the
    // current page's 32 at a time by toggling visibility in resized().
    std::array<StepCell, LoopSaboteurProcessor::kMaxSteps> stepCells;

    // Waveform strip instance.
    WaveformStrip waveformStrip;

    // Length controls — pair of +/- buttons plus a numeric readout.
    juce::TextButton lengthMinusButton { "-" };
    juce::TextButton lengthPlusButton  { "+" };
    juce::Label      lengthValueLabel;
    juce::Label      lengthLabel;

    // ----- Lock Edit mode --------------------------------------------
    // When editLockStep >= 0 the user has shift-clicked a cell to
    // dedicate the knobs to writing into that step's p-lock table. We
    // detach every SliderAttachment and install onValueChange lambdas
    // that call processorRef.setStepLock. Exiting restores the APVTS
    // attachments so the knobs resume driving the Act values.
    int  editLockStep = -1;
    void enterLockEditMode (int stepIdx);
    void exitLockEditMode  ();

    // Iterate all 26 per-Act knobs alongside their APVTS parameter IDs.
    // Used by enter/exitLockEditMode to (re)attach attachments en masse.
    struct KnobBinding { LabeledKnob* k; const char* id; };
    std::vector<KnobBinding> allKnobs();

    // --- helpers -------------------------------------------------------
    void configureKnob (LabeledKnob& k,
                        const juce::String& caption,
                        const juce::String& paramId,
                        const juce::String& suffix = {});

    void refreshSceneButtons();
    void refreshStepCells();
    void refreshPlayhead();
    void refreshLengthDisplay();
    void refreshPageButton();
    void refreshKnobLockDirty();    // repaint labels for Lock Edit mode
    void refreshAccentColours();    // push Col::accent to all cached component colours
    void refreshLfoTargetMarkers(); // set "lfoTargeted" property on each knob (cyan pointer)
    // v0.38 — right-click on any knob pops a menu that assigns the
    // currently-selected Act's MOD 1/2/3/4 to that knob's lockable slot,
    // or clears all MODs pointing at this slot on the current Act. The
    // help callout promised this worked since v0.33; now it actually does.
    void showKnobModAssignMenu (LabeledKnob& k);
    void applyLengthDelta (int delta);
    void pageChanged();

    void timerCallback() override;

    // Cached last-seen playing step so we only repaint when it changes.
    int lastPlayingStep = -1;

    // v0.9 — cached last-seen sequencer length. AUTO-follow mode can
    // change seqLength from the audio/message thread without the editor
    // knowing, so the timer polls for it and refreshes the cell "active"
    // flags and the length readout when it drifts.
    int lastSeenLength = -1;

    // v0.34 — nav-bar hover tinting. Each nav button gets a signature
    // colour (Freeze=blue, Bypass=red, Seq=green, Mod=cyan, etc.). On
    // mouse-enter we tint the text colour for the "off" state to that
    // colour; on mouse-exit we restore a dim grey. The "on" state is
    // already wired with textColourOnId so engagement persists the colour.
    struct NavHoverListener : public juce::MouseListener
    {
        juce::Button*   button       { nullptr };
        juce::Colour    hoverColour  { 0xffffffff };
        juce::Colour    normalColour { 0xff999999 };
        NavHoverListener (juce::Button& b, juce::Colour hover, juce::Colour normal)
            : button (&b), hoverColour (hover), normalColour (normal)
        {
            button->setColour (juce::TextButton::textColourOffId, normalColour);
            button->addMouseListener (this, false);
        }
        ~NavHoverListener() override
        {
            if (button != nullptr) button->removeMouseListener (this);
        }
        void mouseEnter (const juce::MouseEvent&) override
        {
            if (button != nullptr)
                button->setColour (juce::TextButton::textColourOffId, hoverColour);
        }
        void mouseExit (const juce::MouseEvent&) override
        {
            if (button != nullptr)
                button->setColour (juce::TextButton::textColourOffId, normalColour);
        }
    };
    std::vector<std::unique_ptr<NavHoverListener>> navHoverListeners;

    // Easter egg: "LOOP PACIFIST" — fades in over ~10 s when both
    // CHANCE and MIX are at exactly 0%.
    float pacifistFade = 0.0f;   // 0 = SABOTEUR, 1 = PACIFIST

    // Easter egg: triple-click title → mirror UI for 3 s.
    int    titleClickCount   = 0;
    double lastTitleClickMs  = 0.0;
    bool   mirrorActive      = false;
    double mirrorEndMs       = 0.0;

    // v0.42.5 — Easter egg: double-click the word "RATCHET" in the footer
    // legend to release a rat across the editor. A keyboard trigger was
    // tried first (typing R-A-T) but Logic's R=record stole the keystrokes.
    // The 'rat' hiding inside 'ratchet' is the pun; the legend text is
    // already on screen so the hit-region bounds are captured in the paint
    // lambda that lays out the legend row.
    juce::Rectangle<int> ratchetLegendBounds;

    // v0.42.5 — Act copy/paste clipboard. Stores a serialised Act
    // snapshot (knobs + MODs + Engine bundle) so a user can copy one
    // Act slot's state and paste it over another. Uses the existing
    // serializeAct / loadActFromXml plumbing already in the processor
    // — same XML shape as the `.lpsbact` preset file format, so a
    // single implementation covers both code paths.
    std::unique_ptr<juce::XmlElement> actClipboard;
    int actClipboardSourceIdx = -1;   // the slot it was copied from, for the menu label

    // Easter egg: triple-click "COLOUR" → swap orange/yellow palette.
    int    colourClickCount  = 0;
    double lastColourClickMs = 0.0;
    bool   colourSwapped     = false;
    // v0.41 — true when the swap was triggered by a debug-menu force; the
    // timer auto-reverts when the force window elapses.
    bool   colourSwappedByForce = false;

    // Easter egg: swing at exactly 54% → "DILLA" label for 2 s.
    bool   dillaActive       = false;
    double dillaEndMs        = 0.0;

    // Easter egg: "AMEN OF SABOTAGE" when amen break pattern detected.
    bool   amenActive        = false;

    // Easter egg: producer tag popup.
    double producerTagEndMs  = 0.0;
    bool   producerTagActive = false;

    // Easter egg: BITS at 8 → CRUSH label becomes "PAULA" for 2 s.
    bool   paulaActive       = false;
    double paulaEndMs        = 0.0;

    // Easter egg: all active steps muted → Guru Meditation.
    bool   guruActive        = false;
    double guruEndMs         = 0.0;
    bool   guruFlash         = false;  // toggles for blink effect

    // v0.41 — debug-menu test toggles. Each entry below corresponds to one
    // egg; setting eggForceUntilMs[idx] in the future forces that egg's
    // visual state to "on" until the timestamp passes (15 s by default).
    // Lets us QA each visual without having to set up its natural trigger.
    enum EggId
    {
        EggPacifist = 0,
        EggMirror,
        EggColourSwap,
        EggDilla,
        EggProducerTag,
        EggAmen,
        EggPaula,
        EggGuru,
        Egg303,
        Egg606,
        Egg808,
        EggRat,
        NumEggs
    };
    juce::int64 eggForceUntilMs[NumEggs] = {};

    bool isEggForced (int e) const noexcept
    {
        return e >= 0 && e < NumEggs
            && juce::Time::currentTimeMillis() < eggForceUntilMs[e];
    }
    void forceEggOn (int e);

    // v0.40 — "Designed for X" Engine-recall badge. Bounds are sized in
    // resized(); paintContent draws the pill only while the processor's
    // designedFor* values are set; mouseDown applies or dismisses; the
    // 30Hz timer auto-dismisses after kDesignedForTimeoutMs.
    juce::Rectangle<int> designedForBadgeBounds;
    juce::int64 lastSeenDesignedForLoadedAt = 0;
    static constexpr juce::int64 kDesignedForTimeoutMs = 12000;

    // v0.42.3 — Easter egg: a little rat scurries across the bottom of
    // the editor. Triggered by typing "RAT" in quick succession while
    // the editor has focus (see keyPressed). Component is transparent
    // and non-interactive — it sits on top of everything else and
    // destroys itself (via the timer) when the rat runs off the right
    // edge. Deliberately un-skinned: stays as a shape-drawing so it
    // survives theme / L&F changes without maintenance.
    class RatRun  : public juce::Component,
                    private juce::Timer
    {
    public:
        RatRun();
        ~RatRun() override;
        std::function<void()> onFinished;
        void launch (juce::Rectangle<int> parentBounds);
        void paint (juce::Graphics& g) override;
    private:
        void timerCallback() override;
        float xPos   = -80.0f;
        float vx     = 260.0f;    // pixels / second, sensible pace
        int   frame  = 0;
        juce::int64 lastTickMs = 0;
    };

    std::unique_ptr<RatRun> ratRun;
    // Rolling buffer of the last few printable keys the user pressed,
    // with timestamps. Used only for the "RAT" easter egg trigger;
    // intentionally tiny so we never cost anything real.
    struct KeyStroke { char ch; juce::int64 tMs; };
    std::array<KeyStroke, 4> recentKeys { };
    int recentKeysHead = 0;
    void pushRecentKey (char ch);
    bool recentKeysSpell (const char* word, juce::int64 withinMs) const;
    void spawnRatRun();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LoopSaboteurEditor)
};
