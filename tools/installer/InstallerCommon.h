#pragma once

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

/** Shared code of the installer / uninstaller.

    The setup program and the uninstaller are the *same* executable: it installs
    when it starts normally, and uninstalls when it is called with --uninstall or
    when it sits next to an install manifest.

    Everything the installer writes is recorded in a manifest next to the
    uninstaller. The uninstaller only ever deletes

      - files that are listed in that manifest, and
      - only when they are inside the recorded plugin folder or program folder.

    Nothing else on the machine is touched, and no directory is ever deleted
    recursively.
*/
namespace dmbin
{

inline juce::String u8 (const char* text) { return juce::String (juce::CharPointer_UTF8 (text)); }

// the CMake build passes the project version in (DMB_VERSION)
#ifndef DMB_VERSION
 #define DMB_VERSION "0.3.0"
#endif

constexpr const char* kProductName     = "DeepSeek MIDI Bridge";
constexpr const char* kPluginVersion   = DMB_VERSION;
constexpr const char* kBundleName      = "DeepSeek MIDI Bridge.vst3";
constexpr const char* kStaleBundleName = "DeepSeek MIDI Bridge (MIDI).vst3";
constexpr const char* kManifestName    = "install-manifest.txt";

/** Localised file names, so they cannot be constexpr. */
extern const juce::String kReadmeName;
extern const juce::String kUninstallerName;

constexpr const char* kRegUninstallKey = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DeepSeekMidiBridge";
constexpr const char* kRegInfoKey      = "Software\\DeepSeekMidiBridge";

constexpr const char* kPayloadMagic    = "DMB-PAYLOAD-1   ";   // exactly 16 characters
constexpr int kPayloadTrailerSize      = 24;                   // int64 offset + 16 byte magic

using ProgressFn = std::function<void (int step, int total, const juce::String& what)>;

//==============================================================================
/** The files that are appended to the setup executable. */
struct PayloadEntry
{
    juce::String name;              // relative path, '/' separated, e.g. "vst3/.../x.vst3"
    juce::int64 offset = 0;         // absolute offset of the compressed blob in the exe
    juce::int64 compressedSize = 0;
    juce::int64 originalSize = 0;
};

class Payload
{
public:
    /** Builds outExe = a copy of selfExe + all files below contentDir. */
    static bool pack (const juce::File& contentDir, const juce::File& selfExe, const juce::File& outExe,
                      juce::StringArray& log, juce::String& error);

    bool readFrom (const juce::File& selfExe, juce::String& error);

    bool hasContent() const noexcept { return ! entries.empty(); }
    const std::vector<PayloadEntry>& getEntries() const noexcept { return entries; }

    /** Decompresses one entry of the executable into memory. */
    static bool readEntry (const juce::File& selfExe, const PayloadEntry& entry,
                           juce::MemoryBlock& decompressed, juce::String& error);

private:
    std::vector<PayloadEntry> entries;
};

//==============================================================================
/** What the installer wrote - the uninstaller works from this. */
struct Manifest
{
    int format = 1;
    juce::String pluginVersion;
    juce::String installedAt;
    bool machineWide = false;
    juce::File vst3Root;
    juce::File appDir;
    juce::StringArray files;        // absolute paths, in the order they were written

    bool isValid() const noexcept { return appDir != juce::File(); }

    juce::String toString() const;
    static Manifest fromString (const juce::String& text);
};

//==============================================================================
// paths / environment
juce::File getProgramFilesDirectory();
juce::File getDefaultVst3Directory();
juce::File getPerUserVst3Directory();
juce::File getDefaultAppDirectory();
juce::File getAppDataDirectory();
juce::File getUserMediaDirectory();

bool isElevated();
bool canWriteInto (const juce::File& directory);
bool relaunchElevated (const juce::String& extraArguments, juce::String& error);

/** True when Ableton Live (or another host we know about) is running - the plugin
    file is locked then and must not be replaced. */
bool isHostRunning (juce::String& hostName);

/** True when `candidate` is `root` itself or lives inside it (case insensitive). */
bool pathIsInside (const juce::File& candidate, const juce::File& root);

/** Checks the bundle really is ours before anything is deleted or replaced. */
bool looksLikeOurBundle (const juce::File& bundleFolder);

/** Path text as it is shown in the installer / uninstaller windows.

    The account folder of whoever runs the program is printed as the generic name
    "administrator". This only changes what the window displays - the paths that
    are created, written into the manifest or deleted are never touched.
*/
juce::String displayPath (const juce::String& path);
juce::String displayPath (const juce::File& path);

//==============================================================================
// registry (Add/Remove programs entry)
bool writeUninstallRegistry (bool machineWide, const Manifest& manifest, const juce::File& pluginFile, juce::String& error);
void removeUninstallRegistry (juce::StringArray& removedFrom);

//==============================================================================
struct InstallOptions
{
    juce::File vst3Root;
    juce::File appDir;              // empty = the standard program folder

    /** Also install the MIDI-only flavour ("DeepSeek MIDI Bridge (MIDI).vst3").

        Hosts with a dedicated MIDI effect slot (Cubase/Nuendo MIDI inserts, Bitwig
        Note FX) can only use that one; Ableton Live refuses to load it, so it is off
        by default. While it is off, a leftover copy of an older install is removed.
    */
    bool includeMidiVariant = false;

    bool allowHostRunning = false;
};

struct InstallResult
{
    bool ok = false;
    juce::String error;
    juce::File vst3Root, appDir;
    int filesWritten = 0;
    bool machineWide = false;
};

InstallResult runInstall (const InstallOptions& options, const ProgressFn& progress);

//==============================================================================
struct UninstallOptions
{
    bool removeAppData = true;      // %LOCALAPPDATA%\DeepSeekMidiBridge (ours)
    bool removeUserMedia = false;   // Documents\DeepSeek MIDI Bridge (the user's music!)
    bool removeUnregistered = true; // bundles found without a manifest (older installs)
    bool allowHostRunning = false;
};

struct UninstallResult
{
    bool ok = false;
    juce::String error;
    int filesRemoved = 0;
    int filesKept = 0;
    juce::StringArray keptPaths;
    juce::StringArray removedRoots;
};

UninstallResult runUninstall (const UninstallOptions& options, const ProgressFn& progress);

/** The install that this executable belongs to (empty when there is none). */
Manifest readOwnManifest();

/** The plugin folder of a previous install (from the registry), or an empty file. */
juce::File getRegisteredVst3Directory();

/** Removes the folder again once its files are gone - only deletes empty folders. */
int removeEmptyDirectories (const juce::File& dir);

/** Bundles of ours that live in the standard folders - used when the uninstaller
    runs without a manifest (an install made by hand or by an older build). */
juce::Array<juce::File> findUnregisteredBundles();

} // namespace dmbin
