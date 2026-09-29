#include "MidiCapture.h"

#include <cmath>

namespace dmb
{

MidiCapture::MidiCapture()
{
    notes.reserve ((size_t) kMaxNotesPerSource / 4);
    openNotes.reserve (64);
}

void MidiCapture::prepare (double newSampleRate)
{
    sampleRate = (newSampleRate > 0.0 ? newSampleRate : 44100.0);
    reset();
}

void MidiCapture::reset()
{
    const juce::ScopedLock sl (noteLock);
    notes.clear();
    openNotes.clear();
    keptTakes.clear();
    keptSerial = 0;
    lastProcessedTick = -1.0;
    windowStartTick = 0.0;
    freeTick.store (0.0);
    lastTick.store (0.0);
    sawHostTransport.store (false);
    wasPlayingLast = false;
}

void MidiCapture::clear()
{
    reset();
}

//==============================================================================
void MidiCapture::processBlock (const juce::MidiBuffer& midi,
                                int numSamples,
                                bool hasHostPosition,
                                bool hostIsPlaying,
                                double hostPpqPosition,
                                double hostBpm)
{
    const auto bpm = (hostBpm > 1.0 && hostBpm < 1000.0) ? hostBpm : 120.0;
    lastBpm.store (bpm);
    lastPlaying.store (hostIsPlaying);

    const bool useHost = hasHostPosition
                           && hostIsPlaying
                           && std::isfinite (hostPpqPosition)
                           && hostPpqPosition >= 0.0;

    if (useHost)
        sawHostTransport.store (true);

    const double ticksPerSample = bpm / 60.0 * (double) kTicksPerQuarter / sampleRate;

    const double blockStartTick = useHost ? hostPpqPosition * (double) kTicksPerQuarter
                                          : freeTick.load();

    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();

        if (! message.isNoteOnOrOff())
            continue;

        const double tick = blockStartTick + (double) metadata.samplePosition * ticksPerSample;

        RawEvent event;
        event.tick = (juce::uint32) juce::jlimit (0.0, 4.0e9, tick);
        event.pitch = (juce::uint8) juce::jlimit (0, 127, message.getNoteNumber());
        event.velocity = (juce::uint8) juce::jlimit (0, 127, (int) message.getVelocity());
        event.channel = (juce::uint8) juce::jlimit (1, 16, message.getChannel());
        event.isNoteOn = (message.getVelocity() > 0 ? 1 : 0);

        const auto scope = fifo.write (1);

        if (scope.blockSize1 > 0)
            fifoData[(size_t) scope.startIndex1] = event;
        else if (scope.blockSize2 > 0)
            fifoData[(size_t) scope.startIndex2] = event;
        else
            ++droppedEvents;
    }

    const double blockEndTick = blockStartTick + (double) numSamples * ticksPerSample;

    // Keep the free-running clock aligned with the host timeline. Without this, the
    // first block after the transport stops would jump to a stale position and the
    // rolling window would throw away everything that was just recorded.
    freeTick.store (blockEndTick);
    lastTick.store (blockEndTick);
}

//==============================================================================
bool MidiCapture::update()
{
    std::array<RawEvent, 1024> scratch;
    bool changed = false;

    for (;;)
    {
        const auto ready = fifo.getNumReady();

        if (ready <= 0)
            break;

        const auto toRead = juce::jmin (ready, (int) scratch.size());
        const auto scope = fifo.read (toRead);

        int numRead = 0;

        for (int i = 0; i < scope.blockSize1; ++i)
            scratch[(size_t) numRead++] = fifoData[(size_t) (scope.startIndex1 + i)];

        for (int i = 0; i < scope.blockSize2; ++i)
            scratch[(size_t) numRead++] = fifoData[(size_t) (scope.startIndex2 + i)];

        if (numRead <= 0)
            break;

        const juce::ScopedLock sl (noteLock);
        applyEvents (scratch.data(), numRead);

        changed = true;
    }

    // Standing still: never slide the window, so the material the user just played
    // stays visible and usable until they clear it themselves.
    const auto playing = lastPlaying.load();

    if (wasPlayingLast && ! playing)
    {
        const juce::ScopedLock sl (noteLock);
        keepCurrentTakeLocked();      // put the finished take aside before anything else
        changed = true;
    }

    wasPlayingLast = playing;

    // The window slides with the transport while it is running.
    if (playing)
    {
        const juce::ScopedLock sl (noteLock);
        const auto before = notes.size();
        trimToWindow();

        if (notes.size() != before)
            changed = true;
    }

    return changed;
}

