#pragma once

#include "DmbTypes.h"

namespace dmb
{

/** One host project ("Set" in Ableton Live).

    VST3 gives a plugin no way to ask "which project file am I in?", so the plugin
    builds its own answer:

      - a project is a folder that holds everything belonging to it: the .mid files
        that were generated for it and the captured/imported reference material;
      - the id (and the folder) travels inside the host state, so reopening a
        project always brings back exactly its own material;
      - instances that were never saved yet agree on one project per host run
        through ProjectRegistry (see below).
*/
struct ProjectInfo
{
    juce::String id;             // stable id, e.g. "p-1a2b3c4d"
    juce::String name;           // user visible name, e.g. "我的歌"
    juce::File folder;           // <projects root>/<name>
    juce::int64 createdMs = 0;

    bool isValid() const noexcept { return id.isNotEmpty() && folder != juce::File(); }

    /** Where the captured/imported reference material of this project lives. */
    juce::File libraryDirectory() const;

    /** Small json file that marks the folder as a project folder. */
    juce::File markerFile() const;
};

//==============================================================================
/** The project that *this host run* is working on.

    All instances in one host share one process, so a process wide slot is enough
    for them to agree - which is what makes the cross instance capture bus work:
    an instance published its capture while another one is looking for it, and both
    have to use the same project id.
*/
class ProjectRegistry
{
public:
    struct Snapshot
    {
        ProjectInfo info;
        juce::int64 changedAtMs = 0;
        juce::String changedBy;      // instance id (empty when nobody claimed yet)
    };

    static Snapshot get();

    /** Announces the project of an instance (this is the claim other instances follow). */
    static void publish (const ProjectInfo& info, const juce::String& instanceId);

    /** Heartbeat: keeps the claim alive for other processes without changing it. */
    static void touch();

    /** Tests only. */
    static void resetForTests();
};

//==============================================================================
/** The project agreement between all instances of one host *session*.

    Instances are not always in the same process: Bitwig, REAPER ("run in separate
    process") and Cubase's sandboxing each run a plugin in its own process. The
    in-process ProjectRegistry is therefore backed by a small file, keyed by the
    host application process, that every instance refreshes while it runs:

        %LOCALAPPDATA%\DeepSeekMidiBridge\session-<host>.dmb

    Instances of other processes of the same host read that file, so they end up in
    the same project - which is exactly what the cross instance bus needs.
*/
class ProjectSession
{
public:
    /** Identifies the host application process (stable while it is running). */
    static juce::String hostSessionKey();

    static juce::File getDirectory();
    static void setDirectory (const juce::File& directory);      // tests

    /** The project stored by another instance of this host session (empty when the
        file is missing or its owner stopped refreshing it). */
    static ProjectInfo readFresh (juce::int64 maxAgeMs = 8000, juce::int64* claimedAt = nullptr,
                                  juce::String* claimedBy = nullptr);

    /** Stores / refreshes the project of this host session. */
    static void write (const ProjectInfo& info, juce::int64 claimedAtMs, const juce::String& instanceId);

    /** Refreshes the stored session (heartbeat) so other processes keep adopting it. */
    static void touch();

    /** Removes session files whose host is long gone. */
    static int prune (juce::int64 maxAgeMs = 12 * 60 * 60 * 1000);

    static void resetForTests();
};

//==============================================================================
class ProjectStore
{
public:
    static constexpr const char* kMarkerFileName = ".dmbproject";
    static constexpr const char* kLibraryFolderName = ".dmb-library";

    /** A new project description. Nothing is written to disk yet - the folder is
        created by ensureFolder() the first time something has to be stored. */
    static ProjectInfo create (const juce::File& root, const juce::String& desiredName);

    /** Creates the folder and the marker file. Fails when the folder already
        belongs to a different project (never hijack somebody else's material). */
    static bool ensureFolder (const ProjectInfo& info);

    static bool writeMarker (const ProjectInfo& info);
    static ProjectInfo readMarker (const juce::File& folder);

    /** Every project folder inside root (folders that carry a marker), newest first. */
    static std::vector<ProjectInfo> list (const juce::File& root);

    /** Renames a project and moves its whole folder (generated .mid files included). */
    static ProjectInfo rename (const ProjectInfo& info, const juce::String& newName,
                               const juce::File& root, juce::String& error);

    /** A folder name that is free inside root (adds " (2)", " (3)", ... if needed). */
    static juce::String makeFolderName (const juce::File& root, const juce::String& desiredName,
                                        const juce::File& ignoreFolder = {});

    static juce::String makeProjectId();

    /** The window title of this process, which for Ableton Live is
        "<Set name> - Ableton Live 12 Suite". Empty when it cannot be read. */
    static juce::String hostProjectTitle();

    /** "My Song" for the title above ("" when there is nothing usable). */
    static juce::String projectNameFromHostTitle (const juce::String& hostTitle);
};

/** The folder that holds one folder per project. */
juce::File getDefaultProjectsRoot();

} // namespace dmb
