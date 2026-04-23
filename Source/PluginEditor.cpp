#include "PluginEditor.h"
#include <algorithm>  // v0.38 — std::sort for alphabetised MOD target dropdown
#include <vector>     // v0.38 — temp sort buffer

// ============================================================================
//  Full editor with knob grid, scene bank, waveform strip, and step
//  sequencer.
//
//  v0.7: p-locks, ratio probability, per-step freeze, per-step grab
//        offset, scale quantise, 64 steps, clear-all, SABOTAGE, momentary
//        freeze toggle.
//  v0.8: 128 steps (A/B/C/D pages), right-click context menu on every
//        cell, grab offset painted per-cell, knob label highlights in
//        Lock Edit mode, settings gear + help callout in the title
//        strip, UI scale preset, waveform can be hidden, editor prefs
//        persist inside the processor state XML.
//  v0.9: ratio counter fix (cache key is the monotonic seq step, not
//        the wrapped index), p-locks-only steps fire via live knob
//        values as the base, SCAT renamed to SABOTAGE with a clearer
//        tooltip, grab-offset rendered as "←N" so it reads as "N slices
//        back", lockDirty label colour is now yellow instead of the
//        accent orange, AUTO-follow length mode in the settings popup
//        that snaps seqLength to the smallest 32-step page boundary
//        containing every populated step.
// ============================================================================

namespace
{
    // Shared colour palette. Extracted as a tiny local namespace so if
    // we finally break Theme.h out into its own file, the renames are
    // all in one place.
    // v0.28 — K5 Stencil palette. High-contrast black/white/orange.
    namespace Col
    {
        const auto bg          = juce::Colour::fromRGB (24,  24,  24);   // near-black
        const auto panel       = juce::Colour::fromRGB (26,  26,  26);   // panels
        const auto panelDeep   = juce::Colour::fromRGB (14,  14,  14);   // deep inset
        /*mutable*/ auto accent      = juce::Colour::fromRGB (255, 90,  60);   // "bad idea" orange
        /*mutable*/ auto accentDim   = juce::Colour::fromRGB (120, 45,  30);
        /*mutable*/ auto lockDirty   = juce::Colour::fromRGB (255, 204, 0);    // p-lock yellow
        const auto textBright  = juce::Colour::fromRGB (238, 238, 238);  // near-white
        const auto textDim     = juce::Colour::fromRGB (140, 140, 150);
        const auto freezeOn    = juce::Colour::fromRGB (74,  158, 255);  // blue
        const auto seqOn       = juce::Colour::fromRGB (51,  204, 102);  // green
        const auto stepEmpty   = juce::Colour::fromRGB (26,  26,  26);   // matches panel
        const auto stepInactive= juce::Colour::fromRGB (17,  17,  17);
        /*mutable*/ auto stepAccent       = juce::Colour::fromRGB (42,  42,  48);  // accent bg (lighter)
        /*mutable*/ auto stepAccentStripe = juce::Colour::fromRGB (255, 90,  60);
        /*mutable*/ auto stepStamp        = juce::Colour::fromRGB (255, 90,  60);  // stamped = orange
        /*mutable*/ auto stepStampAccent  = juce::Colour::fromRGB (255, 90,  60);  // same
        const auto modCyan     = juce::Colour::fromRGB (0,   204, 255);  // LFO mod indicator
        const auto playing     = juce::Colour::fromRGB (238, 238, 238);  // playhead = white

        // Original values for easter egg colour swap.
        const auto accentOrig      = juce::Colour::fromRGB (255, 90,  60);
        const auto accentDimOrig   = juce::Colour::fromRGB (120, 45,  30);
        const auto lockDirtyOrig   = juce::Colour::fromRGB (255, 204, 0);

        inline void swapToYellow()
        {
            accent          = lockDirtyOrig;
            accentDim       = juce::Colour::fromRGB (120, 96,  0);
            lockDirty       = accentOrig;
            stepAccentStripe = lockDirtyOrig;
            stepStamp        = lockDirtyOrig;
            stepStampAccent  = lockDirtyOrig;
        }
        inline void swapToNormal()
        {
            accent          = accentOrig;
            accentDim       = accentDimOrig;
            lockDirty       = lockDirtyOrig;
            stepAccentStripe = accentOrig;
            stepStamp        = accentOrig;
            stepStampAccent  = accentOrig;
        }
    }
}

// ============================================================================
//  StepCell — custom component for a single sequencer grid cell
// ============================================================================
// v0.41 — definitions for the birthday force-override timestamps.
// Default 0 means "no force" (any current time will be greater).
juce::int64 LoopSaboteurEditor::StepCell::force303UntilMs = 0;
juce::int64 LoopSaboteurEditor::StepCell::force606UntilMs = 0;
juce::int64 LoopSaboteurEditor::StepCell::force808UntilMs = 0;

void LoopSaboteurEditor::StepCell::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);

    // v0.30 — Two-zone step cell:
    //   TOP: Act area — changes colour when an act is assigned / playing.
    //   BOTTOM: Info bar — always dark, houses indicator dots + step number.
    constexpr float infoH = 22.0f;
    auto topZone    = r.withTrimmedBottom (infoH);
    auto bottomZone = r.withTrimmedTop (r.getHeight() - infoH);

    // --- TOP ZONE ---
    // v0.30 — accented steps with an act are inverted: dark bg, orange letter.
    const bool accentedAct = active && accentBeat && (scene >= 0) && ! playing;

    auto topFill = ! active     ? Col::stepInactive
                 : playing      ? Col::playing
                 : accentedAct  ? juce::Colour (0xff1a1a1a)   // inverted: dark bg
                 : (scene >= 0) ? Col::stepStamp
                                : (accentBeat ? Col::stepAccent
                                              : Col::stepEmpty);
    g.setColour (topFill);
    g.fillRect (topZone);

    // Accent stripe — 3px orange top edge on non-act accent beats.
    if (active && accentBeat && ! playing && scene < 0)
    {
        g.setColour (Col::stepAccentStripe.withAlpha (0.5f));
        g.fillRect (topZone.withHeight (3.0f));
    }

    // --- BOTTOM ZONE — always dark ---
    g.setColour (active ? juce::Colour (0xff161619) : Col::stepInactive);
    g.fillRect (bottomZone);

    // Border — selection highlight (cyan) takes precedence, then lock-edit (yellow),
    // then normal thin dark edge on all steps.
    if (selected)
    {
        g.setColour (juce::Colour (0xff00CCFF));  // cyan selection highlight
        g.drawRect (r, 2.0f);
    }
    else if (editing)
    {
        g.setColour (Col::lockDirty);
        g.drawRect (r, 2.0f);
    }
    else if (active && ! playing)
    {
        g.setColour (juce::Colour (0xff333333));
        g.drawRect (r, 1.0f);
    }

    if (! active)
        return;

    // --- TOP ZONE CONTENT: Act letter or dash ---
    if (scene >= 0)
    {
        // Accented act = orange letter on dark bg (inverted). Normal = dark on orange.
        g.setColour (accentedAct ? Col::accent : Col::bg);
        g.setFont (juce::Font (juce::FontOptions ("Impact", 28.0f, juce::Font::plain)));
        const juce::String letter (juce::String::charToString ((juce::juce_wchar) ('A' + scene)));
        g.drawText (letter, topZone.toNearestInt(), juce::Justification::centred);
    }
    else
    {
        g.setColour (juce::Colour (0xff444444));
        g.setFont (juce::Font (juce::FontOptions ("Impact", 20.0f, juce::Font::plain)));
        g.drawText (juce::String::fromUTF8 ("\xe2\x80\x93"),
                    topZone.toNearestInt(), juce::Justification::centred);
    }

    // --- BOTTOM ZONE CONTENT: dots + step number ---
    // Dots left-to-right: freeze(blue), probability(green),
    // ratchet(white), p-lock(yellow). Step number on the right.
    const float dotSize = 6.0f;
    const float dotY    = bottomZone.getCentreY() - dotSize * 0.5f;
    float dotX = bottomZone.getX() + 3.0f;
    const float dotGap = 2.0f;

    if (freezeHold)
    {
        g.setColour (Col::freezeOn);
        g.fillEllipse (dotX, dotY, dotSize, dotSize);
        dotX += dotSize + dotGap;
    }
    if (ratio > 0)
    {
        g.setColour (Col::seqOn);
        g.fillEllipse (dotX, dotY, dotSize, dotSize);
        dotX += dotSize + dotGap;
    }
    if (ratchet > 1)
    {
        g.setColour (Col::textBright);
        g.fillEllipse (dotX, dotY, dotSize, dotSize);
        dotX += dotSize + dotGap;
    }
    if (hasLocks)
    {
        g.setColour (Col::lockDirty);
        g.fillEllipse (dotX, dotY, dotSize, dotSize);
    }

    // Step number — right side of info bar.
    g.setColour (juce::Colour (0xff999999));
    g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    g.drawText (juce::String (stepIndex + 1),
                bottomZone.toNearestInt().withTrimmedRight (3),
                juce::Justification::centredRight);

    // Mute overlay — v0.39: darken the top (Act) zone and slash
    // diagonally across it, but leave the bottom info bar brighter so
    // the probability / freeze / ratchet / p-lock indicators stay
    // readable. Probability still advances on muted steps (see
    // PluginProcessor.cpp v0.39 fire logic) and PREV / !PREV on the
    // next step look at what THIS step would have fired — so the
    // green probability dot here is still meaningful and needs to be
    // visible at a glance. A thin diagonal tick is extended through
    // the bottom zone too so the "muted" read is unambiguous without
    // clobbering the indicator dots.
    if (muted)
    {
        // Darken only the top zone.
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRect (topZone);

        // Soft dim on the bottom zone — enough to read as muted,
        // not so much that the probability dot disappears.
        g.setColour (juce::Colours::black.withAlpha (0.22f));
        g.fillRect (bottomZone);

        // Heavy slash across the top zone.
        const float m = 4.0f;
        g.setColour (Col::textBright.withAlpha (0.9f));
        const juce::Line<float> slashTop (topZone.getX()     + m, topZone.getBottom() - m,
                                          topZone.getRight() - m, topZone.getY()      + m);
        g.drawLine (slashTop, 2.0f);

        // Lighter slash through the bottom zone, so the visual
        // strike reads edge-to-edge without bisecting the dots.
        g.setColour (Col::textBright.withAlpha (0.35f));
        const juce::Line<float> slashBot (bottomZone.getX()     + m, bottomZone.getBottom() - m,
                                          bottomZone.getRight() - m, bottomZone.getY()      + m);
        g.drawLine (slashBot, 1.0f);
    }

    // v0.42.4 — The 303/606/808 "birthday" egg previously drew a tiny
    // coloured ring on step 1. Steve said it made no sense there and
    // asked for it to move into the header. The ring is gone; on 3/3,
    // 6/6 or 8/8 (or when forced via the debug menu) the header now
    // shows "<3 303" etc. next to "/ AMFAS" in the machine's signature
    // colour. See the header paint block in paintContent().
}

juce::String LoopSaboteurEditor::StepCell::getTooltip()
{
    // Build a short but informative tip describing everything this
    // particular cell currently does, so hovering a mystery step tells
    // you exactly what's going on without a right-click.
    juce::String tip;
    tip << "Step " << juce::String (stepIndex + 1);

    if (! active)
    {
        tip << " (inactive - beyond pattern length)";
        return tip;
    }

    // v0.12 — bullets were rendering as mojibake on some machines,
    // so every bullet and dash is plain ASCII now.
    if (scene >= 0)
        tip << " | Act "
            << juce::String::charToString ((juce::juce_wchar) ('A' + scene));
    else
        tip << " | empty";

    if (ratio > 0)
        tip << " | probability "
            << LoopSaboteurProcessor::ratioShortLabel (ratio);

    if (freezeHold)
        tip << " | freeze hold";

    if (hasLocks)
        tip << " | p-locks";

    if (ratchet > 1)
    {
        tip << "\nRatchet: " << juce::String (ratchet)
            << "x fires per step";
    }

    if (muted)
    {
        // v0.39 — probability bookkeeping now runs on muted steps too,
        // so clarify: the step is silent but the dice still roll.
        // That matters for IF PREV / !PREV chains and for keeping A:B
        // ratio phase consistent when unmuting mid-performance.
        if (ratio > 0)
            tip << "\nMUTED (silent - probability still rolls)";
        else
            tip << "\nMUTED (silent)";
    }

    tip << "\nLeft-click = stamp | right-click = menu";
    return tip;
}

void LoopSaboteurEditor::StepCell::mouseDown (const juce::MouseEvent& e)
{
    if (! active) return;

    // Modifier routing. v0.8: right/ctrl click now opens a full popup
    // context menu instead of just wiping — it's the discoverable
    // alternative to the modifier-click shortcuts. Shift enters
    // Lock Edit mode. Cmd toggles freeze-hold. Alt cycles ratio
    // probability. Plain left click stamps the currently-selected Act.
    // v0.31: Ctrl+Shift+click for range selection.
    if (e.mods.isCtrlDown() && e.mods.isShiftDown())
    {
        if (onSelectRange) onSelectRange();
    }
    else if (e.mods.isRightButtonDown() || e.mods.isCtrlDown())
    {
        if (onContextMenu) onContextMenu();
    }
    else if (e.mods.isShiftDown())
    {
        if (onShiftClick) onShiftClick();
    }
    else if (e.mods.isCommandDown())
    {
        if (onToggleFreeze) onToggleFreeze();
    }
    else if (e.mods.isAltDown())
    {
        if (onCycleRatio) onCycleRatio();
    }
    else
    {
        if (onSet) onSet();
    }
}

// ============================================================================
//  WaveformStrip — reads the processor's peak ring and paints a
//  scrolling bar history. The static playhead lives on the right edge
//  (newest sample). v0.8 polled by the editor's 60 Hz timer with
//  halved bucket size on the processor side for a much smoother scroll.
// ============================================================================
void LoopSaboteurEditor::WaveformStrip::paint (juce::Graphics& g)
{
    g.fillAll (Col::panelDeep);
    if (proc == nullptr) return;

    auto r = getLocalBounds().toFloat();
    const int w = (int) r.getWidth();
    if (w <= 0) return;

    const int ringSize = LoopSaboteurProcessor::kPeakRingSize;
    const int writePos = proc->getPeakWritePos();
    const float centerY = r.getCentreY();
    const float halfH   = r.getHeight() * 0.5f - 1.0f;
    const float height  = r.getHeight();

    // v0.42.5 — Proper bipolar waveform silhouette. The earlier "line
    // trace" version only used +peak as y, which collapsed the waveform
    // into a single squiggle at the top half of the strip — user
    // testing flagged it as looking flat / like just a line. The peak
    // ring stores magnitude-per-bucket (always >= 0), so we render
    // that as a symmetric envelope mirrored around the centre line:
    //   top    = centreY - peak * halfH
    //   bottom = centreY + peak * halfH
    // We build ONE closed path (top-edge left→right, then bottom-edge
    // right→left) and fill it — that gives the classic DAW waveform
    // blob shape. A thin outline on top of the fill keeps the edge
    // crisp so quiet passages still read against the panel background.

    // Faint horizontal grid lines at 25% and 75%.
    g.setColour (juce::Colour (0x08ffffff));
    g.drawHorizontalLine ((int) (height * 0.25f), 0.0f, (float) w);
    g.drawHorizontalLine ((int) (height * 0.75f), 0.0f, (float) w);
    // Centre line.
    g.setColour (juce::Colour (0x18ffffff));
    g.drawHorizontalLine ((int) centerY, 0.0f, (float) w);

    juce::Path waveFill;
    juce::Path waveTopOutline;
    juce::Path waveBotOutline;

    // Walk the ring once and capture each column's peak so we can draw
    // the top edge forwards and bottom edge in reverse without a
    // second pass.
    std::vector<float> peaks;
    peaks.reserve ((size_t) w);
    for (int x = 0; x < w; ++x)
    {
        const int idxFromEnd = (w - 1) - x;
        int ringIdx = (writePos - 1 - idxFromEnd) % ringSize;
        if (ringIdx < 0) ringIdx += ringSize;
        peaks.push_back (juce::jmin (proc->getPeak (ringIdx), 1.0f));
    }

    // Top edge (y decreases as peak increases), left to right.
    waveFill.startNewSubPath (0.0f, centerY - peaks[0] * halfH);
    waveTopOutline.startNewSubPath (0.0f, centerY - peaks[0] * halfH);
    for (int x = 1; x < w; ++x)
    {
        const float y = centerY - peaks[(size_t) x] * halfH;
        waveFill.lineTo ((float) x, y);
        waveTopOutline.lineTo ((float) x, y);
    }

    // Bottom edge (mirror), right to left, closing the fill shape.
    waveBotOutline.startNewSubPath ((float) (w - 1),
                                    centerY + peaks[(size_t) (w - 1)] * halfH);
    for (int x = w - 1; x >= 0; --x)
    {
        const float y = centerY + peaks[(size_t) x] * halfH;
        waveFill.lineTo ((float) x, y);
        if (x < w - 1)
            waveBotOutline.lineTo ((float) x, y);
    }
    waveFill.closeSubPath();

    // Filled body — brighter than the old 7% wash because the new
    // silhouette is the primary read, not just an accent under a line.
    g.setColour (Col::accent.withAlpha (0.32f));
    g.fillPath (waveFill);

    // Crisp edges on top and bottom so the shape stays legible against
    // busy backgrounds and quiet samples don't get swallowed.
    g.setColour (Col::accent);
    g.strokePath (waveTopOutline, juce::PathStrokeType (1.2f));
    g.strokePath (waveBotOutline, juce::PathStrokeType (1.2f));

    // v0.30+ — slice position highlight. When a voice is active, draw a
    // semi-transparent accent region showing where the slice is reading
    // from in the buffer. The region is mapped from buffer-sample-space
    // to waveform-pixel-space via the peak ring bucket size.
    {
        const auto snap = proc->getVoiceSnapshot();
        if (snap.active && snap.totalSamples > 0)
        {
            const int cbSize   = proc->getCircularBufferSize();
            const int cbWrite  = proc->getCircularWritePos();
            const int bucketSz = LoopSaboteurProcessor::kPeakBucketSamples;

            if (cbSize > 0)
            {
                // How far behind the write head is the current read pos?
                int readOffset = cbWrite - (int) snap.readPos;
                if (readOffset < 0) readOffset += cbSize;

                // Slice length in samples.
                const int sliceLen = snap.totalSamples;

                // Convert to pixels. The rightmost pixel = write head.
                // Each pixel = one peak bucket = bucketSz samples.
                const float pixPerSample = (float) w / (float) (ringSize * bucketSz);
                const float sliceStartPx = (float) (w - 1) - (float) readOffset * pixPerSample;
                const float sliceWidthPx = (float) sliceLen * pixPerSample;
                const float sliceEndPx   = sliceStartPx + sliceWidthPx;

                // Modulate alpha by fadeGain for pulsing/breathing effect as voice fades.
                // v0.38 — bumped baseAlpha from 0.12 to 0.28 per Steve's
                // feedback that the loopback highlight was too faint to
                // read at a glance. Edge line also nudged up so the
                // trailing read-head stays visible on a busy waveform.
                const float baseAlpha = 0.28f;
                const float fillAlpha = baseAlpha * snap.fadeGain;
                const float edgeAlpha = 0.9f * snap.fadeGain;

                // If the slice start is off the left edge, draw a left-pointing indicator.
                if (sliceStartPx < 0.0f)
                {
                    // Draw a small left-pointing triangle at the left edge.
                    // This indicates "the slice is playing but further back than visible".
                    const float arrowSize = 8.0f;
                    const float arrowX = 2.0f;
                    const float arrowMidY = centerY;

                    juce::Path leftArrow;
                    leftArrow.startNewSubPath (arrowX + arrowSize, arrowMidY - arrowSize * 0.6f);
                    leftArrow.lineTo (arrowX, arrowMidY);
                    leftArrow.lineTo (arrowX + arrowSize, arrowMidY + arrowSize * 0.6f);
                    leftArrow.closeSubPath();

                    g.setColour (Col::playing.withAlpha (0.5f * snap.fadeGain));
                    g.fillPath (leftArrow);
                }

                // Draw the visible slice region.
                const float x0 = juce::jmax (0.0f, sliceStartPx);
                const float x1 = juce::jmin ((float) w, sliceEndPx);

                if (x1 > x0)
                {
                    // v0.35 — switched from accent (orange) to playing
                    // (white), matching the playhead and active-step
                    // styling. More visible against the orange waveform.
                    g.setColour (Col::playing.withAlpha (fillAlpha));
                    g.fillRect (x0, 0.0f, x1 - x0, height);

                    // Bright leading-edge line at the voice read position (trailing edge of slice).
                    // This is the actual read head position.
                    g.setColour (Col::playing.withAlpha (edgeAlpha));
                    g.drawLine (x1, 0.0f, x1, height, 1.5f);
                }
            }
        }
    }

    // v0.5.0 — slice output waveform overlay (when in Slice view mode).
    if (showSliceView)
    {
        const int sliceRingSize = LoopSaboteurProcessor::kSlicePeakRingSize;
        const int sliceWritePos = proc->getSlicePeakWritePos();

        juce::Path sliceFill;
        bool sfirst = true;
        for (int x = 0; x < w; ++x)
        {
            const int idxFromEnd = (w - 1) - x;
            int sRingIdx = (sliceWritePos - 1 - idxFromEnd) % sliceRingSize;
            if (sRingIdx < 0) sRingIdx += sliceRingSize;
            const float sp = juce::jmin (proc->getSlicePeak (sRingIdx), 1.0f);
            const float yTop = centerY - sp * halfH;
            const float yBot = centerY + sp * halfH;
            if (sfirst) { sliceFill.startNewSubPath ((float) x, yTop); sfirst = false; }
            else        { sliceFill.lineTo ((float) x, yTop); }
        }
        // Bottom edge right→left
        for (int x = w - 1; x >= 0; --x)
        {
            const int idxFromEnd = (w - 1) - x;
            int sRingIdx = (sliceWritePos - 1 - idxFromEnd) % sliceRingSize;
            if (sRingIdx < 0) sRingIdx += sliceRingSize;
            const float sp = juce::jmin (proc->getSlicePeak (sRingIdx), 1.0f);
            sliceFill.lineTo ((float) x, centerY + sp * halfH);
        }
        sliceFill.closeSubPath();

        g.setColour (juce::Colour (0x5500ccff));  // cyan tint
        g.fillPath (sliceFill);

        // "SLICE" label
        g.setColour (juce::Colour (0xaa00ccff));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("SLICE", r.reduced (4), juce::Justification::topRight);
    }
    else
    {
        // "INPUT" label when toggled
        g.setColour (juce::Colour (0x44ffffff));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("INPUT", r.reduced (4), juce::Justification::topRight);
    }

    // Playhead line at the right edge.
    g.setColour (Col::playing);
    g.drawLine ((float) (w - 1), 0.0f, (float) (w - 1), height, 1.5f);
}

void LoopSaboteurEditor::WaveformStrip::mouseDown (const juce::MouseEvent&)
{
    showSliceView = ! showSliceView;
    repaint();
}

