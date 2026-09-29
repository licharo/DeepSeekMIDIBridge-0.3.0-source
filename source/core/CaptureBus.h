#pragma once

#include "DmbTypes.h"

namespace dmb
{

/** Cross-instance MIDI bus.

    A plugin instance cannot read the MIDI of another host track by itself, so the
    instances cooperate through a small directory of files:

      - an instance placed on a source track captures the MIDI it receives and
        publishes a snapshot to the bus;
      - an instance placed on a target track reads every snapshot and therefore
        "sees" the notes of the other tracks.

    Publishing is atomic (write to a temporary file, then rename), so a reader can
    never observe a half written snapshot and no locking between the two
    processes/instances is required.
*/
class CaptureBus
{
public:
    static juce::File getRootDirectory();
    static void setRootDirectory (const juce::File& dir);

    static void publish (const SourceTrack& source);
    static void remove (const juce::String& instanceId);

    /** Returns every source published within maxAgeMs by an instance other than
        ignoreInstanceId, newest first.

        When projectId is not empty only the entries that belong to that host project
        are returned, so a freshly opened project never shows the material of the
        project that was open before.
    */
    static std::vector<SourceTrack> readAll (juce::int64 maxAgeMs = 30000,
                                             const juce::String& ignoreInstanceId = {},
                                             const juce::String& projectId = {});

    /** Deletes snapshots that have not been updated for maxAgeMs (housekeeping). */
    static int pruneExpired (juce::int64 maxAgeMs = 6 * 60 * 60 * 1000);

    static juce::String makeInstanceId();

    /** True when DMB_BUS_DIR forced the bus location (tests use this). */
    static bool isRedirectedByEnvironment();

    static constexpr int kFormatVersion = 5;

    /** Shared with the persistent source library. */
    static juce::MemoryBlock serialiseTrack (const SourceTrack& source);
    static bool deserialiseTrack (const juce::MemoryBlock& block, SourceTrack& result);

private:
    static juce::File getRoot();
    static juce::File fileForId (const juce::File& dir, const juce::String& id);

};

} // namespace dmb
