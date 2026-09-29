#pragma once

#include "DmbTypes.h"

#include <array>
#include <memory>

namespace dmb
{

/** Plays a generated clip as MIDI output of the plugin.

    The clip can either follow the host transport (synced, optionally looping) or
    run on the plugin's own clock (used by the standalone build / the 试听 button).
    Everything here is audio-thread safe: the clip is handed over with a spin lock
    that the audio thread only ever tries to acquire.
*/
class GeneratedMidiPlayer
{
public:
    GeneratedMidiPlayer();

    /** Message thread. */
    void setClip (std::shared_ptr<const GeneratedClip> clip);
    void clearClip();
    bool hasClip() const;

    /** Audio thread only. */
    void prepare (double sampleRate);
    void processBlock (juce::MidiBuffer& midi,
                       int numSamples,
                       bool hasHostPosition,
                       bool hostIsPlaying,
                       double hostPpqPosition,
                       double hostBpm,
                       bool syncToHost,
                       bool loopPlayback,
                       bool freeRun);

    void releaseAllNotes (juce::MidiBuffer& midi, int sampleOffset = 0);

private:
    struct PlayNote
    {
        int start = 0;
        int end = 0;
        int pitch = 60;
        int channel = 1;
        int velocity = 100;
    };

    struct SoundingNote
    {
        double endTick = 0.0;
        int pitch = 60;
        int channel = 1;
    };

    juce::SpinLock clipLock;
    std::shared_ptr<const GeneratedClip> pendingClip;
    std::shared_ptr<const GeneratedClip> activeClip;
    std::vector<PlayNote> notes;
    double clipLengthTicks = 0.0;

    double sampleRate = 44100.0;
    double freeTick = 0.0;
    double lastBlockEnd = -1.0e18;
    int cursor = 0;

    static constexpr int kMaxSounding = 512;
    std::array<SoundingNote, (size_t) kMaxSounding> sounding;
    int numSounding = 0;

    void rebuildFromClip();
    void addSounding (double endTick, int pitch, int channel);
    void startNote (juce::MidiBuffer& midi, const PlayNote& note, int sampleOffset);
    double clampToClip (double tick) const;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GeneratedMidiPlayer)
};

} // namespace dmb
