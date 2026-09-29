#pragma once

#include "DmbTypes.h"

#include <array>

namespace dmb
{

/** Audio-thread safe MIDI capture.

    The audio thread only ever writes compact fixed size records into a lock free
    FIFO. A background thread turns those records into notes, keeps a rolling
    window of the most recent bars and hands out snapshots.
*/
class MidiCapture
{
public:
    MidiCapture();

    void prepare (double newSampleRate);
    void reset();

    /** Audio thread. */
    void processBlock (const juce::MidiBuffer& midi,
                       int numSamples,
                       bool hasHostPosition,
                       bool hostIsPlaying,
                       double hostPpqPosition,
                       double hostBpm);

    /** Background thread: drains the FIFO, returns true when the notes changed. */
    bool update();

    void setWindowBars (int bars) noexcept   { windowBars = juce::jlimit (1, 64, bars); }
    int  getWindowBars() const noexcept      { return windowBars; }

    void setBeatsPerBar (int beats) noexcept { beatsPerBar = juce::jlimit (1, 16, beats); }

    /** Any thread. Returns the captured material as a source track, with tick 0
        aligned to a bar line of the captured window.
    */
    SourceTrack snapshot (const juce::String& id, const juce::String& name) const;

    /** Takes that were put aside automatically (transport stopped, loop wrapped or
        playback jumped back), so nothing the user recorded is ever lost. */
    std::vector<SourceTrack> getKeptTakes (const juce::String& idPrefix, const juce::String& namePrefix) const;
    int getKeptTakeCount() const;

    /** Rename or drop one kept take (id comes from getKeptTakes()). */
    bool renameKeptTake (const juce::String& id, const juce::String& newName);
    bool removeKeptTake (const juce::String& id);

    void clear();

    double getCurrentTick() const noexcept   { return lastTick.load(); }
    bool hasHostTransport() const noexcept   { return sawHostTransport.load(); }
    juce::int64 getDroppedEvents() const noexcept;
    juce::int64 getTotalEvents() const noexcept;

private:
    struct RawEvent
    {
        juce::uint32 tick = 0;
        juce::uint8 pitch = 0;
        juce::uint8 velocity = 0;
        juce::uint8 channel = 1;
        juce::uint8 isNoteOn = 0;
    };

    struct OpenNote
    {
        int key = 0;
        int startTick = 0;
        int velocity = 100;
    };

    static constexpr int kFifoSize = 8192;

    juce::AbstractFifo fifo { kFifoSize };
    std::array<RawEvent, (size_t) kFifoSize> fifoData;

    double sampleRate = 44100.0;
    std::atomic<double> freeTick { 0.0 };          // used when the host gives no timeline
    std::atomic<double> lastTick { 0.0 };
    std::atomic<double> lastBpm { 120.0 };
    std::atomic<bool> lastPlaying { false };
    std::atomic<bool> sawHostTransport { false };
    int windowBars = 8;
    int beatsPerBar = 4;

    mutable juce::CriticalSection noteLock;
    std::vector<Note> notes;
    std::vector<OpenNote> openNotes;
    double windowStartTick = 0.0;
    double lastProcessedTick = -1.0;
    juce::int64 droppedEvents = 0;
    juce::int64 totalEvents = 0;

    /** Takes put aside so a later restart/loop cannot wipe what was captured. */
    struct KeptTake
    {
        SourceTrack source;
        juce::int64 stampMs = 0;
        juce::int64 serial = 0;
        juce::String customName;      // set when the user renames it
    };

    static constexpr int kMaxKeptTakes = 6;
    std::vector<KeptTake> keptTakes;
    juce::int64 keptSerial = 0;
    bool wasPlayingLast = false;

    void applyEvents (const RawEvent* events, int count);
    void pushNote (int key, int startTick, int lengthTicks, int velocity);
    void trimToWindow();
    SourceTrack buildTakeLocked() const;      // caller holds noteLock
    void keepCurrentTakeLocked();             // caller holds noteLock

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiCapture)
};

} // namespace dmb
