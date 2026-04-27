#pragma once

#include <JuceHeader.h>

// Impact font helper — Linux typically doesn't have Impact installed, and
// JUCE 8's HarfBuzz text shaper segfaults when asked to shape text with a
// missing font. This returns a safe alternative on Linux.
namespace FontHelper
{
    inline juce::String impactFace()
    {
       #if JUCE_LINUX
        return "Liberation Sans";
       #else
        return "Impact";
       #endif
    }

    inline juce::Font impact (float size, int style = juce::Font::plain)
    {
        return juce::Font (juce::FontOptions (impactFace(), size, style));
    }
}
