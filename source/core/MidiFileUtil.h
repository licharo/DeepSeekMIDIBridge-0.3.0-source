#pragma once

#include "DmbTypes.h"

namespace dmb
{

/** Builds a MIDI file from a generated clip.
    @param onlyTrackIndex  -1 for all tracks, otherwise a single track index.
*/
juce::MidiFile buildMidiFile (const GeneratedClip& clip, int onlyTrackIndex = -1);

/** Writes <name>.mid plus one file per track when there is more than one track.
    Updates clip.file / clip.perTrackFiles. */
bool writeClipToDisk (GeneratedClip& clip, const juce::File& directory, juce::String& error);

juce::File getDefaultOutputDirectory();

/** Reads a .mid file and turns every MIDI track into a SourceTrack, so it can be
    used as reference material (used by the standalone build and by the UI). */
std::vector<SourceTrack> importMidiFile (const juce::File& file, juce::String& error);

} // namespace dmb