// ============================================================================
//  Editor ctor / dtor
// ============================================================================
LoopSaboteurEditor::LoopSaboteurEditor (LoopSaboteurProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    // v0.25 — 8 category-grouped knob panel + act bank + waveform strip +
    // sequencer + title/footer. Height is tuned so there is zero slack
    // below the step row — the footer sits right against the sequencer.
    //   46 (title) + 558 (knobs) + 24 (global mix) + 60 (acts) + 28 (wave) + 132 (seq) + 28 (foot) = 876
    // v0.8 — setSize is a "design resolution"; applyUiScale below
    // multiplies it and installs an AffineTransform so the editor
    // draws at 1:1 while the outer window picks the requested size.
    // v0.13 — children now live on innerContent so the editor itself
    // owns only that one child. The content container's internal size
    // stays pinned at the design resolution and a scale transform is
    // applied in resized() to match the host window.
    addAndMakeVisible (innerContent);
    innerContent.onPaint = [this] (juce::Graphics& g) { paintContent (g); };
    // v0.38 — category-panel tooltips. The lookup walks categoryPanels
    // and returns the tooltip string for whichever panel the current
    // mouse position is inside. Empty strings mean "fall through to
    // whatever else TooltipWindow would pick up (usually nothing, since
    // knobs sitting on top of us return their own tooltips first).
    innerContent.onGetTooltip = [this] () -> juce::String
    {
        const auto localPos = innerContent.getMouseXYRelative();
        for (const auto& panel : categoryPanels)
        {
            if (panel.tooltip.isNotEmpty() && panel.bounds.contains (localPos))
                return panel.tooltip;
        }
        return {};
    };
    innerContent.onMouseDown = [this] (const juce::MouseEvent& e)
    {
        const double now = juce::Time::getMillisecondCounterHiRes();

        // Easter egg: double-click on the title text → mirror UI for 3 s.
        auto titleHitArea = juce::Rectangle<int> (20, 0, 300, 46);
        if (titleHitArea.contains (e.getPosition()))
        {
            if (now - lastTitleClickMs > 500.0) titleClickCount = 0;
            lastTitleClickMs = now;
            ++titleClickCount;
            if (titleClickCount >= 2)
            {
                titleClickCount = 0;
                mirrorActive = true;
                mirrorEndMs  = now + 3000.0;
                resized();   // re-applies transform with mirror
            }
        }

        // Easter egg: double-click on "COLOUR" label → swap orange/yellow.
        if (categoryPanels[2].bounds.isEmpty() == false)
        {
            auto colourHit = juce::Rectangle<int> (
                categoryPanels[2].bounds.getX(),
                categoryPanels[2].bounds.getY(),
                150, 30);
            if (colourHit.contains (e.getPosition()))
            {
                if (now - lastColourClickMs > 500.0) colourClickCount = 0;
                lastColourClickMs = now;
                ++colourClickCount;
                if (colourClickCount >= 2)
                {
                    colourClickCount = 0;
                    colourSwapped = ! colourSwapped;
                    if (colourSwapped) Col::swapToYellow();
                    else               Col::swapToNormal();
                    refreshAccentColours();
                }
            }
        }

        // v0.42.5 — Easter egg: double-click the "RATCHET" word in the
        // footer legend releases a rat. Bounds are captured each paint
        // pass in paintContent() where the legend is drawn; the hit
        // region is just the text span, not the coloured dot.
        if (e.getNumberOfClicks() == 2
            && ! ratchetLegendBounds.isEmpty()
            && ratchetLegendBounds.contains (e.getPosition()))
        {
            spawnRatRun();  // no-op if a rat is already running
        }

        // Easter egg: right-click "/ AMFAS" → producer tag popup.
        if (e.mods.isRightButtonDown() || e.mods.isCtrlDown())
        {
            // AMFAS text sits to the right of the title. Hit the whole
            // version/AMFAS strip area generously.
            auto amfasHit = juce::Rectangle<int> (280, 0, 300, 46);
            if (amfasHit.contains (e.getPosition()))
            {
                producerTagActive = true;
                producerTagEndMs  = juce::Time::getMillisecondCounterHiRes() + 3000.0;
                innerContent.repaint();
            }
        }

    };
    setSize (960, 876);
    setWantsKeyboardFocus (true);  // v0.30 — Cmd+Z undo

    // v0.13 — lookahead is gone from the UI. Clear any legacy non-zero
    // value (e.g. state files written in v0.12) so the plugin reports
    // zero latency immediately and the waveform paints full-width.
    if (processorRef.getLookaheadMs() != 0)
        processorRef.setLookaheadMs (0);

    // --- knobs --------------------------------------------------------
    configureKnob (chance,    "CHANCE",     LoopSaboteurProcessor::kParamChance);
    // CHANCE: flanking style (transparent outline), white value arc, dark bubble text.
    chance.slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colours::transparentBlack);
    chance.slider.setColour (juce::Slider::rotarySliderFillColourId,    Col::textBright);
    chance.slider.setColour (juce::Slider::textBoxTextColourId,         juce::Colour (0xff181818));
    configureKnob (division,  "DIVISION",   LoopSaboteurProcessor::kParamDivision);
    configureKnob (lookback,  "LOOKBACK",   LoopSaboteurProcessor::kParamLookback);
    configureKnob (rate,      "RATE",       LoopSaboteurProcessor::kParamRate);
    configureKnob (judder,    "JUDDER",     LoopSaboteurProcessor::kParamJudder);
    configureKnob (judderDiv, "JUDDER DIV", LoopSaboteurProcessor::kParamJudderDiv);
    configureKnob (pitch,     "PITCH",      LoopSaboteurProcessor::kParamPitch, "st");
    configureKnob (slide,     "SLIDE",      LoopSaboteurProcessor::kParamSlide, "st");
    configureKnob (decay,     "DECAY",      LoopSaboteurProcessor::kParamDecay);
    configureKnob (reverse,   "REVERSE",    LoopSaboteurProcessor::kParamReverse);
    configureKnob (crunch,    "BITS",       LoopSaboteurProcessor::kParamCrunch);
    // v0.22 — override getTextFromValue via displayOverride so the
    // attachment can't silently revert to pctString.
    crunch.slider.displayOverride = [] (double v)
    {
        if (v < 0.001) return juce::String ("OFF");
        const int bits = 16 - (int) (v * 14.0);
        return juce::String (bits) + "-bit";
    };
    crunch.slider.updateText();

    configureKnob (crushRate, "RATE",       LoopSaboteurProcessor::kParamCrushRate);
    crushRate.slider.displayOverride = [this] (double v)
    {
        if (v < 0.001) return juce::String ("OFF");
        const double sr = processorRef.getSampleRate();
        if (sr > 0.0)
        {
            const int maxHold = juce::jmax (2, (int) (sr / 2000.0));
            const double t = v * v;
            const int hold = 1 + (int) (t * (double) (maxHold - 1));
            const double effective = sr / (double) hold;
            if (effective >= 1000.0)
                return juce::String (effective / 1000.0, 1) + "k";
            return juce::String ((int) effective) + "Hz";
        }
        return juce::String ("---");
    };
    crushRate.slider.updateText();
    configureKnob (mix,       "MIX",        LoopSaboteurProcessor::kParamMix);
    // MIX: flanking style (transparent outline), orange value arc, black value text.
    mix.slider.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colours::transparentBlack);
    mix.slider.setColour (juce::Slider::rotarySliderFillColourId,    Col::accent);
    mix.slider.setColour (juce::Slider::textBoxTextColourId,         juce::Colour (0xff181818));
    configureKnob (drive,     "DRIVE",      LoopSaboteurProcessor::kParamDrive);
    configureKnob (tone,      "TONE",       LoopSaboteurProcessor::kParamTone);
    // v0.24 — bipolar display: centre = "FLAT", below = "DARK x%", above = "BRIGHT x%"
    tone.slider.displayOverride = [] (double v)
    {
        if (v > 0.49 && v < 0.51) return juce::String ("FLAT");
        if (v < 0.5)
        {
            const int pct = (int) std::round ((0.5 - v) * 200.0);
            return juce::String ("DARK ") + juce::String (pct) + "%";
        }
        const int pct = (int) std::round ((v - 0.5) * 200.0);
        return juce::String ("BRIGHT ") + juce::String (pct) + "%";
    };
    tone.slider.updateText();
    configureKnob (feedback,  "FEEDBACK",   LoopSaboteurProcessor::kParamFeedback);
    configureKnob (tape,      "TAPE",       LoopSaboteurProcessor::kParamTape);
    configureKnob (ringMod,   "RING MOD",   LoopSaboteurProcessor::kParamRingMod);
    configureKnob (varispeed, "VARISPEED",  LoopSaboteurProcessor::kParamVarispeed);
    configureKnob (stereo,    "STEREO",     LoopSaboteurProcessor::kParamStereo);
    configureKnob (stretch,   "STRETCH",    LoopSaboteurProcessor::kParamStretch);

    // Mangle row (v0.6).
    configureKnob (shimmer,   "SHIMMER",    LoopSaboteurProcessor::kParamShimmer);
    configureKnob (res,       "RES",        LoopSaboteurProcessor::kParamRes);
    configureKnob (fold,      "FOLD",       LoopSaboteurProcessor::kParamFold);
    configureKnob (gate,      "SHAPE",      LoopSaboteurProcessor::kParamGate);
    configureKnob (smear,     "SMEAR",      LoopSaboteurProcessor::kParamSmear);
    configureKnob (stutter,   "STUTTER",    LoopSaboteurProcessor::kParamStutter);
    configureKnob (chaos,     "CHAOS",      LoopSaboteurProcessor::kParamChaos);

    // v0.14 — SEQ RATE is now a horizontal slider in the sequencer header
    // row (alongside Swing), freeing the controls strip for wider cells.

    // --- v0.13 — percentage display sweep ------------------------------
    // Steve wants every raw-0..1 knob reading as "0%..100%" on the
    // slider label, matching the parameter-level string functions now
    // set on the APVTS side. We apply the same tiny formatter lambda to
    // every knob in the list below so host automation, slider readout,
    // and the plugin header all agree. No " per hit" / " rev" suffixes
    // any more — they took up too much room and Steve prefers the
    // plain "NN%" form.
    auto pctFmt = [] (double v)
    {
        return juce::String ((int) std::round (v * 100.0)) + "%";
    };
    auto pctParse = [] (const juce::String& s)
    {
        return juce::jlimit (0.0, 1.0,
                             s.retainCharacters ("0123456789.").getDoubleValue() / 100.0);
    };
    LabeledKnob* pctKnobs[] = {
        &chance, &decay, &reverse, &crunch, &mix,
        &drive, &feedback, &tape, &ringMod, &varispeed, &stereo, &stretch,
        &shimmer, &res, &fold, &gate, &smear, &chaos
    };
    for (auto* k : pctKnobs)
    {
        k->slider.textFromValueFunction  = pctFmt;
        k->slider.valueFromTextFunction  = pctParse;
        k->slider.updateText();
    }

    // PITCH/SLIDE display: integer semitones when whole, two-decimal
    // when fractional, with a signed prefix. We deliberately DON'T
    // append " st" here — configureKnob already sets " st" as the
    // slider's text value suffix, so returning it from the lambda
    // produced the "0 st st" double-suffix bug spotted in v0.5.
    auto semitoneFmt = [] (double v)
    {
        const double rounded = std::round (v);
        const bool whole = std::abs (v - rounded) < 0.005;
        const juce::String sign = v > 0.0 ? "+" : "";
        return whole
            ? sign + juce::String ((int) rounded)
            : sign + juce::String (v, 2);
    };
    pitch.slider.textFromValueFunction = semitoneFmt;
    slide.slider.textFromValueFunction = semitoneFmt;
    pitch.slider.updateText();
    slide.slider.updateText();

    // v0.14 — SEQ RATE combo in the sequencer header row.
    seqRateCombo.setColour (juce::ComboBox::backgroundColourId, Col::panelDeep);
    seqRateCombo.setColour (juce::ComboBox::textColourId,       Col::textBright);
    seqRateCombo.setColour (juce::ComboBox::outlineColourId,    Col::panel);
    innerContent.addAndMakeVisible (seqRateCombo);
    {
        auto pushChoices = [this] (juce::ComboBox& combo, const juce::String& paramId)
        {
            if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (
                               processorRef.apvts.getParameter (paramId)))
                combo.addItemList (cp->choices, 1);
        };
        pushChoices (seqRateCombo, LoopSaboteurProcessor::kParamSeqRate);
    }
    seqRateAttachment = std::make_unique<ComboAttachment> (
        processorRef.apvts, LoopSaboteurProcessor::kParamSeqRate, seqRateCombo);

    seqRateLabel.setText ("RATE", juce::dontSendNotification);
    seqRateLabel.setJustificationType (juce::Justification::centredRight);
    seqRateLabel.setColour (juce::Label::textColourId, Col::textDim);
    seqRateLabel.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    innerContent.addAndMakeVisible (seqRateLabel);

    // Horizontal swing slider — lives in the sequencer header row
    // where Scale used to be. Range 50–75% matching the drum-machine
    // convention: 50% = straight, 66.67% = triplet, 75% = dotted-eighth.
    swingSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    swingSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 18);
    swingSlider.setRange (0.5, 0.75, 0.001);
    swingSlider.setColour (juce::Slider::trackColourId,           Col::accent);
    swingSlider.setColour (juce::Slider::backgroundColourId,      Col::panelDeep);
    swingSlider.setColour (juce::Slider::thumbColourId,           Col::textBright);
    swingSlider.setColour (juce::Slider::textBoxTextColourId,     Col::textBright);
    swingSlider.setColour (juce::Slider::textBoxBackgroundColourId, Col::panelDeep);
    swingSlider.setColour (juce::Slider::textBoxOutlineColourId,  juce::Colours::transparentBlack);
    swingSlider.textFromValueFunction = [] (double v)
    {
        return juce::String ((int) std::round (v * 100.0)) + "%";
    };
    swingSlider.valueFromTextFunction = [] (const juce::String& s)
    {
        return juce::jlimit (0.5, 0.75,
                             s.retainCharacters ("0123456789.").getDoubleValue() / 100.0);
    };
    swingSlider.updateText();
    innerContent.addAndMakeVisible (swingSlider);

    swingLabel.setText ("SWING", juce::dontSendNotification);
    swingLabel.setJustificationType (juce::Justification::centredRight);
    swingLabel.setColour (juce::Label::textColourId, Col::textDim);
    swingLabel.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    innerContent.addAndMakeVisible (swingLabel);

    swingAttachment = std::make_unique<SliderAttachment> (
        processorRef.apvts, LoopSaboteurProcessor::kParamSwing, swingSlider);

    // --- Global MIX slider -------------------------------------------
    // Horizontal slider above the Acts strip. 0–100%, default 100%.
    globalMixSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    globalMixSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 18);
    globalMixSlider.setRange (0.0, 1.0, 0.001);
    globalMixSlider.setColour (juce::Slider::trackColourId,           Col::accent);
    globalMixSlider.setColour (juce::Slider::backgroundColourId,      Col::panelDeep);
    globalMixSlider.setColour (juce::Slider::thumbColourId,           Col::textBright);
    globalMixSlider.setColour (juce::Slider::textBoxTextColourId,     Col::textBright);
    globalMixSlider.setColour (juce::Slider::textBoxBackgroundColourId, Col::panelDeep);
    globalMixSlider.setColour (juce::Slider::textBoxOutlineColourId,  juce::Colours::transparentBlack);
    globalMixSlider.textFromValueFunction = [] (double v)
    {
        return juce::String ((int) std::round (v * 100.0)) + "%";
    };
    globalMixSlider.valueFromTextFunction = [] (const juce::String& s)
    {
        return juce::jlimit (0.0, 1.0,
                             s.retainCharacters ("0123456789.").getDoubleValue() / 100.0);
    };
    globalMixSlider.updateText();
    innerContent.addAndMakeVisible (globalMixSlider);

    globalMixLabel.setText ("GLOBAL MIX", juce::dontSendNotification);
    globalMixLabel.setJustificationType (juce::Justification::centredRight);
    globalMixLabel.setColour (juce::Label::textColourId, Col::textDim);
    globalMixLabel.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    innerContent.addAndMakeVisible (globalMixLabel);

    globalMixAttachment = std::make_unique<SliderAttachment> (
        processorRef.apvts, LoopSaboteurProcessor::kParamGlobalMix, globalMixSlider);

    // --- FINE mode ----------------------------------------------------
    // v0.11 — the FINE button is gone from the title strip. Fine mode
    // now lives inside the settings popup as a checkbox. fineMode is
    // still the editor-local boolean; toggling it in the menu calls
    // applyPitchSlideFineMode() directly.
    applyPitchSlideFineMode();

    // --- Act I/O button ----------------------------------------------
    // Mockup: small bordered button for acts menu.
    ioButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff1a1a1a));
    ioButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff888888));
    ioButton.onClick = [this] { showActIoMenu(); };
    innerContent.addAndMakeVisible (ioButton);

    // v0.30 — preset next/prev arrows for quick browsing.
    for (auto* btn : { &presetPrevButton, &presetNextButton })
    {
        btn->setColour (juce::TextButton::buttonColourId,  juce::Colour (0xff1a1a1a));
        btn->setColour (juce::TextButton::textColourOffId, juce::Colour (0xff888888));
        innerContent.addAndMakeVisible (*btn);
    }
    presetPrevButton.onClick = [this] { applyPresetDelta (-1); };
    presetNextButton.onClick = [this] { applyPresetDelta (+1); };

    // v0.30 — preset name label between the arrows. Clickable to open menu.
    // BUG 2 fix: changed from "—" (em-dash) to "No preset" (plain ASCII) to avoid UTF-8 garbling
    presetNameLabel.setText ("No Preset", juce::dontSendNotification);
    presetNameLabel.setJustificationType (juce::Justification::centred);
    presetNameLabel.setColour (juce::Label::textColourId, Col::textBright);
    // v0.36 — 11pt → 13pt bold. Slightly bigger so the preset name is
    // easy to scan from across the room without being louder than the
    // knob captions.
    presetNameLabel.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    presetNameLabel.addMouseListener (this, false);
    innerContent.addAndMakeVisible (presetNameLabel);

    // --- title-bar buttons --------------------------------------------
    // Mockup: transparent bg, coloured border+text when on.
    freezeButton.setButtonText ("FREEZE");
    freezeButton.setClickingTogglesState (true);
    freezeButton.setColour (juce::TextButton::buttonColourId,     juce::Colour (0x00000000));
    freezeButton.setColour (juce::TextButton::buttonOnColourId,   juce::Colour (0x114a9eff));
    freezeButton.setColour (juce::TextButton::textColourOffId,    juce::Colour (0xff999999));
    freezeButton.setColour (juce::TextButton::textColourOnId,     Col::freezeOn);
    innerContent.addAndMakeVisible (freezeButton);

    // v0.8 — pull freeze mode + waveform visibility + UI scale out of
    // the persisted processor state so reopening the plugin restores
    // the user's preferences.
    freezeMomentary = processorRef.getFreezeMomentaryMode();
    bypassButton.setToggleState (processorRef.isMasterBypassed(), juce::dontSendNotification);
    waveformVisible = processorRef.getWaveformVisible();
    uiScale         = processorRef.getUiScale();

    applyFreezeMode();   // installs the freezeAttachment for the chosen mode

    // SEQ button lives next to FREEZE; not an APVTS parameter since
    // sequencer mode isn't something a host should automate — it's a
    // plugin-level mode flag.
    // v0.12 — Steve flagged the bare "SEQ" label as ambiguous (looked
    // like an effect name, not a mode toggle). It's now "SEQ ON" with
    // a longer tooltip spelling out what switching it off actually
    // does (freehand chopper driven by the live knob values).
    seqOnButton.setClickingTogglesState (true);
    seqOnButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0x00000000));
    seqOnButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0x1133cc66));
    seqOnButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff999999));
    seqOnButton.setColour (juce::TextButton::textColourOnId,   Col::seqOn);
    seqOnButton.setToggleState (processorRef.isSeqOn(), juce::dontSendNotification);
    seqOnButton.setTooltip ("Toggle the step sequencer on or off. When OFF the plugin "
                            "runs in freehand mode, chopping the input at the Division "
                            "knob's rate with the CHANCE knob deciding each fire. When "
                            "ON the grid below drives firing via stamped Acts, per-step "
                            "p-locks, ratios, and ratchets.");
    seqOnButton.onClick = [this]
    {
        processorRef.setSeqOn (seqOnButton.getToggleState());
    };
    innerContent.addAndMakeVisible (seqOnButton);

    // v0.8 — settings (gear) and help (?) buttons. Settings opens a
    // PopupMenu with freeze mode / waveform visibility / UI scale;
    // help opens a tiny CallOutBox with the modifier-click crib sheet.
    // v0.11 — button now reads "Settings" (was "SET"); settings popup
    // has grown to include grid accent, transport mode, and fine-tune
    // mode, so the cryptic abbreviation earned a demotion.
    // v0.24 — master bypass. Click to toggle all processing off.
    // The circular buffer keeps recording so you can un-bypass and
    // immediately chop.
    bypassButton.setClickingTogglesState (true);
    bypassButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0x00000000));
    bypassButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0x11cc3333));
    bypassButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff999999));
    bypassButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xffcc3333));
    bypassButton.setTooltip ("Master bypass - disables all FX processing.");
    bypassButton.onClick = [this]
    {
        processorRef.setMasterBypass (bypassButton.getToggleState());
    };
    innerContent.addAndMakeVisible (bypassButton);

    // --- MOD button (LFO page toggle) --------------------------------
    // v0.36 — neutral grey text when idle (matches SETTINGS), cyan only
    // when ACTIVE. Hover brightening is handled by StencilLookAndFeel's
    // drawButtonBackground which brightens the fill on highlight; the
    // text brighten-on-hover is added inside drawButtonText (below) so
    // that MOD lights up cyan on mouse-over without being permanently lit.
    modButton.setClickingTogglesState (true);
    modButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0x00000000));
    modButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0x224fd1e0));
    modButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff999999));
    modButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff4fd1e0));
    // Tag the button so the L&F knows to brighten to cyan on hover.
    modButton.getProperties().set ("hoverAccent", (int) 0xff4fd1e0);
    modButton.setTooltip ("Toggle the LFO modulation configuration page.");
    modButton.onClick = [this]
    {
        modPageVisible = modButton.getToggleState();
        // v0.42.2 — MOD and SETTINGS are full-screen overlay pages and
        // shouldn't stack; opening one closes the other. Previously the
        // SETTINGS page would stay alive underneath and clicking its
        // still-highlighted button did nothing visible (it just flipped a
        // bool with no paint route out). Force SETTINGS closed here.
        if (modPageVisible && settingsPageVisible)
        {
            settingsPageVisible = false;
            settingsButton.setToggleState (false, juce::dontSendNotification);
        }
        if (modPageVisible && !modPage)
        {
            modPage = std::make_unique<ModPage> (&processorRef);
            modPage->setupLfoControls();
            modPage->applyStencilLookAndFeel (&stencilLF);
            innerContent.addAndMakeVisible (*modPage);
        }
        // v0.37 — when opening the MOD page, jump to the tab for the
        // Act the user is currently editing. Previously it always
        // reverted to whichever tab you had open last, which meant
        // editing on Act C → clicking MOD → staring at Act A.
        if (modPageVisible && modPage)
            modPage->setCurrentActTab (processorRef.getSelectedScene());
        resized();
    };
    innerContent.addAndMakeVisible (modButton);

    // v0.41 — SETTINGS now toggles a full-screen settings page (mirrors
    // MOD). Alt+click still drops into the hidden debug menu, which is
    // handled inside toggleSettingsPage(). The button itself behaves like
    // a clicking-toggle: lights up accent-orange when the page is open.
    settingsButton.setButtonText ("TOOLS");
    settingsButton.setClickingTogglesState (true);
    settingsButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0x00000000));
    settingsButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0x22ff5a3c));
    settingsButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff999999));
    settingsButton.setColour (juce::TextButton::textColourOnId,   Col::accent);
    settingsButton.getProperties().set ("hoverAccent", (int) Col::accent.getARGB());
    settingsButton.setTooltip ("Open the tools page (alt-click for debug menu).");
    settingsButton.onClick = [this] { toggleSettingsPage(); };
    innerContent.addAndMakeVisible (settingsButton);

    helpButton.setColour (juce::TextButton::buttonColourId,  juce::Colour (0x00000000));
    helpButton.setColour (juce::TextButton::textColourOffId, juce::Colour (0xff999999));
    helpButton.setTooltip ("Show the modifier-click crib sheet.");
    helpButton.onClick = [this] { showHelpCallout(); };
    innerContent.addAndMakeVisible (helpButton);

    // v0.28 — apply stencil LookAndFeel to all title-bar buttons.
    freezeButton.setLookAndFeel (&stencilLF);
    seqOnButton.setLookAndFeel (&stencilLF);
    bypassButton.setLookAndFeel (&stencilLF);
    modButton.setLookAndFeel (&stencilLF);
    settingsButton.setLookAndFeel (&stencilLF);
    helpButton.setLookAndFeel (&stencilLF);
    ioButton.setLookAndFeel (&stencilLF);

    // v0.34 — nav-bar hover tinting. Each top-strip button previews
    // its "engaged" colour on mouse-over; the engaged colour persists
    // once the state is active (handled by textColourOnId elsewhere).
    {
        const juce::Colour dimGrey (0xff999999);
        const juce::Colour dimGrey2 (0xff888888);
        navHoverListeners.clear();
        navHoverListeners.emplace_back (std::make_unique<NavHoverListener> (freezeButton,   Col::freezeOn,                dimGrey));
        navHoverListeners.emplace_back (std::make_unique<NavHoverListener> (seqOnButton,    Col::seqOn,                   dimGrey));
        navHoverListeners.emplace_back (std::make_unique<NavHoverListener> (bypassButton,   juce::Colour (0xffcc3333),    dimGrey));
        // v0.36 — MOD: grey when idle (matches SETTINGS), cyan on hover.
        // Cyan when ACTIVE is handled separately via textColourOnId. The
        // previous "already cyan" idle colour was the reason MOD looked
        // permanently lit regardless of toggle state.
        navHoverListeners.emplace_back (std::make_unique<NavHoverListener> (modButton,      juce::Colour (0xff4fd1e0),    dimGrey));
        navHoverListeners.emplace_back (std::make_unique<NavHoverListener> (settingsButton, Col::accent,                  dimGrey));
        navHoverListeners.emplace_back (std::make_unique<NavHoverListener> (helpButton,     Col::accent,                  dimGrey));
        navHoverListeners.emplace_back (std::make_unique<NavHoverListener> (ioButton,       Col::accent,                  dimGrey2));
        navHoverListeners.emplace_back (std::make_unique<NavHoverListener> (presetPrevButton, Col::accent,                dimGrey2));
        navHoverListeners.emplace_back (std::make_unique<NavHoverListener> (presetNextButton, Col::accent,                dimGrey2));
    }

    // --- act bank (scene slots renamed "Acts" in the UI) ------------
    // v0.10 — STORE button removed. Knob edits are auto-mirrored into
    // the selected Act by the processor's APVTS listener, so just
    // moving a knob now commits the change. Populated acts have a
    // brighter fill colour; right-click opens a context menu
    // with "Reset to defaults".
    for (int i = 0; i < LoopSaboteurProcessor::kNumScenes; ++i)
    {
        auto& letterBtn = sceneButtons[i];
        letterBtn.setButtonText (juce::String::charToString ((juce::juce_wchar) ('A' + i)));
        letterBtn.setClickingTogglesState (false);
        letterBtn.setColour (juce::TextButton::buttonColourId,   Col::panel);
        letterBtn.setColour (juce::TextButton::buttonOnColourId, Col::accent);
        letterBtn.setColour (juce::TextButton::textColourOffId,  Col::textDim);
        letterBtn.setColour (juce::TextButton::textColourOnId,   Col::bg);
        letterBtn.setTooltip ("Click to load Act "
                              + juce::String::charToString ((juce::juce_wchar) ('A' + i))
                              + " into the knobs. Right-click for options. "
                              + "Populated acts have a brighter fill colour.");
        letterBtn.onClick = [this, i]
        {
            // Selecting an Act pushes its stored values into the APVTS
            // so the knobs snap to it. Further knob edits auto-mirror
            // back into this Act until a different one is selected.
            // If we're in Lock Edit mode, selecting a different Act
            // would be confusing — exit edit mode first.
            if (editLockStep >= 0) exitLockEditMode();
            processorRef.setSelectedScene (i);
            processorRef.loadSceneToKnobs (i);
            refreshSceneButtons();
            updatePresetNameLabel();  // BUG 1 fix: update label for the newly selected scene
        };
        letterBtn.onContextMenu = [this, i] { showActContextMenu (i); };
        innerContent.addAndMakeVisible (letterBtn);
    }

    refreshSceneButtons();

    // --- sequencer-header global buttons ------------------------------
    auto styleHeaderBtn = [] (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId,   Col::panel);
        b.setColour (juce::TextButton::buttonOnColourId, Col::accent);
        b.setColour (juce::TextButton::textColourOffId,  Col::textDim);
        b.setColour (juce::TextButton::textColourOnId,   Col::bg);
    };

    styleHeaderBtn (clearAllButton);
    clearAllButton.setTooltip ("Clear every step's scene, ratio, freeze hold, grab offset, and p-locks.");
    clearAllButton.onClick = [this]
    {
        if (editLockStep >= 0) exitLockEditMode();
        processorRef.pushUndoSnapshot();
        processorRef.clearAllSteps();
        refreshStepCells();
    };
    innerContent.addAndMakeVisible (clearAllButton);

    // v0.30 — UNDO. Single-level, restores the step grid from before
    // the last destructive operation (Clear, Randomize, Pattern).
    styleHeaderBtn (undoButton);
    undoButton.setTooltip ("Undo the last destructive step operation (Clear, Randomize, Pattern). Also Cmd+Z.");
    undoButton.onClick = [this]
    {
        if (processorRef.popUndo())
        {
            refreshStepCells();
            refreshSceneButtons();
            refreshLengthDisplay();
            refreshPageButton();
        }
    };
    innerContent.addAndMakeVisible (undoButton);

    // v0.11 — RANDOMIZE. Click opens a popup with tiers from "just
    // shuffle step placements" up to "randomize the whole rig".
    // Right-click clears all step placements.
    styleHeaderBtn (randomizeButton);
    randomizeButton.setButtonText ("RANDOM");
    randomizeButton.setTooltip (
        "RANDOMISE - click for a menu of musical random generators. "
        "Right-click to wipe all step stamps.");
    randomizeButton.onClick = [this] { showRandomizeMenu(); };
    randomizeButton.onRightClick = [this]
    {
        processorRef.pushUndoSnapshot();
        processorRef.clearAllSteps();
        refreshStepCells();
    };
    innerContent.addAndMakeVisible (randomizeButton);

    // v0.14 — four page buttons A/B/C/D replace the cycling button.
    pageLabel.setText ("PAGE", juce::dontSendNotification);
    pageLabel.setJustificationType (juce::Justification::centredRight);
    pageLabel.setColour (juce::Label::textColourId, Col::textDim);
    pageLabel.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    innerContent.addAndMakeVisible (pageLabel);
    {
        const char letters[] = { 'A', 'B', 'C', 'D' };
        for (int p = 0; p < 4; ++p)
        {
            auto& pb = pageButtons[(size_t) p];
            pb.setButtonText (juce::String::charToString ((juce::juce_wchar) letters[p]));
            styleHeaderBtn (pb);
            pb.setClickingTogglesState (false);
            pb.onClick = [this, p]
            {
                // v0.16 — manual page selection overrides page-follows-
                // playhead so the timer doesn't snap us back 33 ms later.
                processorRef.setPageFollowsPlayhead (false);
                currentPage = p;
                pageChanged();
            };
            // v0.31 — right-click to copy/paste the current page
            pb.onRightClick = [this, p]
            {
                currentPage = p;  // ensure we're on the clicked page
                showPageContextMenu();
            };
            innerContent.addAndMakeVisible (pb);
        }
        refreshPageButton();
    }

    // --- scale quantise dropdowns -------------------------------------
    // Scale combos — still wired to APVTS for state persistence and
    // automation, but hidden from the main UI. Controlled via the
    // Settings menu instead.
    scaleLabel.setText ("SCALE", juce::dontSendNotification);
    scaleLabel.setVisible (false);

    scaleRootCombo.setColour (juce::ComboBox::backgroundColourId, Col::panelDeep);
    scaleRootCombo.setColour (juce::ComboBox::textColourId,       Col::textBright);
    scaleRootCombo.setColour (juce::ComboBox::outlineColourId,    Col::panel);
    scaleRootCombo.setVisible (false);
    innerContent.addChildComponent (scaleRootCombo);

    scaleTypeCombo.setColour (juce::ComboBox::backgroundColourId, Col::panelDeep);
    scaleTypeCombo.setColour (juce::ComboBox::textColourId,       Col::textBright);
    scaleTypeCombo.setColour (juce::ComboBox::outlineColourId,    Col::panel);
    scaleTypeCombo.setVisible (false);
    innerContent.addChildComponent (scaleTypeCombo);

    // v0.8 — ComboBoxAttachment does NOT auto-populate the combo items
    // from the underlying Choice parameter (it only maps the selected
    // index). We have to push the items ourselves before the
    // attachment is built, otherwise the dropdowns are empty. Pull
    // the choices directly off the AudioParameterChoice via dynamic
    // cast so we don't have to duplicate the option strings.
    auto pushChoicesInto = [this] (juce::ComboBox& combo, const juce::String& paramId)
    {
        if (auto* cp = dynamic_cast<juce::AudioParameterChoice*> (
                           processorRef.apvts.getParameter (paramId)))
        {
            combo.addItemList (cp->choices, 1);
        }
    };
    pushChoicesInto (scaleRootCombo, LoopSaboteurProcessor::kParamScaleRoot);
    pushChoicesInto (scaleTypeCombo, LoopSaboteurProcessor::kParamScaleType);

    scaleRootAttachment = std::make_unique<ComboAttachment> (
        processorRef.apvts, LoopSaboteurProcessor::kParamScaleRoot, scaleRootCombo);
    scaleTypeAttachment = std::make_unique<ComboAttachment> (
        processorRef.apvts, LoopSaboteurProcessor::kParamScaleType, scaleTypeCombo);

    // --- step grid ----------------------------------------------------
    for (int i = 0; i < LoopSaboteurProcessor::kMaxSteps; ++i)
    {
        auto& cell = stepCells[i];
        cell.setIndex (i);
        cell.setSceneIndex (processorRef.getStepScene (i));
        cell.setRatioIndex (processorRef.getStepRatio (i));
        cell.setFreezeHold (processorRef.getStepFreeze (i));
        cell.setHasLocks   (processorRef.stepHasAnyLock (i));
        cell.setRatchet    (processorRef.getStepRatchet (i));  // v0.12
        cell.setMuted      (processorRef.getStepMute    (i));  // v0.13
        cell.setActive     (i < processorRef.getSeqLength());

        cell.onSet = [this, i]
        {
            // Click stamps whichever Act is currently selected in the
            // bank onto this step. If the step already holds that act,
            // toggle it off — makes single-click erase natural when
            // you're laying down a pattern with one Act.
            const int selected = processorRef.getSelectedScene();
            const int cur      = processorRef.getStepScene (i);
            const int next     = (cur == selected) ? -1 : selected;
            processorRef.setStepScene (i, next);
            stepCells[i].setSceneIndex (next);
        };
        cell.onContextMenu = [this, i] { showStepCellMenu (i); };
        cell.onCycleRatio = [this, i]
        {
            // Cycle through the ratio table (0 = inherit Act chance,
            // 1..kNumRatios-1 = N:M ratios). Rolls back to 0 at the end.
            const int cur  = processorRef.getStepRatio (i);
            const int next = (cur + 1) % LoopSaboteurProcessor::kNumRatios;
            processorRef.setStepRatio (i, next);
            stepCells[i].setRatioIndex (next);
        };
        cell.onToggleFreeze = [this, i]
        {
            // Per-step freeze hold: while the playhead is on this step,
            // the capture buffer is frozen regardless of FREEZE/MOM.
            const bool next = ! processorRef.getStepFreeze (i);
            processorRef.setStepFreeze (i, next);
            stepCells[i].setFreezeHold (next);
        };
        cell.onShiftClick = [this, i]
        {
            // Shift toggles Lock Edit mode on this cell. Re-shift-clicking
            // the same cell exits; shift-clicking a different cell
            // switches the edit target without needing to exit first.
            if (editLockStep == i)
                exitLockEditMode();
            else
            {
                if (editLockStep >= 0) exitLockEditMode();
                enterLockEditMode (i);
            }
        };
        cell.onSelectRange = [this, i]
        {
            // v0.31 — Ctrl+Shift+click for multi-step selection.
            // First click sets selectionStart, second click sets selectionEnd.
            if (selectionStart < 0)
            {
                // First click: set start
                selectionStart = i;
                selectionEnd   = i;
            }
            else
            {
                // Second click: set end and complete selection
                selectionEnd = i;
                // Ensure start <= end for easier iteration
                if (selectionStart > selectionEnd)
                    std::swap (selectionStart, selectionEnd);
            }
            updateSelectionHighlight();
        };
        innerContent.addAndMakeVisible (cell);
    }

    // --- length +/- buttons + value readout ---------------------------
    lengthLabel.setText ("LENGTH", juce::dontSendNotification);
    lengthLabel.setJustificationType (juce::Justification::centredRight);
    lengthLabel.setColour (juce::Label::textColourId, Col::textDim);
    lengthLabel.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
    innerContent.addAndMakeVisible (lengthLabel);

    auto configureNudgeButton = [] (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId,  Col::panel);
        b.setColour (juce::TextButton::textColourOffId, Col::textBright);
        // v0.38 — tick-up / tick-down while the mouse is held. Previously
        // Steve had to click once per step change which was maddening for
        // 16 → 64 kinds of moves. initialDelay gives the normal "one
        // click = one tick" feel; after 350ms the button starts auto-
        // repeating at 12 Hz (~83ms per tick) until released.
        b.setTriggeredOnMouseDown (true);
        b.setRepeatSpeed (350, 83);
    };
    configureNudgeButton (lengthMinusButton);
    configureNudgeButton (lengthPlusButton);
    lengthMinusButton.onClick = [this] { applyLengthDelta (-1); };
    lengthPlusButton .onClick = [this] { applyLengthDelta (+1); };
    innerContent.addAndMakeVisible (lengthMinusButton);
    innerContent.addAndMakeVisible (lengthPlusButton);

    lengthValueLabel.setJustificationType (juce::Justification::centred);
    lengthValueLabel.setColour (juce::Label::textColourId,       Col::textBright);
    lengthValueLabel.setColour (juce::Label::backgroundColourId, Col::panelDeep);
    lengthValueLabel.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    // v0.13 — Steve wanted to type the length instead of only using +/-.
    // Single-click to edit, Enter to commit, Escape to cancel. The onTextChange
    // handler parses, clamps to [2..kMaxSteps], and pipes through setSeqLength
    // so AUTO-follow gets disabled and the grid reflows the same way the
    // nudge buttons do.
    lengthValueLabel.setEditable (true, true, false);
    lengthValueLabel.setColour (juce::Label::textWhenEditingColourId, Col::textBright);
    lengthValueLabel.setColour (juce::Label::backgroundWhenEditingColourId, Col::panelDeep);
    lengthValueLabel.setColour (juce::Label::outlineWhenEditingColourId, Col::accent);
    lengthValueLabel.onTextChange = [this]
    {
        const auto typed = lengthValueLabel.getText()
                             .retainCharacters ("0123456789");
        if (typed.isEmpty())
        {
            refreshLengthDisplay();
            return;
        }
        const int requested = typed.getIntValue();
        const int clamped   = juce::jlimit (2,
                                            (int) LoopSaboteurProcessor::kMaxSteps,
                                            requested);
        const int current   = processorRef.getSeqLength();
        if (clamped != current)
            applyLengthDelta (clamped - current);
        else
            refreshLengthDisplay();  // snap display back to canonical form
    };
    innerContent.addAndMakeVisible (lengthValueLabel);
    refreshLengthDisplay();

    // --- waveform strip -----------------------------------------------
    waveformStrip.setProcessor (&processorRef);
    innerContent.addAndMakeVisible (waveformStrip);

    // v0.8 — 60 Hz timer. Playhead repaint is still gated on actual
    // change; the waveform strip updates each tick and now gets a
    // genuinely smooth scroll (combined with the halved bucket size
    // on the processor side).
    startTimerHz (60);

    // v0.12 — make the editor freely resizable. Earlier versions
    // offered a fixed menu of zoom presets (0.75/1.0/1.25/1.5), which
    // was fine on laptops but left 27" users with either "too small"
    // or "too big". The constrainer locks the aspect ratio to the
    // design size so the scaling transform stays clean; the min/max
    // are half and double the design size respectively.
    resizeConstrainer.setFixedAspectRatio ((double) kDesignW / (double) kDesignH);
    resizeConstrainer.setSizeLimits ((int) (kDesignW * 0.5f),
                                     (int) (kDesignH * 0.5f),
                                     (int) (kDesignW * 2.0f),
                                     (int) (kDesignH * 2.0f));
    setConstrainer (&resizeConstrainer);
    setResizable (true, true);

    // Initialize tooltip window based on saved setting
    if (processorRef.getTooltipsEnabled())
    {
        tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 800);
        tooltipWindow->setLookAndFeel (&stencilLF);
    }

    // Apply persisted UI scale last so the rest of the component tree
    // is already built when we install the AffineTransform. After
    // v0.12 applyUiScale just seeds an initial outer-window size; the
    // transform is actually installed inside resized() so future drags
    // and preset clicks take the same code path.
    applyUiScale (uiScale);

    // v0.13 — ensure the step grid reflects the current accent / ratchet
    // / lock state on first paint. Previously these only got pushed onto
    // the cells via explicit refresh calls triggered by user actions,
    // so a freshly-opened editor showed a flat sequencer until you did
    // something that refreshed it.
    refreshStepCells();
}

LoopSaboteurEditor::~LoopSaboteurEditor()
{
    stopTimer();
    // If we're leaving while still in lock edit mode, make sure the
    // knob attachments come back before destruction so nothing dangles.
    if (editLockStep >= 0) exitLockEditMode();

    // v0.28 — clear stencil LookAndFeel before destruction.
    freezeButton.setLookAndFeel (nullptr);
    seqOnButton.setLookAndFeel (nullptr);
    bypassButton.setLookAndFeel (nullptr);
    modButton.setLookAndFeel (nullptr);
    settingsButton.setLookAndFeel (nullptr);
    helpButton.setLookAndFeel (nullptr);
    ioButton.setLookAndFeel (nullptr);

    // Clear LookAndFeel on all knob sliders and labels.
    LabeledKnob* allKnobs[] = {
        &chance, &division, &lookback, &rate, &judder, &judderDiv,
        &pitch, &slide, &decay, &reverse, &crunch, &crushRate, &mix,
        &drive, &tone, &feedback, &tape, &ringMod, &varispeed, &stereo,
        &stretch, &shimmer, &res, &fold, &gate, &smear, &stutter, &chaos
    };
    for (auto* k : allKnobs)
    {
        k->slider.setLookAndFeel (nullptr);
        k->label.setLookAndFeel (nullptr);
    }
}

// ============================================================================
//  Knob setup
// ============================================================================
// v0.11 — static map of per-knob tooltip text. Keyed on paramId so we
// don't have to thread the description string through configureKnob's
// call sites. Missing entries just mean "no tooltip".
static juce::String describeKnob (const juce::String& paramId)
{
    using P = LoopSaboteurProcessor;

    // v0.12 — ASCII only. JUCE/macOS renders the embedded font at the
    // tooltip size without a full UTF-8 fallback chain, so em-dashes
    // and arrows were coming out as mojibake (see v0.11 screenshots).
    // "Dumb quotes and ASCII dashes only" keeps every string safe.
    if (paramId == P::kParamChance)    return "CHANCE\nHow often this Act retriggers. 0 = never, 1 = every slice.";
    if (paramId == P::kParamDivision)  return "DIVISION\nSlice length for the chopper, from 4-bar drones down to 32nds.\nLong divisions (1/2 and up) give slow, drifting patterns.";
    if (paramId == P::kParamLookback)  return "LOOKBACK\nHow far back in the buffer to grab a slice.\n0 = now, higher = further into the past.";
    if (paramId == P::kParamRate)      return "RATE\nPlayback speed for the grabbed slice.\nLower = longer/lower, higher = shorter/higher.";
    if (paramId == P::kParamJudder)    return "JUDDER\nStuttered re-triggers inside one slice.\nSets the number of hits.";
    if (paramId == P::kParamJudderDiv) return "JUDDER DIV\nDivision the judder hits subdivide at.\nInteracts with JUDDER count.";
    if (paramId == P::kParamPitch)     return "PITCH\nSemitone shift applied to the slice.\nFINE mode (in Tools) allows cents.";
    if (paramId == P::kParamSlide)     return "SLIDE\nPitch bend across the slice in semitones,\nfrom start to end.";
    if (paramId == P::kParamDecay)     return "DECAY\nLevel of each retrigger as a percentage\nof the last one. 100% = no decay.";
    if (paramId == P::kParamReverse)   return "REVERSE\nProbability that the grabbed slice plays\nbackwards.";
    if (paramId == P::kParamCrunch)    return "BITS\nBit-depth reduction. Lowers resolution\nfor a crunchy, lo-fi digital texture.\nSee Tools for Anti-alias and Mu-law\noptions that change the character.";
    if (paramId == P::kParamCrushRate) return "RATE\nSample-rate decimation. Holds each\nsample longer for aliased, gritty\ndownsampling.\nSee Tools for Anti-alias and Mu-law\noptions that change the character.";
    if (paramId == P::kParamMix)       return "MIX\nDry/wet balance between the untouched\ninput and the processed slice.";
    if (paramId == P::kParamDrive)     return "DRIVE\nSoft tape saturation before the filter.\nAdds warmth and harmonics.";
    if (paramId == P::kParamTone)      return "TONE\nBipolar tilt EQ. Centre = no effect.\nLeft = darker (LP filter).\nRight = brighter (HF shelf boost).";
    if (paramId == P::kParamFeedback)  return "FEEDBACK\nHow much of the wet signal is fed back\ninto the buffer for subsequent slices.";
    if (paramId == P::kParamTape)      return "TAPE\nCombined tape degradation. Low = gentle\nwow drift, higher adds flutter wobble.\nFull cassette character.";
    if (paramId == P::kParamRingMod)   return "RING MOD\nMultiplies the slice by a sine wave.\nLow = subtle tremolo, high = metallic\natonal textures. 20 Hz to 2 kHz.";
    if (paramId == P::kParamVarispeed) return "VARISPEED\nDJ brake / spindown effect. Each slice\ndecelerates over its length. Higher =\nslower, more dramatic pitch dive.";
    if (paramId == P::kParamStereo)    return "STEREO\nPer-slice random panning. Each slice\ngets a random pan position. Higher =\nwider spread across the stereo field.";
    if (paramId == P::kParamStretch)   return "STRETCH\nGranular time-stretch. Slows playback\nwithout changing pitch. Higher = more\nstretched. Overlapping grains preserve\ntonal character.";
    if (paramId == P::kParamShimmer)   return "SHIMMER\nOctave-up feedback layer,\nshimmer-reverb style.";
    if (paramId == P::kParamRes)       return "RES\nResonant filter sweep amount on the\nwet slice.";
    if (paramId == P::kParamFold)      return "FOLD\nWavefolder drive. Clean at zero,\nmetallic/harmonic as it climbs.";
    if (paramId == P::kParamGate)      return "SHAPE\nBipolar transient control. Left = Soft\n(LPG-style attack softening, vactrol decay).\nRight = Snappy (transient emphasis, punch).\nCentre = bypass. Re-pings on each judder.";
    if (paramId == P::kParamSmear)     return "SMEAR\nBlurs slice boundaries with a short\ndiffusion tail.";
    if (paramId == P::kParamStutter)   return "STUTTER\nMicro-loop buffer repeat. Captures a\ntiny window and loops it - glitch-hop\nbuffer freeze. Higher = smaller loop.";
    if (paramId == P::kParamChaos)     return "CHAOS\nRandom per-slice modulation on other\nparameters. The wildcard.";
    if (paramId == P::kParamSeqRate)   return "SEQ RATE\nClock division the sequencer advances at\n(relative to host BPM).";

    return {};
}

void LoopSaboteurEditor::configureKnob (LabeledKnob& k,
                                        const juce::String& caption,
                                        const juce::String& paramId,
                                        const juce::String& suffix)
{
    k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    k.slider.setColour (juce::Slider::rotarySliderFillColourId,    Col::accent);
    k.slider.setColour (juce::Slider::rotarySliderOutlineColourId, Col::textBright);
    k.slider.setColour (juce::Slider::thumbColourId,               Col::accent);
    k.slider.setColour (juce::Slider::textBoxTextColourId,         Col::textBright);
    k.slider.setColour (juce::Slider::textBoxOutlineColourId,      juce::Colours::transparentBlack);
    k.slider.setColour (juce::Slider::textBoxBackgroundColourId,   juce::Colours::transparentBlack);
    k.slider.setLookAndFeel (&stencilLF);
    innerContent.addAndMakeVisible (k.slider);

    k.label.setText (caption, juce::dontSendNotification);
    // v0.36 — centredBottom keeps the caption hugging the knob directly
    // below it, rather than floating in the middle of the label area.
    k.label.setJustificationType (juce::Justification::centredBottom);
    k.label.setColour (juce::Label::textColourId, juce::Colour::fromRGB (210, 210, 220));
    // v0.36 — knob captions (PITCH, DECAY, CRUNCH, …). 14.5pt bold with
    // a stencil-friendly kerning bump. getLabelFont now honours this
    // (previously the L&F forced every label to 12pt so setFont bumps
    // never rendered). 17pt was too shouty — 14.5pt reads bold and
    // confident without swallowing the knob.
    auto labelFont = juce::Font (juce::FontOptions (14.5f, juce::Font::bold));
    labelFont.setExtraKerningFactor (0.08f);
    k.label.setFont (labelFont);
    k.label.setLookAndFeel (&stencilLF);
    innerContent.addAndMakeVisible (k.label);

    // v0.11 — hover tooltips on both label and slider. Users often
    // hover the label first (it's the large bold text) so it has to
    // carry the description too; the slider handles repeat-hovers
    // once the user has found the knob. Label doesn't implement
    // TooltipClient out of the box so we use juce::SettableTooltip on
    // the Label/Slider's attached Component tooltip API.
    const auto desc = describeKnob (paramId);
    if (desc.isNotEmpty())
    {
        k.slider.setTooltip (desc);
        k.label.setTooltip  (desc);
    }

    k.attachment = std::make_unique<SliderAttachment> (
        processorRef.apvts, paramId, k.slider);

    // v0.38 — stash paramId on the knob for the right-click MOD menu.
    // The slider's onRightClick closure captures a reference to this
    // LabeledKnob so the menu builder can look up the lockable slot.
    k.paramId = paramId;
    k.slider.onRightClick = [this, &k] { showKnobModAssignMenu (k); };
}

// ============================================================================
//  Paint — chrome, background, panel separators
// ============================================================================
void LoopSaboteurEditor::paint (juce::Graphics& g)
{
    // v0.13 — the outer editor just paints the background fill so
    // nothing peeks through if the aspect-ratio constrainer is ever
    // bypassed. All the real panel art is drawn on innerContent via
    // paintContent(), which runs in the design-coordinate space.
    g.fillAll (Col::bg);
}