void MidiCapture::applyEvents (const RawEvent* events, int count)
{
    for (int i = 0; i < count; ++i)
    {
        const auto& event = events[i];
        const double tick = (double) event.tick;

        // A backwards jump of more than a beat means the host restarted or looped:
        // the take being built is finished, so put it aside first - nothing that was
        // captured should disappear just because playback started somewhere else.
        if (lastProcessedTick >= 0.0 && tick < lastProcessedTick - (double) kTicksPerQuarter)
        {
            keepCurrentTakeLocked();
            notes.clear();
            openNotes.clear();
            windowStartTick = tick;
        }

        lastProcessedTick = tick;

        const int key = ((int) event.pitch << 8) | (int) event.channel;

        if (event.isNoteOn != 0 && event.velocity > 0)
        {
            for (auto it = openNotes.begin(); it != openNotes.end(); ++it)
            {
                if (it->key == key)
                {
                    pushNote (key, it->startTick, (int) tick - it->startTick, it->velocity);
                    openNotes.erase (it);
                    break;
                }
            }

            openNotes.push_back ({ key, (int) tick, (int) event.velocity });
        }
        else
        {
            for (auto it = openNotes.begin(); it != openNotes.end(); ++it)
            {
                if (it->key == key)
                {
                    const auto length = juce::jmax (1, (int) tick - it->startTick);
                    pushNote (key, it->startTick, length, it->velocity != 0 ? it->velocity : 100);
                    openNotes.erase (it);
                    break;
                }
            }
        }

        ++totalEvents;
    }
}

void MidiCapture::pushNote (int key, int startTick, int lengthTicks, int velocity)
{
    Note note;
    note.tick = juce::jmax (0, startTick);
    note.pitch = juce::jlimit (0, 127, key >> 8);
    note.channel = juce::jlimit (1, 16, key & 0xff);
    note.velocity = juce::jlimit (1, 127, velocity);
    note.lengthTicks = juce::jlimit (1, 16 * kTicksPerQuarter, lengthTicks);

    // Keep the vector ordered by tick; note-ons usually arrive in order already.
    if (notes.empty() || notes.back().tick <= note.tick)
    {
        notes.push_back (note);
    }
    else
    {
        const auto pos = std::lower_bound (notes.begin(), notes.end(), note,
                                           [] (const Note& a, const Note& b) { return a.tick < b.tick; });
        notes.insert (pos, note);
    }
}

void MidiCapture::trimToWindow()
{
    const double barTicks = (double) beatsPerBar * (double) kTicksPerQuarter;
    const double windowTicks = (double) windowBars * barTicks;

    // The window is anchored to the current transport position, not to the last
    // note that happened to arrive.
    const double end = juce::jmax (lastProcessedTick, lastTick.load());
    const double start = end - windowTicks;

    windowStartTick = std::floor (start / barTicks) * barTicks;

    notes.erase (std::remove_if (notes.begin(), notes.end(),
                                 [this] (const Note& n)
                                 {
                                     return (double) n.endTick() < windowStartTick;
                                 }),
                 notes.end());

    if ((int) notes.size() > kMaxNotesPerSource)
        notes.erase (notes.begin(), notes.begin() + ((int) notes.size() - kMaxNotesPerSource));
}

