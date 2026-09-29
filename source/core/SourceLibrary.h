#pragma once

#include "DmbTypes.h"

namespace dmb
{

/** Persistent source library.

    Everything the plugin can use as reference material - captured takes and
    imported .mid files - is stored here, so that closing and reopening the host
    does not lose it.

    Since every host project gets its own folder, the library directory is passed
    in explicitly (it is <project folder>\.dmb-library):

        <输出根目录>\<工程名>\.dmb-library\<id>.dmbsrc

    Entries are written atomically and can be renamed or deleted individually.
*/
class SourceLibrary
{
public:
    /** The library of the versions before project folders existed
        (%LOCALAPPDATA%\DeepSeekMidiBridge\library). Entries found there are moved
        into a project of their own the first time the plugin runs (see
        PluginProcessor::migrateLegacyLibrary). */
    static juce::File getLegacyDirectory();

    /** Old, process wide entry point - kept for compatibility with existing code
        paths and the tests. */
    static juce::File getDirectory();
    static void setDirectory (const juce::File& directory);

    /** Stores a source (a fresh id is assigned when the source has none).
        @returns the id of the stored entry, or an empty string on failure. */
    static juce::String save (SourceTrack source);
    static juce::String save (const juce::File& directory, SourceTrack source);

    static bool remove (const juce::String& id);
    static bool remove (const juce::File& directory, const juce::String& id);

    static bool rename (const juce::String& id, const juce::String& newName);
    static bool rename (const juce::File& directory, const juce::String& id, const juce::String& newName);

    /** Every stored entry, newest last. */
    static std::vector<SourceTrack> loadAll();
    static std::vector<SourceTrack> loadAll (const juce::File& directory);

    /** Removes all entries whose origin starts with the given text.
        @returns how many were removed. */
    static int removeByOrigin (const juce::String& originPrefix);
    static int removeByOrigin (const juce::File& directory, const juce::String& originPrefix);

    /** Keeps at most maxEntries entries (oldest ones are dropped). */
    static void prune (int maxEntries = 32);
    static void prune (const juce::File& directory, int maxEntries = 32);

    /** How many entries a directory holds. */
    static int countEntries (const juce::File& directory);

    /** Moves every entry of one library into another (used to fold the material of
        the pre-project-folder library into a project).
        @returns how many entries were moved. */
    static int moveEntriesTo (const juce::File& from, const juce::File& to);

    /** Normalises a stored file back to its id. */
    static juce::String idFromFile (const juce::File& file);

private:
    static juce::File& defaultSlot();
};

} // namespace dmb