void LoopSaboteurEditor::paintContent (juce::Graphics& g)
{
    g.fillAll (Col::bg);

    // v0.28 — Title strip (stencil style): dark bg + orange bottom border.
    auto titleArea = innerContent.getLocalBounds().removeFromTop (46);
    g.setColour (Col::bg);
    g.fillRect (titleArea);

    // Subtle dark separator line (no orange border).
    g.setColour (juce::Colour (0xff333333));
    g.fillRect (titleArea.getX(), titleArea.getBottom() - 1,
                titleArea.getWidth(), 1);

    // "LOOP SABOTEUR" / "LOOP PACIFIST" easter egg fade.
    // v0.42.3 — April 1st easter egg: the main title reads "LOOP
    // GUARDIAN" instead of "LOOP SABOTEUR" (12 letters matches the
    // original so the header layout is unchanged). The pacifist fade
    // still does its normal thing on top of whichever base name is
    // shown, so holding the pacifist trigger on April 1st still
    // crossfades to "LOOP PACIFIST".
    {
        const auto today = juce::Time::getCurrentTime();
        const bool isAprilFool = (today.getMonth() == 3    // 0-indexed: March=2, April=3
                                   && today.getDayOfMonth() == 1);
        const juce::String baseTitle = isAprilFool ? "LOOP DEFENDER"
                                                   : "LOOP SABOTEUR";

        auto titleFont = juce::Font (juce::FontOptions ("Impact", 32.0f, juce::Font::plain));
        g.setFont (titleFont);
        auto titleTextArea = titleArea.withTrimmedLeft (20).withTrimmedRight (420);

        if (pacifistFade <= 0.0f)
        {
            g.setColour (Col::accent);
            g.drawFittedText (baseTitle, titleTextArea,
                              juce::Justification::centredLeft, 1);
        }
        else if (pacifistFade >= 1.0f)
        {
            g.setColour (Col::seqOn);   // green
            g.drawFittedText ("LOOP PACIFIST", titleTextArea,
                              juce::Justification::centredLeft, 1);
        }
        else
        {
            // Cross-fade: draw both, modulating alpha.
            g.setColour (Col::accent.withAlpha (1.0f - pacifistFade));
            g.drawFittedText (baseTitle, titleTextArea,
                              juce::Justification::centredLeft, 1);
            g.setColour (Col::seqOn.withAlpha (pacifistFade));
            g.drawFittedText ("LOOP PACIFIST", titleTextArea,
                              juce::Justification::centredLeft, 1);
        }

        const int titleTextW = (int) titleFont.getStringWidthFloat (baseTitle) + 4;

        // "vX.Y Alpha / AMFAS" — bottom-aligned with the title baseline.
        // Impact 32px has a descent of ~6px, so the baseline sits ~6px above
        // the bottom of the centred text. We offset the smaller text to match.
        // v0.42 — version string now pulls from JucePlugin_VersionString
        // (populated from the CMake project VERSION), with " Alpha" suffixed
        // so testers can see the build is pre-release at a glance.
        const int titleBaseline = titleArea.getCentreY() + 10;  // approx baseline of 32px Impact
        auto verArea = juce::Rectangle<int> (20 + titleTextW + 14, titleBaseline - 12,
                                              260, 14);
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        g.setColour (Col::accent);
        const juce::String verStr = juce::String ("v")
                                    + juce::String (JucePlugin_VersionString)
                                    + " Beta";
        g.drawFittedText (verStr, verArea, juce::Justification::bottomLeft, 1);
        const int verW = (int) g.getCurrentFont().getStringWidthFloat (verStr) + 8;
        auto amfasArea = verArea.withTrimmedLeft (verW);
        g.setColour (Col::textBright);
        g.drawFittedText ("/ AMFAS", amfasArea, juce::Justification::bottomLeft, 1);

        // v0.42.4 — birthday tag: "<3 303" / "<3 606" / "<3 808"
        // on 3/3, 6/6, or 8/8 respectively. Silent, no audio impact.
        // The debug menu force-on still drives this via
        // StepCell::force*UntilMs so testers can QA the visual without
        // changing the clock. Painted in each machine's signature colour
        // immediately after "/ AMFAS".
        {
            const int month = today.getMonth() + 1;   // getMonth() is 0-based
            const int day   = today.getDayOfMonth();
            const juce::int64 nowMs = juce::Time::currentTimeMillis();
            const bool is303 = (month == 3 && day == 3) || (nowMs < StepCell::force303UntilMs);
            const bool is606 = (month == 6 && day == 6) || (nowMs < StepCell::force606UntilMs);
            const bool is808 = (month == 8 && day == 8) || (nowMs < StepCell::force808UntilMs);

            juce::String bdayTag;
            juce::Colour bdayCol;
            if      (is303) { bdayTag = "<3 303"; bdayCol = juce::Colour (0xffff8800); }
            else if (is606) { bdayTag = "<3 606"; bdayCol = juce::Colour (0xff33cc66); }
            else if (is808) { bdayTag = "<3 808"; bdayCol = juce::Colour (0xff4a9eff); }

            if (bdayTag.isNotEmpty())
            {
                const int amfasW = (int) g.getCurrentFont().getStringWidthFloat ("/ AMFAS") + 12;
                auto tagArea = amfasArea.withTrimmedLeft (amfasW);
                g.setColour (bdayCol);
                g.drawFittedText (bdayTag, tagArea, juce::Justification::bottomLeft, 1);
            }
        }
    }

    // v0.28 — K5 flanking columns: CHANCE (solid orange), MIX (solid white).
    // v0.29 — p-lock dirty: entire column goes yellow.
    // v0.36 — CHANCE / MIX vertical labels. Previously looked weedy
    // because the path was fitted into a 180×44 box that left the type
    // floating inside the stripe at a modest size. Reworked as a
    // dedicated path-based renderer: Impact at a fuller size, sheared
    // BEFORE rotation so it matches the stencil italic of SLICE/PLAY/
    // COLOUR/CRUNCH/SPACE, and sized against the actual stripe width so
    // the text fills the column confidently.
    auto paintVerticalStencilLabel = [&] (const juce::Rectangle<int>& stripe,
                                          const juce::String& caption,
                                          juce::Colour fill,
                                          juce::Colour text)
    {
        if (stripe.isEmpty())
            return;

        g.setColour (fill);
        g.fillRect (stripe);

        // v0.36 — the flanking knob is laid out at stripe.centreY + 60
        // with a ~45px visual radius. Text MUST stay clear of that zone
        // or it draws behind the knob. Carve out an explicit "text zone"
        // in the upper portion of the stripe and fit the caption to it.
        // v0.37 — text was floating too far above the knob. Tightened the
        // gap and explicitly shifted the text centre down 40px so the
        // caption sits close to the knob like the category stripe titles.
        const int knobCentreY   = stripe.getCentreY() + 60;
        const int knobVisualTop = knobCentreY - 28;       // tighter buffer (was -34)
        const int textZoneTop    = stripe.getY() + 14;
        const int textZoneBottom = knobVisualTop - 4;     // tight gap so text hugs knob
        const int textZoneHeight = textZoneBottom - textZoneTop;
        if (textZoneHeight < 40) return;

        // Font size: capped so it doesn't blow past the stripe width.
        const float stripeW = (float) stripe.getWidth();
        const float fontPt  = juce::jlimit (28.0f, 46.0f, stripeW * 0.72f);

        auto font = juce::Font (juce::FontOptions ("Impact", fontPt, juce::Font::plain));
        font.setExtraKerningFactor (0.04f);

        // Before rotation the text is laid out horizontally. Its horizontal
        // span (boxW) becomes the vertical span once rotated -90°.
        // cy is pulled DOWN 40px from the geometric centre of the text
        // zone so the caption sits just above the knob (not floating way
        // up at the top of the stripe). boxW is then clamped so the
        // bottom of the rotated text still clears the knob visual top.
        const float cx = (float) stripe.getCentreX();
        const float cyNatural = (float) (textZoneTop + textZoneBottom) * 0.5f;
        const float cy = cyNatural + 40.0f;
        const float boxW = juce::jmax (40.0f,
                                        (float) (textZoneBottom - (int) cy) * 2.0f);

        juce::GlyphArrangement ga;
        ga.addFittedText (font, caption,
                          cx - boxW * 0.5f, cy - fontPt * 0.6f,
                          boxW, fontPt * 1.2f,
                          juce::Justification::centred, 1);
        juce::Path p;
        ga.createPath (p);

        // Shear before rotation (horizontal shear -> vertical lean once rotated).
        const float halfPi = juce::MathConstants<float>::halfPi;
        auto xform = juce::AffineTransform::shear (-0.14f, 0.0f)
                        .rotated (-halfPi, cx, cy);
        p.applyTransform (xform);

        g.setColour (text);
        g.fillPath (p);
    };

    paintVerticalStencilLabel (chanceColumnBounds, "CHANCE",
                               chance.lockDirty ? Col::lockDirty : Col::accent,
                               Col::bg);
    paintVerticalStencilLabel (mixColumnBounds, "MIX",
                               mix.lockDirty ? Col::lockDirty : Col::textBright,
                               Col::bg);

    // v0.28 — Category stripes (stencil style): skewed label + horizontal line.
    // The shear is anchored around each label's own y position so the
    // italic effect doesn't drift with absolute y (which was pushing
    // lower labels off the left edge).
    {
        for (const auto& panel : categoryPanels)
        {
            if (panel.bounds.isEmpty() || panel.label.isEmpty())
                continue;

            const float top  = (float) panel.bounds.getY();
            const float left = (float) panel.bounds.getX();
            const float right = (float) panel.bounds.getRight();

            // Skewed category label in Impact font — drawn INSIDE the
            // cell's top 30px stripe area so it never overlaps the
            // previous row's value text.
            auto textRect = juce::Rectangle<float> (left + 4.0f, top + 2.0f, 180.0f, 26.0f);
            float anchorY = top + 15.0f;
            g.saveState();
            g.addTransform (juce::AffineTransform::translation (0.0f, -anchorY)
                                .sheared (-0.14f, 0.0f)
                                .translated (0.0f, anchorY));
            g.setColour (Col::accent);
            auto catFont = juce::Font (juce::FontOptions ("Impact", 26.0f, juce::Font::plain));
            catFont.setExtraKerningFactor (0.18f);
            g.setFont (catFont);
            // Easter egg: CRUNCH → PAULA when 8-bit is active.
            const auto paintLabel = (paulaActive && panel.label == "CRUNCH")
                                  ? juce::String ("PAULA") : panel.label;
            g.drawText (paintLabel, textRect, juce::Justification::centredLeft);
            g.restoreState();

            // Horizontal slash line at bottom of stripe area.
            const float lineY = top + 24.0f;
            const float lineStart = left + 120.0f;
            const float lineEnd   = right - 8.0f;
            if (lineEnd > lineStart)
            {
                g.setColour (Col::accent);
                g.fillRect (juce::Rectangle<float> (lineStart, lineY, lineEnd - lineStart, 3.0f));
            }
        }
    }

    // v0.28 — Acts section: dark bg + stencil header.
    const int knobsBottom = 46 + 558;
    const int globalMixH  = 24;       // global MIX slider row
    const int sceneTop    = knobsBottom + globalMixH;
    const int sceneHeight = 60;
    auto sceneArea = juce::Rectangle<int> (0, sceneTop, innerContent.getWidth(), sceneHeight);
    g.setColour (juce::Colour (0xff111111));
    g.fillRect (sceneArea);

    // Top border.
    g.setColour (juce::Colour (0xff333333));
    g.fillRect (sceneArea.getX(), sceneArea.getY(), sceneArea.getWidth(), 2);

    // "ACTS OF SABOTAGE" in skewed stencil font (locally anchored shear).
    {
        auto actsRect = sceneArea.withTrimmedLeft (16).withTrimmedTop (6).withHeight (24);
        float anchorY = (float) actsRect.getCentreY();
        g.saveState();
        g.addTransform (juce::AffineTransform::translation (0.0f, -anchorY)
                            .sheared (-0.14f, 0.0f)
                            .translated (0.0f, anchorY));
        g.setColour (Col::accent);
        auto actsFont = juce::Font (juce::FontOptions ("Impact", 20.0f, juce::Font::plain));
        actsFont.setExtraKerningFactor (0.16f);
        g.setFont (actsFont);
        g.drawText (amenActive ? "AMEN OF SABOTAGE" : "ACTS OF SABOTAGE",
                    actsRect, juce::Justification::centredLeft);
        g.restoreState();
    }

    // Waveform strip panel background. v0.8 — settings menu can hide
    // the strip entirely; when hidden we skip the panel paint AND the
    // layout code reclaims the 36px so the sequencer slides up.
    const int waveTop    = sceneTop + sceneHeight;
    const int waveHeight = waveformVisible ? 28 : 0;
    if (waveformVisible)
    {
        auto waveArea = juce::Rectangle<int> (0, waveTop, innerContent.getWidth(), waveHeight);
        g.setColour (Col::panel);
        g.fillRect (waveArea);
    }

    // v0.28 — Sequencer strip: dark bg + stencil header.
    const int stepTop    = waveTop + waveHeight;
    const int stepHeight = 132;
    auto stepArea = juce::Rectangle<int> (0, stepTop, innerContent.getWidth(), stepHeight);
    g.setColour (juce::Colour (0xff141414));
    g.fillRect (stepArea);

    // Top border.
    g.setColour (juce::Colour (0xff333333));
    g.fillRect (stepArea.getX(), stepArea.getY(), stepArea.getWidth(), 1);

    // v0.42.5 — The decorative "SEQUENCER" stencil that used to sit in
    // the top-left of stepArea was removed. Its 120px real estate is
    // now the home of the SEQ ON toggle (see layoutContent()), moved
    // down from the title strip after user testing flagged the old
    // position as confusingly detached from the sequencer it controls.

    // Lock Edit mode status — centred in the header row between Clear and RATE.
    if (editLockStep >= 0)
    {
        g.setColour (Col::lockDirty);
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        // Place in the middle gap of the sequencer header row.
        auto hint = juce::Rectangle<int> (stepArea.getX() + 610, stepArea.getY(),
                                           240, 34);
        g.drawText ("EDIT LOCKS  STEP " + juce::String (editLockStep + 1),
                    hint, juce::Justification::centredLeft);
    }

    // v0.28 — Footer: legend bar (top) + tagline (bottom), matching mockup CSS.
    auto footerArea = innerContent.getLocalBounds().removeFromBottom (28);

    // Legend row (coloured dots + labels).
    auto legendArea = footerArea.removeFromTop (16);
    g.setColour (juce::Colour (0xff141414));
    g.fillRect (legendArea);
    g.setColour (juce::Colour (0xff222222));
    g.fillRect (legendArea.withHeight (1));

    {
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        int lx = legendArea.getX() + 20;
        int ly = legendArea.getCentreY();

        auto drawLegendItem = [&] (juce::Colour dot, const juce::String& text)
        {
            g.setColour (dot);
            g.fillEllipse ((float) lx, (float) ly - 4.0f, 8.0f, 8.0f);
            lx += 12;
            g.setColour (juce::Colour (0xff777777));
            g.drawText (text, lx, legendArea.getY(), 100, legendArea.getHeight(),
                        juce::Justification::centredLeft);
            lx += (int) juce::Font (juce::FontOptions (12.0f)).getStringWidthFloat (text) + 14;
        };

        drawLegendItem (juce::Colour (0xff666666), "GRID ACCENT");
        drawLegendItem (Col::lockDirty, "P-LOCK");
        drawLegendItem (Col::freezeOn,  "FREEZE");
        drawLegendItem (Col::seqOn,     "PROBABILITY");

        // v0.42.5 — Capture the on-screen bounds of the "RATCHET" text so
        // the easter egg double-click handler has something to hit-test
        // against. drawLegendItem advances `lx` by: 12 (dot + gap) +
        // textWidth + 14 (trailing pad), so the text span is everything
        // between those two paddings.
        const int ratPreLx = lx;
        drawLegendItem (Col::textBright, "RATCHET");
        ratchetLegendBounds = juce::Rectangle<int> (
            ratPreLx + 12,
            legendArea.getY(),
            juce::jmax (0, lx - ratPreLx - 12 - 14),
            legendArea.getHeight());

        drawLegendItem (juce::Colour (0xff4fd1e0), "MOD");
    }

    // Easter egg: Guru Meditation — flashing red bar over the footer.
    if (guruActive && guruFlash)
    {
        auto guruArea = innerContent.getLocalBounds().removeFromBottom (28);
        g.setColour (juce::Colour (0xffcc0000));
        g.fillRect (guruArea);
        // Red border blink.
        g.setColour (juce::Colour (0xff880000));
        g.drawRect (guruArea, 2);
        // Text.
        g.setColour (juce::Colours::black);
        g.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
        g.drawText ("Guru Meditation  #00000030.0000AAC0",
                    guruArea, juce::Justification::centred);
    }

    // v0.40 — "Designed for X" badge. Appears top-center for ~10 s after
    // a preset loads with an Engine block while "Presets recall Engine"
    // is OFF. Click = apply; right-click or auto-timeout = dismiss.
    if (designedForBadgeBounds.getWidth() > 0
        && processorRef.getDesignedForStage() >= 0)
    {
        const auto stage  = processorRef.getDesignedForStage();
        const auto interp = processorRef.getDesignedForInterp();
        juce::String txt;
        txt << "Designed for: ";
        if (stage >= 0)  txt << loopsab::outputStageShortName (stage);
        if (interp >= 0) txt << " - " << loopsab::interpShortName (interp);
        txt << "  -  click to apply";

        const auto box = designedForBadgeBounds;
        // Subtle dark pill with accent border.
        g.setColour (juce::Colour (0xee1a1a1a));
        g.fillRoundedRectangle (box.toFloat(), 6.0f);
        g.setColour (Col::accent);
        g.drawRoundedRectangle (box.toFloat().reduced (0.5f), 6.0f, 1.2f);
        g.setColour (juce::Colour (0xffe8e8e8));
        g.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::plain)));
        g.drawFittedText (txt, box.reduced (10, 0),
                          juce::Justification::centred, 1);
    }

    // Easter egg: producer tag — painted last so it floats on top of all
    // other elements. Tooltip-style dark pill + subtle border.
    if (producerTagActive)
    {
        const juce::String tagText =
            "made with too many late nights and not enough sleep. "
            "this is why allmyfriendsaresynths";

        juce::Font tagFont (juce::FontOptions (11.0f, juce::Font::italic));
        const int textW = (int) tagFont.getStringWidthFloat (tagText);
        const int padX = 8, padY = 3;
        const int boxH = 14 + padY * 2;
        auto tagBox = juce::Rectangle<int> (20, titleArea.getBottom() + 2,
                                             textW + padX * 2, boxH);

        g.setColour (juce::Colour (0xee111111));
        g.fillRoundedRectangle (tagBox.toFloat(), 4.0f);
        g.setColour (juce::Colour (0x55ffffff));
        g.drawRoundedRectangle (tagBox.toFloat().reduced (0.5f), 4.0f, 1.0f);

        g.setColour (juce::Colour (0xffd8d8d8));
        g.setFont (tagFont);
        g.drawText (tagText, tagBox.reduced (padX, padY),
                    juce::Justification::centredLeft);
    }
}

// ============================================================================
//  Layout
// ============================================================================
void LoopSaboteurEditor::resized()
{
    // v0.13 — the editor owns one child (innerContent). We size it to
    // the design resolution, install an AffineTransform::scale on it so
    // the rendered size matches the host window, and then lay its
    // children out once using the design-coordinate budget. No more
    // recursive setSize() trick — dragging the window no longer causes
    // the jitter Steve saw in v0.12.
    if (getWidth() <= 0 || getHeight() <= 0)
        return;

    const float sx = (float) getWidth()  / (float) kDesignW;
    const float sy = (float) getHeight() / (float) kDesignH;
    const float s  = juce::jlimit (0.5f, 2.0f, juce::jmin (sx, sy));

    if (std::abs (uiScale - s) > 1.0e-4f)
    {
        uiScale = s;
        processorRef.setUiScale (s);
    }

    // Easter egg: horizontal mirror — flip around centre X.
    auto xform = juce::AffineTransform::scale (s);
    if (mirrorActive)
        xform = juce::AffineTransform::scale (-s, s)
                    .translated ((float) getWidth(), 0.0f);
    innerContent.setTransform (xform);
    innerContent.setBounds (0, 0, kDesignW, kDesignH);

    layoutContent();

    // v0.40 — "Designed for X" badge. Centered just below the title strip,
    // 360px wide x 22px tall. Bounds are in innerContent coordinates so
    // the same rectangle works for both paint and hit-test.
    {
        const int w = 380, h = 22;
        const int x = (kDesignW - w) / 2;
        const int y = 50;  // titleArea is 46px; 4px gap below.
        designedForBadgeBounds = { x, y, w, h };
    }

    innerContent.repaint();
}

// ============================================================================
// v0.42.3 — RatRun easter egg. A stylised little rat runs across the
// bottom of the editor on cue. Shapes-based drawing (no binary assets)
// so it survives theme / L&F changes without maintenance. The component
// intercepts no mouse events — it just decorates the top of the z-order.
// ============================================================================

LoopSaboteurEditor::RatRun::RatRun()
{
    setInterceptsMouseClicks (false, false);
    setWantsKeyboardFocus (false);
    setOpaque (false);
}

LoopSaboteurEditor::RatRun::~RatRun() { stopTimer(); }

void LoopSaboteurEditor::RatRun::launch (juce::Rectangle<int> parentBounds)
{
    setBounds (parentBounds);
    xPos  = -80.0f;
    frame = 0;
    lastTickMs = juce::Time::currentTimeMillis();
    startTimerHz (60);
    repaint();
}

void LoopSaboteurEditor::RatRun::timerCallback()
{
    const auto nowMs = juce::Time::currentTimeMillis();
    const float dt   = juce::jlimit (0.0f, 0.1f,
                                     (float) (nowMs - lastTickMs) * 0.001f);
    lastTickMs = nowMs;
    xPos += vx * dt;
    ++frame;
    if (xPos > (float) getWidth() + 80.0f)
    {
        stopTimer();
        if (onFinished) onFinished();
        return;
    }
    repaint();
}

void LoopSaboteurEditor::RatRun::paint (juce::Graphics& g)
{
    // Draw along a shallow bob so the rat "scampers" instead of sliding.
    const float baseY = (float) getHeight() - 34.0f;
    const float bob   = std::sin ((float) frame * 0.45f) * 2.0f;
    const float x = xPos;
    const float y = baseY + bob;

    // Subtle drop shadow — a dark squashed ellipse under the feet.
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillEllipse (x + 4.0f, y + 22.0f, 48.0f, 4.0f);

    const juce::Colour fur   = juce::Colour (0xff3a3a3a);
    const juce::Colour furHi = juce::Colour (0xff5a5a5a);
    const juce::Colour nose  = juce::Colour (0xffd88aa0);
    const juce::Colour eye   = juce::Colour (0xffff5a3c);   // accent-orange beady eye

    // Tail — cubic curve wagging out the back, thickness tapering via
    // stroke. Phase-offset from the body bob so it looks whippy.
    {
        juce::Path tail;
        const float tx0 = x + 10.0f,   ty0 = y + 14.0f;
        const float wag = std::sin ((float) frame * 0.55f + 1.0f) * 4.0f;
        tail.startNewSubPath (tx0, ty0);
        tail.cubicTo (x - 4.0f,  ty0 - 2.0f + wag,
                      x - 16.0f, ty0 + 6.0f - wag,
                      x - 22.0f, ty0 + 2.0f + wag * 0.5f);
        g.setColour (furHi);
        g.strokePath (tail, juce::PathStrokeType (2.2f,
                                                   juce::PathStrokeType::curved,
                                                   juce::PathStrokeType::rounded));
    }

    // Body — elongated ellipse.
    g.setColour (fur);
    g.fillEllipse (x + 6.0f, y + 6.0f, 36.0f, 16.0f);

    // Head — smaller ellipse up front + pointed snout.
    g.fillEllipse (x + 34.0f, y + 6.0f, 20.0f, 14.0f);
    {
        juce::Path snout;
        snout.startNewSubPath (x + 50.0f, y + 9.0f);
        snout.lineTo          (x + 60.0f, y + 13.0f);
        snout.lineTo          (x + 50.0f, y + 16.0f);
        snout.closeSubPath();
        g.fillPath (snout);
    }

    // Ears — two little round tufts on top of the head.
    g.setColour (furHi);
    g.fillEllipse (x + 38.0f, y + 2.0f, 6.0f, 6.0f);
    g.fillEllipse (x + 46.0f, y + 2.0f, 6.0f, 6.0f);

    // Nose (pink) + eye (accent-orange), both tiny.
    g.setColour (nose);
    g.fillEllipse (x + 58.0f, y + 12.0f, 2.5f, 2.5f);
    g.setColour (eye);
    g.fillEllipse (x + 46.0f, y + 9.5f, 2.0f, 2.0f);

    // Legs — four stubby vertical bars. Front pair and back pair have
    // opposite phases so it looks like a trot.
    auto leg = [&] (float lx, float baseTop, float phase)
    {
        const float lift = juce::jmax (0.0f,
                                       std::sin ((float) frame * 0.6f + phase)) * 3.0f;
        g.setColour (fur);
        g.fillRoundedRectangle (lx, y + baseTop - lift, 3.0f, 6.0f + lift, 1.2f);
    };
    leg (x + 12.0f, 18.0f, 0.0f);
    leg (x + 18.0f, 18.0f, 3.0f);
    leg (x + 32.0f, 18.0f, 3.0f);
    leg (x + 38.0f, 18.0f, 0.0f);
}

// --- key-history helpers for the easter-egg trigger ------------------------

void LoopSaboteurEditor::pushRecentKey (char ch)
{
    recentKeys[(size_t) recentKeysHead] = { ch, juce::Time::currentTimeMillis() };
    recentKeysHead = (recentKeysHead + 1) % (int) recentKeys.size();
}

bool LoopSaboteurEditor::recentKeysSpell (const char* word, juce::int64 withinMs) const
{
    const int wordLen = (int) std::strlen (word);
    const int bufLen  = (int) recentKeys.size();
    if (wordLen <= 0 || wordLen > bufLen) return false;

    // Walk backwards from the most-recently-written slot and compare
    // against the word read right-to-left.
    const juce::int64 nowMs = juce::Time::currentTimeMillis();
    int idx = (recentKeysHead - 1 + bufLen) % bufLen;
    for (int i = wordLen - 1; i >= 0; --i)
    {
        const auto& ks = recentKeys[(size_t) idx];
        const char wantUpper = (char) std::toupper ((unsigned char) word[i]);
        const char gotUpper  = (char) std::toupper ((unsigned char) ks.ch);
        if (gotUpper != wantUpper) return false;
        if (nowMs - ks.tMs > withinMs) return false;
        idx = (idx - 1 + bufLen) % bufLen;
    }
    return true;
}

void LoopSaboteurEditor::spawnRatRun()
{
    // Never stack multiple rats — honour the one already running.
    if (ratRun != nullptr) return;

    ratRun = std::make_unique<RatRun>();
    addAndMakeVisible (*ratRun);
    ratRun->toFront (false);
    ratRun->onFinished = [this]
    {
        // Defer destruction: timerCallback is on the stack when this
        // fires, and deleting `this` (the RatRun) from inside its own
        // callback is unsafe. Use MessageManager::callAsync.
        juce::MessageManager::callAsync ([this] { ratRun.reset(); });
    };
    ratRun->launch (getLocalBounds());
}

// v0.30 — Cmd+Z (Mac) / Ctrl+Z (Win/Lin) single-level undo for the
// step grid. Restores the snapshot captured before the last destructive
// bulk operation (clear, randomize, pattern apply).
bool LoopSaboteurEditor::keyPressed (const juce::KeyPress& key)
{
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'Z')
    {
        if (processorRef.popUndo())
        {
            refreshStepCells();
            refreshSceneButtons();
            refreshLengthDisplay();
            refreshPageButton();
        }
        return true;
    }

    // v0.41.1 — Escape closes whichever overlay page is open, so the user is
    // never stranded if a header button is somehow obscured.
    if (key == juce::KeyPress::escapeKey)
    {
        if (settingsPageVisible)
        {
            settingsButton.setToggleState (false, juce::dontSendNotification);
            settingsPageVisible = false;
            resized();
            return true;
        }
        if (modPageVisible)
        {
            modButton.setToggleState (false, juce::dontSendNotification);
            modPageVisible = false;
            resized();
            return true;
        }
    }

    // v0.42.5 — Rat easter egg was previously triggered by typing R-A-T,
    // but letters R/A/T collide with Logic Pro's transport shortcuts
    // (R = record) so the plugin never actually saw them. The trigger
    // now fires on a double-click of the word "RATCHET" in the footer
    // legend — see the paintContent() legend code for the captured hit
    // region, and innerContent.onMouseDown in the constructor for the
    // double-click handler. The pushRecentKey / recentKeysSpell helpers
    // are retained for any future multi-key combo that doesn't clash
    // with DAW shortcuts.
    return false;
}

void LoopSaboteurEditor::mouseDown (const juce::MouseEvent& e)
{
    // Check if click is on the preset name label — open preset browser
    // directly (categories visible) rather than the full Acts I/O menu.
    if (presetNameLabel.getScreenBounds().contains (e.getScreenPosition()))
    {
        showPresetLoadMenu();
        return;
    }

    // v0.40 — "Designed for X" Engine-recall badge.
    if (processorRef.getDesignedForStage() >= 0
        && designedForBadgeBounds.getWidth() > 0)
    {
        const auto screenBox = innerContent.localAreaToGlobal (designedForBadgeBounds);
        if (screenBox.contains (e.getScreenPosition()))
        {
            if (e.mods.isPopupMenu() || e.mods.isRightButtonDown())
            {
                // Right-click dismisses without applying.
                processorRef.clearDesignedFor();
            }
            else
            {
                // Left-click applies the preset's intended Engine.
                processorRef.applyDesignedFor();
            }
            innerContent.repaint();
            return;
        }
    }

    juce::AudioProcessorEditor::mouseDown (e);
}

void LoopSaboteurEditor::layoutContent()
{
    auto bounds = juce::Rectangle<int> (0, 0, kDesignW, kDesignH);

    // --- title strip --------------------------------------------------
    // Order from the right edge:
    //   ?  TOOLS  MOD  FREEZE  SEQ ON  BYPASS
    // (left-to-right reads: BYPASS / SEQ ON / FREEZE / MOD / TOOLS / ?)
    auto titleArea = bounds.removeFromTop (46);
    helpButton      .setBounds (titleArea.removeFromRight (40).reduced (6, 7));
    settingsButton  .setBounds (titleArea.removeFromRight (90).reduced (6, 7));
    modButton       .setBounds (titleArea.removeFromRight (60).reduced (6, 7));
    freezeButton    .setBounds (titleArea.removeFromRight (85).reduced (6, 7));
    // v0.42.5 — SEQ ON used to live here in the title strip but user
    // testing flagged that as confusingly far from the sequencer itself.
    // It now sits at the left edge of the sequencer header row, in the
    // footprint previously occupied by the "SEQUENCER" stencil label
    // (see stepArea layout below and the removed paint block in
    // paintContent). Title strip just loses this 85px slot.
    bypassButton    .setBounds (titleArea.removeFromRight (85).reduced (6, 7));

    // --- footer strip (legend + tagline) --------------------------------
    bounds.removeFromBottom (28);

    // --- v0.28 K5 layout: CHANCE column | 7-col grid | MIX column ---
    // CHANCE and MIX are pulled out of the grid into solid-colour
    // flanking columns (painted in paintContent). The centre grid is
    // 7 wide × 4 rows with category stripes above each section.
    // v0.41 — three mutually-exclusive layout modes:
    //   modPageVisible       → MOD page overlay
    //   settingsPageVisible  → SettingsPage overlay
    //   neither              → standard knob grid + act bank
    // We force one to win if both are set (Settings wins, since the
    // user just opened it). The buttons themselves are radio-ish in
    // practice — clicking SETTINGS while MOD is open just opens
    // SETTINGS on top; the next resized() will re-evaluate.
    if (settingsPageVisible)
    {
        // Use the same vertical area MOD claims so the page sits in
        // the same window real-estate. 618 = knob strip (558) + acts (60).
        auto pageArea = bounds.removeFromTop (618);
        if (settingsPage)
        {
            settingsPage->setBounds (pageArea.reduced (16, 8));
            settingsPage->setVisible (true);
        }
        if (modPage) modPage->setVisible (false);

        for (const auto& kb : allKnobs())
        {
            kb.k->slider.setVisible (false);
            kb.k->label.setVisible (false);
        }
        for (auto& btn : sceneButtons)
            btn.setVisible (false);
        ioButton.setVisible (false);
        presetPrevButton.setVisible (false);
        presetNextButton.setVisible (false);
        presetNameLabel.setVisible (false);
    }
    else if (modPageVisible)
    {
        // Layout the MOD page overlay instead (knob grid + act bank area)
        auto modArea = bounds.removeFromTop (618);  // 558 (knobs) + 60 (acts)
        if (modPage)
        {
            modPage->setBounds (modArea.reduced (16, 8));
            modPage->setVisible (true);
            modPage->updateFromProcessor();
        }
        if (settingsPage) settingsPage->setVisible (false);

        // Hide all the knob controls and act buttons
        for (const auto& kb : allKnobs())
        {
            kb.k->slider.setVisible (false);
            kb.k->label.setVisible (false);
        }
        for (auto& btn : sceneButtons)
            btn.setVisible (false);
        ioButton.setVisible (false);
        presetPrevButton.setVisible (false);
        presetNextButton.setVisible (false);
        presetNameLabel.setVisible (false);
    }
    else
    {
        if (modPage)
            modPage->setVisible (false);
        if (settingsPage)
            settingsPage->setVisible (false);

        // Show all knob controls and act buttons
        for (const auto& kb : allKnobs())
        {
            kb.k->slider.setVisible (true);
            kb.k->label.setVisible (true);
        }
        for (auto& btn : sceneButtons)
            btn.setVisible (true);
        ioButton.setVisible (true);
        presetPrevButton.setVisible (true);
        presetNextButton.setVisible (true);
        presetNameLabel.setVisible (true);

        auto knobStrip = bounds.removeFromTop (558);
    const int flankW = 70;
    chanceColumnBounds = knobStrip.removeFromLeft (flankW);
    mixColumnBounds    = knobStrip.removeFromRight (flankW);
    auto knobArea      = knobStrip.reduced (8, 0).withTrimmedTop (6).withTrimmedBottom (6);
    {
        const int cols = 7;
        const int rows = 4;
        const int cellW = knobArea.getWidth() / cols;
        const int cellH = knobArea.getHeight() / rows;

        auto placeKnob = [&] (LabeledKnob& k, int col, int row)
        {
            auto cell = juce::Rectangle<int> (knobArea.getX() + col * cellW,
                                               knobArea.getY() + row * cellH,
                                               cellW, cellH).reduced (2, 0);
            cell.removeFromTop (34);   // category stripe gap
            // v0.36 — label rect OVERLAPS the slider's top padding so the
            // caption sits right against the knob circle. Slider keeps
            // its full remaining height (the knob is drawn centred inside
            // so the overlap only covers its empty top margin, not the
            // circle itself). centredBottom justification means text
            // hugs the overlap boundary ≈ top of the visible knob.
            // v0.37 — labelArea shortened 30→22 so the bottom-anchored
            // text sits ~8px higher, clear of the knob circle.
            auto labelArea = juce::Rectangle<int> (cell.getX(), cell.getY(),
                                                    cell.getWidth(), 22);
            k.label.setBounds (labelArea);
            k.slider.setBounds (cell);
        };

        // CHANCE — centred in its flanking column. Hide the horizontal
        // label (the painted vertical text IS the label).
        // Height 90 so the value text (painted at cy+radius+3) isn't clipped.
        {
            auto cc = chanceColumnBounds.reduced (4, 0);
            auto chSlider = cc.withSizeKeepingCentre (cc.getWidth(), 90);
            chSlider.translate (0, 60);   // push well below vertical text
            chance.slider.setBounds (chSlider);
            chance.label.setBounds (juce::Rectangle<int>());   // hide
        }

        // MIX — same treatment.
        {
            auto mc = mixColumnBounds.reduced (4, 0);
            auto mxSlider = mc.withSizeKeepingCentre (mc.getWidth(), 90);
            mxSlider.translate (0, 60);
            mix.slider.setBounds (mxSlider);
            mix.label.setBounds (juce::Rectangle<int>());   // hide
        }

        // Helper: place a knob using a custom column count (for rows with < 7 knobs).
        auto placeKnobFlex = [&] (LabeledKnob& k, int col, int row, int numCols)
        {
            const int flexW = knobArea.getWidth() / numCols;
            auto cell = juce::Rectangle<int> (knobArea.getX() + col * flexW,
                                               knobArea.getY() + row * cellH,
                                               flexW, cellH).reduced (2, 0);
            cell.removeFromTop (34);   // category stripe gap
            // v0.36 — label overlaps slider's top padding (see placeKnob comment).
            // v0.37 — labelArea 30→22 so bottom-anchored text sits ~8px higher.
            auto labelArea = juce::Rectangle<int> (cell.getX(), cell.getY(),
                                                    cell.getWidth(), 22);
            k.label.setBounds (labelArea);
            k.slider.setBounds (cell);
        };

        // Row 0 — SLICE (6 knobs, spread across full width)
        placeKnobFlex (lookback, 0, 0, 6);
        placeKnobFlex (division, 1, 0, 6);
        placeKnobFlex (rate,     2, 0, 6);
        placeKnobFlex (stretch,  3, 0, 6);
        placeKnobFlex (stutter,  4, 0, 6);
        placeKnobFlex (decay,    5, 0, 6);

        // Row 1 — PLAY (7 knobs)
        placeKnob (pitch,    0, 1);
        placeKnob (slide,    1, 1);
        placeKnob (reverse,  2, 1);
        placeKnob (judder,   3, 1);
        placeKnob (judderDiv,4, 1);
        placeKnob (gate,     5, 1);
        placeKnob (chaos,    6, 1);

        // Row 2 — COLOUR (7 knobs)
        placeKnob (drive,    0, 2);
        placeKnob (fold,     1, 2);
        placeKnob (ringMod,  2, 2);
        placeKnob (tape,     3, 2);
        placeKnob (feedback, 4, 2);
        placeKnob (res,      5, 2);
        placeKnob (tone,     6, 2);

        // Row 3 — CRUNCH (2) + SPACE (4), 6 knobs spread across full width
        placeKnobFlex (crunch,   0, 3, 6);
        placeKnobFlex (crushRate,1, 3, 6);
        placeKnobFlex (shimmer,  2, 3, 6);
        placeKnobFlex (smear,    3, 3, 6);
        placeKnobFlex (varispeed,4, 3, 6);
        placeKnobFlex (stereo,   5, 3, 6);

        // Category panels — stencil stripes (painted as skewed text + line).
        auto colSpanRect = [&] (int colStart, int colEnd, int row, int numCols = 7) {
            const int cw = knobArea.getWidth() / numCols;
            return juce::Rectangle<int> (knobArea.getX() + colStart * cw,
                                          knobArea.getY() + row * cellH,
                                          (colEnd - colStart + 1) * cw, cellH);
        };

        // v0.38 — each category carries a tooltip explaining WHY these
        // knobs live together. Shown when the mouse hovers over the
        // stencil label between knobs (knob-hover still shows per-knob
        // tooltips). Keep each string short — the tip is a one-line
        // "why this grouping" label, not a manual.
        // v0.39 - ASCII-only to match the per-knob tooltip constraint
        // noted in describeKnob(): JUCE's tooltip font has no UTF-8
        // fallback chain on macOS so em-dashes render as mojibake.
        // Each string describes the knobs that actually live in that
        // row, framed so the grouping reads as a coherent stage of
        // the signal's life rather than a taxonomic claim about what
        // each knob "really" is.  Feedback is the awkward one - it's
        // a loop-buffer re-inject which could be argued into SPACE,
        // but it sits next to Tone/Res physically and the honest
        // framing is that it builds DENSITY through the filter pair,
        // not spatial width.  Described here as such.
        categoryPanels[0] = { colSpanRect (0, 5, 0, 6),  Col::accent, "SLICE",
            "SLICE - picking the fragment.\n"
            "Lookback, Division and Rate choose WHERE and HOW\n"
            "BIG the grab is; Stretch expands it across time;\n"
            "Stutter retriggers it; Decay sets the level of each\n"
            "retrigger. Everything that shapes the raw slice\n"
            "before it gets voiced." };
        categoryPanels[1] = { colSpanRect (0, 6, 1, 7),  Col::accent, "PLAY",
            "PLAY - voicing the slice.\n"
            "Pitch and Slide set intonation; Reverse flips\n"
            "direction; Judder + Judder Div multiply the hit\n"
            "into rapid retriggers inside the slice; Gate\n"
            "shapes the amplitude envelope; Chaos sprays\n"
            "timing and pitch randomness per voice." };
        categoryPanels[2] = { colSpanRect (0, 6, 2, 7),  Col::accent, "COLOUR",
            "COLOUR - tonal character stamped onto the voice.\n"
            "Tone + Res form a resonant filter; Feedback loops\n"
            "the wet back into the grab buffer, building density\n"
            "through that filter into howl or drone; Drive\n"
            "saturates; Fold wavefolds; RingMod adds metallic\n"
            "sidebands; Tape adds wow and flutter. Everything\n"
            "that changes what the slice SOUNDS like." };
        categoryPanels[3] = { colSpanRect (0, 1, 3, 6),  Col::accent, "CRUNCH",
            "CRUNCH - bite and sizzle.\n"
            "Bits reduces word depth for a gritty, grainy\n"
            "buzz; Rate holds each sample longer for aliased,\n"
            "sizzling downsampling. Lo-fi degradation with\n"
            "character, not destruction. Reminiscent of\n"
            "vintage samplers. Check settings for more..." };
        categoryPanels[4] = { colSpanRect (2, 5, 3, 6),  Col::accent, "SPACE",
            "SPACE - physicality and motion.\n"
            "Shimmer is an octave-up reverb halo; Smear is a\n"
            "short delay blur; Varispeed is a per-slice tape-\n"
            "brake pitch dive; Stereo spreads the image.\n"
            "Everything that puts the voice in a room and\n"
            "pushes it around." };
        categoryPanels[5] = { {}, juce::Colour(), {}, {} };
        categoryPanels[6] = { {}, juce::Colour(), {}, {} };
        categoryPanels[7] = { {}, juce::Colour(), {}, {} };
    }

        // --- global mix slider --------------------------------------------
        {
            auto gmArea = bounds.removeFromTop (24);
            auto labelArea = gmArea.removeFromLeft (90);
            globalMixLabel.setBounds (labelArea.reduced (4, 2));
            globalMixSlider.setBounds (gmArea.reduced (8, 2));
        }

        // --- act bank -----------------------------------------------------
        auto sceneArea = bounds.removeFromTop (60);
        {
            auto header = sceneArea.withTrimmedTop (4).withHeight (24);
            ioButton.setBounds (header.removeFromRight (100).reduced (12, 4));
            // v0.30 — preset next/prev arrows sit to the left of ACTS ACTS,
            // with preset name label in between.
            presetNextButton.setBounds (header.removeFromRight (28).reduced (2, 4));
            presetNameLabel.setBounds (header.removeFromRight (120).reduced (4, 4));
            presetPrevButton.setBounds (header.removeFromRight (28).reduced (2, 4));

            auto content = sceneArea.withTrimmedTop (22).reduced (16, 6);

            const int slots = LoopSaboteurProcessor::kNumScenes;
            const int slotW = content.getWidth() / slots;

            for (int i = 0; i < slots; ++i)
            {
                auto slot = juce::Rectangle<int> (content.getX() + i * slotW,
                                                   content.getY(),
                                                   slotW, content.getHeight()).reduced (4, 0);

                // v0.10 — STORE button is gone, so the letter button gets
                // the full slot height for a chunkier, more tappable target.
                sceneButtons[i].setBounds (slot.reduced (2, 2));
            }
        }
    }

    // --- waveform strip ----------------------------------------------
    // When hidden (settings menu), reclaim the 36px and skip binding
    // any bounds — the component stays invisible and takes no room.
    if (waveformVisible)
    {
        auto waveArea = bounds.removeFromTop (28);
        waveformStrip.setBounds (waveArea.reduced (16, 4));
        waveformStrip.setVisible (true);
    }
    else
    {
        waveformStrip.setVisible (false);
    }

    // --- sequencer strip (header row + cells + global controls) -----
    auto stepArea = bounds.removeFromTop (132);
    {
        // v0.16 — header row layout, left-to-right:
        //   SEQ ON | A B C D | LENGTH: - N + | RANDOM | Clear | ... RATE | SWING
        // v0.42.5 — SEQ ON reclaimed the 120px leftmost slot (previously a
        //           decorative "SEQUENCER" stencil) after user testing.
        // Top padding so the row sits comfortably below the waveform/acts border.
        stepArea.removeFromTop (4);
        auto headerRow = stepArea.removeFromTop (30);

        // v0.42.5 — SEQ ON toggle lives at the leftmost 120px of the
        // sequencer header row. Sits comfortably clear of the yellow
        // EDIT LOCKS overlay (which paints starting at x≈610 when
        // lock-edit mode is active) and next to the page buttons /
        // length / clear controls it gates.
        seqOnButton.setBounds (headerRow.removeFromLeft (120).reduced (12, 2));

        auto hr = headerRow.reduced (4, 2);
        const int gap = 8;

        // PAGE: A B C D (no label)
        pageLabel.setBounds ({}); // hidden
        for (int p = 0; p < 4; ++p)
        {
            pageButtons[(size_t) p].setBounds (hr.removeFromLeft (26));
            hr.removeFromLeft (3);
        }
        hr.removeFromLeft (gap);

        // LENGTH: label - N +
        lengthLabel      .setBounds (hr.removeFromLeft (56));
        lengthMinusButton.setBounds (hr.removeFromLeft (22));
        lengthValueLabel .setBounds (hr.removeFromLeft (34));
        lengthPlusButton .setBounds (hr.removeFromLeft (22));
        hr.removeFromLeft (gap * 2);   // v0.30 — wider gap before RANDOM

        // RANDOM + Clear + Undo
        randomizeButton .setBounds (hr.removeFromLeft (76));
        hr.removeFromLeft (gap);
        clearAllButton  .setBounds (hr.removeFromLeft (50));
        hr.removeFromLeft (gap);
        undoButton      .setBounds (hr.removeFromLeft (46));
        hr.removeFromLeft (gap * 2);   // gap before EDIT LOCKS / RATE

        // Right side: RATE combo + SWING slider
        auto swingArea = hr.removeFromRight (160);
        swingLabel .setBounds (swingArea.removeFromLeft (44));
        swingSlider.setBounds (swingArea);

        hr.removeFromRight (gap);
        auto rateArea = hr.removeFromRight (100);
        seqRateLabel .setBounds (rateArea.removeFromLeft (36));
        seqRateCombo .setBounds (rateArea);

        // Cells get the full width below the header.
        auto content = stepArea.reduced (16, 6);

        // --- 32 visible step cells on the left ----------------------
        // We allocate kMaxSteps cells but only show one page at a
        // time. Invisible cells have their bounds left at whatever,
        // and setVisible(false) keeps them out of the hit-test.
        const int visibleCells = LoopSaboteurProcessor::kStepsPerPage;
        const int cellW = content.getWidth() / visibleCells;
        const int pageStart = currentPage * LoopSaboteurProcessor::kStepsPerPage;

        for (int i = 0; i < LoopSaboteurProcessor::kMaxSteps; ++i)
            stepCells[i].setVisible (false);

        for (int i = 0; i < visibleCells; ++i)
        {
            const int abs = pageStart + i;
            if (abs >= LoopSaboteurProcessor::kMaxSteps) break;
            auto cellR = juce::Rectangle<int> (content.getX() + i * cellW,
                                                content.getY(),
                                                cellW, content.getHeight()).reduced (1, 2);
            stepCells[abs].setBounds (cellR);
            stepCells[abs].setVisible (true);
        }
    }

    // (Settings page removed — using popup menu instead.)
}

