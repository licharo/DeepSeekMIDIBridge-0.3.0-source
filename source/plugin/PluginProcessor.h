#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>

#include "core/ApiProbe.h"
#include "core/SourceLibrary.h"
#include "core/CaptureBus.h"
#include "core/DmbTypes.h"
#include "core/GeneratedMidiPlayer.h"
#include "core/GenerationService.h"
#include "core/MidiCapture.h"
#include "core/ProjectStore.h"
#include "core/Settings.h"

/** Audio processor shared by both plugin flavours.

    Roles (both can be active at the same time):

      - capture: whatever MIDI arrives on the host track is turned into a snapshot
        and published on the cross-instance bus, so instances on other tracks can
        see it;
      - generate: the captured material of the other tracks + the user's request
        are sent to the DeepSeek API; the returned notes are written to .mid files
        (drag and drop into the host) and can optionally be played straight out of
        the plugin.
*/
class DeepSeekMidiBridgeProcessor : public juce::AudioProcessor
{
public:
    DeepSeekMidiBridgeProcessor();
    ~DeepSeekMidiBridgeProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    /** The host tells us which track we live on (VST3 channel context). Live sends
        this, so the UI can show the real track name instead of a guessed one. */
    void updateTrackProperties (const TrackProperties& properties) override;
    juce::String getHostTrackName() const;

    //==============================================================================
    juce::AudioProcessorValueTreeState& getValueTreeState() { return apvts; }

    dmb::PluginSettings getSettings() const;
    void setSettings (const dmb::PluginSettings& newSettings);
    void saveSettings();

    dmb::GenerationService& getGenerationService() { return generation; }
    dmb::ApiProbe& getApiProbe() { return apiProbe; }

    /** Every track the plugin can currently see: the other instances' captures,
        imported .mid files and this instance's own capture. */
    std::vector<dmb::SourceTrack> getAvailableSources() const;

    /** The most recent capture of *this* instance (used for the local preview). */
    dmb::SourceTrack getLocalCapture() const;

    /** Captures that were put aside automatically so they are never lost, and a way
        to throw everything captured away on purpose. */
    std::vector<dmb::SourceTrack> getKeptCaptures() const;
    int getKeptCaptureCount() const;
    void clearLocalCapture();

    /** Rename / remove a single entry shown in the source list.
        Works for kept captures ("kept-..."), imported files ("file:...") and the
        live capture of this instance; returns false when the entry cannot be
        changed here (e.g. it belongs to another plugin instance). */
    bool renameSource (const juce::String& id, const juce::String& newName);
    bool removeSource (const juce::String& id);

    std::vector<dmb::SourceTrack> getImportedSources() const;
    void addImportedSources (std::vector<dmb::SourceTrack> sources);
    void clearImportedSources();

    std::shared_ptr<const dmb::GeneratedClip> getGeneratedClip() const;
    juce::int64 getClipVersion() const noexcept { return clipVersion.load(); }
    void clearGeneratedClip();

    juce::String getPromptText() const;
    void setPromptText (const juce::String& text);

    void startGeneration (const juce::String& requirement, const std::vector<dmb::SourceTrack>& sources);
    void cancelGeneration();

    void setFreeRunPreview (bool shouldPlay);
    bool isFreeRunPreview() const noexcept { return freeRunPreview.load(); }

    juce::File getOutputDirectory() const;
    void setOutputDirectory (const juce::File& directory);

    //==============================================================================
    /** The host project ("Set") this instance belongs to.

        Everything the plugin produces or remembers is scoped to it: the folder that
        holds the generated .mid files, the captured/imported reference material and
        the cross instance capture bus. Opening another project therefore shows that
        project's material only (see core/ProjectStore.h). */
    dmb::ProjectInfo getProject() const;
    juce::String getProjectName() const;
    juce::File getProjectFolder() const;

    /** The folder that holds one folder per project (the "输出目录" setting). */
    juce::File getProjectsRoot() const;

    /** Every project that exists inside the projects root, sorted by name. */
    std::vector<dmb::ProjectInfo> listProjects() const;

    /** Renames the current project and moves its folder (generated files included). */
    bool renameProject (const juce::String& newName, juce::String& error);

    /** Makes this instance (and the other instances of this host run) use another
        existing project folder. */
    bool switchProject (const juce::File& folder, juce::String& error);