//==============================================================================
SourceTrack MidiCapture::buildTakeLocked() const
{
    SourceTrack source;
    source.ppq = kTicksPerQuarter;
    source.bpm = lastBpm.load();
    source.playing = false;
    source.stampMs = juce::Time::currentTimeMillis();

    const auto shift = (int) std::floor (windowStartTick);
    const auto end = juce::jmax (lastProcessedTick, lastTick.load());

    source.notes.reserve (notes.size());

    for (const auto& n : notes)
    {
        auto copy = n;
        copy.tick = juce::jmax (0, n.tick - shift);
        source.notes.push_back (copy);
    }

    source.firstTick = 0.0;
    source.lastTick = source.notes.empty() ? 0.0
                                           : juce::jmin (juce::jmax (0.0, end - (double) shift),
                                                         (double) source.notes.back().endTick());

    return source;
}

void MidiCapture::keepCurrentTakeLocked()
{
    if (notes.empty())
        return;

    auto take = buildTakeLocked();

    if (take.notes.empty())
        return;

    // Continuing the same take (same starting point)? Then update it in place
    // instead of filling the list with near-duplicates.
    if (! keptTakes.empty())
    {
        auto& previous = keptTakes.back().source;

        const auto sameStart = ! previous.notes.empty()
                                 && std::abs (previous.notes.front().tick - take.notes.front().tick)
                                        < kTicksPerQuarter;

        if (sameStart)
        {
            const auto identical = previous.notes.size() == take.notes.size()
                                     && std::abs (previous.lastTick - take.lastTick) < 1.0;

            if (identical)
                return;

            if (take.notes.size() >= previous.notes.size())
            {
                previous = take;
                keptTakes.back().stampMs = take.stampMs;
                return;
            }
        }
    }

    keptTakes.push_back ({ std::move (take), juce::Time::currentTimeMillis(), ++keptSerial, {} });

    while ((int) keptTakes.size() > kMaxKeptTakes)
        keptTakes.erase (keptTakes.begin());
}

std::vector<SourceTrack> MidiCapture::getKeptTakes (const juce::String& idPrefix,
                                                    const juce::String& namePrefix) const
{
    const juce::ScopedLock sl (noteLock);

    std::vector<SourceTrack> result;
    result.reserve (keptTakes.size());

    int index = 0;

    for (const auto& kept : keptTakes)
    {
        if (kept.source.notes.empty())
            continue;

        ++index;

        auto copy = kept.source;
        copy.id = idPrefix + juce::String (kept.serial);          // stable across deletions
        copy.name = kept.customName.isNotEmpty()
                      ? kept.customName
                      : (namePrefix + " " + juce::Time (kept.stampMs).formatted ("%H:%M:%S"));
        copy.playing = false;
        result.push_back (std::move (copy));
    }

    return result;
}

int MidiCapture::getKeptTakeCount() const
{
    const juce::ScopedLock sl (noteLock);
    return (int) keptTakes.size();
}

bool MidiCapture::renameKeptTake (const juce::String& id, const juce::String& newName)
{
    const juce::ScopedLock sl (noteLock);
    const auto serial = (juce::int64) id.fromLastOccurrenceOf ("-", false, false).getLargeIntValue();

    for (auto& kept : keptTakes)
    {
        if (kept.serial == serial)
        {
            kept.customName = newName.trim();
            return true;
        }
    }

    return false;
}

bool MidiCapture::removeKeptTake (const juce::String& id)
{
    const juce::ScopedLock sl (noteLock);
    const auto serial = (juce::int64) id.fromLastOccurrenceOf ("-", false, false).getLargeIntValue();

    for (auto it = keptTakes.begin(); it != keptTakes.end(); ++it)
    {
        if (it->serial == serial)
        {
            keptTakes.erase (it);
            return true;
        }
    }

    return false;
}

//==============================================================================
SourceTrack MidiCapture::snapshot (const juce::String& id, const juce::String& name) const
{
    const juce::ScopedLock sl (noteLock);

    auto source = buildTakeLocked();
    source.id = id;
    source.name = name;
    source.playing = lastPlaying.load();
    source.stampMs = juce::Time::currentTimeMillis();

    return source;
}

juce::int64 MidiCapture::getDroppedEvents() const noexcept { return droppedEvents; }
juce::int64 MidiCapture::getTotalEvents() const noexcept   { return totalEvents; }

} // namespace dmb