// ============================================================================
//  Small refresh helpers
// ============================================================================
void LoopSaboteurEditor::refreshSceneButtons()
{
    const int sel = processorRef.getSelectedScene();
    for (int i = 0; i < LoopSaboteurProcessor::kNumScenes; ++i)
    {
        auto& b = sceneButtons[i];
        b.setToggleState (i == sel, juce::dontSendNotification);
        b.populated = processorRef.isSceneStored (i);
        b.repaint();
    }
}

void LoopSaboteurEditor::refreshLengthDisplay()
{
    lengthValueLabel.setText (juce::String (processorRef.getSeqLength()),
                              juce::dontSendNotification);
}

void LoopSaboteurEditor::refreshPageButton()
{
    const int seqLen = processorRef.getSeqLength();

    // Snap currentPage back FIRST if the sequence was shortened,
    // so the toggle/enable state below uses the correct page.
    const int maxPage = juce::jmax (0, (seqLen - 1) / LoopSaboteurProcessor::kStepsPerPage);
    const bool snapped = currentPage > maxPage;
    if (snapped)
        currentPage = maxPage;

    for (int p = 0; p < 4; ++p)
    {
        // Toggle: only the active page lights up.
        pageButtons[(size_t) p].setToggleState (p == currentPage,
                                                 juce::dontSendNotification);

        // v0.17 — enable page buttons based on sequence length.
        const int pageStart = p * LoopSaboteurProcessor::kStepsPerPage;
        const bool reachable = (seqLen > pageStart);
        pageButtons[(size_t) p].setEnabled (reachable);
        pageButtons[(size_t) p].setAlpha (reachable ? 1.0f : 0.35f);
    }

    // If we snapped, refresh the cell layout for the new page.
    if (snapped)
        pageChanged();
}

void LoopSaboteurEditor::pageChanged()
{
    refreshPageButton();
    // Re-layout the step cells with the new page. Cheap — only the
    // step strip bounds actually change.
    resized();
    // Cached playhead index is only valid for the previous page — let
    // the next timer tick refresh it for the new page.
    lastPlayingStep = -1;
    refreshStepCells();
    repaint();
}

void LoopSaboteurEditor::applyLengthDelta (int delta)
{
    // v0.9 — AUTO mode owns the length. Nudging the length manually is
    // a strong signal that the user wants to take back control, so we
    // flip AUTO off rather than fighting them for it.
    if (processorRef.getAutoFollowLength())
        processorRef.setAutoFollowLength (false);

    const int current = processorRef.getSeqLength();
    const int next    = juce::jlimit (2, LoopSaboteurProcessor::kMaxSteps,
                                      current + delta);
    if (next == current) return;
    processorRef.setSeqLength (next);
    for (int i = 0; i < LoopSaboteurProcessor::kMaxSteps; ++i)
        stepCells[i].setActive (i < next);
    refreshLengthDisplay();
}

void LoopSaboteurEditor::refreshStepCells()
{
    const int accent = processorRef.getGridAccentInterval();
    for (int i = 0; i < LoopSaboteurProcessor::kMaxSteps; ++i)
    {
        auto& cell = stepCells[i];
        cell.setSceneIndex (processorRef.getStepScene (i));
        cell.setRatioIndex (processorRef.getStepRatio (i));
        cell.setFreezeHold (processorRef.getStepFreeze (i));
        cell.setHasLocks   (processorRef.stepHasAnyLock (i));
        cell.setRatchet    (processorRef.getStepRatchet (i));  // v0.12
        cell.setMuted      (processorRef.getStepMute    (i));  // v0.13
        cell.setActive     (i < processorRef.getSeqLength());
        cell.setEditingLock(i == editLockStep);
        // v0.13 — accent-beat flag. accent=0 disables; otherwise every
        // Nth cell gets the brighter fill, landing on the LAST step of
        // each group (index 3, 7, 11, 15... for accent=4) so the stripes
        // read as bar-end markers — matching the feel Steve wanted from
        // the grid accent. Earlier versions highlighted index 0, 4, 8...
        cell.setAccentBeat (accent > 0 && ((i + 1) % accent) == 0);
    }
}

void LoopSaboteurEditor::refreshPlayhead()
{
    const int cur = processorRef.getCurrentPlayingStep();
    if (cur == lastPlayingStep)
        return;

    // v0.42.4 — REMOVED the old v0.18 "always flash step 0 on play
    // start" hack. The intent back then was to paper over a missed
    // step-0 tick if the timer woke up after the processor had
    // already advanced, but the side effect was that step 1 flashed
    // on EVERY press of play even when the DAW transport was
    // genuinely starting mid-pattern (e.g. resuming at step 13). The
    // visual lie was worse than the one frame of missed highlight.
    // Just track the processor's actual current step.
    if (lastPlayingStep >= 0 && lastPlayingStep < LoopSaboteurProcessor::kMaxSteps)
        stepCells[lastPlayingStep].setPlaying (false);
    if (cur >= 0 && cur < LoopSaboteurProcessor::kMaxSteps)
        stepCells[cur].setPlaying (true);

    lastPlayingStep = cur;
}

void LoopSaboteurEditor::timerCallback()
{
    // v0.40 — "Designed for X" Engine-recall badge management. The
    // processor stamps designedForLoadedAtMs whenever it loads a state
    // with an Engine block. We track the last seen stamp and (a) repaint
    // once when a new badge appears, and (b) clear+repaint after
    // kDesignedForTimeoutMs.
    {
        const auto loadedAt = processorRef.getDesignedForLoadedAtMs();
        if (loadedAt != lastSeenDesignedForLoadedAt)
        {
            lastSeenDesignedForLoadedAt = loadedAt;
            innerContent.repaint (designedForBadgeBounds);
        }
        if (processorRef.getDesignedForStage() >= 0
            && loadedAt > 0
            && juce::Time::currentTimeMillis() - loadedAt > kDesignedForTimeoutMs)
        {
            processorRef.clearDesignedFor();
            innerContent.repaint (designedForBadgeBounds);
        }
    }

    // v0.10 — page-follows-playhead. Flip the visible page BEFORE we
    // refresh the playhead so the newly-visible page paints the cell
    // highlighted from its first frame. We only flip if the processor
    // is actively reporting a current step (seq on + transport rolling)
    // and only if the step's owning page differs from the current page.
    if (processorRef.getPageFollowsPlayhead())
    {
        const int cur = processorRef.getCurrentPlayingStep();
        if (cur >= 0)
        {
            const int owningPage = cur / LoopSaboteurProcessor::kStepsPerPage;
            if (owningPage != currentPage
                && owningPage >= 0
                && owningPage < LoopSaboteurProcessor::kNumPages)
            {
                currentPage = owningPage;
                pageChanged();
            }
        }
    }

    refreshPlayhead();

    // v0.16 — keep page button enabled/greyed state up to date as
    // steps are stamped or cleared.
    refreshPageButton();

    // Waveform strip is polled at timer rate — repaint unconditionally
    // at 30 Hz. The component's paint() is cheap (iterates ~900
    // ringbuf samples and draws vertical lines), well under 1 ms.
    waveformStrip.repaint();

    // v0.33 — keep LFO-targeted cyan pointer indicators in sync with
    // current Act's LFO config. refreshLfoTargetMarkers only repaints
    // knobs whose flag actually changed, so this is cheap.
    refreshLfoTargetMarkers();

    // Also keep the SEQ toggle and selected-scene highlight in sync in
    // case state was loaded from disk or changed via host automation.
    const bool seq = processorRef.isSeqOn();
    if (seqOnButton.getToggleState() != seq)
        seqOnButton.setToggleState (seq, juce::dontSendNotification);

    const int sel = processorRef.getSelectedScene();
    if (! sceneButtons[sel].getToggleState())
        refreshSceneButtons();

    // v0.9 — AUTO-follow pattern length is applied from inside the
    // processor when the user stamps/clears a step, so the editor has
    // to watch the length atomic and refresh the cell-active flags and
    // the length readout when it drifts.
    const int curLen = processorRef.getSeqLength();
    if (curLen != lastSeenLength)
    {
        lastSeenLength = curLen;
        for (int i = 0; i < LoopSaboteurProcessor::kMaxSteps; ++i)
            stepCells[i].setActive (i < curLen);
        refreshLengthDisplay();
    }

    // Easter egg: LOOP PACIFIST. All 8 Acts have MIX at 0% →
    // fade the title over ~10 seconds. Snaps back quickly (~1 s).
    {
        // v0.5.0 — also trigger LOOP PACIFIST when Global Mix is at 0%.
        const float gMix = processorRef.apvts.getRawParameterValue (LoopSaboteurProcessor::kParamGlobalMix)->load();
        bool allMixZero = (gMix < 0.01f);
        if (! allMixZero)
        {
            allMixZero = true;
            for (int i = 0; i < LoopSaboteurProcessor::kNumScenes; ++i)
            {
                if (processorRef.getSceneMix (i) > 0.01f)
                { allMixZero = false; break; }
            }
        }
        // v0.41 — debug-menu force override snaps the fade straight to 1.0
        // for the duration so we don't have to wait the full ~10 s ramp.
        const bool isPacifist = allMixZero || isEggForced (EggPacifist);
        const float prevFade = pacifistFade;
        if (isEggForced (EggPacifist))
            pacifistFade = 1.0f;
        else if (isPacifist)
            pacifistFade = juce::jmin (1.0f, pacifistFade + (1.0f / 600.0f));
        else
            pacifistFade = juce::jmax (0.0f, pacifistFade - (1.0f / 60.0f));

        // Only repaint the header area when the fade is actively changing.
        if (pacifistFade != prevFade)
            repaint (0, 0, getWidth(), 46);
    }

    // Easter egg: end the mirror after 3 seconds.
    if (mirrorActive)
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (now >= mirrorEndMs)
        {
            mirrorActive = false;
            resized();
        }
    }

    // Easter egg: Dilla swing. When swing is exactly 54%, show "DILLA"
    // label on the SWING knob. Swing tick 54 defined a generation.
    // v0.35 — snap on/off exactly at 54% (no lingering timer). Matches the
    // tightened Paula behaviour on the CRUNCH category label.
    {
        const int swingPct = (int) std::round (swingSlider.getValue() * 100.0);
        const bool isDilla = (swingPct == 54) || isEggForced (EggDilla);
        if (isDilla != dillaActive)
        {
            dillaActive = isDilla;
            dillaEndMs  = 0.0;  // unused now; kept for state-file compatibility
            if (isDilla)
            {
                swingLabel.setText ("DILLA", juce::dontSendNotification);
                swingLabel.setColour (juce::Label::textColourId, Col::accent);
            }
            else
            {
                swingLabel.setText ("SWING", juce::dontSendNotification);
                swingLabel.setColour (juce::Label::textColourId, Col::textDim);
            }
        }
    }

    // Easter egg: producer tag expiry.
    if (producerTagActive)
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (now >= producerTagEndMs)
        {
            producerTagActive = false;
            innerContent.repaint (0, 0, innerContent.getWidth(), 70);
        }
    }

    // Easter egg: "AMEN OF SABOTAGE". Classic amen break = 6 hits in
    // a bar on steps 1, 3, 4, 5, 6 (step 2 empty). Check the first
    // 8 steps: acts on 1,3,4,5,6 and empty on 2,7,8.
    {
        const bool prevAmen = amenActive;
        bool amenPattern = true;
        // Steps with acts (0-indexed): 0, 2, 3, 4, 5
        // Steps empty (0-indexed): 1, 6, 7
        const int amenOn[]  = { 0, 2, 3, 4, 5 };
        const int amenOff[] = { 1, 6, 7 };
        for (int s : amenOn)
        {
            if (s >= LoopSaboteurProcessor::kMaxSteps) { amenPattern = false; break; }
            if (processorRef.getStepScene (s) < 0) { amenPattern = false; break; }
        }
        if (amenPattern)
        {
            for (int s : amenOff)
            {
                if (s >= LoopSaboteurProcessor::kMaxSteps) break;
                if (processorRef.getStepScene (s) >= 0) { amenPattern = false; break; }
            }
        }
        // Also require seq length <= 8 so it's a single-bar pattern.
        if (amenPattern && processorRef.getSeqLength() > 8)
            amenPattern = false;

        amenActive = amenPattern || isEggForced (EggAmen);
        if (amenActive != prevAmen)
            repaint();
    }

    // Easter egg: Paula. BITS knob showing 8-bit → CRUNCH becomes "PAULA".
    // v0.35 — tightened: only lit while the display reads exactly "8-bit".
    // As soon as the knob leaves that band the label snaps back to CRUNCH
    // (no 2 s fade). Matches the same math used by the slider's
    // displayOverride so on/off boundaries line up perfectly.
    {
        const float crunchVal = (float) crunch.slider.getValue();
        const bool  displayIsOff = (crunchVal < 0.001f);
        const int   bits         = displayIsOff ? 0 : (16 - (int) (crunchVal * 14.0f));
        const bool  showPaula    = ((! displayIsOff) && (bits == 8)) || isEggForced (EggPaula);
        if (showPaula != paulaActive)
        {
            paulaActive = showPaula;
            paulaEndMs  = 0.0;  // unused now; kept for state-file compatibility
            repaint();
        }
    }

    // Easter egg: Guru Meditation. All active steps muted → flashing
    // red "Guru Meditation" in the footer for 3 seconds.
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (! guruActive)
        {
            const int seqLen = processorRef.getSeqLength();
            bool allMuted = (seqLen > 0);
            for (int i = 0; i < seqLen && allMuted; ++i)
            {
                if (! processorRef.getStepMute (i))
                    allMuted = false;
            }
            if (allMuted)
            {
                guruActive = true;
                guruEndMs  = now + 3000.0;
                guruFlash  = true;
                repaint();
            }
        }
        else
        {
            // Blink at ~4 Hz (toggle every 8 timer ticks at 60 Hz).
            static int guruTickCount = 0;
            ++guruTickCount;
            if (guruTickCount % 8 == 0)
            {
                guruFlash = ! guruFlash;
                repaint (0, getHeight() - 40, getWidth(), 40);
            }
            if (now >= guruEndMs)
            {
                guruActive = false;
                guruFlash  = false;
                guruTickCount = 0;
                repaint();
            }
        }
    }

    // v0.41 — debug-menu force expiry: if the colour-swap was triggered by
    // a force toggle and the window has elapsed, swap the palette back to
    // normal. Other eggs handle expiry on their own (Mirror/ProducerTag/Guru
    // via their *EndMs timers; Pacifist/Dilla/Amen/Paula derive from
    // current params and revert on the next tick; birthday eggs are checked
    // inside StepCell::paint each frame).
    if (colourSwappedByForce && ! isEggForced (EggColourSwap))
    {
        if (colourSwapped)
        {
            colourSwapped = false;
            Col::swapToNormal();
            refreshAccentColours();
        }
        colourSwappedByForce = false;
    }

    // v0.30 — dim/brighten the UNDO button based on availability.
    const float undoAlpha = processorRef.hasUndo() ? 1.0f : 0.35f;
    if (std::abs (undoButton.getAlpha() - undoAlpha) > 0.01f)
        undoButton.setAlpha (undoAlpha);

    // BUG 1 fix: update preset label on timer tick so it reflects user edits
    // (when preset idx becomes -1 after knob motion)
    updatePresetNameLabel();
}

// ============================================================================
//  FINE pitch/slide mode
// ============================================================================
void LoopSaboteurEditor::applyPitchSlideFineMode()
{
    // v0.12 — previous versions tried to fight the APVTS SliderAttachment
    // by calling setRange(..., 1.0) on the slider. That didn't stick: the
    // attachment keeps restoring the parameter's own continuous
    // NormalisableRange and drags stayed cents-precise even with FINE
    // off. The new approach flips SnapSlider::snapWhole instead, which
    // hooks into Slider::snapValue() and rounds to the nearest semitone
    // for every mid-drag position. updateText() is still called so any
    // display already sitting between integer values redraws.
    pitch.slider.snapWhole = ! fineMode;
    slide.slider.snapWhole = ! fineMode;

    // v0.42 — the old force-round-on-disable block that used to live here
    // was removed so toggling FINE off doesn't clobber a fractional value
    // the user had already dialled in. snapWhole affects future drags
    // only; any existing cents-precise value stays put until the user
    // deliberately moves the knob, which is the behaviour the user
    // expects when treating FINE as a global "how does the knob behave
    // when I grab it" toggle rather than a destructive edit.

    pitch.slider.updateText();
    slide.slider.updateText();
}

// ============================================================================
//  Freeze mode (Toggle vs Momentary)
// ============================================================================
void LoopSaboteurEditor::applyFreezeMode()
{
    // v0.8 — freeze mode is now driven by the settings popup rather
    // than a dedicated button. The actual toggle/momentary plumbing
    // is unchanged: momentary mode detaches the APVTS attachment and
    // drives processorRef.setMomentaryFreeze on mouseDown/mouseUp;
    // toggle mode re-attaches the APVTS button attachment.
    if (freezeMomentary)
    {
        freezeAttachment.reset();
        freezeButton.setClickingTogglesState (false);
        freezeButton.setToggleState (false, juce::dontSendNotification);
        freezeButton.momentaryMode = true;
        freezeButton.onMomentary = [this] (bool down)
        {
            processorRef.setMomentaryFreeze (down);
        };
    }
    else
    {
        processorRef.setMomentaryFreeze (false);
        freezeButton.momentaryMode = false;
        freezeButton.onMomentary = nullptr;
        freezeButton.setClickingTogglesState (true);
        freezeAttachment = std::make_unique<ButtonAttachment> (
            processorRef.apvts, LoopSaboteurProcessor::kParamFreeze, freezeButton);
    }

    processorRef.setFreezeMomentaryMode (freezeMomentary);
}

// ============================================================================
//  Lock Edit mode — route all knob turns to processorRef.setStepLock
//  for a single target step, until the user shift-clicks again.
// ============================================================================
std::vector<LoopSaboteurEditor::KnobBinding> LoopSaboteurEditor::allKnobs()
{
    // Order here does not need to match kLockable in the processor —
    // lockableIndexForId does the slot lookup by parameter ID. We just
    // need to hit every per-Act knob exactly once.
    return {
        { &division, LoopSaboteurProcessor::kParamDivision },
        { &lookback, LoopSaboteurProcessor::kParamLookback },
        { &rate,     LoopSaboteurProcessor::kParamRate     },
        { &judder,   LoopSaboteurProcessor::kParamJudder   },
        { &judderDiv,LoopSaboteurProcessor::kParamJudderDiv},
        { &pitch,    LoopSaboteurProcessor::kParamPitch    },
        { &slide,    LoopSaboteurProcessor::kParamSlide    },
        { &decay,    LoopSaboteurProcessor::kParamDecay    },
        { &reverse,  LoopSaboteurProcessor::kParamReverse  },
        { &crunch,   LoopSaboteurProcessor::kParamCrunch   },
        { &crushRate,LoopSaboteurProcessor::kParamCrushRate},
        { &mix,      LoopSaboteurProcessor::kParamMix      },
        { &drive,    LoopSaboteurProcessor::kParamDrive    },
        { &tone,     LoopSaboteurProcessor::kParamTone     },
        { &feedback, LoopSaboteurProcessor::kParamFeedback },
        { &tape,     LoopSaboteurProcessor::kParamTape     },
        { &ringMod,  LoopSaboteurProcessor::kParamRingMod  },
        { &varispeed,LoopSaboteurProcessor::kParamVarispeed},
        { &stereo,   LoopSaboteurProcessor::kParamStereo   },
        { &stretch,  LoopSaboteurProcessor::kParamStretch  },
        { &shimmer,  LoopSaboteurProcessor::kParamShimmer  },
        { &res,      LoopSaboteurProcessor::kParamRes      },
        { &fold,     LoopSaboteurProcessor::kParamFold     },
        { &gate,     LoopSaboteurProcessor::kParamGate     },
        { &smear,    LoopSaboteurProcessor::kParamSmear    },
        { &stutter,  LoopSaboteurProcessor::kParamStutter  },
        { &chaos,    LoopSaboteurProcessor::kParamChaos    },
        { &chance,   LoopSaboteurProcessor::kParamChance   },
    };
}

void LoopSaboteurEditor::enterLockEditMode (int stepIdx)
{
    editLockStep = stepIdx;

    // Detach every per-Act APVTS attachment and point the slider at
    // the step's current lock value (or the APVTS value if unlocked).
    // Install an onValueChange lambda that writes to the step's lock
    // slot. We capture the slider pointer directly so each lambda
    // reads its own slider's value. v0.8 — also tag the LabeledKnob
    // lockDirty flag so its label can render in the highlight colour.
    for (auto& kb : allKnobs())
    {
        kb.k->attachment.reset();

        const int slot = LoopSaboteurProcessor::lockableIndexForId (kb.id);

        float v = processorRef.getStepLock (stepIdx, slot);
        const bool hadLock = ! std::isnan (v);
        if (! hadLock)
        {
            // Fall back to the current APVTS value — that's what the
            // knob was showing a moment ago and keeps turning it feel
            // continuous.
            if (auto* rp = processorRef.apvts.getRawParameterValue (kb.id))
                v = rp->load();
        }
        kb.k->slider.setValue (v, juce::dontSendNotification);
        kb.k->lockDirty = hadLock;

        auto* sliderPtr = &kb.k->slider;
        auto* knobPtr   = kb.k;
        kb.k->slider.onValueChange = [this, sliderPtr, knobPtr, slot]
        {
            if (editLockStep < 0) return;   // extra guard
            processorRef.setStepLock (editLockStep, slot,
                                      (float) sliderPtr->getValue());
            if (editLockStep >= 0 && editLockStep < LoopSaboteurProcessor::kMaxSteps)
                stepCells[editLockStep].setHasLocks (
                    processorRef.stepHasAnyLock (editLockStep));
            // Turning the knob installs a lock in that slot (the call
            // above writes even if the value matches the Act) so the
            // label should now read as dirty.
            knobPtr->lockDirty = true;
            refreshKnobLockDirty();
            repaint();   // v0.29 — flanking column bg is painted by the editor
        };
    }

    // v0.42.6 — global mix slider: same p-lock wiring as the rotary knobs.
    {
        globalMixAttachment.reset();
        const int gmSlot = LoopSaboteurProcessor::lockableIndexForId (
            LoopSaboteurProcessor::kParamGlobalMix);
        float v = processorRef.getStepLock (stepIdx, gmSlot);
        globalMixLockDirty = ! std::isnan (v);
        if (std::isnan (v))
            v = processorRef.apvts.getRawParameterValue (
                    LoopSaboteurProcessor::kParamGlobalMix)->load();
        globalMixSlider.setValue (v, juce::dontSendNotification);
        globalMixSlider.onValueChange = [this, gmSlot]
        {
            if (editLockStep < 0) return;
            processorRef.setStepLock (editLockStep, gmSlot,
                                      (float) globalMixSlider.getValue());
            if (editLockStep >= 0 && editLockStep < LoopSaboteurProcessor::kMaxSteps)
                stepCells[editLockStep].setHasLocks (
                    processorRef.stepHasAnyLock (editLockStep));
            globalMixLockDirty = true;
            refreshGlobalMixHighlight();
            repaint();
        };
    }

    refreshKnobLockDirty();
    refreshGlobalMixHighlight();
    refreshStepCells();
    repaint();
}

void LoopSaboteurEditor::exitLockEditMode()
{
    if (editLockStep < 0) return;

    editLockStep = -1;

    // Clear the onValueChange hook FIRST, then reattach the APVTS
    // attachment. The attachment's own callbacks will immediately
    // snap the slider back to the current parameter value — we don't
    // want those snaps to fire our lock-writing lambda.
    for (auto& kb : allKnobs())
    {
        kb.k->slider.onValueChange = nullptr;
        kb.k->attachment = std::make_unique<SliderAttachment> (
            processorRef.apvts, kb.id, kb.k->slider);
        kb.k->lockDirty = false;
    }

    // v0.42.6 — restore global mix slider attachment.
    globalMixSlider.onValueChange = nullptr;
    globalMixAttachment = std::make_unique<SliderAttachment> (
        processorRef.apvts, LoopSaboteurProcessor::kParamGlobalMix, globalMixSlider);
    globalMixLockDirty = false;

    refreshKnobLockDirty();
    refreshGlobalMixHighlight();
    refreshStepCells();
    repaint();
}

void LoopSaboteurEditor::refreshAccentColours()
{
    // Re-push Col::accent and Col::lockDirty to every component that
    // caches them via setColour(). Called when the colour-swap easter
    // egg toggles the palette.
    for (auto& kb : allKnobs())
    {
        if (! kb.k->lockDirty)
        {
            kb.k->slider.setColour (juce::Slider::rotarySliderFillColourId, Col::accent);
            kb.k->slider.setColour (juce::Slider::thumbColourId,            Col::accent);
        }
        else
        {
            kb.k->slider.setColour (juce::Slider::rotarySliderFillColourId, Col::lockDirty);
        }
        kb.k->slider.repaint();
    }
    // MIX arc is accent when not dirty.
    if (! mix.lockDirty)
        mix.slider.setColour (juce::Slider::rotarySliderFillColourId, Col::accent);

    // Swing track.
    swingSlider.setColour (juce::Slider::trackColourId, Col::accent);
    swingSlider.repaint();

    // Act buttons — their paintButton reads hardcoded hex, but the
    // selected state uses 0xffff5a3c. We update buttonOnColourId so
    // JUCE picks it up, though ActButton::paintButton uses its own
    // literal. We'll repaint them and they'll read Col::accent via
    // the refreshSceneButtons path.
    for (auto& sb : sceneButtons)
    {
        sb.setColour (juce::TextButton::buttonOnColourId, Col::accent);
        sb.repaint();
    }

    // Header buttons.
    clearAllButton.setColour (juce::TextButton::buttonOnColourId, Col::accent);
    clearAllButton.repaint();
    undoButton.setColour (juce::TextButton::buttonOnColourId, Col::accent);
    undoButton.repaint();
    randomizeButton.setColour (juce::TextButton::buttonOnColourId, Col::accent);
    randomizeButton.repaint();

    // Page buttons.
    for (auto& pb : pageButtons)
    {
        pb.setColour (juce::TextButton::buttonOnColourId, Col::accent);
        pb.repaint();
    }

    // Length label outline.
    lengthValueLabel.setColour (juce::Label::outlineWhenEditingColourId, Col::accent);

    // Force full repaint of all children.
    innerContent.repaint();
}

// ============================================================================
//  v0.41 — Debug-menu egg test toggles
//  Each call sets a 15 s force window for the named egg and immediately
//  triggers the visual side-effects (palette swap, mirror transform, etc.)
//  so the test action is observable on the very next paint without needing
//  to wait for a parameter-driven timer tick.
// ============================================================================
void LoopSaboteurEditor::forceEggOn (int e)
{
    if (e < 0 || e >= NumEggs) return;

    const juce::int64 nowMs = juce::Time::currentTimeMillis();
    eggForceUntilMs[e] = nowMs + 15000;

    switch (e)
    {
        case EggPacifist:
            pacifistFade = 1.0f;
            repaint (0, 0, getWidth(), 46);
            break;

        case EggMirror:
            mirrorActive = true;
            mirrorEndMs  = juce::Time::getMillisecondCounterHiRes() + 15000.0;
            resized();
            break;

        case EggColourSwap:
            if (! colourSwapped)
            {
                colourSwapped        = true;
                colourSwappedByForce = true;
                Col::swapToYellow();
                refreshAccentColours();
            }
            break;

        case EggProducerTag:
            producerTagActive = true;
            producerTagEndMs  = juce::Time::getMillisecondCounterHiRes() + 15000.0;
            innerContent.repaint();
            break;

        case EggDilla:
        case EggAmen:
        case EggPaula:
            // These are derived from current params on each timer tick;
            // the OR with isEggForced() inside the timer picks up the
            // override on the next pass. Force a repaint to make the
            // first frame show up immediately.
            repaint();
            break;

        case EggGuru:
            guruActive = true;
            guruEndMs  = juce::Time::getMillisecondCounterHiRes() + 15000.0;
            guruFlash  = true;
            repaint();
            break;

        case Egg303:
            StepCell::force303UntilMs = nowMs + 15000;
            innerContent.repaint();   // v0.42.4 — tag lives in header now
            break;

        case Egg606:
            StepCell::force606UntilMs = nowMs + 15000;
            innerContent.repaint();
            break;

        case Egg808:
            StepCell::force808UntilMs = nowMs + 15000;
            innerContent.repaint();
            break;

        case EggRat:
            // v0.42.5 — Debug-menu force. spawnRatRun() is a no-op if a
            // rat is already running, so spamming the menu entry won't
            // stack rats — it'll just wait for the current one to finish.
            spawnRatRun();
            break;

        default:
            break;
    }
}