    /** Creates a new, empty project folder and switches to it. */
    bool createProject (const juce::String& name, juce::String& error);

    /** Moves material from the pre-project-folder library into a project named
        "未归类（旧素材）" (called once, from the background thread).
        @returns how many entries were moved. */
    int migrateLegacyLibrary();

    /** Entries waiting in that "未归类（旧素材）" project (0 when there are none). */
    int getDetachedLibraryCount() const;

    /** Makes sure this instance knows which project it belongs to. Nothing is written
        to disk - the project folder appears when something is stored in it (a
        capture, an import, a generated .mid) or when the host saves the project. */
    void ensureProject();

    /** Same as ensureProject(), and creates the project folder right away. */
    void ensureProjectFolder();

    /** Folds the "未归类（旧素材）" material into the current project. */
    bool mergeDetachedLibrary (juce::String& error);

    dmb::GenerationSpec makeGenerationSpec() const;
    juce::String getInstanceId() const { return instanceId; }
    double getHostBpm() const noexcept { return lastKnownBpm.load(); }
    bool getHostIsPlaying() const noexcept { return lastHostPlaying.load(); }
    double getHostPpq() const noexcept { return lastHostPpq.load(); }

    /** Called by the background thread. */
    void maybeAutoGenerate();
    void persistNewCaptures();
    void refreshLibrarySources();
    void publishCapture();
    void refreshVisibleSources();
    void pollGenerationResult();

    juce::CriticalSection& getSourceLock() { return sourceLock; }

private:
    class BackgroundThread : public juce::Thread
    {
    public:
        explicit BackgroundThread (DeepSeekMidiBridgeProcessor& ownerToUse);
        void run() override;

    private:
        DeepSeekMidiBridgeProcessor& owner;
    };

    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    float getFloatParam (const char* id) const;
    bool getBoolParam (const char* id) const;
    int getIntParam (const char* id) const;

    //==============================================================================
    // project handling (see core/ProjectStore.h)
    void resolveProject();
    void adoptProject (const dmb::ProjectInfo& info, bool announce);
    void syncProjectFromRegistry();
    dmb::ProjectInfo getProjectForWrite();
    bool isProjectResolved() const;
    juce::String suggestedProjectName() const;
    dmb::ProjectInfo findDetachedLibraryProject() const;
    void remapClipPaths (const juce::File& oldFolder, const juce::File& newFolder);

    juce::AudioProcessorValueTreeState apvts;

    dmb::MidiCapture capture;
    dmb::GeneratedMidiPlayer player;
    dmb::GenerationService generation;
    dmb::ApiProbe apiProbe;

    mutable juce::CriticalSection settingsLock;
    dmb::PluginSettings settings;

    // which host project this instance belongs to (see core/ProjectStore.h)
    mutable juce::CriticalSection projectLock;
    dmb::ProjectInfo project;
    juce::int64 projectAdoptedMs = 0;
    bool projectResolved = false;
    bool projectFolderEnsured = false;
    juce::int64 lastRegistrySyncMs = 0;
    bool legacyLibraryChecked = false;

    mutable juce::CriticalSection sourceLock;
    std::vector<dmb::SourceTrack> busSources;
    std::vector<dmb::SourceTrack> importedSources;
    std::vector<dmb::SourceTrack> librarySources;
    juce::StringArray savedCaptureSignatures;
    dmb::SourceTrack localSnapshot;

    mutable juce::CriticalSection trackInfoLock;
    juce::String hostTrackName;

    mutable juce::CriticalSection clipLock;
    std::shared_ptr<const dmb::GeneratedClip> generatedClip;
    juce::String promptText;
    std::atomic<juce::int64> clipVersion { 0 };

    const juce::String instanceId;
    std::unique_ptr<BackgroundThread> backgroundThread;

    std::atomic<double> lastKnownBpm { 120.0 };
    std::atomic<double> lastHostPpq { 0.0 };
    std::atomic<bool> lastHostPlaying { false };
    std::atomic<bool> freeRunPreview { false };
    std::atomic<double> lastLoopBars { 0.0 };
    bool wasPlaying = false;
    juce::int64 lastAutoGenerateMs = 0;

    // background thread only
    juce::String publishSignature;
    int publishHeartbeat = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeepSeekMidiBridgeProcessor)
};
