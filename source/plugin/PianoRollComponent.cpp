#include "PianoRollComponent.h"

PianoRollView::PianoRollView()
{
    setInterceptsMouseClicks (false, false);
}

void PianoRollView::clear()
{
    items.clear();
    caption = dmb::utf8 ("（没有数据）");
    numTracks = 0;
    repaint();
}

void PianoRollView::rebuild (const std::vector<const std::vector<dmb::Note>*>& trackNotes,
                             int ppqToUse,
                             const juce::String& captionText)
{
    items.clear();
    ppq = juce::jmax (1, ppqToUse);
    caption = captionText;

    lowest = 127;
    highest = 0;

    double lastBeat = 1.0;
    int trackIndex = 0;

    for (auto* notes : trackNotes)
    {
        ++trackIndex;

        if (notes == nullptr)
            continue;

        for (const auto& note : *notes)
        {
            Item item;
            item.track = trackIndex;
            item.tick = note.tick;
            item.pitch = note.pitch;
            item.lengthTicks = note.lengthTicks;
            item.channel = note.channel;
            items.push_back (item);

            lowest = juce::jmin (lowest, note.pitch);
            highest = juce::jmax (highest, note.pitch);
            lastBeat = juce::jmax (lastBeat, note.endTick() / (double) ppq);
        }
    }

    numTracks = juce::jmax (1, trackIndex);

    if (items.empty())
    {
        lowest = 48;
        highest = 72;
    }

    totalBeats = juce::jmax (1.0, lastBeat);
    repaint();
}

void PianoRollView::setSource (const dmb::SourceTrack& source)
{
    rebuild ({ &source.notes }, source.ppq,
             source.name + dmb::utf8 ("  ·  ") + juce::String ((int) source.notes.size()) + dmb::utf8 (" 音符"));
}

void PianoRollView::setClip (const dmb::GeneratedClip& clip)
{
    std::vector<const std::vector<dmb::Note>*> tracks;

    for (const auto& track : clip.tracks)
        tracks.push_back (&track.notes);

    juce::String title = clip.title;

    if (title.isEmpty())
        title = dmb::utf8 ("生成结果");

    rebuild (tracks, clip.ppq,
             title + dmb::utf8 ("  ·  ") + juce::String (clip.totalNotes()) + dmb::utf8 (" 音符  ·  ")
             + juce::String (clip.bars) + dmb::utf8 (" 小节"));
}

//==============================================================================
void PianoRollView::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat();

    g.fillAll (juce::Colour (0xff1b1d22));
    g.setColour (juce::Colour (0xff2c3038));
    g.drawRect (area, 1.0f);

    auto headerArea = area.removeFromTop (18.0f);

    g.setColour (juce::Colour (0xff8fa0b5));
    g.setFont (12.0f);
    g.drawText (caption, headerArea.reduced (6.0f, 0.0f), juce::Justification::centredLeft, true);

    auto roll = area.reduced (4.0f, 2.0f);

    if (items.empty() || roll.getHeight() < 8.0f || roll.getWidth() < 8.0f)
    {
        g.setColour (juce::Colour (0xff555a63));
        g.drawText (dmb::utf8 ("（没有数据）"), roll, juce::Justification::centred, false);
        return;
    }

    const int lowPitch = juce::jlimit (0, 127, lowest - 2);
    const int highPitch = juce::jlimit (0, 127, highest + 2);
    const auto pitchSpan = juce::jmax (1, highPitch - lowPitch);

    const auto beatsPerBarD = (double) juce::jmax (1, beatsPerBar);
    const auto bars = juce::jmax (1.0, std::ceil (totalBeats / beatsPerBarD));
    const auto totalTicks = bars * beatsPerBarD * (double) ppq;

    auto tickToX = [&] (double tick) { return roll.getX() + (float) (tick / totalTicks) * roll.getWidth(); };
    auto pitchToY = [&] (int pitch)
    {
        const auto normalised = (double) (pitch - lowPitch) / (double) pitchSpan;
        return roll.getBottom() - (float) normalised * roll.getHeight();
    };

    // bar / beat grid
    g.setColour (juce::Colour (0xff262a31));

    for (double beat = 0.0; beat <= bars * beatsPerBarD; beat += 1.0)
    {
        const auto x = tickToX (beat * ppq);
        const bool isBar = std::fmod (beat, beatsPerBarD) < 0.001;
        g.setColour (isBar ? juce::Colour (0xff3a4048) : juce::Colour (0xff262a31));
        g.drawVerticalLine ((int) x, roll.getY(), roll.getBottom());
    }

    // notes
    for (const auto& item : items)
    {
        const auto x1 = tickToX ((double) item.tick);
        const auto x2 = tickToX ((double) item.endTick());
        const auto y = pitchToY (item.pitch);

        const auto hue = std::fmod ((float) item.track * 0.17f + (float) item.channel * 0.05f, 1.0f);

        g.setColour (juce::Colour::fromHSV (hue, 0.55f, 0.92f, 1.0f));
        g.fillRect (juce::Rectangle<float> (x1, y - 2.0f, juce::jmax (2.0f, x2 - x1), 4.0f));
    }
}