void LoopSaboteurEditor::showKnobModAssignMenu (LabeledKnob& k)
{
    // v0.38 — right-click on any knob pops this menu. We show four MOD
    // items (MOD 1..4) that each route the currently-selected Act's
    // corresponding LFO at this knob's lockable slot. A final "Clear"
    // item removes every MOD on the current Act that's pointing at
    // this slot, which matches most users' expectation for "get rid of
    // it". Existing MODs on OTHER Acts are left alone — each Act's MOD
    // bank is independent.
    const juce::String paramId = k.paramId;
    const int slot = LoopSaboteurProcessor::lockableIndexForId (paramId);
    if (slot < 0)
        return;  // knob has no lockable route; menu would be useless

    const int act = processorRef.getSelectedScene();
    const juce::String actLetter = juce::String::charToString ((char) ('A' + act));

    juce::PopupMenu m;
    // v0.39 - ASCII arrow (was \u2192) + stencilLF to match every
    // other popup menu in the plugin. The mod-assign menu was the
    // only one that rendered in JUCE's default system style, which
    // looked out of place next to the stencilled ACTS / settings /
    // step-cell menus.
    m.addSectionHeader (k.label.getText() + "  ->  Act " + actLetter);

    for (int li = 0; li < LoopSaboteurProcessor::kNumLfosPerAct; ++li)
    {
        const int currentTarget = processorRef.getLfoTargetSlot (act, li);
        const bool already = (currentTarget == slot);
        m.addItem ("Assign to MOD " + juce::String (li + 1),
                   true, already,
                   [this, act, li, slot]
                   {
                       processorRef.setLfoTargetSlot (act, li, slot);
                       // Repaint so the cyan body indicator kicks in
                       // immediately without waiting for the 30 Hz timer.
                       refreshLfoTargetMarkers();
                   });
    }

    m.addSeparator();
    m.addItem ("Clear all MODs on this knob (Act " + actLetter + ")",
               true, false,
               [this, act, slot]
               {
                   for (int li = 0; li < LoopSaboteurProcessor::kNumLfosPerAct; ++li)
                   {
                       if (processorRef.getLfoTargetSlot (act, li) == slot)
                           processorRef.setLfoTargetSlot (act, li, -1);
                   }
                   refreshLfoTargetMarkers();
               });

    // ── v0.5.0 — Crunch options on Bits / Rate knobs ──────────────
    if (paramId == LoopSaboteurProcessor::kParamCrunch
        || paramId == LoopSaboteurProcessor::kParamCrushRate)
    {
        m.addSeparator();
        m.addSectionHeader ("Crunch Options (Act " + actLetter + ")");

        const bool aa = processorRef.getActCrushAA (act);
        m.addItem ("Anti-alias", true, aa,
                   [this, act, aa]
                   {
                       processorRef.setActCrushAA (act, ! aa);
                       repaint();
                   });

        const bool mu = processorRef.getActCrushMu (act);
        m.addItem ("Mu-law companding", true, mu,
                   [this, act, mu]
                   {
                       processorRef.setActCrushMu (act, ! mu);
                       repaint();
                   });

        m.addSeparator();
        m.addSectionHeader ("Interpolation");

        const int curInterp = processorRef.getActInterpMode (act);
        for (int im = 0; im < (int) loopsab::kNumInterpModes; ++im)
        {
            juce::String label (loopsab::interpShortName (im));
            label += "  —  ";
            label += loopsab::interpDescription (im);

            m.addItem (label, true, (curInterp == im),
                       [this, act, im]
                       {
                           processorRef.setActInterpMode (act, im);
                           repaint();
                       });
        }
    }

    // ── v0.5.0 — Filter mode on Tone / Res knobs ─────────────────
    if (paramId == LoopSaboteurProcessor::kParamTone
        || paramId == LoopSaboteurProcessor::kParamRes)
    {
        m.addSeparator();
        m.addSectionHeader ("Filter Type (Act " + actLetter + ")");

        const int curFilter = processorRef.getActFilterMode (act);
        for (int fm = 0; fm < (int) loopsab::kNumFilterModes; ++fm)
        {
            juce::String label (loopsab::filterShortName (fm));
            label += "  —  ";
            label += loopsab::filterDescription (fm);

            m.addItem (label, true, (curFilter == fm),
                       [this, act, fm]
                       {
                           processorRef.setActFilterMode (act, fm);
                           repaint();
                       });
        }

        m.addSeparator();
        const bool dryTone = processorRef.getActFxOnDryTone (act);
        m.addItem ("Apply to dry signal", true, dryTone,
                   [this, act, dryTone]
                   {
                       processorRef.setActFxOnDryTone (act, ! dryTone);
                       repaint();
                   });
    }

    // ── v0.5.0 — Drive type on Drive knob ────────────────────────
    if (paramId == LoopSaboteurProcessor::kParamDrive)
    {
        m.addSeparator();
        m.addSectionHeader ("Drive Type (Act " + actLetter + ")");

        const int cur = processorRef.getActDriveType (act);
        for (int i = 0; i < (int) loopsab::kNumDriveTypes; ++i)
        {
            juce::String label (loopsab::driveShortName (i));
            label += "  —  ";
            label += loopsab::driveDescription (i);

            m.addItem (label, true, (cur == i),
                       [this, act, i]
                       {
                           processorRef.setActDriveType (act, i);
                           repaint();
                       });
        }

        m.addSeparator();
        const bool dryDrive = processorRef.getActFxOnDryDrive (act);
        m.addItem ("Apply to dry signal", true, dryDrive,
                   [this, act, dryDrive]
                   {
                       processorRef.setActFxOnDryDrive (act, ! dryDrive);
                       repaint();
                   });
    }

    // ── v0.5.0 — Fold topology on Fold knob ─────────────────────
    if (paramId == LoopSaboteurProcessor::kParamFold)
    {
        m.addSeparator();
        m.addSectionHeader ("Fold Shape (Act " + actLetter + ")");

        const int cur = processorRef.getActFoldTopology (act);
        for (int i = 0; i < (int) loopsab::kNumFoldTopologies; ++i)
        {
            juce::String label (loopsab::foldShortName (i));
            label += "  —  ";
            label += loopsab::foldDescription (i);

            m.addItem (label, true, (cur == i),
                       [this, act, i]
                       {
                           processorRef.setActFoldTopology (act, i);
                           repaint();
                       });
        }

        m.addSeparator();
        const bool dryFold = processorRef.getActFxOnDryFold (act);
        m.addItem ("Apply to dry signal", true, dryFold,
                   [this, act, dryFold]
                   {
                       processorRef.setActFxOnDryFold (act, ! dryFold);
                       repaint();
                   });
    }

    // ── v0.5.0 — Shimmer octave on Shimmer knob ─────────────────
    if (paramId == LoopSaboteurProcessor::kParamShimmer)
    {
        m.addSeparator();
        m.addSectionHeader ("Shimmer Interval (Act " + actLetter + ")");

        const int cur = processorRef.getActShimmerOctave (act);
        for (int i = 0; i < (int) loopsab::kNumShimmerOctaves; ++i)
        {
            juce::String label (loopsab::shimmerShortName (i));
            label += "  —  ";
            label += loopsab::shimmerDescription (i);

            m.addItem (label, true, (cur == i),
                       [this, act, i]
                       {
                           processorRef.setActShimmerOctave (act, i);
                           repaint();
                       });
        }
    }

    // ── v0.5.0 — Smear character on Smear knob ──────────────────
    if (paramId == LoopSaboteurProcessor::kParamSmear)
    {
        m.addSeparator();
        m.addSectionHeader ("Smear Character (Act " + actLetter + ")");

        const int cur = processorRef.getActSmearCharacter (act);
        for (int i = 0; i < (int) loopsab::kNumSmearCharacters; ++i)
        {
            juce::String label (loopsab::smearShortName (i));
            label += "  —  ";
            label += loopsab::smearDescription (i);

            m.addItem (label, true, (cur == i),
                       [this, act, i]
                       {
                           processorRef.setActSmearCharacter (act, i);
                           repaint();
                       });
        }
    }

    // ── v0.5.0 — Stutter window on Stutter knob ─────────────────
    if (paramId == LoopSaboteurProcessor::kParamStutter)
    {
        m.addSeparator();
        m.addSectionHeader ("Stutter Window (Act " + actLetter + ")");

        const int cur = processorRef.getActStutterWindow (act);
        for (int i = 0; i < (int) loopsab::kNumStutterWindows; ++i)
        {
            juce::String label (loopsab::stutterShortName (i));
            label += "  —  ";
            label += loopsab::stutterDescription (i);

            m.addItem (label, true, (cur == i),
                       [this, act, i]
                       {
                           processorRef.setActStutterWindow (act, i);
                           repaint();
                       });
        }
    }

    // ── v0.5.0 — Varispeed curve on Varispeed knob ──────────────
    if (paramId == LoopSaboteurProcessor::kParamVarispeed)
    {
        m.addSeparator();
        m.addSectionHeader ("Brake Curve (Act " + actLetter + ")");

        const int cur = processorRef.getActVarispeedCurve (act);
        for (int i = 0; i < (int) loopsab::kNumVarispeedCurves; ++i)
        {
            juce::String label (loopsab::varispeedShortName (i));
            label += "  —  ";
            label += loopsab::varispeedDescription (i);

            m.addItem (label, true, (cur == i),
                       [this, act, i]
                       {
                           processorRef.setActVarispeedCurve (act, i);
                           repaint();
                       });
        }
    }

    // ── v0.5.0 — Slide curve on Slide knob ──────────────────────
    if (paramId == LoopSaboteurProcessor::kParamSlide)
    {
        m.addSeparator();
        m.addSectionHeader ("Slide Curve (Act " + actLetter + ")");

        const int cur = processorRef.getActSlideCurve (act);
        for (int i = 0; i < (int) loopsab::kNumSlideCurves; ++i)
        {
            juce::String label (loopsab::slideShortName (i));
            label += "  —  ";
            label += loopsab::slideDescription (i);

            m.addItem (label, true, (cur == i),
                       [this, act, i]
                       {
                           processorRef.setActSlideCurve (act, i);
                           repaint();
                       });
        }
    }

    // ── v0.5.0 — Reverse mode on Reverse knob ───────────────────
    if (paramId == LoopSaboteurProcessor::kParamReverse)
    {
        m.addSeparator();
        m.addSectionHeader ("Reverse Mode (Act " + actLetter + ")");

        const int cur = processorRef.getActReverseMode (act);
        for (int i = 0; i < (int) loopsab::kNumReverseModes; ++i)
        {
            juce::String label (loopsab::reverseShortName (i));
            label += "  —  ";
            label += loopsab::reverseDescription (i);

            m.addItem (label, true, (cur == i),
                       [this, act, i]
                       {
                           processorRef.setActReverseMode (act, i);
                           repaint();
                       });
        }
    }

    // ── v0.5.0 — Stereo mode on Stereo knob ─────────────────────
    if (paramId == LoopSaboteurProcessor::kParamStereo)
    {
        m.addSeparator();
        m.addSectionHeader ("Stereo Mode (Act " + actLetter + ")");

        const int cur = processorRef.getActStereoMode (act);
        auto addStereoItem = [this, &m, cur, act] (const juce::String& label, int mode)
        {
            m.addItem (label, true, (cur == mode),
                       [this, act, mode]
                       {
                           processorRef.setActStereoMode (act, mode);
                           repaint();
                       });
        };
        addStereoItem ("Random",    0);
        addStereoItem ("Ping-Pong", 1);
        addStereoItem ("Auto-Pan",  2);
        addStereoItem ("Haas",      3);
    }

    // ── v0.5.0 — Ring mod quantise on Ring Mod knob ──────────────
    if (paramId == LoopSaboteurProcessor::kParamRingMod)
    {
        m.addSeparator();
        m.addSectionHeader ("Ring Mod (Act " + actLetter + ")");

        const bool quant = processorRef.getActRingModQuant (act);
        m.addItem ("Quantise to scale", true, quant,
                   [this, act, quant]
                   {
                       processorRef.setActRingModQuant (act, ! quant);
                       repaint();
                   });

        m.addSeparator();
        m.addSectionHeader ("Ring Mod Waveform (Act " + actLetter + ")");

        const int curWave = processorRef.getActRingModWave (act);
        for (int i = 0; i < (int) loopsab::kNumRingModWaves; ++i)
        {
            juce::String label (loopsab::ringModWaveShortName (i));
            label += "  —  ";
            label += loopsab::ringModWaveDescription (i);

            m.addItem (label, true, (curWave == i),
                       [this, act, i]
                       {
                           processorRef.setActRingModWave (act, i);
                           repaint();
                       });
        }

        m.addSeparator();
        const bool dryRing = processorRef.getActFxOnDryRingMod (act);
        m.addItem ("Apply to dry signal", true, dryRing,
                   [this, act, dryRing]
                   {
                       processorRef.setActFxOnDryRingMod (act, ! dryRing);
                       repaint();
                   });
    }

    // ── v0.5.0 — Tape mode on Tape knob ──────────────────────────
    if (paramId == LoopSaboteurProcessor::kParamTape)
    {
        m.addSeparator();
        m.addSectionHeader ("Tape Mode (Act " + actLetter + ")");

        const int cur = processorRef.getActTapeMode (act);
        for (int i = 0; i < (int) loopsab::kNumTapeModes; ++i)
        {
            juce::String label (loopsab::tapeShortName (i));
            label += "  —  ";
            label += loopsab::tapeDescription (i);

            m.addItem (label, true, (cur == i),
                       [this, act, i]
                       {
                           processorRef.setActTapeMode (act, i);
                           repaint();
                       });
        }
    }

    // ── v0.5.0 — Chaos distribution on Chaos knob ───────────────
    if (paramId == LoopSaboteurProcessor::kParamChaos)
    {
        m.addSeparator();
        m.addSectionHeader ("Chaos Distribution (Act " + actLetter + ")");

        const int cur = processorRef.getActChaosDistribution (act);
        for (int i = 0; i < (int) loopsab::kNumChaosDistributions; ++i)
        {
            juce::String label (loopsab::chaosShortName (i));
            label += "  —  ";
            label += loopsab::chaosDescription (i);

            m.addItem (label, true, (cur == i),
                       [this, act, i]
                       {
                           processorRef.setActChaosDistribution (act, i);
                           repaint();
                       });
        }
    }

    // ── v0.5.0 — Feedback character on Feedback knob ────────────
    if (paramId == LoopSaboteurProcessor::kParamFeedback)
    {
        m.addSeparator();
        m.addSectionHeader ("Feedback Character (Act " + actLetter + ")");

        const int cur = processorRef.getActFeedbackCharacter (act);
        for (int i = 0; i < (int) loopsab::kNumFeedbackCharacters; ++i)
        {
            juce::String label (loopsab::feedbackShortName (i));
            label += "  —  ";
            label += loopsab::feedbackDescription (i);

            m.addItem (label, true, (cur == i),
                       [this, act, i]
                       {
                           processorRef.setActFeedbackCharacter (act, i);
                           repaint();
                       });
        }
    }

    // ── v0.5.0 — Stretch mode on Stretch knob ─────────────────────
    if (paramId == LoopSaboteurProcessor::kParamStretch)
    {
        m.addSeparator();
        m.addSectionHeader ("Stretch Mode (Act " + actLetter + ")");

        const int cur = processorRef.getActStretchMode (act);
        for (int i = 0; i < (int) loopsab::kNumStretchModes; ++i)
        {
            juce::String label (loopsab::stretchShortName (i));
            label += "  —  ";
            label += loopsab::stretchDescription (i);

            m.addItem (label, true, (cur == i),
                       [this, act, i]
                       {
                           processorRef.setActStretchMode (act, i);
                           repaint();
                       });
        }
    }

    // ── v0.5.0 — Judder shape on Judder knob ───────────────────
    if (paramId == LoopSaboteurProcessor::kParamJudder)
    {
        m.addSeparator();
        m.addSectionHeader ("Judder Shape (Act " + actLetter + ")");

        const int cur = processorRef.getActJudderShape (act);
        for (int i = 0; i < (int) loopsab::kNumJudderShapes; ++i)
        {
            juce::String label (loopsab::judderShortName (i));
            label += "  —  ";
            label += loopsab::judderDescription (i);

            m.addItem (label, true, (cur == i),
                       [this, act, i]
                       {
                           processorRef.setActJudderShape (act, i);
                           repaint();
                       });
        }
    }

    // ── v0.6.0 — Lookback behaviour on Lookback knob ────────────
    if (paramId == LoopSaboteurProcessor::kParamLookback)
    {
        m.addSeparator();
        m.addSectionHeader ("Lookback Behaviour (Act " + actLetter + ")");

        const int cur = processorRef.getActLookbackBehaviour (act);
        for (int i = 0; i < (int) loopsab::kNumLookbackBehaviours; ++i)
        {
            juce::String label (loopsab::lookbackShortName (i));
            label += "  —  ";
            label += loopsab::lookbackDescription (i);

            m.addItem (label, true, (cur == i),
                       [this, act, i]
                       {
                           processorRef.setActLookbackBehaviour (act, i);
                           repaint();
                       });
        }
    }

    // ── v0.6.0 — Decay curve on Decay knob ──────────────────────
    if (paramId == LoopSaboteurProcessor::kParamDecay)
    {
        m.addSeparator();
        m.addSectionHeader ("Decay Curve (Act " + actLetter + ")");

        const int cur = processorRef.getActDecayCurve (act);
        for (int i = 0; i < (int) loopsab::kNumDecayCurves; ++i)
        {
            juce::String label (loopsab::decayShortName (i));
            label += "  —  ";
            label += loopsab::decayDescription (i);

            m.addItem (label, true, (cur == i),
                       [this, act, i]
                       {
                           processorRef.setActDecayCurve (act, i);
                           repaint();
                       });
        }
    }

    // ── v0.5.0 — Fine-tune mode on Pitch knob ───────────────────
    if (paramId == LoopSaboteurProcessor::kParamPitch)
    {
        m.addSeparator();
        m.addSectionHeader ("Pitch (Global)");

        m.addItem ("Fine-tune mode  (cents)", true, fineMode,
                   [this]
                   {
                       fineMode = ! fineMode;
                       applyPitchSlideFineMode();
                   });
    }

    // Apply stencil look-and-feel + anchor at the slider so the menu
    // appears at the knob.
    m.setLookAndFeel (&stencilLF);
    m.showMenuAsync (juce::PopupMenu::Options()
                         .withTargetComponent (&k.slider));
}

void LoopSaboteurEditor::refreshLfoTargetMarkers()
{
    // v0.33 — draw the pointer line in cyan on knobs whose param is being
    // modulated by an active LFO on the currently-selected Act. We stash
    // a component property on each slider that drawRotarySlider reads.
    // Only repaint when the flag changed, to avoid flicker.
    const int act = processorRef.getSelectedScene();
    for (auto& kb : allKnobs())
    {
        const int slot = LoopSaboteurProcessor::lockableIndexForId (kb.id);
        const bool targeted = (slot >= 0)
            && processorRef.anyLfoTargetsSlot (act, slot);
        auto& props = kb.k->slider.getProperties();
        const bool prev = (bool) props.getWithDefault ("lfoTargeted", false);
        if (prev != targeted)
        {
            props.set ("lfoTargeted", targeted);
            kb.k->slider.repaint();
        }
    }

    // v0.42.6 — global mix slider: cyan track when LFO-targeted.
    refreshGlobalMixHighlight();
}

void LoopSaboteurEditor::refreshGlobalMixHighlight()
{
    // Determine the track colour for the global mix horizontal slider.
    // Priority: yellow (p-lock dirty) > cyan (LFO targeted) > accent (default).
    const int act = processorRef.getSelectedScene();
    const int gmSlot = LoopSaboteurProcessor::lockableIndexForId (
        LoopSaboteurProcessor::kParamGlobalMix);

    const bool lfoTargeted = (gmSlot >= 0)
        && processorRef.anyLfoTargetsSlot (act, gmSlot);
    const bool plockDirty  = globalMixLockDirty;

    auto trackCol = plockDirty  ? Col::lockDirty   // yellow
                  : lfoTargeted ? Col::modCyan      // cyan
                  :               Col::accent;      // default orange

    globalMixSlider.setColour (juce::Slider::trackColourId, trackCol);
    globalMixLabel.setColour (juce::Label::textColourId,
                              plockDirty  ? Col::lockDirty
                            : lfoTargeted ? Col::modCyan
                            :               Col::textDim);
    globalMixSlider.repaint();
    globalMixLabel.repaint();
}

void LoopSaboteurEditor::refreshKnobLockDirty()
{
    // Paint the label in the yellow "lockDirty" colour when the knob
    // currently represents a step-local override, otherwise back to the
    // dim default caption colour. Called from enter/exit/onValueChange.
    // v0.9 — used to be Col::accent (orange) but Steve couldn't read
    // that over the "bad idea" orange palette, so we now use a
    // saturated yellow.
    for (auto& kb : allKnobs())
    {
        const bool dirty = kb.k->lockDirty;
        const bool isChance = (kb.k == &chance);
        const bool isMix    = (kb.k == &mix);

        kb.k->label.setColour (juce::Label::textColourId,
                                dirty ? Col::lockDirty
                                      : juce::Colour::fromRGB (185, 185, 195));

        // Preserve flanking knobs' special arc colours (CHANCE=white, MIX=orange).
        auto defaultFill = isChance ? Col::textBright
                         : Col::accent;
        kb.k->slider.setColour (juce::Slider::rotarySliderFillColourId,
                                 dirty ? Col::lockDirty : defaultFill);

        // Flanking knobs ALWAYS keep dark value text (black on yellow bg);
        // grid knobs flip to yellow when dirty.
        auto defaultTextCol = (isChance || isMix)
                            ? juce::Colour (0xff181818)
                            : Col::textBright;
        kb.k->slider.setColour (juce::Slider::textBoxTextColourId,
                                 (dirty && !isChance && !isMix)
                                     ? Col::lockDirty : defaultTextCol);

        kb.k->label.repaint();
        kb.k->slider.repaint();
    }
}

// ============================================================================
//  Act I/O — export/import single or all Acts as .lpsbact / .lpsbbank XML
// ============================================================================
void LoopSaboteurEditor::updatePresetNameLabel()
{
    // BUG 1 fix: read preset idx from processor for the currently selected scene
    const int selectedScene = processorRef.getSelectedScene();
    const int presetIdx = processorRef.getScenePresetIdx (selectedScene);

    juce::String resolvedName;
    if (presetIdx >= 0 && presetIdx < LoopSaboteurProcessor::getNumActPresets())
        resolvedName = LoopSaboteurProcessor::getActPreset (presetIdx).name;
    else
        // BUG 2 fix: use "No preset" (plain ASCII) instead of "—" (em-dash) to avoid UTF-8 garbling
        resolvedName = "No Preset";

    presetNameLabel.setText (resolvedName, juce::dontSendNotification);
}

void LoopSaboteurEditor::applyPresetDelta (int delta)
{
    const int numPresets = LoopSaboteurProcessor::getNumActPresets();
    if (numPresets == 0) return;

    // BUG 1 fix: read current scene's preset idx from processor
    const int sel = processorRef.getSelectedScene();
    const int currentPresetIdx = processorRef.getScenePresetIdx (sel);

    // Wrap around in both directions.
    int next = currentPresetIdx + delta;
    if (next < 0)            next = numPresets - 1;
    if (next >= numPresets)   next = 0;

    processorRef.applyActPreset (sel, next);
    processorRef.loadSceneToKnobs (sel);
    refreshSceneButtons();
    updatePresetNameLabel();
}

void LoopSaboteurEditor::showActIoMenu()
{
    juce::PopupMenu m;
    const int selected = processorRef.getSelectedScene();
    const juce::String letter (juce::String::charToString ((juce::juce_wchar) ('A' + selected)));

    // v0.13 — Preset banks. Steve asked for named "character" starting
    // points under the Acts Acts button: one click loads values like
    // "Jungle Stretch" or "Machine Gun" into the current Act (or fills
    // every slot for the "bank" variant).
    m.addSectionHeader ("PRESETS");
    {
        // v0.7.0 — user presets first, factory in a subfolder.
        juce::PopupMenu presetCurrent;
        const int numPresets = LoopSaboteurProcessor::getNumActPresets();
        const int numUser = processorRef.getNumUserPresets();

        // --- User presets (top level, with sub-categories) ---------------
        {
            // Build a lambda for the per-user-preset sub-entry (load / reveal / delete).
            auto buildUserEntry = [this, &letter] (int u) -> juce::PopupMenu
            {
                const juce::String name = processorRef.getUserPresetName (u);
                juce::PopupMenu entry;
                entry.addItem ("Load into act " + letter,
                               [this, u]
                               {
                                   const int sel = processorRef.getSelectedScene();
                                   if (processorRef.applyUserPreset (sel, u))
                                   {
                                       processorRef.loadSceneToKnobs (sel);
                                       refreshSceneButtons();
                                       updatePresetNameLabel();
                                   }
                               });
                entry.addSeparator();
                entry.addItem ("Reveal file",
                               [this, u]
                               {
                                   processorRef.getUserPresetFile (u).revealToUser();
                               });
                const auto presetFile = processorRef.getUserPresetFile (u);
                entry.addItem ("Delete",
                               [this, presetFile, name]
                               {
                                   juce::AlertWindow::showAsync (
                                       juce::MessageBoxOptions()
                                           .withIconType (juce::MessageBoxIconType::WarningIcon)
                                           .withTitle ("Delete user preset")
                                           .withMessage ("Delete \"" + name + "\" permanently?")
                                           .withButton ("Delete")
                                           .withButton ("Cancel"),
                                       [this, presetFile] (int result)
                                       {
                                           if (result == 1 && presetFile.existsAsFile())
                                           {
                                               presetFile.deleteFile();
                                               processorRef.rescanUserPresets();
                                           }
                                       });
                               });
                return entry;
            };

            // Collect user sub-categories.
            juce::StringArray userCats;
            for (int u = 0; u < numUser; ++u)
            {
                const auto cat = processorRef.getUserPresetCategory (u);
                if (cat.isNotEmpty() && ! userCats.contains (cat))
                    userCats.add (cat);
            }
            userCats.sort (true);

            // Root-level user presets (no category).
            for (int u = 0; u < numUser; ++u)
            {
                if (processorRef.getUserPresetCategory (u).isNotEmpty()) continue;
                const juce::String name = processorRef.getUserPresetName (u);
                presetCurrent.addSubMenu (name, buildUserEntry (u));
            }

            // Sub-category submenus with rename/delete.
            for (const auto& cat : userCats)
            {
                juce::PopupMenu catMenu;
                for (int u = 0; u < numUser; ++u)
                {
                    if (processorRef.getUserPresetCategory (u) != cat) continue;
                    const juce::String name = processorRef.getUserPresetName (u);
                    catMenu.addSubMenu (name, buildUserEntry (u));
                }
                catMenu.addSeparator();
                catMenu.addItem ("Rename category...",
                                 [this, cat]
                                 {
                                     auto* w = new juce::AlertWindow ("Rename category",
                                         "Enter a new name for \"" + cat + "\":",
                                         juce::MessageBoxIconType::NoIcon);
                                     w->addTextEditor ("newName", cat, "New name:");
                                     w->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
                                     w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                                     w->enterModalState (true,
                                         juce::ModalCallbackFunction::create (
                                             [this, w, cat] (int result)
                                             {
                                                 if (result == 1)
                                                     processorRef.renameUserPresetCategory (
                                                         cat, w->getTextEditorContents ("newName"));
                                                 delete w;
                                             }), false);
                                 });
                catMenu.addItem ("Delete category (move presets to root)",
                                 [this, cat]
                                 {
                                     processorRef.deleteUserPresetCategory (cat);
                                 });
                presetCurrent.addSubMenu (cat, catMenu);
            }

            if (numUser == 0)
                presetCurrent.addItem ("(no user presets yet)", false, false, [] {});
        }

        presetCurrent.addSeparator();

        // --- Factory presets in a subfolder -------------------------------
        {
            juce::PopupMenu factoryMenu;

            juce::StringArray categories;
            for (int p = 0; p < numPresets; ++p)
            {
                const juce::String cat (LoopSaboteurProcessor::getActPreset (p).category);
                if (! categories.contains (cat))
                    categories.add (cat);
            }

            if (categories.size() <= 1)
            {
                for (int p = 0; p < numPresets; ++p)
                {
                    const auto& preset = LoopSaboteurProcessor::getActPreset (p);
                    factoryMenu.addItem (juce::String (preset.name),
                                         [this, p]
                                         {
                                             const int sel = processorRef.getSelectedScene();
                                             processorRef.applyActPreset (sel, p);
                                             processorRef.loadSceneToKnobs (sel);
                                             refreshSceneButtons();
                                             updatePresetNameLabel();
                                         });
                }
            }
            else
            {
                for (const auto& cat : categories)
                {
                    juce::PopupMenu catMenu;
                    for (int p = 0; p < numPresets; ++p)
                    {
                        const auto& preset = LoopSaboteurProcessor::getActPreset (p);
                        if (juce::String (preset.category) != cat) continue;
                        catMenu.addItem (juce::String (preset.name),
                                         [this, p]
                                         {
                                             const int sel = processorRef.getSelectedScene();
                                             processorRef.applyActPreset (sel, p);
                                             processorRef.loadSceneToKnobs (sel);
                                             refreshSceneButtons();
                                             updatePresetNameLabel();
                                         });
                    }
                    factoryMenu.addSubMenu (cat, catMenu);
                }
            }

            presetCurrent.addSubMenu ("Factory (" + juce::String (numPresets) + ")", factoryMenu);
        }

        m.addSubMenu ("Load preset into act (" + letter + ")...", presetCurrent);

        // v0.7.0 — styled save dialog with category picker.
        m.addItem ("Save act " + letter + " as user preset...",
                   [this, letter] { showSavePresetDialog ("Act " + letter); });

        // v0.30 — "Fill all 8" removed per Steve's request.
    }

    m.addSeparator();
    m.addSectionHeader ("EXPORT");
    m.addItem ("Export current act (" + letter + ")...", [this] { exportCurrentAct(); });
    m.addItem ("Export all 8 acts...",                   [this] { exportAllActs();    });
    m.addSeparator();
    m.addSectionHeader ("IMPORT");
    m.addItem ("Import into current act (" + letter + ")...", [this] { importToCurrentAct(); });
    m.addItem ("Import entire bank...",                       [this] { importAllActs();      });
    m.addSeparator();
    m.addSectionHeader ("RESET");
    m.addItem ("Clear all 8 acts",
               [this]
               {
                   if (editLockStep >= 0) exitLockEditMode();
                   processorRef.resetAllScenesToDefaults();
                   // Reload the currently-selected Act so the knobs
                   // snap to the freshly-cleaned state.
                   processorRef.loadSceneToKnobs (processorRef.getSelectedScene());
                   refreshSceneButtons();
               });
    // v0.38 — tick-item toggle that controls whether an Act clear
    // (either per-Act or the "Clear all 8 acts" above) also wipes the
    // 4 MODs wired into that Act. Default is OFF (i.e. MODs DO get
    // cleared) because most workflows treat an Act + its MODs as a
    // single unit. Tick it to keep your modulation rigs alive while
    // you rebuild the Act knobs.
    {
        const bool preserve = processorRef.getPreserveLfosOnClear();
        m.addItem ("Preserve MODs when clearing acts",
                   true,      // enabled
                   preserve,  // ticked
                   [this, preserve]
                   {
                       processorRef.setPreserveLfosOnClear (! preserve);
                   });
    }

    m.setLookAndFeel (&stencilLF);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (ioButton));
}

// v0.5.0 — clicking the preset name label opens just the preset browser.
// v0.7.0 — user presets are shown first (at the top level, with sub-
// categories as submenus). Factory presets are tucked into a "Factory"
// subfolder at the bottom so users' own sounds take priority.
void LoopSaboteurEditor::showPresetLoadMenu()
{
    juce::PopupMenu m;
    const int numPresets = LoopSaboteurProcessor::getNumActPresets();

    // --- 1. User presets (top level) -----------------------------------
    {
        const int numUser = processorRef.getNumUserPresets();

        // Collect user categories (sub-folders).
        juce::StringArray userCats;
        for (int u = 0; u < numUser; ++u)
        {
            const auto cat = processorRef.getUserPresetCategory (u);
            if (cat.isNotEmpty() && ! userCats.contains (cat))
                userCats.add (cat);
        }
        userCats.sort (true);

        // Root-level user presets (no category).
        for (int u = 0; u < numUser; ++u)
        {
            if (processorRef.getUserPresetCategory (u).isNotEmpty()) continue;
            const juce::String name = processorRef.getUserPresetName (u);
            m.addItem (name,
                       [this, u]
                       {
                           const int sel = processorRef.getSelectedScene();
                           if (processorRef.applyUserPreset (sel, u))
                           {
                               processorRef.loadSceneToKnobs (sel);
                               refreshSceneButtons();
                               updatePresetNameLabel();
                           }
                       });
        }

        // Sub-category submenus with rename/delete at the bottom.
        for (const auto& cat : userCats)
        {
            juce::PopupMenu catMenu;
            for (int u = 0; u < numUser; ++u)
            {
                if (processorRef.getUserPresetCategory (u) != cat) continue;
                const juce::String name = processorRef.getUserPresetName (u);
                catMenu.addItem (name,
                                 [this, u]
                                 {
                                     const int sel = processorRef.getSelectedScene();
                                     if (processorRef.applyUserPreset (sel, u))
                                     {
                                         processorRef.loadSceneToKnobs (sel);
                                         refreshSceneButtons();
                                         updatePresetNameLabel();
                                     }
                                 });
            }
            catMenu.addSeparator();
            catMenu.addItem ("Rename category...",
                             [this, cat]
                             {
                                 auto* w = new juce::AlertWindow ("Rename category",
                                     "Enter a new name for \"" + cat + "\":",
                                     juce::MessageBoxIconType::NoIcon);
                                 w->addTextEditor ("newName", cat, "New name:");
                                 w->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
                                 w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
                                 w->enterModalState (true,
                                     juce::ModalCallbackFunction::create (
                                         [this, w, cat] (int result)
                                         {
                                             if (result == 1)
                                                 processorRef.renameUserPresetCategory (
                                                     cat, w->getTextEditorContents ("newName"));
                                             delete w;
                                         }), false);
                             });
            catMenu.addItem ("Delete category (move presets to root)",
                             [this, cat]
                             {
                                 processorRef.deleteUserPresetCategory (cat);
                             });
            m.addSubMenu (cat, catMenu);
        }

        if (numUser == 0)
            m.addItem ("(no user presets yet)", false, false, [] {});
    }

    m.addSeparator();

    // --- 2. Factory presets in a "Factory" subfolder -------------------
    {
        juce::PopupMenu factoryMenu;

        // Collect unique factory categories.
        juce::StringArray categories;
        for (int p = 0; p < numPresets; ++p)
        {
            const juce::String cat (LoopSaboteurProcessor::getActPreset (p).category);
            if (! categories.contains (cat))
                categories.add (cat);
        }

        if (categories.size() <= 1)
        {
            for (int p = 0; p < numPresets; ++p)
            {
                const auto& preset = LoopSaboteurProcessor::getActPreset (p);
                factoryMenu.addItem (juce::String (preset.name),
                                     [this, p]
                                     {
                                         const int sel = processorRef.getSelectedScene();
                                         processorRef.applyActPreset (sel, p);
                                         processorRef.loadSceneToKnobs (sel);
                                         refreshSceneButtons();
                                         updatePresetNameLabel();
                                     });
            }
        }
        else
        {
            for (const auto& cat : categories)
            {
                juce::PopupMenu catMenu;
                for (int p = 0; p < numPresets; ++p)
                {
                    const auto& preset = LoopSaboteurProcessor::getActPreset (p);
                    if (juce::String (preset.category) != cat) continue;
                    catMenu.addItem (juce::String (preset.name),
                                     [this, p]
                                     {
                                         const int sel = processorRef.getSelectedScene();
                                         processorRef.applyActPreset (sel, p);
                                         processorRef.loadSceneToKnobs (sel);
                                         refreshSceneButtons();
                                         updatePresetNameLabel();
                                     });
                }
                factoryMenu.addSubMenu (cat, catMenu);
            }
        }

        m.addSubMenu ("Factory (" + juce::String (numPresets) + ")", factoryMenu);
    }

    m.setLookAndFeel (&stencilLF);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (presetNameLabel));
}

// v0.7.0 — themed preset save overlay with category combo box.
void LoopSaboteurEditor::showSavePresetDialog (const juce::String& defaultName)
{
    if (! savePresetOverlay)
    {
        savePresetOverlay = std::make_unique<SavePresetOverlay>();
        savePresetOverlay->setLookAndFeel (&stencilLF);
        addChildComponent (*savePresetOverlay);
    }

    // Populate with existing categories.
    savePresetOverlay->configure (defaultName, processorRef.getUserPresetCategories());
    savePresetOverlay->setBounds (getLocalBounds());
    savePresetOverlay->setVisible (true);
    savePresetOverlay->toFront (true);

    savePresetOverlay->onSave = [this] (const juce::String& name, const juce::String& cat)
    {
        const int sel = processorRef.getSelectedScene();
        processorRef.saveCurrentActAsUserPreset (sel, name, cat);
        savePresetOverlay->setVisible (false);
        updatePresetNameLabel();
    };
    savePresetOverlay->onCancel = [this]
    {
        savePresetOverlay->setVisible (false);
    };
}

void LoopSaboteurEditor::exportCurrentAct()
{
    const int idx = processorRef.getSelectedScene();
    auto xml = processorRef.serializeAct (idx);
    if (xml == nullptr) return;

    const juce::String letter (juce::String::charToString ((juce::juce_wchar) ('A' + idx)));
    activeChooser = std::make_unique<juce::FileChooser> (
        "Export act " + letter,
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
            .getChildFile ("LoopSaboteur_Act_" + letter + ".lpsbact"),
        "*.lpsbact");

    activeChooser->launchAsync (
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, xmlHolder = std::shared_ptr<juce::XmlElement> (std::move (xml))] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            if (f.getFileExtension().isEmpty())
                f = f.withFileExtension ("lpsbact");
            xmlHolder->writeTo (f, juce::XmlElement::TextFormat());
        });
}

void LoopSaboteurEditor::exportAllActs()
{
    auto xml = processorRef.serializeAllActs();
    if (xml == nullptr) return;

    activeChooser = std::make_unique<juce::FileChooser> (
        "Export all acts",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
            .getChildFile ("LoopSaboteur_Bank.lpsbbank"),
        "*.lpsbbank");

    activeChooser->launchAsync (
        juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
            | juce::FileBrowserComponent::warnAboutOverwriting,
        [this, xmlHolder = std::shared_ptr<juce::XmlElement> (std::move (xml))] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            if (f.getFileExtension().isEmpty())
                f = f.withFileExtension ("lpsbbank");
            xmlHolder->writeTo (f, juce::XmlElement::TextFormat());
        });
}

void LoopSaboteurEditor::importToCurrentAct()
{
    const int idx = processorRef.getSelectedScene();

    activeChooser = std::make_unique<juce::FileChooser> (
        "Import act file",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
        "*.lpsbact;*.xml");

    activeChooser->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this, idx] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File() || ! f.existsAsFile()) return;
            if (auto el = juce::XmlDocument::parse (f))
            {
                if (processorRef.loadActFromXml (*el, idx))
                {
                    processorRef.loadSceneToKnobs (idx);
                    refreshSceneButtons();
                }
            }
        });
}

void LoopSaboteurEditor::importAllActs()
{
    activeChooser = std::make_unique<juce::FileChooser> (
        "Import act bank",
        juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
        "*.lpsbbank;*.xml");

    activeChooser->launchAsync (
        juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
        [this] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File() || ! f.existsAsFile()) return;
            if (auto el = juce::XmlDocument::parse (f))
            {
                if (processorRef.loadAllActsFromXml (*el))
                {
                    // Re-pull the currently selected act into the knobs
                    // so the user sees what just arrived.
                    processorRef.loadSceneToKnobs (processorRef.getSelectedScene());
                    refreshSceneButtons();
                }
            }
        });
}

