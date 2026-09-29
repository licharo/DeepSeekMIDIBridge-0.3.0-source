#include "GeneratedMidiPlayer.h"

#include <cmath>

namespace dmb
{

GeneratedMidiPlayer::GeneratedMidiPlayer() = default;

void GeneratedMidiPlayer::prepare (double newSampleRate)
{
    sampleRate = (newSampleRate > 0.0 ? newSampleRate : 44100.0);
    freeTick = 0.0;
    lastBlockEnd = -1.0e18;
    cursor = 0;
    numSounding = 0;
}

void GeneratedMidiPlayer::setClip (std::shared_ptr<const GeneratedClip> clip)
{
    const juce::SpinLock::ScopedLockType sl (clipLock);
    pendingClip = std::move (clip);
}

void GeneratedMidiPlayer::clearClip()
{
    const juce::SpinLock::ScopedLockType sl (clipLock);
    pendingClip.reset();
}

bool GeneratedMidiPlayer::hasClip() const
{
    const juce::SpinLock::ScopedLockType sl (clipLock);
    return pendingClip != nullptr && ! pendingClip->isEmpty();
}

void GeneratedMidiPlayer::rebuildFromClip()
{
    notes.clear();
    clipLengthTicks = 0.0;

    if (activeClip == nullptr)
        return;

    clipLengthTicks = (double) activeClip->ppq * (double) activeClip->beatsPerBar * (double) activeClip->bars;

    for (const auto& track : activeClip->tracks)
    {
        for (const auto& note : track.notes)
        {
            PlayNote p;
            p.start = note.tick;
            p.end = note.endTick();
            p.pitch = juce::jlimit (0, 127, note.pitch);
            p.channel = juce::jlimit (1, 16, note.channel);
            p.velocity = juce::jlimit (1, 127, note.velocity);
            notes.push_back (p);
        }
    }

    std::sort (notes.begin(), notes.end(),
               [] (const PlayNote& a, const PlayNote& b) { return a.start < b.start; });

    cursor = 0;
    numSounding = 0;
    lastBlockEnd = -1.0e18;
    freeTick = 0.0;
}

//==============================================================================
void GeneratedMidiPlayer::addSounding (double endTick, int pitch, int channel)
{
    if (numSounding >= kMaxSounding)
        return;

    SoundingNote note;
    note.endTick = endTick;
    note.pitch = pitch;
    note.channel = channel;
    sounding[(size_t) numSounding++] = note;
}

void GeneratedMidiPlayer::startNote (juce::MidiBuffer& midi, const PlayNote& note, int sampleOffset)
{
    midi.addEvent (juce::MidiMessage::noteOn (note.channel, note.pitch, (juce::uint8) note.velocity),
                   juce::jmax (0, sampleOffset));
}

void GeneratedMidiPlayer::releaseAllNotes (juce::MidiBuffer& midi, int sampleOffset)
{
    for (int i = 0; i < numSounding; ++i)
        midi.addEvent (juce::MidiMessage::noteOff (sounding[(size_t) i].channel, sounding[(size_t) i].pitch),
                       juce::jmax (0, sampleOffset));

    numSounding = 0;
}

double GeneratedMidiPlayer::clampToClip (double tick) const
{
    if (clipLengthTicks > 0.0)
        return juce::jlimit (0.0, clipLengthTicks, tick);

    return juce::jmax (0.0, tick);
}

//==============================================================================
void GeneratedMidiPlayer::processBlock (juce::MidiBuffer& midi,
                                        int numSamples,
                                        bool hasHostPosition,
                                        bool hostIsPlaying,
                                        double hostPpqPosition,
                                        double hostBpm,
                                        bool syncToHost,
                                        bool loopPlayback,
                                        bool freeRun)
{
    // Pick up a newly generated clip without ever blocking the audio thread.
    {
        const juce::SpinLock::ScopedTryLockType lock (clipLock);

        if (lock.isLocked() && pendingClip != activeClip)
        {
            activeClip = pendingClip;
            rebuildFromClip();
        }
    }

    if (activeClip == nullptr || notes.empty())
    {
        releaseAllNotes (midi);
        return;
    }

    const auto bpm = (hostBpm > 1.0 && hostBpm < 1000.0) ? hostBpm : 120.0;
    const double ticksPerSample = bpm / 60.0 * (double) activeClip->ppq / sampleRate;

    const bool useHostTransport = syncToHost && hasHostPosition && hostIsPlaying;

    if (! useHostTransport && ! freeRun)
    {
        releaseAllNotes (midi);
        lastBlockEnd = -1.0e18;
        return;
    }

    double position = useHostTransport ? hostPpqPosition * (double) activeClip->ppq : freeTick;

    const double blockLength = (double) numSamples * ticksPerSample;
    double blockEnd = position + blockLength;

    if (! useHostTransport)
        freeTick = blockEnd;

    // A jump in the timeline (loop, seek, restart) invalidates everything.
    const double tolerance = juce::jmax (2.0, blockLength * 0.25);

    if (std::abs (position - lastBlockEnd) > tolerance || position < lastBlockEnd - tolerance)
    {
        releaseAllNotes (midi);
        cursor = 0;

        const auto startTick = (loopPlayback && clipLengthTicks > 0.0)
                                 ? std::fmod (juce::jmax (0.0, position), clipLengthTicks)
                                 : juce::jmax (0.0, position);

        for (size_t i = 0; i < notes.size(); ++i)
        {
            if ((double) notes[i].start >= startTick)
            {
                cursor = (int) i;
                break;
            }

            cursor = (int) notes.size();
        }
    }

    lastBlockEnd = blockEnd;

    // Emit note-offs whose end falls inside this block.
    for (int i = numSounding; --i >= 0;)
    {
        const auto endTick = sounding[(size_t) i].endTick;

        if (endTick <= blockEnd)
        {
            const auto offset = (int) std::lround ((endTick - position) / ticksPerSample);
            midi.addEvent (juce::MidiMessage::noteOff (sounding[(size_t) i].channel, sounding[(size_t) i].pitch),
                           juce::jlimit (0, juce::jmax (0, numSamples - 1), offset));

            for (int j = i; j < numSounding - 1; ++j)
                sounding[(size_t) j] = sounding[(size_t) (j + 1)];

            --numSounding;
        }
    }

    if (clipLengthTicks <= 0.0)
    {
        releaseAllNotes (midi);
        return;
    }

    const auto emitRange = [this, &midi, position, blockEnd, ticksPerSample, numSamples]
                           (double rangeStart, double rangeEnd, double offsetTicks)
    {
        while (cursor < (int) notes.size())
        {
            const auto& note = notes[(size_t) cursor];
            const auto noteStart = (double) note.start;

            if (noteStart < rangeStart - 1.0)
            {
                ++cursor;
                continue;
            }

            if (noteStart >= rangeEnd)
                break;

            const auto offset = (int) std::lround ((noteStart + offsetTicks - position) / ticksPerSample);
            const auto onOffset = juce::jlimit (0, juce::jmax (0, numSamples - 1), offset);
            startNote (midi, note, onOffset);

            const auto endTick = clampToClip ((double) note.end);

            if (endTick > blockEnd)
            {
                addSounding (endTick, note.pitch, note.channel);
            }
            else
            {
                const auto offOffset = (int) std::lround ((endTick + offsetTicks - position) / ticksPerSample);
                midi.addEvent (juce::MidiMessage::noteOff (note.channel, note.pitch),
                               juce::jlimit (onOffset, juce::jmax (0, numSamples - 1), offOffset));
            }

            ++cursor;
        }
    };

    const double startWrapped = (loopPlayback ? std::fmod (juce::jmax (0.0, position), clipLengthTicks)
                                              : juce::jmax (0.0, position));

    if (startWrapped >= clipLengthTicks)
    {
        // one-shot playback has run past the end of the clip
        if (! loopPlayback)
            releaseAllNotes (midi);

        return;
    }

    const double endWrapped = startWrapped + blockLength;

    if (loopPlayback && endWrapped > clipLengthTicks)
    {
        emitRange (startWrapped, clipLengthTicks, 0.0);

        const auto wrappedTicks = endWrapped - clipLengthTicks;
        cursor = 0;
        emitRange (0.0, wrappedTicks, clipLengthTicks);
    }
    else
    {
        emitRange (startWrapped, endWrapped, 0.0);
    }
}

} // namespace dmb
