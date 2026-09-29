#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/DmbTypes.h"

/** Tiny piano roll used both for the captured tracks and for the generated clip. */
class PianoRollView : public juce::Component
{
public:
    PianoRollView();

    void setSource (const dmb::SourceTrack& source);
    void setClip (const dmb::GeneratedClip& clip);
    void clear();

    void paint (juce::Graphics& g) override;

private:
    struct Item
    {
        int track = 0;
        int tick = 0;
        int pitch = 60;
        int lengthTicks = 120;
        int channel = 1;

        int endTick() const noexcept { return tick + juce::jmax (1, lengthTicks); }
    };

    void rebuild (const std::vector<const std::vector<dmb::Note>*>& trackNotes,
                  int ppqToUse,
                  const juce::String& captionText);

    std::vector<Item> items;
    juce::String caption { dmb::utf8 ("（没有数据）") };
    int ppq = dmb::kTicksPerQuarter;
    int beatsPerBar = 4;
    int numTracks = 0;
    int lowest = 48;
    int highest = 72;
    double totalBeats = 4.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PianoRollView)
};