// ============================================================================
//  v0.8 — Settings popup, help CallOutBox, UI scale, step cell menu
// ============================================================================
// v0.41 — SETTINGS button click handler. Alt+click → debug menu.
// Plain click → toggle the SettingsPage (mirrors MOD page behaviour).
// The legacy showSettingsMenu() popup is still defined below so it can
// be used as a fallback / for screenshots / by future contextual access
// — but the SETTINGS button no longer routes to it on the primary path.
void LoopSaboteurEditor::toggleSettingsPage()
{
    if (juce::ModifierKeys::currentModifiers.isAltDown())
    {
        // Restore button toggle state since we won't actually open the page.
        settingsPageVisible = false;
        settingsButton.setToggleState (false, juce::dontSendNotification);
        showDebugMenu();
        return;
    }

    settingsPageVisible = settingsButton.getToggleState();
    // v0.42.2 — mutual exclusion with the MOD page. See the matching
    // block in modButton.onClick for the other direction.
    if (settingsPageVisible && modPageVisible)
    {
        modPageVisible = false;
        modButton.setToggleState (false, juce::dontSendNotification);
    }
    if (settingsPageVisible && ! settingsPage)
    {
        loopsab::SettingsHooks hooks;
        hooks.getFineMode         = [this] { return fineMode; };
        hooks.setFineMode         = [this] (bool v) { fineMode = v; applyPitchSlideFineMode(); };
        hooks.getFreezeMomentary  = [this] { return freezeMomentary; };
        hooks.setFreezeMomentary  = [this] (bool v) { freezeMomentary = v; applyFreezeMode(); };
        hooks.onAccentChanged     = [this] { refreshStepCells(); repaint(); };
        hooks.onWaveformVisChanged= [this] { waveformVisible = processorRef.getWaveformVisible();
                                              resized(); repaint(); };
        hooks.applyTooltipsEnabled= [this] (bool v)
        {
            if (v)
            {
                tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 800);
                tooltipWindow->setLookAndFeel (&stencilLF);
            }
            else
                tooltipWindow.reset();
        };
        hooks.resetUiScale        = [this] { applyUiScale (1.0f); };

        settingsPage = std::make_unique<loopsab::SettingsPage> (&processorRef, std::move (hooks));
        settingsPage->buildControls();
        settingsPage->applyStencilLookAndFeel (&stencilLF);
        // v0.41.1 — insert BEHIND the header buttons so the SETTINGS button
        // stays clickable (same way MOD page sits behind its title strip).
        // addAndMakeVisible appends to the child list, which also pushes the
        // component to the front in paint/hit-test order; we then bring the
        // title-strip buttons back in front so they always receive clicks.
        innerContent.addAndMakeVisible (*settingsPage);
        bypassButton   .toFront (false);
        seqOnButton    .toFront (false);
        freezeButton   .toFront (false);
        modButton      .toFront (false);
        settingsButton .toFront (false);
        helpButton     .toFront (false);
    }
    if (settingsPageVisible && settingsPage)
    {
        // v0.41.2 — mirror the MOD page behaviour: land on the tab for the
        // Act the user is currently editing so per-Act settings are shown
        // for the scene that's actually playing.
        settingsPage->setCurrentActTab (processorRef.getSelectedScene());
        settingsPage->updateFromProcessor();
    }
    resized();
}

void LoopSaboteurEditor::showSettingsMenu()
{
    // v0.30 — Option+click opens the hidden debug menu instead.
    if (juce::ModifierKeys::currentModifiers.isAltDown())
    {
        showDebugMenu();
        return;
    }

    juce::PopupMenu m;

    // --- GRID ACCENT --------------------------------------------------
    // v0.11 — OctaMed-style. 0 means "never accent", everything else
    // is "highlight every Nth step" so the user can see divisions at
    // a glance while stamping. Stored on the processor so it persists
    // across sessions.
    m.addSectionHeader ("GRID ACCENT");
    {
        const int cur = processorRef.getGridAccentInterval();
        auto addAccentItem = [this, &m, cur] (const juce::String& label, int n)
        {
            m.addItem (label, true, cur == n,
                       [this, n]
                       {
                           processorRef.setGridAccentInterval (n);
                           refreshStepCells();
                           repaint();
                       });
        };
        addAccentItem ("Off",         0);
        addAccentItem ("Every 2",     2);
        addAccentItem ("Every 3",     3);
        addAccentItem ("Every 4",     4);
        addAccentItem ("Every 6",     6);
        addAccentItem ("Every 8",     8);
    }

    // --- TRANSPORT ----------------------------------------------------
    // v0.11 — playback direction for the sequencer. processBlock uses
    // this atomic to remap curSeqStepAbs before indexing into the step
    // table, so the audio thread sees the change without any locking.
    m.addSeparator();
    m.addSectionHeader ("SEQUENCER TRANSPORT");
    {
        const int cur = processorRef.getSeqTransport();
        auto addTransportItem = [this, &m, cur] (const juce::String& label, int t)
        {
            m.addItem (label, true, cur == t,
                       [this, t] { processorRef.setSeqTransport (t); });
        };
        addTransportItem ("Forward",    LoopSaboteurProcessor::kTransportForward);
        addTransportItem ("Reverse",    LoopSaboteurProcessor::kTransportReverse);
        addTransportItem ("Ping-Pong",  LoopSaboteurProcessor::kTransportPingPong);
        addTransportItem ("Random",     LoopSaboteurProcessor::kTransportRandom);
    }

    // --- FREEZE MODE --------------------------------------------------
    m.addSeparator();
    m.addSectionHeader ("FREEZE MODE");
    m.addItem ("Toggle (latching)",  true, ! freezeMomentary,
               [this]
               {
                   if (freezeMomentary)
                   {
                       freezeMomentary = false;
                       applyFreezeMode();
                   }
               });
    m.addItem ("Momentary (hold)",   true,   freezeMomentary,
               [this]
               {
                   if (! freezeMomentary)
                   {
                       freezeMomentary = true;
                       applyFreezeMode();
                   }
               });

    // --- PITCH / SLIDE ------------------------------------------------
    // v0.11 — FINE mode moved off the title strip into this menu. When
    // ticked PITCH/SLIDE drag with 0.01 st resolution; otherwise they
    // snap to whole semitones.
    m.addSeparator();
    m.addSectionHeader ("PITCH / SLIDE");
    m.addItem ("Fine-tune mode  (cents)", true, fineMode,
               [this]
               {
                   fineMode = ! fineMode;
                   applyPitchSlideFineMode();
               });

    // --- SCALE QUANTISE -----------------------------------------------
    // v0.14 — moved from the sequencer header row into this menu to
    // make room for the horizontal Swing slider.
    m.addSeparator();
    m.addSectionHeader ("SCALE QUANTISE");
    {
        // Root submenu
        juce::PopupMenu rootMenu;
        auto* rootParam = dynamic_cast<juce::AudioParameterChoice*> (
            processorRef.apvts.getParameter (LoopSaboteurProcessor::kParamScaleRoot));
        if (rootParam)
        {
            const int curRoot = rootParam->getIndex();
            for (int i = 0; i < (int) rootParam->choices.size(); ++i)
            {
                const int idx = i;
                rootMenu.addItem (rootParam->choices[i], true, curRoot == idx,
                                  [rootParam, idx] { rootParam->beginChangeGesture();
                                                     rootParam->setValueNotifyingHost (
                                                         (float) idx / (float) (rootParam->choices.size() - 1));
                                                     rootParam->endChangeGesture(); });
            }
        }
        m.addSubMenu ("Root", rootMenu);

        // Scale type submenu
        juce::PopupMenu scaleMenu;
        auto* scaleParam = dynamic_cast<juce::AudioParameterChoice*> (
            processorRef.apvts.getParameter (LoopSaboteurProcessor::kParamScaleType));
        if (scaleParam)
        {
            const int curScale = scaleParam->getIndex();
            for (int i = 0; i < (int) scaleParam->choices.size(); ++i)
            {
                const int idx = i;
                scaleMenu.addItem (scaleParam->choices[i], true, curScale == idx,
                                   [scaleParam, idx] { scaleParam->beginChangeGesture();
                                                       scaleParam->setValueNotifyingHost (
                                                           (float) idx / (float) (scaleParam->choices.size() - 1));
                                                       scaleParam->endChangeGesture(); });
            }
        }
        m.addSubMenu ("Scale", scaleMenu);
    }

    // --- ENGINE -----------------------------------------------------------
    // v0.40 — Global character bundle: Output Stage (post-mix), Output Mix
    // (wet/dry of the stage), Interpolation (slice playback), and the two
    // CRUNCH toggles below. Saved as a unit in presets and recallable via
    // the "Presets recall Engine" toggle further down.
    m.addSeparator();
    m.addSectionHeader ("ENGINE - OUTPUT STAGE");
    {
        juce::PopupMenu stages;
        const int curStage = processorRef.getOutputStageMode();
        for (int i = 0; i < (int) loopsab::kNumOutputStages; ++i)
        {
            juce::String label;
            label << loopsab::outputStageShortName (i);
            if (i != loopsab::kOutClean)
                label << "  -  " << loopsab::outputStageDescription (i);
            stages.addItem (label, true, curStage == i,
                            [this, i] { processorRef.setOutputStageMode (i); });
        }
        m.addSubMenu ("Output stage:  " + juce::String (loopsab::outputStageShortName (curStage)), stages);
    }
    // Output Mix as a 5-position radio submenu (no front-panel chrome added).
    {
        juce::PopupMenu mixMenu;
        const float curMix = processorRef.getOutputStageMix();
        auto addMix = [this, &mixMenu, curMix] (const juce::String& label, float v)
        {
            const bool tick = std::abs (curMix - v) < 0.05f;
            mixMenu.addItem (label, true, tick,
                             [this, v] { processorRef.setOutputStageMix (v); });
        };
        addMix ("0%   (off)",  0.00f);
        addMix ("25%",         0.25f);
        addMix ("50%",         0.50f);
        addMix ("75%",         0.75f);
        addMix ("100%  (full)", 1.00f);
        m.addSubMenu ("Output mix:  " + juce::String (juce::roundToInt (curMix * 100)) + "%", mixMenu);
    }

    m.addSeparator();
    m.addSectionHeader ("ENGINE - INTERPOLATION");
    {
        const int curInterp = processorRef.getInterpMode();
        auto addInterp = [this, &m, curInterp] (int i)
        {
            juce::String label;
            label << loopsab::interpShortName (i)
                  << "  -  " << loopsab::interpDescription (i);
            m.addItem (label, true, curInterp == i,
                       [this, i] { processorRef.setInterpMode (i); });
        };
        addInterp (loopsab::kInterpLinear);
        addInterp (loopsab::kInterpDrop);
        addInterp (loopsab::kInterpCubic);
    }

    // --- CRUNCH -----------------------------------------------------------
    // v0.22 — anti-alias and mu-law companding are optional post-
    // processing inside the BITS/RATE crunch engine. v0.40 — these are
    // now part of the ENGINE bundle for preset recall.
    m.addSeparator();
    m.addSectionHeader ("ENGINE - CRUNCH");
    m.addItem ("Anti-alias filter  -  smooths bit-reduction aliasing",
               true, processorRef.getCrushAntiAlias(),
               [this]
               {
                   processorRef.setCrushAntiAlias (! processorRef.getCrushAntiAlias());
               });
    m.addItem ("Mu-law companding  -  S950-style soft quantisation",
               true, processorRef.getCrushMuLaw(),
               [this]
               {
                   processorRef.setCrushMuLaw (! processorRef.getCrushMuLaw());
               });
    m.addItem ("Apply to all audio (not just slices)",
               true, processorRef.getCrushAllAudio(),
               [this]
               {
                   processorRef.setCrushAllAudio (! processorRef.getCrushAllAudio());
               });

    // --- RING MOD --------------------------------------------------------
    m.addSeparator();
    m.addSectionHeader ("RING MOD");
    m.addItem ("Quantise to scale", true, processorRef.getRingModQuantise(),
               [this]
               {
                   processorRef.setRingModQuantise (! processorRef.getRingModQuantise());
               });

    // --- STEREO MODE -----------------------------------------------------
    // v0.27 — four stereo spread modes, selectable as radio items.
    m.addSeparator();
    m.addSectionHeader ("STEREO");
    {
        const int curMode = processorRef.getStereoMode();
        auto addStereoItem = [this, &m, curMode] (const juce::String& label, int mode)
        {
            m.addItem (label, true, curMode == mode,
                       [this, mode] { processorRef.setStereoMode (mode); });
        };
        addStereoItem ("Random",    LoopSaboteurProcessor::kStereoRandom);
        addStereoItem ("Ping-Pong", LoopSaboteurProcessor::kStereoPingPong);
        addStereoItem ("Auto-Pan",  LoopSaboteurProcessor::kStereoAutoPan);
        addStereoItem ("Haas",      LoopSaboteurProcessor::kStereoHaas);
    }

    // --- GLOBAL FX -------------------------------------------------------
    // v0.25 — when ticked, the effect is applied to the entire master
    // output, not just the wet slice. Useful for colouring the full mix.
    m.addSeparator();
    {
        juce::PopupMenu fxSub;

        // v0.30 — "All" toggle: enables or disables every FX on dry at once.
        const bool allOn = processorRef.getGlobalDrive()
                        && processorRef.getGlobalTone()
                        && processorRef.getGlobalRingMod()
                        && processorRef.getGlobalFold();
        fxSub.addItem ("All", true, allOn,
                        [this, allOn]
                        {
                            const bool v = ! allOn;
                            processorRef.setGlobalDrive     (v);
                            processorRef.setGlobalTone      (v);
                            processorRef.setGlobalRingMod   (v);
                            processorRef.setGlobalFold      (v);
                        });
        fxSub.addSeparator();

        fxSub.addItem ("Drive",     true, processorRef.getGlobalDrive(),
                        [this] { processorRef.setGlobalDrive    (! processorRef.getGlobalDrive()); });
        fxSub.addItem ("Tone",      true, processorRef.getGlobalTone(),
                        [this] { processorRef.setGlobalTone     (! processorRef.getGlobalTone()); });
        fxSub.addItem ("Ring Mod",  true, processorRef.getGlobalRingMod(),
                        [this] { processorRef.setGlobalRingMod  (! processorRef.getGlobalRingMod()); });
        fxSub.addItem ("Fold",      true, processorRef.getGlobalFold(),
                        [this] { processorRef.setGlobalFold     (! processorRef.getGlobalFold()); });
        m.addSubMenu ("FX ON DRY SIGNAL", fxSub);
    }

    // --- LFO MODULATION --------------------------------------------------
    // LFO config moved to the dedicated MOD page (button in title bar).

    // --- DISPLAY ------------------------------------------------------
    m.addSeparator();
    m.addSectionHeader ("DISPLAY");
    m.addItem ("Show waveform strip", true, waveformVisible,
               [this]
               {
                   waveformVisible = ! waveformVisible;
                   processorRef.setWaveformVisible (waveformVisible);
                   resized();
                   repaint();
               });

    m.addItem ("Show tooltips", true, processorRef.getTooltipsEnabled(),
               [this]
               {
                   const bool v = ! processorRef.getTooltipsEnabled();
                   processorRef.setTooltipsEnabled (v);
                   // Enable/disable the tooltip window
                   if (v)
                   {
                       tooltipWindow = std::make_unique<juce::TooltipWindow> (this, 800);
                       tooltipWindow->setLookAndFeel (&stencilLF);
                   }
                   else
                       tooltipWindow.reset();
               });

    // v0.10 — when enabled, the editor polls the playing step and
    // flips the visible page (A/B/C/D) to whichever page owns it.
    // Only the *display* follows; the underlying length is untouched.
    m.addItem ("Page follows playhead",
               true,
               processorRef.getPageFollowsPlayhead(),
               [this]
               {
                   const bool was = processorRef.getPageFollowsPlayhead();
                   processorRef.setPageFollowsPlayhead (! was);
               });

    // v0.13 — lookahead removed from the settings menu. The waveform
    // now paints as a full-width history again (no DJ-style right half),
    // and the user-facing control was only there to feed that view.
    // The processor still owns the atomics and public API so state
    // files written in v0.12 with a non-zero lookahead still load
    // without breaking; setLookaheadMs is forced to 0 in
    // ensureLookaheadOff() during editor construction so any such file
    // silently clears the stale setting on first load.

    // --- UI SCALE -----------------------------------------------------
    // v0.12 — the window is freely resizable via the bottom-right
    // drag handle, so the old 75/100/125/150 preset menu is gone.
    // A "Reset size" entry snaps back to the 1x design size for
    // anyone who's dragged themselves into a corner.
    m.addSeparator();
    m.addSectionHeader ("UI SCALE");
    m.addItem ("Reset to 100%", [this] { applyUiScale (1.0f); });

    // v0.30 — DEBUG: round-trip state validation.
    m.setLookAndFeel (&stencilLF);
    m.showMenuAsync (juce::PopupMenu::Options()
                         .withTargetComponent (settingsButton));
}

// v0.30 — hidden debug menu (Option+click SETTINGS).
void LoopSaboteurEditor::showDebugMenu()
{
    juce::PopupMenu m;

    // --- CPU WATCHDOG -------------------------------------------------
    {
        const float worst = processorRef.readAndResetCpuWorstCase();
        m.addSectionHeader ("CPU");
        m.addItem ("Worst-case: " + juce::String (worst * 100.0f, 1) + "% of buffer",
                   false, false, [] {});
    }

    // --- VOICE STATE --------------------------------------------------
    {
        auto vs = processorRef.getVoiceSnapshot();
        m.addSeparator();
        m.addSectionHeader ("VOICE");
        if (vs.active)
        {
            const char scn[] = "ABCDEFGH";
            juce::String info = "ACTIVE";
            if (vs.sceneIdx >= 0 && vs.sceneIdx < 8)
                info += juce::String ("  scene:") + scn[vs.sceneIdx];
            info += "  pos:" + juce::String ((int) vs.readPos)
                  + "  rem:" + juce::String (vs.samplesRemaining)
                  + "/" + juce::String (vs.totalSamples)
                  + "  fade:" + juce::String (vs.fadeGain, 2)
                  + "  mix:" + juce::String (vs.voiceMix, 2);
            if (vs.juddersRemaining > 0)
                info += "  jud:" + juce::String (vs.juddersRemaining);
            m.addItem (info, false, false, [] {});
        }
        else
        {
            m.addItem ("IDLE", false, false, [] {});
        }
    }

    // --- FIRE TIMING --------------------------------------------------
    {
        m.addSeparator();
        m.addSectionHeader ("TIMING");
        const float us = processorRef.getLastFireTimingUs();
        m.addItem ("Last fire delta: " + juce::String (us, 1) + " us",
                   false, false, [] {});
    }

    // --- ACTIONS ------------------------------------------------------
    m.addSeparator();
    m.addSectionHeader ("ACTIONS");

    m.addItem ("Force fire (knob values)",
               [this] { processorRef.debugForceFire(); });

    m.addItem ("Dump step grid to clipboard",
               [this]
               {
                   auto grid = processorRef.debugDumpStepGrid();
                   juce::SystemClipboard::copyTextToClipboard (grid);
                   juce::AlertWindow::showMessageBoxAsync (
                       juce::MessageBoxIconType::InfoIcon,
                       "Step Grid", "Copied to clipboard:\n\n" + grid);
               });

    m.addItem ("Inspect all p-locks",
               [this]
               {
                   auto locks = processorRef.debugDumpAllLocks();
                   juce::SystemClipboard::copyTextToClipboard (locks);
                   juce::AlertWindow::showMessageBoxAsync (
                       juce::MessageBoxIconType::InfoIcon,
                       "Parameter Locks", locks);
               });

    m.addItem ("Dump freeze buffer to WAV",
               [this]
               {
                   auto desktop = juce::File::getSpecialLocation (
                       juce::File::userDesktopDirectory);
                   auto dest = desktop.getChildFile ("LoopSaboteur_buffer_dump.wav");
                   if (processorRef.debugDumpBufferToWav (dest))
                       juce::AlertWindow::showMessageBoxAsync (
                           juce::MessageBoxIconType::InfoIcon,
                           "Buffer Dump",
                           "Saved to:\n" + dest.getFullPathName());
                   else
                       juce::AlertWindow::showMessageBoxAsync (
                           juce::MessageBoxIconType::WarningIcon,
                           "Buffer Dump", "Failed to write WAV.");
               });

    m.addItem ("Validate state round-trip",
               [this]
               {
                   auto result = processorRef.validateStateRoundTrip();
                   if (result.isEmpty())
                       juce::AlertWindow::showMessageBoxAsync (
                           juce::MessageBoxIconType::InfoIcon,
                           "State round-trip",
                           "All fields matched. Save/recall is clean.");
                   else
                       juce::AlertWindow::showMessageBoxAsync (
                           juce::MessageBoxIconType::WarningIcon,
                           "State round-trip MISMATCHES",
                           result);
               });

    m.addItem ("Dump all to file",
               [this]
               {
                   // Helper to convert LFO shape index to name
                   auto lfoShapeName = [] (int shape) -> juce::String
                   {
                       switch (shape)
                       {
                           case 0: return "Sine";
                           case 1: return "Triangle";
                           case 2: return "Saw";
                           case 3: return "Square";
                           case 4: return "S&H";
                           default: return "Unknown";
                       }
                   };

                   // Get timestamp for filename
                   auto now = juce::Time::getCurrentTime();
                   juce::String timestamp = now.formatted ("%Y%m%d_%H%M%S");
                   juce::String filename = "LoopSaboteur_dump_" + timestamp + ".txt";

                   // Create file on desktop
                   auto desktop = juce::File::getSpecialLocation (
                       juce::File::userDesktopDirectory);
                   auto dumpFile = desktop.getChildFile (filename);

                   // Build dump content
                   juce::String dump;
                   dump << "=== Loop Saboteur Debug Dump ===" << juce::newLine;
                   dump << "Date: " << now.toString (true, true, true, true) << juce::newLine;
                   dump << juce::newLine;

                   // --- CPU
                   {
                       const float worst = processorRef.readAndResetCpuWorstCase();
                       dump << "--- CPU ---" << juce::newLine;
                       dump << "Worst-case block: " << juce::String (worst * 100.0f, 1) << "%" << juce::newLine;
                       dump << juce::newLine;
                   }

                   // --- VOICE
                   {
                       auto vs = processorRef.getVoiceSnapshot();
                       const char scn[] = "ABCDEFGH";
                       dump << "--- Voice ---" << juce::newLine;
                       dump << "Active: " << (vs.active ? "yes" : "no") << juce::newLine;
                       if (vs.active)
                       {
                           dump << "ReadPos: " << juce::String ((int) vs.readPos) << juce::newLine;
                           dump << "Samples remaining: " << juce::String (vs.samplesRemaining)
                               << " / " << juce::String (vs.totalSamples) << juce::newLine;
                           dump << "Fade gain: " << juce::String (vs.fadeGain, 2) << juce::newLine;
                           dump << "Judders remaining: " << juce::String (vs.juddersRemaining) << juce::newLine;
                           if (vs.sceneIdx >= 0 && vs.sceneIdx < 8)
                               dump << "Scene: " << scn[vs.sceneIdx] << juce::newLine;
                           else
                               dump << "Scene: none" << juce::newLine;
                           dump << "Voice mix: " << juce::String (vs.voiceMix, 2) << juce::newLine;
                       }
                       dump << juce::newLine;
                   }

                   // --- SEQUENCER
                   {
                       dump << "--- Sequencer ---" << juce::newLine;
                       dump << "Seq on: " << (processorRef.isSeqOn() ? "yes" : "no") << juce::newLine;
                       dump << "Length: " << juce::String (processorRef.getSeqLength()) << juce::newLine;
                       dump << "Current step: " << juce::String (processorRef.getCurrentPlayingStep()) << juce::newLine;
                       dump << "Selected scene: " << juce::String (processorRef.getSelectedScene()) << juce::newLine;
                       dump << juce::newLine;
                   }

                   // --- GLOBAL FX
                   {
                       dump << "--- Global FX ---" << juce::newLine;
                       dump << "Crunch all: " << (processorRef.getCrushAllAudio() ? "on" : "off") << juce::newLine;
                       dump << "Global Drive: " << (processorRef.getGlobalDrive() ? "on" : "off") << juce::newLine;
                       dump << "Global Tone: " << (processorRef.getGlobalTone() ? "on" : "off") << juce::newLine;
                       dump << "Global Ring Mod: " << (processorRef.getGlobalRingMod() ? "on" : "off") << juce::newLine;
                       dump << "Global Fold: " << (processorRef.getGlobalFold() ? "on" : "off") << juce::newLine;
                       dump << juce::newLine;
                   }

                   // --- LFOs (v0.32: per-Act, 4 LFOs each)
                   {
                       dump << "--- LFOs (per Act) ---" << juce::newLine;
                       static const char* actNames = "ABCDEFGH";
                       for (int si = 0; si < LoopSaboteurProcessor::kNumScenes; ++si)
                       {
                           bool anyActive = false;
                           for (int li = 0; li < LoopSaboteurProcessor::kNumLfosPerAct; ++li)
                               if (std::abs (processorRef.getLfoDepth (si, li)) > 0.0001f
                                   && processorRef.getLfoTargetSlot (si, li) >= 0)
                                   anyActive = true;
                           if (! anyActive) continue;
                           dump << "Act " << juce::String::charToString (actNames[si]) << ":" << juce::newLine;
                           for (int li = 0; li < LoopSaboteurProcessor::kNumLfosPerAct; ++li)
                           {
                               dump << "  LFO " << (li + 1) << ": "
                                   << "shape=" << lfoShapeName (processorRef.getLfoShape (si, li))
                                   << " rate=" << juce::String (processorRef.getLfoRateIdx (si, li))
                                   << " depth=" << juce::String (processorRef.getLfoDepth (si, li), 3)
                                   << " target=";
                               int tgt = processorRef.getLfoTargetSlot (si, li);
                               if (tgt < 0)
                                   dump << "none";
                               else
                                   dump << juce::String (tgt);
                               dump << juce::newLine;
                           }
                       }
                       dump << juce::newLine;
                   }

                   // --- UI PREFS
                   {
                       dump << "--- UI Prefs ---" << juce::newLine;
                       dump << "Scale: " << juce::String (processorRef.getUiScale(), 2) << juce::newLine;
                       dump << "Waveform visible: " << (processorRef.getWaveformVisible() ? "yes" : "no") << juce::newLine;
                       dump << "Tooltips: " << (processorRef.getTooltipsEnabled() ? "on" : "off") << juce::newLine;
                       dump << juce::newLine;
                   }

                   // --- STEP GRID
                   {
                       dump << "--- Step Grid ---" << juce::newLine;
                       dump << processorRef.debugDumpStepGrid() << juce::newLine;
                       dump << juce::newLine;
                   }

                   // --- P-LOCKS
                   {
                       dump << "--- P-Locks ---" << juce::newLine;
                       dump << processorRef.debugDumpAllLocks() << juce::newLine;
                       dump << juce::newLine;
                   }

                   // --- STATE ROUND-TRIP
                   {
                       dump << "--- State Round-Trip ---" << juce::newLine;
                       auto result = processorRef.validateStateRoundTrip();
                       dump << (result.isEmpty() ? "PASS" : result) << juce::newLine;
                   }

                   // Write to file
                   if (dumpFile.replaceWithText (dump))
                   {
                       juce::AlertWindow::showMessageBoxAsync (
                           juce::MessageBoxIconType::InfoIcon,
                           "Dump Complete",
                           "Saved to:\n" + dumpFile.getFullPathName());
                   }
                   else
                   {
                       juce::AlertWindow::showMessageBoxAsync (
                           juce::MessageBoxIconType::WarningIcon,
                           "Dump Failed",
                           "Could not write to:\n" + dumpFile.getFullPathName());
                   }
               });

    // --- EASTER EGG STATUS --------------------------------------------
    m.addSeparator();
    m.addSectionHeader ("EASTER EGGS");
    auto eggStatus = [&] (const juce::String& name, bool active) -> juce::String
    {
        return name + ": " + (active ? "ACTIVE" : "off");
    };
    m.addItem (eggStatus ("Pacifist",       pacifistFade >= 0.01f),   false, false, [] {});
    m.addItem (eggStatus ("Mirror",         mirrorActive),            false, false, [] {});
    m.addItem (eggStatus ("Colour Swap",    colourSwapped),           false, false, [] {});
    m.addItem (eggStatus ("Dilla",          dillaActive),             false, false, [] {});
    m.addItem (eggStatus ("Producer Tag",   producerTagActive),       false, false, [] {});
    m.addItem (eggStatus ("Amen",           amenActive),              false, false, [] {});
    m.addItem (eggStatus ("Paula",          paulaActive),             false, false, [] {});
    m.addItem (eggStatus ("Guru Meditation", guruActive),             false, false, [] {});
    {
        const juce::int64 nowMs = juce::Time::currentTimeMillis();
        m.addItem (eggStatus ("303 Bday",  nowMs < StepCell::force303UntilMs), false, false, [] {});
        m.addItem (eggStatus ("606 Bday",  nowMs < StepCell::force606UntilMs), false, false, [] {});
        m.addItem (eggStatus ("808 Bday",  nowMs < StepCell::force808UntilMs), false, false, [] {});
    }
    m.addItem (eggStatus ("Rat Run",        ratRun != nullptr),       false, false, [] {});

    // --- TEST EGGS (force-on for 15 s) --------------------------------
    m.addSeparator();
    m.addSectionHeader ("TEST EGGS (15 s)");
    m.addItem ("Trigger: Pacifist",        [this] { forceEggOn (EggPacifist); });
    m.addItem ("Trigger: Mirror",          [this] { forceEggOn (EggMirror); });
    m.addItem ("Trigger: Colour Swap",     [this] { forceEggOn (EggColourSwap); });
    m.addItem ("Trigger: Dilla",           [this] { forceEggOn (EggDilla); });
    m.addItem ("Trigger: Producer Tag",    [this] { forceEggOn (EggProducerTag); });
    m.addItem ("Trigger: Amen",            [this] { forceEggOn (EggAmen); });
    m.addItem ("Trigger: Paula",           [this] { forceEggOn (EggPaula); });
    m.addItem ("Trigger: Guru Meditation", [this] { forceEggOn (EggGuru); });
    m.addItem ("Trigger: 303 Bday",        [this] { forceEggOn (Egg303); });
    m.addItem ("Trigger: 606 Bday",        [this] { forceEggOn (Egg606); });
    m.addItem ("Trigger: 808 Bday",        [this] { forceEggOn (Egg808); });
    m.addItem ("Trigger: Rat Run",         [this] { forceEggOn (EggRat); });

    m.setLookAndFeel (&stencilLF);
    m.showMenuAsync (juce::PopupMenu::Options()
                         .withTargetComponent (settingsButton));
}

void LoopSaboteurEditor::showHelpCallout()
{
    // v0.42.4 — cheatsheet is longer than the callout's vertical budget,
    // so the bottom was being clipped. Wrap the Label in a Viewport and
    // let it scroll vertically instead of truncating. Content stays the
    // same; only the container changed. Width of the label is reduced
    // by the scrollbar gutter so text doesn't slide under the thumb.
    const juce::String helpText =
        juce::String ("LOOP SABOTEUR - QUICK KEYS\n\n")
        + "ACTS\n"
        + "  click        load Act into knobs\n"
        + "  right-click  Act menu (reset, preset)\n"
        + "\n"
        + "STEPS\n"
        + "  click        stamp selected Act\n"
        + "  right-click  step menu (copy / paste / clear)\n"
        + "  shift-click  edit p-locks for this step\n"
        + "  cmd-click    toggle freeze hold\n"
        + "  alt-click    cycle ratio (1/1 .. 1/8)\n"
        + "\n"
        + "STEP TRIG CONDITIONS (right-click step > Trig)\n"
        + "  A:B        fire every A of B passes\n"
        + "  PREV/!PREV fire if prev step did/didn't\n"
        + "  ONCE       fire once per transport run\n"
        + "  25/50/75%  per-step random coin flip\n"
        + "\n"
        + "HEADER BUTTONS\n"
        + "  RANDOM     pattern generators + grab offsets\n"
        + "  ACTS ACTS  bulk Act menu + presets + MOD-clear toggle\n"
        + "  MOD        modulation page (cyan when open)\n"
        + "  TOOLS      creative tools page (alt-click = debug menu)\n"
        + "  SYNC|FREE  segmented: DAW tempo vs free-run\n"
        + "  HELP       this panel\n"
        + "\n"
        + "MOD PAGE\n"
        + "  right-click any knob to assign a MOD (1-4).\n"
        + "  cyan knob body = a MOD is targeting that knob.\n"
        + "  X-Mod row lets a MOD modulate another MOD's depth\n"
        + "  within the same Act.\n"
        + "  Clearing an Act also wipes its MODs by default;\n"
        + "  flip \"Preserve MODs\" in ACTS ACTS to keep them.\n"
        + "\n"
        + "TOOLS PAGE\n"
        + "  Full-screen overlay. Left column = ENGINE for the\n"
        + "  currently-selected Act: Output Stage, Output Mix,\n"
        + "  Interpolation, Crunch flags. Every Act has its own\n"
        + "  Engine; switching Acts crossfades the master bus\n"
        + "  over ~8 ms so stage changes don't pop.\n"
        + "  Middle/right columns: grid accent, transport,\n"
        + "  scale quantise, stereo mode, display toggles.\n"
        + "\n"
        + "PRESETS\n"
        + "  top-bar selector, or load from ACTS ACTS menu.\n"
        + "  includes Cage 4'33\", Paula, Amen, and more.\n"
        + "  .lpsbact files are per-Act (version 2 carries the\n"
        + "  Engine bundle); .lpsbactbank files carry all 8.\n"
        + "\n"
        + "Hover any knob for a description.\n"
        + "Full manual: MANUAL.md (see README.md for index).";

    // Measure content height: line count × line-height + borders. This
    // sizes the inner label so the viewport knows how far it can scroll.
    juce::StringArray helpLines;
    helpLines.addLines (helpText);
    const int borderY = 16;
    const int lineH   = 18;   // matches 14pt bold at default line spacing
    const int labelH  = borderY * 2 + helpLines.size() * lineH + 12;

    // Outer container passed to the CallOutBox (takes ownership).
    const int viewportW = 440;
    const int viewportH = 560;
    auto* content = new juce::Component();
    content->setSize (viewportW, viewportH);
    content->setLookAndFeel (&stencilLF);

    // The label holding the cheatsheet text. Sized to its natural
    // height so the viewport has something to scroll.
    const int scrollbarGutter = 14;
    auto* lbl = new juce::Label ({}, {});
    lbl->setJustificationType (juce::Justification::topLeft);
    lbl->setColour (juce::Label::textColourId, Col::textBright);
    lbl->setColour (juce::Label::backgroundColourId, juce::Colour (0xff1a1a1a));
    lbl->setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
    lbl->setBorderSize (juce::BorderSize<int> (borderY, 18, borderY, 18));
    lbl->setText (helpText, juce::dontSendNotification);
    lbl->setBounds (0, 0, viewportW - scrollbarGutter, labelH);

    // Viewport gives us vertical scrolling if the text is taller than
    // the visible area. We always show the v-scrollbar so the affordance
    // is obvious; h-scrollbar is off so long lines would just clip
    // (they already fit the 440 width).
    auto* viewport = new juce::Viewport();
    viewport->setScrollBarsShown (true, false);
    viewport->setViewedComponent (lbl, true /* delete when removed */);
    viewport->setBounds (0, 0, viewportW, viewportH);
    viewport->setColour (juce::ScrollBar::thumbColourId, juce::Colour (0xff555555));
    content->addAndMakeVisible (viewport);

    // Ownership: CallOutBox::launchAsynchronously takes a unique_ptr.
    juce::CallOutBox::launchAsynchronously (std::unique_ptr<juce::Component> (content),
                                             helpButton.getScreenBounds(),
                                             nullptr);
}

void LoopSaboteurEditor::applyUiScale (float s)
{
    // v0.13 — unchanged surface: apply a logical scale to the outer
    // editor by setting its size to (kDesignW*s, kDesignH*s). resized()
    // will pick that up, derive the scale factor, and transform
    // innerContent accordingly. Persisted value is updated inside
    // resized() so that both drag-resize and preset-applied scales go
    // through the same code path.
    s = juce::jlimit (0.5f, 2.0f, s);
    setSize ((int) std::round (kDesignW * s),
             (int) std::round (kDesignH * s));
}

// v0.30 — step copy/paste. Copies all per-step data into the editor's
// clipboard so it can be pasted onto another step.
void LoopSaboteurEditor::copyStep (int stepIdx)
{
    if (stepIdx < 0 || stepIdx >= LoopSaboteurProcessor::kMaxSteps)
        return;

    stepClipboard.valid   = true;
    stepClipboard.scene   = processorRef.getStepScene   (stepIdx);
    stepClipboard.ratio   = processorRef.getStepRatio   (stepIdx);
    stepClipboard.freeze  = processorRef.getStepFreeze  (stepIdx);
    stepClipboard.ratchet = processorRef.getStepRatchet  (stepIdx);
    stepClipboard.mute    = processorRef.getStepMute     (stepIdx);

    for (int k = 0; k < LoopSaboteurProcessor::kNumLockableParams; ++k)
        stepClipboard.locks[k] = processorRef.getStepLock (stepIdx, k);
}

void LoopSaboteurEditor::pasteStep (int stepIdx)
{
    if (stepIdx < 0 || stepIdx >= LoopSaboteurProcessor::kMaxSteps)
        return;
    if (! stepClipboard.valid)
        return;

    processorRef.pushUndoSnapshot();

    processorRef.setStepScene   (stepIdx, stepClipboard.scene);
    processorRef.setStepRatio   (stepIdx, stepClipboard.ratio);
    processorRef.setStepFreeze  (stepIdx, stepClipboard.freeze);
    processorRef.setStepRatchet (stepIdx, stepClipboard.ratchet);
    processorRef.setStepMute    (stepIdx, stepClipboard.mute);

    for (int k = 0; k < LoopSaboteurProcessor::kNumLockableParams; ++k)
        processorRef.setStepLock (stepIdx, k, stepClipboard.locks[k]);

    // Refresh the cell visuals.
    stepCells[stepIdx].setSceneIndex (stepClipboard.scene);
    stepCells[stepIdx].setRatioIndex (stepClipboard.ratio);
    stepCells[stepIdx].setFreezeHold (stepClipboard.freeze);
    stepCells[stepIdx].setRatchet    (stepClipboard.ratchet);
    stepCells[stepIdx].setMuted      (stepClipboard.mute);
    stepCells[stepIdx].setHasLocks   (processorRef.stepHasAnyLock (stepIdx));

    if (editLockStep == stepIdx)
        refreshKnobLockDirty();
}

// ============================================================================
//  v0.31 — Multi-step selection and range copy/paste
// ============================================================================
void LoopSaboteurEditor::clearSelection()
{
    // Clear the selection highlight from all cells
    for (int i = 0; i < LoopSaboteurProcessor::kMaxSteps; ++i)
        stepCells[i].setSelected (false);
    selectionStart = -1;
    selectionEnd   = -1;
}

void LoopSaboteurEditor::updateSelectionHighlight()
{
    // Repaint all cells, highlighting those within the selection range
    if (! hasSelection())
        return;

    for (int i = 0; i < LoopSaboteurProcessor::kMaxSteps; ++i)
    {
        const bool inRange = (i >= selectionStart && i <= selectionEnd);
        stepCells[i].setSelected (inRange);
    }
}

void LoopSaboteurEditor::copySelection()
{
    if (! hasSelection())
        return;

    multiStepClipboard.clear();

    // Copy all steps from selectionStart to selectionEnd (inclusive)
    for (int i = selectionStart; i <= selectionEnd; ++i)
    {
        if (i < 0 || i >= LoopSaboteurProcessor::kMaxSteps)
            continue;

        StepClipboard clipboard;
        clipboard.valid   = true;
        clipboard.scene   = processorRef.getStepScene   (i);
        clipboard.ratio   = processorRef.getStepRatio   (i);
        clipboard.freeze  = processorRef.getStepFreeze  (i);
        clipboard.ratchet = processorRef.getStepRatchet (i);
        clipboard.mute    = processorRef.getStepMute    (i);

        for (int k = 0; k < LoopSaboteurProcessor::kNumLockableParams; ++k)
            clipboard.locks[k] = processorRef.getStepLock (i, k);

        multiStepClipboard.push_back (clipboard);
    }
}

void LoopSaboteurEditor::pasteSelection (int startStep)
{
    if (multiStepClipboard.empty())
        return;
    if (startStep < 0 || startStep >= LoopSaboteurProcessor::kMaxSteps)
        return;

    processorRef.pushUndoSnapshot();

    // Paste the clipboard starting at startStep, wrapping if needed
    for (size_t idx = 0; idx < multiStepClipboard.size(); ++idx)
    {
        int targetStep = (startStep + idx) % LoopSaboteurProcessor::kMaxSteps;

        const auto& clipboard = multiStepClipboard[idx];
        processorRef.setStepScene   (targetStep, clipboard.scene);
        processorRef.setStepRatio   (targetStep, clipboard.ratio);
        processorRef.setStepFreeze  (targetStep, clipboard.freeze);
        processorRef.setStepRatchet (targetStep, clipboard.ratchet);
        processorRef.setStepMute    (targetStep, clipboard.mute);

        for (int k = 0; k < LoopSaboteurProcessor::kNumLockableParams; ++k)
            processorRef.setStepLock (targetStep, k, clipboard.locks[k]);

        // Refresh the cell visuals.
        stepCells[targetStep].setSceneIndex (clipboard.scene);
        stepCells[targetStep].setRatioIndex (clipboard.ratio);
        stepCells[targetStep].setFreezeHold (clipboard.freeze);
        stepCells[targetStep].setRatchet    (clipboard.ratchet);
        stepCells[targetStep].setMuted      (clipboard.mute);
        stepCells[targetStep].setHasLocks   (processorRef.stepHasAnyLock (targetStep));

        if (editLockStep == targetStep)
            refreshKnobLockDirty();
    }
}

void LoopSaboteurEditor::copyPage()
{
    // Copy all 16 steps of the current page into multiStepClipboard
    const int pageStart = currentPage * LoopSaboteurProcessor::kStepsPerPage;
    const int pageEnd   = pageStart + LoopSaboteurProcessor::kStepsPerPage;

    multiStepClipboard.clear();

    for (int i = pageStart; i < pageEnd && i < LoopSaboteurProcessor::kMaxSteps; ++i)
    {
        StepClipboard clipboard;
        clipboard.valid   = true;
        clipboard.scene   = processorRef.getStepScene   (i);
        clipboard.ratio   = processorRef.getStepRatio   (i);
        clipboard.freeze  = processorRef.getStepFreeze  (i);
        clipboard.ratchet = processorRef.getStepRatchet (i);
        clipboard.mute    = processorRef.getStepMute    (i);

        for (int k = 0; k < LoopSaboteurProcessor::kNumLockableParams; ++k)
            clipboard.locks[k] = processorRef.getStepLock (i, k);

        multiStepClipboard.push_back (clipboard);
    }
}

void LoopSaboteurEditor::pastePage()
{
    if (multiStepClipboard.empty())
        return;

    processorRef.pushUndoSnapshot();

    const int pageStart = currentPage * LoopSaboteurProcessor::kStepsPerPage;
    const int pageEnd   = pageStart + LoopSaboteurProcessor::kStepsPerPage;

    // Paste the clipboard onto the current page, cycling through the clipboard
    for (int i = pageStart; i < pageEnd && i < LoopSaboteurProcessor::kMaxSteps; ++i)
    {
        const size_t clipboardIdx = (i - pageStart) % multiStepClipboard.size();
        const auto& clipboard = multiStepClipboard[clipboardIdx];

        processorRef.setStepScene   (i, clipboard.scene);
        processorRef.setStepRatio   (i, clipboard.ratio);
        processorRef.setStepFreeze  (i, clipboard.freeze);
        processorRef.setStepRatchet (i, clipboard.ratchet);
        processorRef.setStepMute    (i, clipboard.mute);

        for (int k = 0; k < LoopSaboteurProcessor::kNumLockableParams; ++k)
            processorRef.setStepLock (i, k, clipboard.locks[k]);

        // Refresh the cell visuals.
        stepCells[i].setSceneIndex (clipboard.scene);
        stepCells[i].setRatioIndex (clipboard.ratio);
        stepCells[i].setFreezeHold (clipboard.freeze);
        stepCells[i].setRatchet    (clipboard.ratchet);
        stepCells[i].setMuted      (clipboard.mute);
        stepCells[i].setHasLocks   (processorRef.stepHasAnyLock (i));

        if (editLockStep == i)
            refreshKnobLockDirty();
    }
}

void LoopSaboteurEditor::showStepCellMenu (int stepIdx)
{
    if (stepIdx < 0 || stepIdx >= LoopSaboteurProcessor::kMaxSteps)
        return;

    juce::PopupMenu m;

    m.addSectionHeader ("STEP " + juce::String (stepIdx + 1));

    // v0.12 — "Stamp Act" is gone; plain left-click already stamps,
    // so the menu entry was just noise. Clear is still useful though
    // (less fussy than alt-left / shift etc).
    m.addItem ("Clear step", [this, stepIdx]
    {
        processorRef.setStepScene  (stepIdx, -1);
        processorRef.setStepRatio  (stepIdx, 0);
        processorRef.setStepFreeze (stepIdx, false);
        processorRef.setStepRatchet (stepIdx, 1);    // v0.12
        processorRef.setStepMute    (stepIdx, false); // v0.13
        processorRef.clearStepLocks (stepIdx);
        stepCells[stepIdx].setSceneIndex (-1);
        stepCells[stepIdx].setRatioIndex (0);
        stepCells[stepIdx].setFreezeHold (false);
        stepCells[stepIdx].setHasLocks   (false);
        stepCells[stepIdx].setRatchet    (1);
        stepCells[stepIdx].setMuted      (false);
        if (editLockStep == stepIdx) exitLockEditMode();
    });

    // v0.30 — copy/paste. Copies all per-step data (scene, ratio,
    // freeze, ratchet, mute, p-locks) so you can replicate patterns.
    m.addItem ("Copy step",  [this, stepIdx] { copyStep (stepIdx); });
    m.addItem ("Paste step", stepClipboard.valid, false,
               [this, stepIdx] { pasteStep (stepIdx); });

    // v0.31 — multi-step copy/paste. Requires a selection or clipboard.
    m.addSeparator();
    m.addItem ("Copy range", hasSelection(), false, [this] { copySelection(); });
    m.addItem ("Paste range", !multiStepClipboard.empty(), false,
               [this, stepIdx] { pasteSelection (stepIdx); });

    // v0.13 — manual stutter. Muting a step is a pure fire-gate: every
    // other per-step setting stays in place, we just suppress the fire.
    // Lives at the top of the menu next to Clear because Steve wanted it
    // as a quick-access toggle.
    const bool isMuted = processorRef.getStepMute (stepIdx);
    m.addItem (isMuted ? "Unmute step" : "Mute step",
               true, isMuted,
               [this, stepIdx, isMuted]
               {
                   processorRef.setStepMute (stepIdx, ! isMuted);
                   stepCells[stepIdx].setMuted (! isMuted);
               });

    // Ratio submenu — every entry in the ratio table plus the inherit
    // sentinel. Ticks the current selection.
    // v0.38 — separators now divide classic A:B ratios from the
    // conditional-trigger kinds (PREV, !PREV, ONCE) and from the
    // random-percent kinds (25%, 50%, 75%) so users don't mistake
    // them for one-off A:B entries.
    m.addSeparator();
    {
        juce::PopupMenu ratioMenu;
        const int current = processorRef.getStepRatio (stepIdx);
        for (int r = 0; r < LoopSaboteurProcessor::kNumRatios; ++r)
        {
            // The "0" slot is no override; say so plainly.
            // "Inherit Act chance" was clever but nobody realised
            // "inherit" meant "clear".
            juce::String label;
            if (r == 0)
            {
                label = "Clear (inherit Act)";
            }
            else if (r == 17) label = "PREV  (fires if previous step fired)";
            else if (r == 18) label = "!PREV (fires if previous step did NOT fire)";
            else if (r == 19) label = "ONCE  (fires once, on first pass after transport start)";
            else if (r == 20) label = "25%  (random 1-in-4 per visit)";
            else if (r == 21) label = "50%  (random coin flip per visit)";
            else if (r == 22) label = "75%  (random 3-in-4 per visit)";
            else              label = LoopSaboteurProcessor::ratioLabel (r);

            if (r == 17 || r == 20)
                ratioMenu.addSeparator();

            ratioMenu.addItem (label, true, r == current, [this, stepIdx, r]
            {
                processorRef.setStepRatio (stepIdx, r);
                stepCells[stepIdx].setRatioIndex (r);
            });
        }
        m.addSubMenu ("Trig condition", ratioMenu);
    }

    // v0.12 — ratchet submenu. 1..8 at the musically-useful subset
    // (1, 2, 3, 4, 6, 8). Classic ratchet subdivisions that stay in
    // swing-friendly territory.
    {
        juce::PopupMenu rchMenu;
        const int current = processorRef.getStepRatchet (stepIdx);
        static const int kRatchetValues[] = { 1, 2, 3, 4, 6, 8 };
        for (int rv : kRatchetValues)
        {
            const juce::String label = (rv == 1)
                ? juce::String ("None (1x)")
                : juce::String (rv) + "x";
            rchMenu.addItem (label, true, rv == current,
                             [this, stepIdx, rv]
                             {
                                 processorRef.setStepRatchet (stepIdx, rv);
                                 stepCells[stepIdx].setRatchet (rv);
                             });
        }
        m.addSubMenu ("Ratchet", rchMenu);
    }

    m.addSeparator();
    const bool held = processorRef.getStepFreeze (stepIdx);
    m.addItem ("Freeze hold", true, held, [this, stepIdx, held]
    {
        processorRef.setStepFreeze (stepIdx, ! held);
        stepCells[stepIdx].setFreezeHold (! held);
    });

    const bool isEditing = (editLockStep == stepIdx);
    m.addItem (isEditing ? "Exit lock edit" : "Edit p-locks",
               [this, stepIdx, isEditing]
               {
                   if (isEditing)
                       exitLockEditMode();
                   else
                   {
                       if (editLockStep >= 0) exitLockEditMode();
                       enterLockEditMode (stepIdx);
                   }
               });

    m.addItem ("Clear p-locks", processorRef.stepHasAnyLock (stepIdx),
               false,
               [this, stepIdx]
               {
                   processorRef.clearStepLocks (stepIdx);
                   stepCells[stepIdx].setHasLocks (false);
                   if (editLockStep == stepIdx)
                       refreshKnobLockDirty();
               });

    m.setLookAndFeel (&stencilLF);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&stepCells[stepIdx]));
}

// ============================================================================
//  v0.10 — Act slot context menu (right-click on an Act button)
// ============================================================================
void LoopSaboteurEditor::showActContextMenu (int sceneIdx)
{
    if (sceneIdx < 0 || sceneIdx >= LoopSaboteurProcessor::kNumScenes)
        return;

    const juce::String letter (juce::String::charToString ((juce::juce_wchar) ('A' + sceneIdx)));
    const bool populated = processorRef.isSceneStored (sceneIdx);

    // v0.40 — decide whether any of this Act's MODs are active (rate != 0
    // OR target wired OR cross-mod wired) so the "Clear MODs only" entry
    // can grey itself out when there's nothing to clear.
    bool anyLfoActive = false;
    for (int li = 0; li < 4; ++li)
    {
        if (processorRef.getLfoRateIdx (sceneIdx, li) > 0
         || processorRef.getLfoTargetSlot (sceneIdx, li) >= 0)
        {
            anyLfoActive = true;
            break;
        }
    }

    juce::PopupMenu m;
    m.addSectionHeader ("ACT " + letter);

    m.addItem ("Clear knobs + MODs",
               populated || anyLfoActive,
               false,
               [this, sceneIdx]
               {
                   if (editLockStep >= 0) exitLockEditMode();
                   // Force a full wipe (knobs + MODs) regardless of the
                   // "Preserve MODs" preference — users who right-click
                   // an Act and pick this are asking for a clean slate.
                   const bool prev = processorRef.getPreserveLfosOnClear();
                   processorRef.setPreserveLfosOnClear (false);
                   processorRef.resetSceneToDefaults (sceneIdx);
                   processorRef.setPreserveLfosOnClear (prev);
                   if (processorRef.getSelectedScene() == sceneIdx)
                       processorRef.loadSceneToKnobs (sceneIdx);
                   refreshSceneButtons();
               });

    // v0.40 — Clear just the knob values but leave the 4 MODs alone. For
    // when a modulation rig is dialled in and the user wants to rebuild
    // the static values around it.
    m.addItem ("Clear knobs only (keep MODs)",
               populated,
               false,
               [this, sceneIdx]
               {
                   if (editLockStep >= 0) exitLockEditMode();
                   const bool prev = processorRef.getPreserveLfosOnClear();
                   processorRef.setPreserveLfosOnClear (true);
                   processorRef.resetSceneToDefaults (sceneIdx);
                   processorRef.setPreserveLfosOnClear (prev);
                   if (processorRef.getSelectedScene() == sceneIdx)
                       processorRef.loadSceneToKnobs (sceneIdx);
                   refreshSceneButtons();
               });

    // v0.40 — Clear just the 4 MODs and leave the knob values alone. For
    // when the static Act is the keeper and the modulation rig is the
    // thing that's gone off the rails.
    m.addItem ("Clear MODs only (keep knobs)",
               anyLfoActive,
               false,
               [this, sceneIdx]
               {
                   processorRef.resetLfosForScene (sceneIdx);
                   if (processorRef.getSelectedScene() == sceneIdx)
                   {
                       // Refresh the MOD page so the newly-cleared slots
                       // redraw with their Off/no-target state.
                       if (modPage != nullptr)
                           modPage->updateFromProcessor();
                   }
                   refreshSceneButtons();
               });

    // v0.42.5 — Act copy / paste. Reuses serializeAct() /
    // loadActFromXml() so the clipboard lives in the exact same
    // format as a `.lpsbact` preset; round-tripping through it is
    // lossless. Copy is always available on a populated Act; Paste is
    // enabled whenever the clipboard has something in it, regardless
    // of whether the target slot is currently stored.
    m.addSeparator();

    m.addItem ("Copy Act",
               populated,
               false,
               [this, sceneIdx]
               {
                   actClipboard = processorRef.serializeAct (sceneIdx);
                   actClipboardSourceIdx = sceneIdx;
               });

    juce::String pasteLabel ("Paste Act");
    if (actClipboard != nullptr
        && actClipboardSourceIdx >= 0
        && actClipboardSourceIdx < LoopSaboteurProcessor::kNumScenes)
    {
        const juce::String srcLetter (juce::String::charToString (
            (juce::juce_wchar) ('A' + actClipboardSourceIdx)));
        pasteLabel = "Paste Act  (from " + srcLetter + ")";
    }

    m.addItem (pasteLabel,
               actClipboard != nullptr,
               false,
               [this, sceneIdx]
               {
                   if (actClipboard == nullptr) return;
                   if (editLockStep >= 0) exitLockEditMode();
                   if (processorRef.loadActFromXml (*actClipboard, sceneIdx))
                   {
                       // Reflect the paste on screen straight away: if
                       // the user pasted into the currently-selected
                       // Act, push the knob values down into the live
                       // knob widgets; the MOD page (if open) also
                       // needs a refresh so the LFO rows redraw.
                       if (processorRef.getSelectedScene() == sceneIdx)
                       {
                           processorRef.loadSceneToKnobs (sceneIdx);
                           if (modPage != nullptr)
                               modPage->updateFromProcessor();
                       }
                       refreshSceneButtons();
                   }
               });

    m.setLookAndFeel (&stencilLF);
    m.showMenuAsync (juce::PopupMenu::Options()
                     .withTargetComponent (&sceneButtons[sceneIdx]));
}

// ============================================================================
//  v0.31 — PAGE context menu (right-click on page A/B/C/D button)
// ============================================================================
void LoopSaboteurEditor::showPageContextMenu()
{
    juce::PopupMenu m;

    const juce::String letter (juce::String::charToString ((juce::juce_wchar) ('A' + currentPage)));
    m.addSectionHeader ("PAGE " + letter);

    m.addItem ("Copy page", [this] { copyPage(); });
    m.addItem ("Paste page", !multiStepClipboard.empty(), false,
               [this] { pastePage(); });

    m.setLookAndFeel (&stencilLF);
    m.showMenuAsync (juce::PopupMenu::Options()
                     .withTargetComponent (&pageButtons[currentPage]));
}

// ============================================================================
//  RANDOMIZE popup menu
//  v0.12 — the old "sparse / busy" density rolls were arbitrary and
//  rarely musical. Replaced with a set of actual rhythmic generators —
//  four-on-the-floor, offbeats, Euclidean E(k,n) — plus the existing
//  drift/full value re-rolls and the nuclear EVERYTHING option.
// ============================================================================
void LoopSaboteurEditor::showRandomizeMenu()
{
    juce::PopupMenu m;

    // v0.13 — Patterns submenu. Steve wanted more than just "Four on
    // the floor" here — a whole folder of rhythmic starting points that
    // aren't just coin-flip scatters. Everything in this submenu stamps
    // random populated Acts onto the hit positions; the Act pool is
    // whatever you currently have stored in A-H.
    juce::PopupMenu patterns;
    auto addPattern = [this, &patterns] (const juce::String& label,
                                          std::function<void()> action)
    {
        patterns.addItem (label, [this, action = std::move (action)]
        {
            processorRef.pushUndoSnapshot();
            action();
            refreshStepCells();
        });
    };

    addPattern ("Downbeats",        [this] { processorRef.placeDownbeats();      });
    addPattern ("Backbeat  (2 & 4)", [this] { processorRef.placeBackbeat();       });
    addPattern ("Four on the floor", [this] { processorRef.placeFourOnTheFloor(); });
    addPattern ("Offbeats",          [this] { processorRef.placeOffbeats();       });
    addPattern ("Syncopated  (e&a)", [this] { processorRef.placeSyncopated();     });
    addPattern ("Son clave  (3-3-2)", [this] { processorRef.placeClave();         });
    patterns.addSeparator();
    addPattern ("Sparse (25%)",      [this] { processorRef.placeSparse();         });
    addPattern ("Busy (75%)",        [this] { processorRef.placeBusy();           });
    addPattern ("Saturated (all)",   [this] { processorRef.placeSaturated();      });
    patterns.addSeparator();

    // Euclidean sub-submenu — classic presets plus "go wild".
    juce::PopupMenu euc;
    const int seqLen = processorRef.getSeqLength();
    auto addEuc = [this, &euc, seqLen] (int pulses, const juce::String& suffix)
    {
        const juce::String label = "E(" + juce::String (pulses) + ", "
                                 + juce::String (seqLen) + ")" + suffix;
        euc.addItem (label, [this, pulses]
        {
            processorRef.pushUndoSnapshot();
            processorRef.placeEuclidean (pulses);
            refreshStepCells();
        });
    };
    addEuc (3,  " tresillo");
    addEuc (5,  " cinquillo");
    addEuc (7,  "");
    addEuc (9,  "");
    addEuc (11, "");
    euc.addSeparator();
    euc.addItem ("Go wild",
                 [this]
                 {
                     processorRef.pushUndoSnapshot();
                     const int len = processorRef.getSeqLength();
                     const int lo  = juce::jmax (1, len / 4);
                     const int hi  = juce::jmax (lo + 1, (3 * len) / 4);
                     const int k   = lo + juce::Random::getSystemRandom().nextInt (hi - lo + 1);
                     processorRef.placeEuclidean (k);
                     refreshStepCells();
                 });
    patterns.addSubMenu ("Euclidean...", euc);

    patterns.addSeparator();
    // "Roll me a pattern" — picks any of the patterns above at random.
    patterns.addItem ("Go wild (any pattern)",
                      [this]
                      {
                          processorRef.pushUndoSnapshot();
                          auto& r = juce::Random::getSystemRandom();
                          switch (r.nextInt (9))
                          {
                              case 0: processorRef.placeDownbeats();      break;
                              case 1: processorRef.placeBackbeat();       break;
                              case 2: processorRef.placeFourOnTheFloor(); break;
                              case 3: processorRef.placeOffbeats();       break;
                              case 4: processorRef.placeSyncopated();     break;
                              case 5: processorRef.placeClave();          break;
                              case 6: processorRef.placeBusy();           break;
                              case 7: processorRef.placeSparse();         break;
                              default:
                              {
                                  const int len = processorRef.getSeqLength();
                                  const int k = 3 + r.nextInt (juce::jmax (1, len / 2));
                                  processorRef.placeEuclidean (k);
                                  break;
                              }
                          }
                          refreshStepCells();
                      });

    m.addSubMenu ("Patterns...", patterns);

    m.addSeparator();
    m.addSectionHeader ("VALUES");
    m.addItem ("Subtle",
               [this]
               {
                   processorRef.pushUndoSnapshot();
                   processorRef.randomizeActValues (0.35f);
                   processorRef.loadSceneToKnobs (processorRef.getSelectedScene());
                   refreshSceneButtons();
               });
    m.addItem ("Not Subtle",
               [this]
               {
                   processorRef.pushUndoSnapshot();
                   processorRef.randomizeActValues (1.0f);
                   processorRef.loadSceneToKnobs (processorRef.getSelectedScene());
                   refreshSceneButtons();
               });

    m.addSeparator();
    m.addItem ("Randomise EVERYTHING",
               [this]
               {
                   processorRef.pushUndoSnapshot();
                   processorRef.randomizeEverything();
                   processorRef.loadSceneToKnobs (processorRef.getSelectedScene());
                   refreshStepCells();
                   refreshSceneButtons();
               });

    m.setLookAndFeel (&stencilLF);
    m.showMenuAsync (juce::PopupMenu::Options()
                     .withTargetComponent (&randomizeButton));
}

// ============================================================================
//  ModPage — LFO configuration page
// ============================================================================

// Evaluate an LFO shape at phase ph (0..1) returning -1..+1.
// S&H uses a deterministic hash of integer cycle index so the preview
// is stable across redraws.
static float evalLfoShape (int shape, double totalPhase) noexcept
{
    const double ph = totalPhase - std::floor (totalPhase);
    switch (shape)
    {
        case 0: // Sine
            return (float) std::sin (ph * juce::MathConstants<double>::twoPi);
        case 1: // Triangle
            return (float) (ph < 0.5 ? (4.0 * ph - 1.0) : (3.0 - 4.0 * ph));
        case 2: // Saw
            return (float) (1.0 - 2.0 * ph);
        case 3: // Square
            return ph < 0.5 ? 1.0f : -1.0f;
        case 4: // S&H — deterministic per integer cycle
        {
            auto cycle = (uint32_t) (int64_t) std::floor (totalPhase);
            cycle = cycle * 2654435761u;           // Knuth multiplicative hash
            cycle ^= cycle >> 16;
            const float v = ((float) (cycle & 0xFFFFu) / 32768.0f) - 1.0f;
            return juce::jlimit (-1.0f, 1.0f, v);
        }
        default:
            return 0.0f;
    }
}

// v0.33 — map rate index to quarter-notes per LFO cycle. Mirrors
// lfoDivisionIndexToQuarters in PluginProcessor.cpp.
static double quartersPerLfoCycle (int rateIdx) noexcept
{
    static const double table[] = {
        0.0,          // 0: Off
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
    const int maxIdx = (int) (sizeof (table) / sizeof (table[0])) - 1;
    return table[juce::jlimit (0, maxIdx, rateIdx)];
}

// Advance the free-running preview phase. Used when the host is stopped
// so the scope still scrolls — it moves at the REAL rate for the current
// host BPM (or 120 as fallback). When the host is playing we read the
// engine's live phase directly in paint() and ignore this counter.
// rateIdx == 0 means Off, so we don't advance.
void LoopSaboteurEditor::ModPage::WaveformPreview::advancePreview (double dtSeconds) noexcept
{
    if (rateIdx <= 0)
        return;

    // v0.37 — rate meaning now depends on sync state. Synced LFOs
    // still derive seconds/cycle from quarter-notes × BPM; free LFOs
    // come straight off an Hz table (mirrors lfoHzIndexToHz in
    // PluginProcessor.cpp).
    const bool synced = (proc != nullptr) ? proc->getLfoSync (sceneIdx, lfoIdx) : true;

    double secPerCycle = 0.0;
    if (synced)
    {
        const double qPerCycle = quartersPerLfoCycle (rateIdx);
        if (qPerCycle <= 0.0001) return;
        const double hostBpm = (proc != nullptr) ? proc->getLastKnownBpm() : 120.0;
        const double bpm     = (hostBpm > 1.0) ? hostBpm : 120.0;
        secPerCycle = qPerCycle * (60.0 / bpm);
    }
    else
    {
        static const double hzTable[] = {
            0.0, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 12.0, 16.0, 20.0, 30.0
        };
        const int idx = juce::jlimit (0, 12, rateIdx);
        const double hz = hzTable[idx];
        if (hz <= 1.0e-6) return;
        secPerCycle = 1.0 / hz;
    }

    // Long cycles would look frozen. For free-running preview (host
    // stopped) we cap the displayed cycle time so 16-bar / 0.05 Hz
    // rates still scroll visibly. When the host IS playing, paint()
    // uses the engine's actual phase and this cap doesn't apply.
    constexpr double maxSecPerCycle = 4.0;
    const double effectiveSec = juce::jmin (secPerCycle, maxSecPerCycle);

    previewPhase += dtSeconds / effectiveSec;
    // keep bounded to avoid precision drift over very long sessions
    if (previewPhase > 1.0e6) previewPhase = std::fmod (previewPhase, 1024.0);
}

// Oscilloscope-style preview: shows ~2 cycles scrolling right-to-left.
// The right edge is "now" (current phase), the left edge is the past.
// When rate == Off, waveform is dimmed and static.
void LoopSaboteurEditor::ModPage::WaveformPreview::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f);

    // Background panel with orange top accent
    g.setColour (juce::Colour (0xff1a1a1a));
    g.fillRect (r);
    g.setColour (juce::Colour (0xffff5a3c).withAlpha (0.3f));
    g.fillRect (r.withHeight (3.0f));
    g.setColour (juce::Colour (0xff555555));
    g.drawRect (r, 1.0f);

    if (r.isEmpty())
        return;

    // Zero-line guide
    const float w  = r.getWidth();
    const float cy = r.getCentreY();
    const float maxScale = r.getHeight() * 0.32f;
    g.setColour (juce::Colour (0xff333333));
    g.drawHorizontalLine ((int) cy, r.getX(), r.getRight());

    // v0.33/v0.34 — scale the drawn amplitude by the LFO's depth so
    // the preview actually visualises "how much modulation is happening".
    // Depth is now BIPOLAR (-1..+1): absolute value controls amplitude,
    // sign controls vertical polarity (negative = waveform flipped).
    // A small floor (0.05) keeps the wave barely visible even at zero
    // depth so shape selection remains legible.
    const float depthSigned = (proc != nullptr)
        ? juce::jlimit (-1.0f, 1.0f, proc->getLfoDepth (sceneIdx, lfoIdx))
        : 1.0f;
    const float depthAbs = std::abs (depthSigned);
    const float depthSign = (depthSigned < 0.0f) ? -1.0f : 1.0f;

    // v0.42.5 — X-Mod live amplitude feedback. Two behaviour changes
    // over v0.42.3:
    //
    //   1. We now sample the source's lastOutput (already depth-scaled
    //      by the source's own depth, matching what the engine's
    //      cross-mod loop actually reads) instead of re-deriving the
    //      source's shape value from its phase. The old approach was
    //      broken for ENV sources — env shapes don't advance `phase`
    //      at all (the engine uses envStage/envValue), so cross-mod
    //      from an env source showed zero motion in the preview even
    //      though the audio engine was using its envelope correctly.
    //
    //   2. The modulation is evaluated at EACH x-pixel across the
    //      display, not just once per repaint. This shows the
    //      cross-mod as an amplitude envelope riding ALONG the
    //      displayed waveform — you can see the shape of the source's
    //      influence spread across time, not just a uniform scale of
    //      the whole scope. Source phase at each x is derived from
    //      the source's current phase plus a proportional offset
    //      relative to the cycles shown. For env sources we scale the
    //      envValue directly via lastOutput.
    //
    // Convention: crossDepth is signed (-1..+1). We match the engine
    // formula in PluginProcessor.cpp:
    //   effDepth = jlimit(-1, +1, depth * (1 + depthMod))
    // where depthMod = sum(sourceLastOutput * sourceCrossDepth).

    // First pass: collect the active cross-mod sources for this target
    // so we can (a) build the badge string and (b) reuse the data in
    // the per-x amplitude loop below without re-scanning every pixel.
    struct XSource {
        int   srcIdx;
        int   srcShape;
        float crossDepth;
        float lastOutputNow;
        double phaseNow;
        int   rateIdx;
    };
    std::array<XSource, 4> xSources {};
    int numXSources = 0;
    juce::String xmodSources;      // e.g. "← MOD 2" or "← MOD 2, 3"
    if (proc != nullptr)
    {
        for (int src = 0; src < 4; ++src)
        {
            if (src == lfoIdx) continue;
            const int  tgt = proc->getLfoCrossTarget (sceneIdx, src);
            const float cd = proc->getLfoCrossDepth  (sceneIdx, src);
            if (tgt != lfoIdx || std::abs (cd) < 1.0e-4f) continue;

            auto& xs = xSources[numXSources++];
            xs.srcIdx        = src;
            xs.srcShape      = proc->getLfoShape (sceneIdx, src);
            xs.crossDepth    = cd;
            xs.lastOutputNow = proc->getLfoLastOutput (sceneIdx, src);
            xs.phaseNow      = proc->getLfoPhase      (sceneIdx, src);
            xs.rateIdx       = proc->getLfoRateIdx    (sceneIdx, src);

            // v0.42.3 — must go through juce::String::fromUTF8; the raw
            // narrow-string ctor decodes with the platform default and
            // turns "\xe2\x86\x90" into "â" (a.k.a. mojibake).
            xmodSources += (xmodSources.isEmpty()
                              ? juce::String::fromUTF8 ("\xe2\x86\x90 MOD ")
                              : juce::String (", "))
                         + juce::String (src + 1);
        }
    }

    // xmodGainNow: current instantaneous depthMod, used for the badge
    // pulse colour. Matches engine convention — sum of per-source
    // (lastOutput * crossDepth). Can be negative (inverting) or
    // positive (additive), clamped for display.
    float xmodGainNow = 0.0f;
    for (int i = 0; i < numXSources; ++i)
        xmodGainNow += xSources[i].lastOutputNow * xSources[i].crossDepth;
    xmodGainNow = juce::jlimit (-2.0f, 2.0f, xmodGainNow);

    // Helper to compute a per-x amplitude scale from the current
    // waveform point's phase offset. Called for every x inside the
    // draw loops below. Returns the signed scale already multiplied
    // by maxScale; callers just do `y = cy - val * perXScale`.
    auto effDepthAtPhaseOffset = [&] (double nxAcross) -> float
    {
        if (numXSources == 0)
            return maxScale * depthAbs * depthSign;

        // For each active source, approximate its value at this x.
        // Continuous shapes: advance/rewind its phase proportionally
        // to how far we are across the displayed target cycles.
        // Env shapes: use the cached lastOutput (env doesn't cycle
        // across the scope, it's a one-shot).
        float depthMod = 0.0f;
        for (int i = 0; i < numXSources; ++i)
        {
            const auto& xs = xSources[i];
            float srcVal;
            if (xs.srcShape == (int) LoopSaboteurProcessor::kLfoEnvAHD
                || xs.srcShape == (int) LoopSaboteurProcessor::kLfoEnvFollow)
            {
                // Env/Follow is monopolar 0..+1; lastOutput already has depth
                // baked in, so we use it as-is.
                srcVal = xs.lastOutputNow;
            }
            else
            {
                // Walk the source's phase across the same window the
                // target occupies. nxAcross==1 means "now" (rightmost
                // pixel on target), nxAcross==0 means "cyclesShown
                // cycles ago" in target-time.
                const double tgtCyclesBack = (1.0 - nxAcross) * 2.0;  // assume 2 cycles shown
                // Convert target-time back into source-time. Without
                // actual per-LFO rate tracking we approximate by
                // assuming source was running for the same duration.
                const double srcPhaseHere = xs.phaseNow - tgtCyclesBack;
                srcVal = evalLfoShape (xs.srcShape, srcPhaseHere);
                // Not depth-scaled: we approximate by applying the
                // raw shape × crossDepth, which is close to what the
                // engine does since the source's depth usually
                // multiplies the final effect proportionally.
            }
            depthMod += srcVal * xs.crossDepth;
        }
        depthMod = juce::jlimit (-2.0f, 2.0f, depthMod);

        // Match the engine: effDepth = jlimit(-1, +1, depth * (1 + mod))
        const float effSigned = juce::jlimit (-1.0f, 1.0f,
                                              depthSigned * (1.0f + depthMod));
        // Visibility floor so the wave stays drawable even at zero.
        const float effAbs = juce::jmax (0.05f, std::abs (effSigned));
        const float effSign = (effSigned < 0.0f) ? -1.0f : 1.0f;
        return maxScale * effAbs * effSign;
    };

    // A single "now" scale used for env-mode drawing (which isn't a
    // cycling scope). LFO mode uses effDepthAtPhaseOffset per pixel.
    const float effDepthNow = juce::jmax (0.05f, depthAbs * (1.0f + xmodGainNow));
    const float scale = maxScale * effDepthNow * depthSign;

    // v0.34 — env mode: draw a single AHD envelope cycle spanning the
    // scope. Monopolar (zero line = idle, peak above), flipped below
    // the zero line when depth is negative.
    const bool isEnv    = (shape == (int) LoopSaboteurProcessor::kLfoEnvAHD);
    const bool isFollow = (shape == (int) LoopSaboteurProcessor::kLfoEnvFollow);

    // "Off" state: rate==Off only applies to LFO shapes; env/follow modes
    // ignore rate, so they're never considered "off".
    const bool isOff = (! isEnv && ! isFollow) && (rateIdx <= 0);
    const juce::Colour waveColour = isOff
        ? juce::Colour (0xff555555)
        : juce::Colour (0xffff5a3c);

    juce::Path path;
    path.preallocateSpace (256);
    const float step = 1.0f;
    bool first = true;

    if (isEnv)
    {
        // One full AHD cycle across the scope width. Envelope output
        // is 0..+1, so we draw using a POSITIVE-only amplitude that
        // departs from the zero line. Scale already carries the sign
        // (negative depth → drawn below the zero line).
        const float aMs = (proc != nullptr) ? proc->getLfoAttackMs (sceneIdx, lfoIdx) : 5.0f;
        const float hMs = (proc != nullptr) ? proc->getLfoHoldMs   (sceneIdx, lfoIdx) : 0.0f;
        const float dMs = (proc != nullptr) ? proc->getLfoDecayMs  (sceneIdx, lfoIdx) : 200.0f;
        const float totalMs = juce::jmax (1.0f, aMs + hMs + dMs);

        const float aFrac = aMs / totalMs;
        const float hFrac = hMs / totalMs;
        // const float dFrac = dMs / totalMs; // implied by remainder

        for (float x = r.getX(); x <= r.getRight(); x += step)
        {
            const float nx = (x - r.getX()) / w;           // 0..1 across widget

            float env01;
            if (nx < aFrac)
                env01 = (aFrac > 0.0f) ? (nx / aFrac) : 1.0f;         // attack ramp
            else if (nx < aFrac + hFrac)
                env01 = 1.0f;                                         // hold
            else
            {
                const float t = nx - aFrac - hFrac;
                const float dFracLocal = juce::jmax (1.0e-4f, 1.0f - aFrac - hFrac);
                env01 = juce::jlimit (0.0f, 1.0f, 1.0f - (t / dFracLocal));   // decay
            }

            const float y = cy - env01 * scale;
            if (first) { path.startNewSubPath (x, y); first = false; }
            else       { path.lineTo (x, y); }
        }

        g.setColour (waveColour);
        g.strokePath (path, juce::PathStrokeType (1.5f));

        // Faint stage dividers so the A / H / D proportions are legible.
        g.setColour (juce::Colour (0xff333333));
        const float xA = r.getX() + aFrac * w;
        const float xH = r.getX() + (aFrac + hFrac) * w;
        g.drawVerticalLine ((int) xA, r.getY() + 2.0f, r.getBottom() - 2.0f);
        if (hFrac > 1.0e-4f)
            g.drawVerticalLine ((int) xH, r.getY() + 2.0f, r.getBottom() - 2.0f);
    }
    else if (isFollow)
    {
        // v0.5.0 — Envelope follower: show the current tracked level as
        // a filled bar from the zero line. The bar bounces with input
        // amplitude — gives visual feedback that the follower is alive.
        float envVal = 0.0f;
        if (proc != nullptr)
            envVal = proc->getLfoOutput (sceneIdx, lfoIdx);
        // envVal is bipolar (depth already applied). We want monopolar display.
        const float absVal = juce::jlimit (0.0f, 1.0f, std::abs (envVal));
        const float barH = absVal * maxScale;
        const float barTop = cy - barH * depthSign;

        g.setColour (waveColour.withAlpha (0.25f));
        g.fillRect (r.getX(), juce::jmin (cy, barTop),
                    r.getWidth(), std::abs (barTop - cy));
        g.setColour (waveColour);
        g.drawHorizontalLine ((int) barTop, r.getX(), r.getRight());

        // Label
        g.setColour (juce::Colour (0xff888888));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("ENV FOLLOW", r.reduced (4), juce::Justification::topLeft);
    }
    else
    {
        // LFO mode — scrolling oscilloscope.
        //
        // v0.33 — when host is playing, read the live LFO phase from
        // the processor so the preview is actually synchronised. When
        // stopped, fall back to the free-running previewPhase that the
        // editor timer has been advancing at the real BPM. At slower
        // rates (multi-bar), showing 2 cycles makes the waveform look
        // cramped/scrolly; show a single cycle instead.
        double nowPhase = previewPhase;
        if (proc != nullptr && proc->getIsHostPlaying())
            nowPhase = proc->getLfoPhase (sceneIdx, lfoIdx);

        const float cyclesShown = (quartersPerLfoCycle (rateIdx) >= 4.0) ? 1.0f : 2.0f;

        for (float x = r.getX(); x <= r.getRight(); x += step)
        {
            const float nx = (x - r.getX()) / w;              // 0..1 across widget
            const double phaseAtX = nowPhase - cyclesShown + (double) nx * cyclesShown;
            const float val = evalLfoShape (shape, phaseAtX);
            // v0.42.5 — per-x effective scale so cross-mod paints an
            // amplitude envelope along the waveform rather than a
            // uniform global scale change frame-to-frame.
            const float scaleAtX = effDepthAtPhaseOffset ((double) nx);
            const float y = cy - val * scaleAtX;
            if (first) { path.startNewSubPath (x, y); first = false; }
            else       { path.lineTo (x, y); }
        }

        g.setColour (waveColour);
        g.strokePath (path, juce::PathStrokeType (1.5f));

        // "Now" indicator at the right edge — only shown when running.
        if (! isOff)
        {
            const float rx = r.getRight() - 1.0f;
            g.setColour (waveColour.withAlpha (0.5f));
            g.drawVerticalLine ((int) rx, r.getY(), r.getBottom());
            const float nowVal = evalLfoShape (shape, nowPhase);
            // Use the "now" scale (rightmost pixel) so the dot sits on
            // the waveform, which has its own per-x scale.
            const float nowScale = effDepthAtPhaseOffset (1.0);
            const float dotY = cy - nowVal * nowScale;
            g.setColour (waveColour);
            g.fillEllipse (rx - 3.0f, dotY - 3.0f, 6.0f, 6.0f);
        }
        else
        {
            // "OFF" caption, subtly.
            g.setColour (juce::Colour (0xff777777));
            g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            g.drawText ("OFF", r.reduced (4, 4), juce::Justification::topRight);
        }
    }

    // v0.42.3 — "← MOD N" X-Mod source badge. Draw in the top-left
    // corner (the "← MOD 2" reads as "being modulated by MOD 2").
    // Colour pulsates subtly with the combined xmodGain to reinforce
    // the "live" feeling. Skipped when no sources are modulating.
    if (xmodSources.isNotEmpty())
    {
        const float pulse = juce::jlimit (0.0f, 1.0f,
                                           0.55f + 0.45f * std::abs (xmodGainNow));
        g.setColour (juce::Colour (0xff4fd1e0).withAlpha (pulse));
        g.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
        g.drawText (xmodSources, r.reduced (4, 4), juce::Justification::topLeft);
    }
}

// ModPage main class
void LoopSaboteurEditor::ModPage::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();

    // Background panel
    g.setColour (juce::Colour (0xff1a1a1a));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (juce::Colour (0xffff5a3c).withAlpha (0.2f));
    g.drawRoundedRectangle (r, 6.0f, 1.0f);

    // Title header with Impact font skew
    auto headerR = r.withHeight (50.0f);
    g.setColour (juce::Colour (0xffff5a3c));
    auto font = juce::Font (juce::FontOptions ("Impact", 28.0f, juce::Font::plain));
    font.setExtraKerningFactor (0.15f);
    g.setFont (font);
    float anchorY = headerR.getCentreY();
    g.saveState();
    g.addTransform (juce::AffineTransform::translation (0.0f, -anchorY)
                        .sheared (-0.10f, 0.0f)
                        .translated (0.0f, anchorY));
    g.drawText ("MODULATION", headerR.reduced (16, 0), juce::Justification::centredLeft);
    g.restoreState();
}

void LoopSaboteurEditor::ModPage::resized()
{
    auto bounds = getLocalBounds().reduced (8);
    auto headerArea = bounds.removeFromTop (50);
    auto tabArea = bounds.removeFromTop (40);

    if (!proc)
        return;

    // Act tab buttons: 8 equal-width buttons across the tab area
    const int tabButtonW = tabArea.getWidth() / 8;
    for (int i = 0; i < 8; ++i)
    {
        int x = tabArea.getX() + i * tabButtonW;
        actTabButtons[i].setBounds (x, tabArea.getY(), tabButtonW - 2, 28);
    }

    // 2x2 grid of LFO panels
    const int panelW = bounds.getWidth() / 2;
    const int panelH = bounds.getHeight() / 2;

    for (int lfo = 0; lfo < 4; ++lfo)
    {
        int col = lfo % 2;
        int row = lfo / 2;
        int panelX = bounds.getX() + col * panelW;
        int panelY = bounds.getY() + row * panelH;
        auto panelBounds = juce::Rectangle<int> (panelX, panelY, panelW - 4, panelH - 4).reduced (4);

        // v0.42.4 — budget analysis. Each panel gets ~240pt. Env mode
        // needs: preview + shape row + 3×AHD + depth + target + retrigger
        // + X-Mod. At 70/20/20/20/22 that came out to 248pt and the
        // X-Mod row overlapped Retrigger / spilled into the next panel.
        // In Env mode only, shrink the preview to 56pt and the per-row
        // height to 18pt — that comes in ~222pt and leaves the X-Mod
        // row visible with breathing space. LFO mode is unchanged.
        const int selShape = shapeBoxes[lfo].getSelectedItemIndex();
        const bool isEnv = (selShape == (int) LoopSaboteurProcessor::kLfoEnvAHD);
        const bool isFollow = (selShape == (int) LoopSaboteurProcessor::kLfoEnvFollow);
        const int previewH = (isEnv || isFollow) ? 56 : 70;
        const int envRowH  = 18;

        // Waveform preview
        previews[lfo].setBounds (panelBounds.removeFromTop (previewH).reduced (2));

        // Shape label + selector
        shapeLabels[lfo].setBounds (panelBounds.removeFromTop (16));
        shapeBoxes[lfo].setBounds (panelBounds.removeFromTop (20).reduced (0, 2));

        // v0.42.2 — mode-aware layout. The Rate row and the A/H/D row
        // USED to share the same rectangle (36pt tall), which looked
        // fine for a single Rate combo but crammed three vertical
        // sliders + textboxes into 20pt — the textboxes overlapped
        // labels and each other. Now the two modes use separate rects
        // and the AHD stack gets three dedicated rows so each slider
        // breathes.
        //
        // Env mode also puts "Retrigger Mode:" between Target and the
        // trig combo, and we ALWAYS lay out X-Mod at the bottom so the
        // user doesn't have to guess whether cross-mod is supported.

        if (! isEnv)
        {
            // LFO mode: Rate: label + [combo | SYNC/FREE].
            auto rateRow  = panelBounds.removeFromTop (16);
            rateLabels[lfo].setBounds (rateRow);
            auto rateCtl  = panelBounds.removeFromTop (20).reduced (0, 2);
            const int syncW = 48;
            rateBoxes[lfo].setBounds (rateCtl.withTrimmedRight (syncW + 4));
            syncButtons[lfo].setBounds (rateCtl.removeFromRight (syncW));

            // v0.50 — Envelope follower: show sensitivity slider where the
            // AHD row would normally go.
            if (isFollow)
            {
                const int labelW = 28;
                auto gainRow = panelBounds.removeFromTop (envRowH).reduced (0, 2);
                envFollowGainLabels[lfo].setBounds (gainRow.removeFromLeft (labelW));
                envFollowGainSliders[lfo].setBounds (gainRow);
            }
            else
            {
                // Hide gain slider for regular LFO mode
                auto hidden = juce::Rectangle<int> (rateRow.getX(), rateRow.getY(),
                                                    rateRow.getWidth(), 0);
                envFollowGainLabels[lfo] .setBounds (hidden);
                envFollowGainSliders[lfo].setBounds (hidden);
            }

            // Still set AHD slider bounds (hidden, but bounds need to
            // be sane so they don't land at 0,0,0,0 if shape flips).
            auto hidden = juce::Rectangle<int> (rateRow.getX(), rateRow.getY(),
                                                rateRow.getWidth(), 0);
            attackLabels[lfo] .setBounds (hidden);
            attackSliders[lfo].setBounds (hidden);
            holdLabels[lfo]   .setBounds (hidden);
            holdSliders[lfo]  .setBounds (hidden);
            decayLabels[lfo]  .setBounds (hidden);
            decaySliders[lfo] .setBounds (hidden);
        }
        else
        {
            // Env AHD mode: three dedicated rows. Each row is
            // [inline label (28pt wide) | horizontal bar slider
            // stretching the rest of the row].
            // v0.42.4 — rows shrunk 20→18pt to reclaim ~6pt across
            // the three AHD rows (see panel budget note above).
            const int labelW = 28;
            auto makeRow = [&] (juce::Label& lbl, juce::Slider& sld)
            {
                auto row = panelBounds.removeFromTop (envRowH).reduced (0, 2);
                lbl.setBounds (row.removeFromLeft (labelW));
                sld.setBounds (row);
            };
            makeRow (attackLabels[lfo], attackSliders[lfo]);
            makeRow (holdLabels[lfo],   holdSliders[lfo]);
            makeRow (decayLabels[lfo],  decaySliders[lfo]);

            // Hide the rate row and gain slider by parking them off-layout.
            auto hidden = juce::Rectangle<int> (panelBounds.getX(),
                                                panelBounds.getY(), 0, 0);
            rateLabels[lfo].setBounds (hidden);
            rateBoxes[lfo] .setBounds (hidden);
            syncButtons[lfo].setBounds (hidden);
            envFollowGainLabels[lfo] .setBounds (hidden);
            envFollowGainSliders[lfo].setBounds (hidden);
        }

        // v0.42.3 — Depth / Target / Retrigger all go INLINE in env mode
        // (single row, label on the left, control to the right) to
        // make room for the taller AHD stack above. Non-env mode keeps
        // the stacked label-above-control look unchanged.
        // v0.42.4 — inline rows shrunk 20→18pt (envRowH) to free up
        // another 6pt so X-Mod fits below without overlap.
        auto layoutInlineRow = [&] (juce::Label& lbl, juce::Component& ctrl,
                                     int inlineLabelW)
        {
            auto row = panelBounds.removeFromTop (envRowH).reduced (0, 2);
            lbl.setBounds  (row.removeFromLeft (inlineLabelW));
            ctrl.setBounds (row);
        };

        if (isEnv)
        {
            layoutInlineRow (depthLabels[lfo],  depthSliders[lfo], 52);
            layoutInlineRow (targetLabels[lfo], targetBoxes[lfo],  52);
        }
        else
        {
            depthLabels[lfo].setBounds (panelBounds.removeFromTop (16));
            depthSliders[lfo].setBounds (panelBounds.removeFromTop (20).reduced (0, 2));
            targetLabels[lfo].setBounds (panelBounds.removeFromTop (16));
            targetBoxes[lfo].setBounds (panelBounds.removeFromTop (20).reduced (0, 2));
        }

        // v0.42.2 — "Retrigger Mode:" + combo, only laid out (and only
        // visible, via applyShapeVisibility) in Env mode. Lives between
        // Target and X-Mod so the common rows stay in the same spots
        // regardless of shape.
        // v0.42.3 — always inline in env mode (saves 16pt vs stacked).
        if (isEnv && panelBounds.getHeight() >= envRowH)
        {
            layoutInlineRow (trigLabels[lfo], trigBoxes[lfo], 100);
        }
        else
        {
            auto hidden = juce::Rectangle<int> (panelBounds.getX(),
                                                panelBounds.getY(), 0, 0);
            trigLabels[lfo].setBounds (hidden);
            trigBoxes[lfo] .setBounds (hidden);
        }

        // v0.38 — cross-mod row. One line: label + target combo + depth
        // slider, packed horizontally so we don't eat extra vertical
        // space.
        // v0.42.2 — previously gated on `panelBounds.getHeight() >= 20`
        // which silently dropped X-Mod on tight layouts, making it look
        // like the feature didn't exist at all. Now we reserve the row
        // unconditionally; if we run out of vertical room we let the
        // row shrink to whatever's left rather than hide it entirely.
        {
            const int wanted = 22;
            const int have   = juce::jmax (16, juce::jmin (wanted, panelBounds.getHeight()));
            auto xRow = panelBounds.removeFromTop (have).reduced (0, 2);
            const int labelW = 48;
            const int comboW = juce::jmin (96, (xRow.getWidth() - labelW) / 2);
            crossTargetLabels[lfo].setBounds (xRow.removeFromLeft (labelW));
            crossTargetBoxes[lfo] .setBounds (xRow.removeFromLeft (comboW));
            crossDepthSliders[lfo].setBounds (xRow);
        }
    }
}

void LoopSaboteurEditor::ModPage::setupLfoControls()
{
    if (!proc)
        return;

    // v0.34 — shape list must match LoopSaboteurProcessor::LfoShape order.
    static const char* shapeNames[] = { "Sine", "Triangle", "Saw", "Square", "S&H", "Env AHD", "Env Follow" };
    constexpr int kNumShapeItems = 7;
    // v0.33 — mirrors kLfoDivisionChoices in PluginProcessor.cpp. Index 0 = Off.
    // v0.37 — FREE mode uses a parallel Hz table; the rate combo is
    // repopulated on the fly when the sync toggle flips. See
    // repopulateRateBox().
    constexpr int kNumLfoRateItems = 13;
    // v0.34 — trigger modes (match LoopSaboteurProcessor::LfoTrigMode order).
    static const char* trigNames[] = {
        "Slice", "Active Steps", "Active + Locks", "All Steps"
    };
    constexpr int kNumTrigItems = 4;
    static const char* actNames[] = { "A", "B", "C", "D", "E", "F", "G", "H" };

    // Setup Act tab buttons with stencil styling.
    // setClickingTogglesState(true) is required so the radio-group
    // visual "on" state updates when a tab is clicked — without it the
    // radio logic picks the right tab but the highlight stays on A.
    for (int i = 0; i < 8; ++i)
    {
        actTabButtons[i].setButtonText (actNames[i]);
        actTabButtons[i].setClickingTogglesState (true);
        actTabButtons[i].setRadioGroupId (100);
        actTabButtons[i].setColour (juce::TextButton::buttonColourId,   juce::Colour (0x00000000));
        actTabButtons[i].setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff999999));
        actTabButtons[i].setColour (juce::TextButton::buttonOnColourId, juce::Colour (0x22ff5a3c));
        actTabButtons[i].setColour (juce::TextButton::textColourOnId,   juce::Colour (0xffff5a3c));
        actTabButtons[i].onClick = [this, i] { setCurrentActTab (i); };
        addAndMakeVisible (actTabButtons[i]);
    }
    actTabButtons[0].setToggleState (true, juce::dontSendNotification);
    currentActTab = 0;

    // Setup 4 LFO panels
    for (int lfo = 0; lfo < 4; ++lfo)
    {
        previews[lfo].sceneIdx = currentActTab;
        previews[lfo].lfoIdx = lfo;
        previews[lfo].proc = proc;

        // Shape box
        juce::String shapeLabel;
        shapeLabel << "MOD " << (lfo + 1);
        shapeLabels[lfo].setText (shapeLabel, juce::dontSendNotification);
        shapeLabels[lfo].setJustificationType (juce::Justification::centredLeft);
        shapeLabels[lfo].setColour (juce::Label::textColourId, juce::Colour (0xffff5a3c));
        shapeLabels[lfo].setFont (juce::Font ("Impact", 20.0f, juce::Font::plain));
        addAndMakeVisible (shapeLabels[lfo]);

        shapeBoxes[lfo].clear();
        for (int i = 0; i < kNumShapeItems; ++i)
            shapeBoxes[lfo].addItem (shapeNames[i], i + 1);
        shapeBoxes[lfo].onChange = [this, lfo]
        {
            if (proc)
                proc->setLfoShape (currentActTab, lfo, shapeBoxes[lfo].getSelectedItemIndex());
            previews[lfo].shape = shapeBoxes[lfo].getSelectedItemIndex();
            // v0.34 — swap which row of controls is visible (Rate+Sync
            // for LFOs, A/H/D + Trig for envelopes).
            applyShapeVisibility (lfo);
            // v0.42.2 — resized() now branches on shape (Env uses a
            // 3-row AHD stack, Rate uses a single row). Call it here so
            // the geometry updates immediately when the user flips
            // shape, not on the next external layout event.
            resized();
            previews[lfo].repaint();
        };
        addAndMakeVisible (shapeBoxes[lfo]);

        // Rate box
        rateLabels[lfo].setText ("Rate:", juce::dontSendNotification);
        rateLabels[lfo].setJustificationType (juce::Justification::centredLeft);
        rateLabels[lfo].setColour (juce::Label::textColourId, juce::Colour (0xffebebf0));
        addAndMakeVisible (rateLabels[lfo]);

        // v0.37 — populate with the correct names for the LFO's
        // current sync state (bar divisions vs Hz).
        repopulateRateBox (lfo, proc->getLfoSync (currentActTab, lfo));
        rateBoxes[lfo].onChange = [this, lfo]
        {
            const int idx = rateBoxes[lfo].getSelectedItemIndex();
            if (proc)
                proc->setLfoRateIdx (currentActTab, lfo, idx);
            previews[lfo].rateIdx = idx;
            // When the user flips from Off to a real rate, reset the
            // preview phase so the wave starts from zero.
            if (idx > 0 && previews[lfo].previewPhase == 0.0)
                previews[lfo].previewPhase = 0.0; // explicit no-op for clarity
            previews[lfo].repaint();
        };
        addAndMakeVisible (rateBoxes[lfo]);

        // v0.34 — SYNC / FREE toggle. SYNC = phase derives from host PPQ
        // (lockstep across instances). FREE = runs on the plugin's own
        // sample clock, keeps moving when transport is stopped.
        // v0.36 — segmented "SYNC|FREE" display. Both options are ALWAYS
        // visible; the active half is bright orange, the inactive half
        // is dim grey. Clicking toggles. Removes the old "is this a
        // state or the next action?" ambiguity.
        syncButtons[lfo].setClickingTogglesState (true);
        syncButtons[lfo].setButtonText ("SYNC|FREE");
        syncButtons[lfo].setTooltip ("SYNC locks LFO phase to host PPQ. FREE runs on the plugin clock. Click to toggle.");
        // Tell the L&F to paint this as a segmented toggle — handled by
        // drawButtonText via the "segmentedToggle" property. Button fill
        // stays transparent.
        syncButtons[lfo].setColour (juce::TextButton::buttonColourId,    juce::Colour (0x00000000));
        syncButtons[lfo].setColour (juce::TextButton::buttonOnColourId,  juce::Colour (0x00000000));
        syncButtons[lfo].setColour (juce::TextButton::textColourOffId,   juce::Colour (0xff888888));
        syncButtons[lfo].setColour (juce::TextButton::textColourOnId,    juce::Colour (0xffff5a3c));
        syncButtons[lfo].getProperties().set ("segmentedToggle", juce::var ("SYNC|FREE"));
        syncButtons[lfo].onClick = [this, lfo]
        {
            const bool s = syncButtons[lfo].getToggleState();
            if (proc)
                proc->setLfoSync (currentActTab, lfo, s);
            // v0.37 — flipping sync changes what the rate knob *means*.
            // Rebuild its items so the user sees "1/8" vs "2 Hz" match
            // the actual engine behaviour. Index is preserved.
            repopulateRateBox (lfo, s);
            syncButtons[lfo].repaint();
        };
        addAndMakeVisible (syncButtons[lfo]);

        // v0.34 — envelope A / H / D sliders. Visible only when shape
        // is Env AHD. Ranges match processor clamps.
        // v0.42.2 — setupEnvSlider rewritten. Previous implementation
        // used a LinearVertical slider with a 14pt TextBoxBelow inside a
        // ~20pt-tall slot — the textbox was squashed, overlapped the
        // label above it, and made the three A/H/D widgets look like
        // they were colliding. Now we use a horizontal bar (value drawn
        // in-track, no separate textbox) and a taller dedicated row in
        // resized(). The label sits to the LEFT of the slider (inline),
        // not above it.
        auto setupEnvSlider = [this, lfo] (juce::Slider& s, juce::Label& lbl,
                                           const char* labelText,
                                           float minV, float maxV, float defV,
                                           std::function<void(float)> setter)
        {
            lbl.setText (labelText, juce::dontSendNotification);
            lbl.setJustificationType (juce::Justification::centredLeft);
            lbl.setColour (juce::Label::textColourId, juce::Colour (0xffebebf0));
            lbl.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
            addAndMakeVisible (lbl);

            s.setRange ((double) minV, (double) maxV, 0.1);
            s.setValue ((double) defV, juce::dontSendNotification);
            s.setSliderStyle (juce::Slider::LinearBar);      // value drawn in-track
            s.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 0, 0);  // suppress separate textbox
            s.setTextValueSuffix (" ms");
            // v0.42.3 — LinearBar fill was defaulting to the LookAndFeel's
            // generic slider track (dark navy-ish), which made the bars
            // invisible against the panel background. Force the filled
            // portion to accent-orange, the unfilled portion to a mid-grey
            // so the bar is always legible.
            s.setColour (juce::Slider::trackColourId,      juce::Colour (0xffff5a3c));
            s.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff2a2a2a));
            s.setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffffffff));
            s.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0x00000000));
            s.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0x00000000));
            s.onValueChange = [&s, setter]
            {
                setter ((float) s.getValue());
            };
            addAndMakeVisible (s);
        };
        setupEnvSlider (attackSliders[lfo], attackLabels[lfo], "A", 0.1f, 2000.0f, 5.0f,
                        [this, lfo] (float v) { if (proc) proc->setLfoAttackMs (currentActTab, lfo, v); previews[lfo].repaint(); });
        setupEnvSlider (holdSliders[lfo],   holdLabels[lfo],   "H", 0.0f, 2000.0f, 0.0f,
                        [this, lfo] (float v) { if (proc) proc->setLfoHoldMs   (currentActTab, lfo, v); previews[lfo].repaint(); });
        setupEnvSlider (decaySliders[lfo],  decayLabels[lfo],  "D", 0.1f, 2000.0f, 200.0f,
                        [this, lfo] (float v) { if (proc) proc->setLfoDecayMs  (currentActTab, lfo, v); previews[lfo].repaint(); });

        // v0.50 — Envelope follower sensitivity slider (follow-only).
        envFollowGainLabels[lfo].setText ("Sensitivity:", juce::dontSendNotification);
        envFollowGainLabels[lfo].setJustificationType (juce::Justification::centredLeft);
        envFollowGainLabels[lfo].setColour (juce::Label::textColourId, juce::Colour (0xffebebf0));
        addAndMakeVisible (envFollowGainLabels[lfo]);

        envFollowGainSliders[lfo].setRange (1.0, 10.0, 0.1);
        envFollowGainSliders[lfo].setSliderStyle (juce::Slider::LinearHorizontal);
        envFollowGainSliders[lfo].setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
        envFollowGainSliders[lfo].setDoubleClickReturnValue (true, 1.0);
        envFollowGainSliders[lfo].setColour (juce::Slider::trackColourId,      juce::Colour (0xffff5a3c));
        envFollowGainSliders[lfo].setColour (juce::Slider::backgroundColourId, juce::Colour (0xff2a2a2a));
        envFollowGainSliders[lfo].setColour (juce::Slider::textBoxTextColourId, juce::Colour (0xffffffff));
        envFollowGainSliders[lfo].setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0x00000000));
        envFollowGainSliders[lfo].setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0x00000000));
        envFollowGainSliders[lfo].onValueChange = [this, lfo]
        {
            if (proc)
                proc->setLfoEnvFollowGain (currentActTab, lfo, (float) envFollowGainSliders[lfo].getValue());
        };
        addAndMakeVisible (envFollowGainSliders[lfo]);

        // v0.34 — Trigger combo (env only).
        // v0.42.2 — explicit "Retrigger Mode:" label prefix, matching the
        // style of Depth: / Target: labels above. Without this label the
        // combo just says "All Steps" with no context about what it
        // controls.
        trigLabels[lfo].setText ("Retrigger Mode:", juce::dontSendNotification);
        trigLabels[lfo].setJustificationType (juce::Justification::centredLeft);
        trigLabels[lfo].setColour (juce::Label::textColourId, juce::Colour (0xffebebf0));
        addAndMakeVisible (trigLabels[lfo]);

        trigBoxes[lfo].clear();
        for (int i = 0; i < kNumTrigItems; ++i)
            trigBoxes[lfo].addItem (trigNames[i], i + 1);
        trigBoxes[lfo].setSelectedItemIndex ((int) LoopSaboteurProcessor::kTrigActiveSteps, juce::dontSendNotification);
        trigBoxes[lfo].onChange = [this, lfo]
        {
            if (proc)
                proc->setLfoTrigMode (currentActTab, lfo, trigBoxes[lfo].getSelectedItemIndex());
        };
        addAndMakeVisible (trigBoxes[lfo]);

        // Depth slider
        depthLabels[lfo].setText ("Depth:", juce::dontSendNotification);
        depthLabels[lfo].setJustificationType (juce::Justification::centredLeft);
        depthLabels[lfo].setColour (juce::Label::textColourId, juce::Colour (0xffebebf0));
        addAndMakeVisible (depthLabels[lfo]);

        // v0.34 — depth is now bipolar. Centre-detent at 0 = off;
        // positive = normal polarity, negative = inverted. For env
        // shapes, negative depth turns a rising pluck into a downward dip.
        depthSliders[lfo].setRange (-1.0, 1.0, 0.01);
        depthSliders[lfo].setSliderStyle (juce::Slider::LinearHorizontal);
        depthSliders[lfo].setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
        depthSliders[lfo].setDoubleClickReturnValue (true, 0.0);  // double-click recentres to 0
        depthSliders[lfo].onValueChange = [this, lfo]
        {
            if (proc)
                proc->setLfoDepth (currentActTab, lfo, (float) depthSliders[lfo].getValue());
            // v0.33 — preview amplitude is scaled by depth, so repaint
            // immediately instead of waiting for the next timer tick.
            previews[lfo].repaint();
        };
        addAndMakeVisible (depthSliders[lfo]);

        // Target selector
        targetLabels[lfo].setText ("Target:", juce::dontSendNotification);
        targetLabels[lfo].setJustificationType (juce::Justification::centredLeft);
        targetLabels[lfo].setColour (juce::Label::textColourId, juce::Colour (0xffebebf0));
        addAndMakeVisible (targetLabels[lfo]);

        // v0.38 — alphabetise the dropdown. We keep the item-ID scheme
        // stable (None = 1, slot s → ID s + 2) so that setSelectedId
        // still addresses items by underlying slot regardless of the
        // visual order. Then we sort the slots by display name and add
        // in that order. Selection is now driven by ID, not index.
        targetBoxes[lfo].clear();
        targetBoxes[lfo].addItem ("None", 1);
        {
            std::vector<std::pair<juce::String, int>> sorted;
            sorted.reserve ((size_t) LoopSaboteurProcessor::kNumLockableParams);
            for (int s = 0; s < LoopSaboteurProcessor::kNumLockableParams; ++s)
                sorted.emplace_back (LoopSaboteurProcessor::lockableIdForIndex (s), s);
            std::sort (sorted.begin(), sorted.end(),
                       [] (const auto& a, const auto& b)
                       { return a.first.compareNatural (b.first) < 0; });
            for (const auto& [name, slot] : sorted)
                targetBoxes[lfo].addItem (name, slot + 2);
        }
        targetBoxes[lfo].onChange = [this, lfo]
        {
            if (! proc) return;
            // Decode slot from item ID (stable), not from index (now shuffled).
            const int id   = targetBoxes[lfo].getSelectedId();
            const int slot = (id <= 1) ? -1 : (id - 2);
            proc->setLfoTargetSlot (currentActTab, lfo, slot);
        };
        addAndMakeVisible (targetBoxes[lfo]);

        // v0.38 — cross-mod target + amount. Lets this LFO modulate
        // another LFO's depth in the same Act. "— self —" is kept in
        // the list but disabled, to avoid a runaway feedback of one
        // LFO pushing its own depth to infinity (well, to 0..1 — we
        // clamp effective depth — but the interaction is confusing).
        crossTargetLabels[lfo].setText ("X-Mod:", juce::dontSendNotification);
        crossTargetLabels[lfo].setJustificationType (juce::Justification::centredLeft);
        crossTargetLabels[lfo].setColour (juce::Label::textColourId, juce::Colour (0xffebebf0));
        addAndMakeVisible (crossTargetLabels[lfo]);

        crossTargetBoxes[lfo].clear();
        crossTargetBoxes[lfo].addItem ("None", 1);
        for (int m = 0; m < LoopSaboteurProcessor::kNumLfosPerAct; ++m)
        {
            const juce::String name = "MOD " + juce::String (m + 1) + " depth";
            crossTargetBoxes[lfo].addItem (name, m + 2);
            if (m == lfo)
                crossTargetBoxes[lfo].setItemEnabled (m + 2, false);
        }
        crossTargetBoxes[lfo].onChange = [this, lfo]
        {
            if (! proc) return;
            const int selected = crossTargetBoxes[lfo].getSelectedItemIndex();
            const int crossIdx = selected - 1;  // -1 = None, 0..3 = MOD index
            proc->setLfoCrossTarget (currentActTab, lfo, crossIdx);
        };
        addAndMakeVisible (crossTargetBoxes[lfo]);

        crossDepthSliders[lfo].setRange (-1.0, 1.0, 0.01);
        crossDepthSliders[lfo].setSliderStyle (juce::Slider::LinearHorizontal);
        crossDepthSliders[lfo].setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
        crossDepthSliders[lfo].setDoubleClickReturnValue (true, 0.0);
        crossDepthSliders[lfo].onValueChange = [this, lfo]
        {
            if (proc)
                proc->setLfoCrossDepth (currentActTab, lfo,
                                        (float) crossDepthSliders[lfo].getValue());
        };
        addAndMakeVisible (crossDepthSliders[lfo]);

        // Waveform preview
        addAndMakeVisible (previews[lfo]);
    }

    // Start timer for live animation updates (~30 Hz)
    startTimer (33);
}

void LoopSaboteurEditor::ModPage::timerCallback()
{
    // 30 Hz timer — advance each scope's scrolling phase and repaint.
    constexpr double dtSeconds = 1.0 / 30.0;
    for (int i = 0; i < 4; ++i)
    {
        previews[i].advancePreview (dtSeconds);
        previews[i].repaint();
    }
}

void LoopSaboteurEditor::ModPage::setCurrentActTab (int actIdx)
{
    if (actIdx < 0 || actIdx >= 8)
        return;

    currentActTab = actIdx;

    // Explicitly drive the tab highlight — even with a radio group,
    // the JUCE TextButton can end up visually mismatched with the
    // click-triggered state. Set the winner ON and force all others OFF.
    for (int i = 0; i < 8; ++i)
        actTabButtons[i].setToggleState (i == actIdx, juce::dontSendNotification);

    // Update all 4 LFO panels to reference the new Act.
    for (int lfo = 0; lfo < 4; ++lfo)
        previews[lfo].sceneIdx = currentActTab;

    updateFromProcessor();
}

void LoopSaboteurEditor::ModPage::updateFromProcessor()
{
    if (!proc)
        return;

    for (int lfo = 0; lfo < 4; ++lfo)
    {
        int shape = proc->getLfoShape (currentActTab, lfo);
        shapeBoxes[lfo].setSelectedItemIndex (shape, juce::dontSendNotification);
        previews[lfo].shape = shape;

        // v0.37 — sync state must be queried BEFORE the rate box is
        // populated so we show bar divisions vs Hz to match the
        // Act's stored mode (each Act's 4 LFOs can have independent
        // sync settings).
        const bool synced = proc->getLfoSync (currentActTab, lfo);
        repopulateRateBox (lfo, synced);

        int rateIdx = proc->getLfoRateIdx (currentActTab, lfo);
        rateBoxes[lfo].setSelectedItemIndex (rateIdx, juce::dontSendNotification);
        previews[lfo].rateIdx = rateIdx;

        float depth = proc->getLfoDepth (currentActTab, lfo);
        depthSliders[lfo].setValue (depth, juce::dontSendNotification);

        int targetSlot = proc->getLfoTargetSlot (currentActTab, lfo);
        // v0.38 — items are no longer in slot-order (alphabetised), so
        // select by stable item ID instead of index.
        const int targetId = (targetSlot < 0) ? 1 : (targetSlot + 2);
        targetBoxes[lfo].setSelectedId (targetId, juce::dontSendNotification);

        // v0.34 — sync toggle + envelope controls.
        // v0.36 — text is now a fixed "SYNC|FREE" segmented label, so we
        // only update toggle state and repaint; L&F handles the active-
        // half highlighting.
        syncButtons[lfo].setToggleState (synced, juce::dontSendNotification);
        syncButtons[lfo].repaint();

        attackSliders[lfo].setValue ((double) proc->getLfoAttackMs (currentActTab, lfo), juce::dontSendNotification);
        holdSliders[lfo]  .setValue ((double) proc->getLfoHoldMs   (currentActTab, lfo), juce::dontSendNotification);
        decaySliders[lfo] .setValue ((double) proc->getLfoDecayMs  (currentActTab, lfo), juce::dontSendNotification);
        trigBoxes[lfo].setSelectedItemIndex (proc->getLfoTrigMode (currentActTab, lfo), juce::dontSendNotification);

        // v0.50 — envelope follower sensitivity gain.
        envFollowGainSliders[lfo].setValue ((double) proc->getLfoEnvFollowGain (currentActTab, lfo), juce::dontSendNotification);

        // v0.38 — cross-mod controls.
        const int crossIdx = proc->getLfoCrossTarget (currentActTab, lfo);
        crossTargetBoxes[lfo].setSelectedItemIndex (crossIdx + 1, juce::dontSendNotification);
        crossDepthSliders[lfo].setValue ((double) proc->getLfoCrossDepth (currentActTab, lfo),
                                         juce::dontSendNotification);

        applyShapeVisibility (lfo);
        previews[lfo].repaint();
    }
}

// v0.37 — rebuild the rate combo box items for the given LFO to show
// the rate names matching its sync state: bar divisions when synced,
// Hz values when free. Preserves the selected index across the swap
// (the user's rateIdx position is unchanged; only the label changes).
// Mirrors kLfoDivisionChoices / kLfoHzChoices in PluginProcessor.cpp.
void LoopSaboteurEditor::ModPage::repopulateRateBox (int lfoIdx, bool synced)
{
    if (lfoIdx < 0 || lfoIdx >= 4) return;

    static const char* syncNames[] = {
        "Off",
        "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4",
        "1/2", "1 bar", "2 bars", "4 bars", "8 bars", "16 bars"
    };
    static const char* freeNames[] = {
        "Off",
        "0.05 Hz", "0.1 Hz", "0.25 Hz", "0.5 Hz",
        "1 Hz", "2 Hz", "4 Hz", "8 Hz",
        "12 Hz", "16 Hz", "20 Hz", "30 Hz"
    };
    const char* const* names = synced ? syncNames : freeNames;

    const int prevIdx = rateBoxes[lfoIdx].getSelectedItemIndex();
    rateBoxes[lfoIdx].clear (juce::dontSendNotification);
    for (int i = 0; i < 13; ++i)
        rateBoxes[lfoIdx].addItem (names[i], i + 1);
    if (prevIdx >= 0)
        rateBoxes[lfoIdx].setSelectedItemIndex (prevIdx, juce::dontSendNotification);
}

// v0.34 — Toggle visibility of rate/sync controls vs envelope A/H/D/trig
// controls depending on the currently-selected shape. Called from the shape
// combobox onChange, and also by updateFromProcessor() after a state load.
void LoopSaboteurEditor::ModPage::applyShapeVisibility (int lfoIdx)
{
    if (lfoIdx < 0 || lfoIdx >= 4) return;

    const int shape = shapeBoxes[lfoIdx].getSelectedItemIndex();
    const bool isEnv    = (shape == (int) LoopSaboteurProcessor::kLfoEnvAHD);
    const bool isFollow = (shape == (int) LoopSaboteurProcessor::kLfoEnvFollow);

    // LFO + Follow: show rate/sync row.  Env: hide them.
    const bool showRate = (! isEnv);
    rateLabels[lfoIdx]  .setVisible (showRate);
    rateBoxes[lfoIdx]   .setVisible (showRate);
    syncButtons[lfoIdx] .setVisible (showRate && ! isFollow);  // follow doesn't need sync

    // v0.5.0 — relabel rate row for envelope follower.
    rateLabels[lfoIdx].setText (isFollow ? "Smoothing:" : "Rate:",
                                juce::dontSendNotification);

    // Envelope-only controls.
    attackLabels[lfoIdx] .setVisible (isEnv);
    attackSliders[lfoIdx].setVisible (isEnv);
    holdLabels[lfoIdx]   .setVisible (isEnv);
    holdSliders[lfoIdx]  .setVisible (isEnv);
    decayLabels[lfoIdx]  .setVisible (isEnv);
    decaySliders[lfoIdx] .setVisible (isEnv);
    trigLabels[lfoIdx]   .setVisible (isEnv);
    trigBoxes[lfoIdx]    .setVisible (isEnv);

    // v0.50 — Envelope follower sensitivity slider (follow-only).
    envFollowGainLabels[lfoIdx] .setVisible (isFollow);
    envFollowGainSliders[lfoIdx].setVisible (isFollow);
}

void LoopSaboteurEditor::ModPage::applyStencilLookAndFeel (juce::LookAndFeel* laf)
{
    // Apply stencil styling to all interactive components
    for (int i = 0; i < 8; ++i)
        actTabButtons[i].setLookAndFeel (laf);

    for (int i = 0; i < 4; ++i)
    {
        shapeBoxes[i].setLookAndFeel (laf);
        rateBoxes[i].setLookAndFeel (laf);
        depthSliders[i].setLookAndFeel (laf);
        targetBoxes[i].setLookAndFeel (laf);

        // v0.34 — envelope + sync controls.
        syncButtons[i]  .setLookAndFeel (laf);
        attackSliders[i].setLookAndFeel (laf);
        holdSliders[i]  .setLookAndFeel (laf);
        decaySliders[i] .setLookAndFeel (laf);
        trigBoxes[i]    .setLookAndFeel (laf);
        trigLabels[i]   .setLookAndFeel (laf);

        // v0.38 — cross-mod controls.
        crossTargetBoxes[i] .setLookAndFeel (laf);
        crossDepthSliders[i].setLookAndFeel (laf);
    }
}

LoopSaboteurEditor::ModPage::~ModPage()
{
    stopTimer();
    // Clean up lookAndFeel references
    applyStencilLookAndFeel (nullptr);
}
