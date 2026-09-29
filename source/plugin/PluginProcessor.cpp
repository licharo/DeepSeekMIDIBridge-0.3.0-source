#include "PluginProcessor.h"

#include "PluginEditor.h"
#include "PluginParameters.h"
#include "core/DiagnosticsLog.h"
#include "core/MidiFileUtil.h"
#include "core/MidiJson.h"
#include "core/PromptBuilder.h"

//==============================================================================
DeepSeekMidiBridgeProcessor::DeepSeekMidiBridgeProcessor()
#if DMB_MIDI_ONLY
    : juce::AudioProcessor (BusesProperties()),
#else
    : juce::AudioProcessor (BusesProperties()
                                .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
#endif
      apvts (*this, nullptr, "PARAMETERS", dmb::params::createLayout()),
      instanceId (dmb::CaptureBus::makeInstanceId())
{
    settings = dmb::PluginSettings::load();

    if (settings.api.model.isEmpty())
        settings.api.model = "deepseek-chat";

    if (dmb::CaptureBus::isRedirectedByEnvironment())
    {
        settings.busDirectory = dmb::CaptureBus::getRootDirectory().getFullPathName();
    }
    else if (settings.busDirectory.isNotEmpty())
    {
        dmb::CaptureBus::setRootDirectory (juce::File (settings.busDirectory));
    }
    else
    {
        settings.busDirectory = dmb::CaptureBus::getRootDirectory().getFullPathName();
    }

    capture.setWindowBars (getIntParam (dmb::params::windowBars));
    capture.setBeatsPerBar (getIntParam (dmb::params::beatsPerBar));

    // material saved earlier (captures + imports) is available again right away
    refreshLibrarySources();

    backgroundThread = std::make_unique<BackgroundThread> (*this);
    backgroundThread->startThread (juce::Thread::Priority::low);
}

DeepSeekMidiBridgeProcessor::~DeepSeekMidiBridgeProcessor()
{
    generation.cancel();

    if (backgroundThread != nullptr)
    {
        backgroundThread->signalThreadShouldExit();
        backgroundThread->stopThread (3000);
        backgroundThread.reset();
    }

    dmb::CaptureBus::remove (instanceId);
}

//==============================================================================
void DeepSeekMidiBridgeProcessor::prepareToPlay (double sampleRate, int)
{
    capture.prepare (sampleRate);
    player.prepare (sampleRate);
}

void DeepSeekMidiBridgeProcessor::releaseResources()
{
    capture.clear();
}

bool DeepSeekMidiBridgeProcessor::isMidiEffect() const
{
#if DMB_MIDI_ONLY
    return true;
#else
    return false;
#endif
}

//==============================================================================
void DeepSeekMidiBridgeProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                                juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;

    const auto numSamples = buffer.getNumSamples();

#if ! DMB_MIDI_ONLY
    // Straight audio pass-through: this plugin never changes the sound.
    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        if (buffer.getReadPointer (channel) != buffer.getWritePointer (channel))
            buffer.copyFrom (channel, 0, buffer.getReadPointer (channel), numSamples);
#else
    juce::ignoreUnused (buffer);
#endif

    // ---- transport -----------------------------------------------------------
    bool hasPosition = false;
    bool isPlaying = false;
    double ppq = 0.0;
    double bpm = lastKnownBpm.load();

    if (auto* playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            if (const auto ppqPosition = position->getPpqPosition())
            {
                ppq = *ppqPosition;
                hasPosition = true;
            }

            if (const auto hostBpm = position->getBpm())
                bpm = *hostBpm;

            isPlaying = position->getIsPlaying();
        }
    }

    lastKnownBpm.store (bpm);
    lastHostPpq.store (hasPosition ? ppq : 0.0);
    lastHostPlaying.store (isPlaying);

    // remember the loop length so "生成长度跟随宿主循环" can use it
    if (hasPosition)
    {
        if (auto* playHead = getPlayHead())
        {
            if (const auto position = playHead->getPosition())
            {
                if (const auto loop = position->getLoopPoints())
                {
                    const auto beatsPerBarLocal = juce::jmax (1, getIntParam (dmb::params::beatsPerBar));
                    const auto lengthPpq = loop->ppqEnd - loop->ppqStart;

                    if (lengthPpq > 0.0)
                        lastLoopBars.store (lengthPpq / (double) beatsPerBarLocal);
                }
            }
        }
    }

    // ---- capture the MIDI of the host track --------------------------------
    capture.processBlock (midiMessages, numSamples, hasPosition, isPlaying, ppq, bpm);

    // ---- optionally play the generated MIDI out of the plugin --------------
    const auto liveOut = getBoolParam (dmb::params::liveOutput) || freeRunPreview.load();

    player.processBlock (midiMessages,
                         numSamples,
                         hasPosition,
                         isPlaying,
                         ppq,
                         bpm,
                         liveOut && getBoolParam (dmb::params::syncToHost),
                         getBoolParam (dmb::params::loopPlayback),
                         liveOut && freeRunPreview.load());
}

//==============================================================================
void DeepSeekMidiBridgeProcessor::updateTrackProperties (const TrackProperties& properties)
{
    const juce::ScopedLock sl (trackInfoLock);

    if (properties.name.has_value())
        hostTrackName = properties.name->trim();
}

juce::String DeepSeekMidiBridgeProcessor::getHostTrackName() const
{
    const juce::ScopedLock sl (trackInfoLock);
    return hostTrackName;
}

//==============================================================================
float DeepSeekMidiBridgeProcessor::getFloatParam (const char* id) const
{
    if (auto* value = apvts.getRawParameterValue (id))
        return value->load();

    return 0.0f;
}

bool DeepSeekMidiBridgeProcessor::getBoolParam (const char* id) const
{
    return getFloatParam (id) > 0.5f;
}

int DeepSeekMidiBridgeProcessor::getIntParam (const char* id) const
{
    return (int) std::lround (getFloatParam (id));
}

//==============================================================================
dmb::PluginSettings DeepSeekMidiBridgeProcessor::getSettings() const
{
    const juce::ScopedLock sl (settingsLock);
    return settings;
}

void DeepSeekMidiBridgeProcessor::setSettings (const dmb::PluginSettings& newSettings)
{
    {
        const juce::ScopedLock sl (settingsLock);
        settings = newSettings;

        if (! dmb::CaptureBus::isRedirectedByEnvironment() && settings.busDirectory.isNotEmpty())
            dmb::CaptureBus::setRootDirectory (juce::File (settings.busDirectory));

        capture.setWindowBars (settings.windowBars);
        capture.setBeatsPerBar (settings.beatsPerBar);
    }

    settings.save();
}

void DeepSeekMidiBridgeProcessor::saveSettings()
{
    const juce::ScopedLock sl (settingsLock);
    settings.save();
}

juce::File DeepSeekMidiBridgeProcessor::getOutputDirectory() const
{
    const auto root = getProjectsRoot();

    const juce::ScopedLock sl (projectLock);

    if (project.isValid() && project.folder != juce::File())
        return project.folder;

    return root;
}

void DeepSeekMidiBridgeProcessor::setOutputDirectory (const juce::File& directory)
{
    {
        const juce::ScopedLock sl (settingsLock);
        settings.outputDirectory = directory.getFullPathName();
    }

    settings.save();
}

//==============================================================================
juce::File DeepSeekMidiBridgeProcessor::getProjectsRoot() const
{
    const juce::ScopedLock sl (settingsLock);

    if (settings.outputDirectory.isNotEmpty())
        return juce::File (settings.outputDirectory);

    return dmb::getDefaultOutputDirectory();
}

dmb::ProjectInfo DeepSeekMidiBridgeProcessor::getProject() const
{
    const juce::ScopedLock sl (projectLock);
    return project;
}

bool DeepSeekMidiBridgeProcessor::isProjectResolved() const
{
    const juce::ScopedLock sl (projectLock);
    return projectResolved;
}

juce::String DeepSeekMidiBridgeProcessor::getProjectName() const
{
    const juce::ScopedLock sl (projectLock);

    if (project.name.isNotEmpty())
        return project.name;

    return dmb::utf8 ("（尚未确定工程）");
}

juce::File DeepSeekMidiBridgeProcessor::getProjectFolder() const
{
    const juce::ScopedLock sl (projectLock);
    return project.folder;
}

juce::String DeepSeekMidiBridgeProcessor::suggestedProjectName() const
{
    // Live puts the Set name in its window title ("My Song.als - Ableton Live 12 Suite")
    auto fromHost = dmb::ProjectStore::projectNameFromHostTitle (dmb::ProjectStore::hostProjectTitle());

    if (fromHost.isNotEmpty())
        return fromHost;

    juce::String host (juce::CharPointer_UTF8 (juce::PluginHostType().getHostDescription()));

    if (host.isEmpty())
        host = "DMB";

    return host + " " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M");
}

void DeepSeekMidiBridgeProcessor::adoptProject (const dmb::ProjectInfo& info, bool announce)
{
    if (! info.isValid())
        return;

    bool changed = false;

    {
        const juce::ScopedLock sl (projectLock);

        changed = (project.id != info.id || project.folder != info.folder);

        project = info;
        projectResolved = true;
        projectAdoptedMs = juce::Time::currentTimeMillis();

        if (changed)
            projectFolderEnsured = false;
    }

    if (announce)
        dmb::ProjectRegistry::publish (info, instanceId);

    if (changed)
    {
        dmb::logDiagnostic ("project", juce::String (announce ? "use project: " : "follow project: ")
                                      + info.name + "  id=" + info.id
                                      + "  folder=" + dmb::displayPath (info.folder));
    }
    refreshLibrarySources();
    refreshVisibleSources();
}

void DeepSeekMidiBridgeProcessor::resolveProject()
{
    if (isProjectResolved())
        return;

    dmb::ProjectInfo info;

    const auto registry = dmb::ProjectRegistry::get();
    const bool weCreatedIt = ! registry.info.isValid();

    if (! weCreatedIt)
    {
        // another instance of this host run already decided
        info = registry.info;
    }
    else
    {
        info = dmb::ProjectStore::create (getProjectsRoot(), suggestedProjectName());
    }

    adoptProject (info, weCreatedIt);
}

dmb::ProjectInfo DeepSeekMidiBridgeProcessor::getProjectForWrite()
{
    if (! isProjectResolved())
        resolveProject();

    dmb::ProjectInfo info;
    bool alreadyEnsured = false;

    {
        const juce::ScopedLock sl (projectLock);
        info = project;
        alreadyEnsured = projectFolderEnsured;
    }

    if (! info.isValid() || alreadyEnsured)
        return info;

    if (dmb::ProjectStore::ensureFolder (info))
    {
        const juce::ScopedLock sl (projectLock);
        projectFolderEnsured = true;
        return info;
    }

    // the folder belongs to somebody else (or cannot be created): take a free one
    auto replacement = dmb::ProjectStore::create (getProjectsRoot(), info.name);

    if (dmb::ProjectStore::ensureFolder (replacement))
    {
        adoptProject (replacement, true);
        return replacement;
    }

    dmb::logDiagnostic ("project", "cannot create project folder: " + dmb::displayPath (info.folder));
    return info;
}

void DeepSeekMidiBridgeProcessor::syncProjectFromRegistry()
{
    if (! isProjectResolved())
        return;

    const auto now = juce::Time::currentTimeMillis();

    {
        const juce::ScopedLock sl (projectLock);

        if (now - lastRegistrySyncMs < 500)
            return;

        lastRegistrySyncMs = now;
    }

    const auto registry = dmb::ProjectRegistry::get();

    if (! registry.info.isValid() || registry.changedBy == instanceId)
        return;

    bool shouldAdopt = false;

    {
        const juce::ScopedLock sl (projectLock);
        shouldAdopt = (registry.changedAtMs > projectAdoptedMs);
    }

    if (shouldAdopt)
        adoptProject (registry.info, false);
}

std::vector<dmb::ProjectInfo> DeepSeekMidiBridgeProcessor::listProjects() const
{
    auto result = dmb::ProjectStore::list (getProjectsRoot());

    const auto current = getProject();

    if (current.isValid())
    {
        const bool alreadyThere = std::any_of (result.begin(), result.end(),
                                               [&current] (const dmb::ProjectInfo& p) { return p.folder == current.folder; });

        if (! alreadyThere)
            result.insert (result.begin(), current);
    }

    return result;
}

void DeepSeekMidiBridgeProcessor::remapClipPaths (const juce::File& oldFolder, const juce::File& newFolder)
{
    if (oldFolder == juce::File() || oldFolder == newFolder)
        return;

    std::shared_ptr<const dmb::GeneratedClip> current;

    {
        const juce::ScopedLock sl (clipLock);
        current = generatedClip;
    }

    if (current == nullptr)
        return;

    auto updated = *current;
    bool changed = false;

    auto remap = [&oldFolder, &newFolder, &changed] (juce::File& file)
    {
        if (file.isAChildOf (oldFolder))
        {
            file = newFolder.getChildFile (file.getFileName());
            changed = true;
        }
    };

    remap (updated.file);

    for (auto& file : updated.perTrackFiles)
        remap (file);

    if (! changed)
        return;

    auto shared = std::make_shared<const dmb::GeneratedClip> (std::move (updated));

    {
        const juce::ScopedLock sl (clipLock);
        generatedClip = shared;
    }

    player.setClip (shared);
    ++clipVersion;
}

bool DeepSeekMidiBridgeProcessor::renameProject (const juce::String& newName, juce::String& error)
{
    auto info = getProjectForWrite();

    if (! info.isValid())
    {
        error = dmb::utf8 ("当前还没有工程");
        return false;
    }

    const auto renamed = dmb::ProjectStore::rename (info, newName, getProjectsRoot(), error);

    if (! renamed.isValid())
        return false;

    adoptProject (renamed, true);
    remapClipPaths (info.folder, renamed.folder);

    return true;
}

bool DeepSeekMidiBridgeProcessor::switchProject (const juce::File& folder, juce::String& error)
{
    const auto info = dmb::ProjectStore::readMarker (folder);

    if (! info.isValid())
    {
        error = dmb::utf8 ("这不是一个工程文件夹（缺少 ") + dmb::ProjectStore::kMarkerFileName + "）";
        return false;
    }

    const auto previous = getProject();

    if (previous.id == info.id)
    {
        error = dmb::utf8 ("已经是当前工程");
        return false;
    }

    adoptProject (info, true);
    clearGeneratedClip();      // the previous result belongs to the previous project

    return true;
}

bool DeepSeekMidiBridgeProcessor::createProject (const juce::String& name, juce::String& error)
{
    auto info = dmb::ProjectStore::create (getProjectsRoot(), name);

    if (! dmb::ProjectStore::ensureFolder (info))
    {
        error = dmb::utf8 ("无法创建工程文件夹: ") + dmb::displayPath (info.folder);
        return false;
    }

    adoptProject (info, true);
    clearGeneratedClip();

    return true;
}

dmb::ProjectInfo DeepSeekMidiBridgeProcessor::findDetachedLibraryProject() const
{
    for (const auto& candidate : dmb::ProjectStore::list (getProjectsRoot()))
        if (candidate.name.startsWith (dmb::utf8 ("未归类")))
            return candidate;

    return {};
}

int DeepSeekMidiBridgeProcessor::migrateLegacyLibrary()
{
    const auto legacy = dmb::SourceLibrary::getLegacyDirectory();

    if (dmb::SourceLibrary::countEntries (legacy) == 0)
        return 0;

    auto info = findDetachedLibraryProject();

    if (! info.isValid())
        info = dmb::ProjectStore::create (getProjectsRoot(), dmb::utf8 ("未归类（旧素材）"));

    if (! dmb::ProjectStore::ensureFolder (info))
        return 0;

    const auto moved = dmb::SourceLibrary::moveEntriesTo (legacy, info.libraryDirectory());

    dmb::logDiagnostic ("project", "migrated " + juce::String (moved) + " legacy library entries into "
                                  + dmb::displayPath (info.folder));

    return moved;
}

void DeepSeekMidiBridgeProcessor::ensureProject()
{
    if (! isProjectResolved())
        resolveProject();
}

void DeepSeekMidiBridgeProcessor::ensureProjectFolder()
{
    getProjectForWrite();
}

int DeepSeekMidiBridgeProcessor::getDetachedLibraryCount() const
{
    const auto info = findDetachedLibraryProject();

    if (! info.isValid())
        return 0;

    if (info.id == getProject().id)
        return 0;

    return dmb::SourceLibrary::countEntries (info.libraryDirectory());
}

bool DeepSeekMidiBridgeProcessor::mergeDetachedLibrary (juce::String& error)
{
    const auto detached = findDetachedLibraryProject();

    if (! detached.isValid())
    {
        error = dmb::utf8 ("没有找到「未归类」的素材");
        return false;
    }

    auto current = getProjectForWrite();

    if (! current.isValid())
    {
        error = dmb::utf8 ("当前还没有工程");
        return false;
    }

    if (current.id == detached.id)
    {
        error = dmb::utf8 ("「未归类」就是当前工程");
        return false;
    }

    const auto moved = dmb::SourceLibrary::moveEntriesTo (detached.libraryDirectory(), current.libraryDirectory());

    if (moved == 0)
    {
        error = dmb::utf8 ("「未归类」里没有素材了");
        return false;
    }

    // drop the left over folder when nothing else lives in it
    bool holdsMidi = false;

    for (const auto& file : detached.folder.findChildFiles (juce::File::findFiles, false))
        if (file.hasFileExtension ("mid"))
            holdsMidi = true;

    if (! holdsMidi)
        detached.folder.deleteRecursively();

    refreshLibrarySources();

    return true;
}

//==============================================================================
std::vector<dmb::SourceTrack> DeepSeekMidiBridgeProcessor::getAvailableSources() const
{
    std::vector<dmb::SourceTrack> result;

    {
        const juce::ScopedLock sl (sourceLock);

        result = importedSources;
        result.insert (result.end(), busSources.begin(), busSources.end());
        result.insert (result.end(), librarySources.begin(), librarySources.end());

        if (! localSnapshot.notes.empty())
            result.push_back (localSnapshot);
    }

    // Captures that were put aside when the transport stopped / the loop wrapped.
    // They come after the live ones so the freshest material is at the top.
    auto kept = getKeptCaptures();
    result.insert (result.end(), kept.begin(), kept.end());

    return result;
}

std::vector<dmb::SourceTrack> DeepSeekMidiBridgeProcessor::getKeptCaptures() const
{
    auto baseName = getHostTrackName();

    if (baseName.isEmpty())
        baseName = juce::String (JucePlugin_Name);

    return capture.getKeptTakes ("kept-", baseName + dmb::utf8 (" 保留"));
}

int DeepSeekMidiBridgeProcessor::getKeptCaptureCount() const
{
    return capture.getKeptTakeCount();
}

void DeepSeekMidiBridgeProcessor::clearLocalCapture()
{
    capture.clear();

    {
        const juce::ScopedLock sl (sourceLock);
        localSnapshot = dmb::SourceTrack();
    }

    savedCaptureSignatures.clear();
    publishSignature.clear();

    const auto info = getProject();

    if (info.isValid())
        dmb::SourceLibrary::removeByOrigin (info.libraryDirectory(), dmb::utf8 ("捕获"));

    refreshLibrarySources();
}

//==============================================================================
void DeepSeekMidiBridgeProcessor::refreshLibrarySources()
{
    // only what belongs to this host project - never another project's material
    const auto info = getProject();

    auto loaded = info.isValid() ? dmb::SourceLibrary::loadAll (info.libraryDirectory())
                                 : std::vector<dmb::SourceTrack>();

    const juce::ScopedLock sl (sourceLock);
    librarySources = std::move (loaded);
}

void DeepSeekMidiBridgeProcessor::persistNewCaptures()
{
    // Takes that were put aside (transport stopped / loop wrapped) are written into
    // the persistent library, so nothing is lost when the host is closed.
    auto takes = capture.getKeptTakes ("capture-", dmb::utf8 ("捕获"));

    if (takes.empty())
        return;

    const auto info = getProjectForWrite();

    if (! info.isValid())
        return;

    bool added = false;

    for (auto& take : takes)
    {
        const auto signature = juce::String (take.notes.size()) + ":" + juce::String ((int) take.lastTick)
                             + ":" + take.name;

        if (savedCaptureSignatures.contains (signature))
            continue;

        savedCaptureSignatures.add (signature);

        take.id = {};
        take.origin = dmb::utf8 ("捕获");
        take.playing = false;
        take.projectId = info.id;

        if (dmb::SourceLibrary::save (info.libraryDirectory(), take).isNotEmpty())
            added = true;
    }

    if (added)
        refreshLibrarySources();
}

bool DeepSeekMidiBridgeProcessor::renameSource (const juce::String& id, const juce::String& newName)
{
    const auto name = newName.trim();

    if (name.isEmpty())
        return false;

    if (id.startsWith ("kept-"))
        return capture.renameKeptTake (id, name);

    if (id.startsWith ("lib-"))
    {
        const auto info = getProject();

        const auto ok = info.isValid() && dmb::SourceLibrary::rename (info.libraryDirectory(), id, name);

        if (ok)
            refreshLibrarySources();

        return ok;
    }

    if (id.startsWith ("file:"))
    {
        const juce::ScopedLock sl (sourceLock);

        for (auto& source : importedSources)
        {
            if (source.id == id)
            {
                source.name = name;
                return true;
            }
        }

        return false;
    }

    // the live capture of this instance: renaming it is the same as naming the instance
    if (id == instanceId)
    {
        auto current = getSettings();
        current.sourceName = name;
        setSettings (current);
        return true;
    }

    return false;
}

bool DeepSeekMidiBridgeProcessor::removeSource (const juce::String& id)
{
    if (id.startsWith ("kept-"))
        return capture.removeKeptTake (id);

    if (id.startsWith ("lib-"))
    {
        const auto info = getProject();

        const auto ok = info.isValid() && dmb::SourceLibrary::remove (info.libraryDirectory(), id);

        if (ok)
            refreshLibrarySources();

        return ok;
    }

    if (id.startsWith ("file:"))
    {
        const juce::ScopedLock sl (sourceLock);
        const auto before = importedSources.size();

        importedSources.erase (std::remove_if (importedSources.begin(), importedSources.end(),
                                               [&id] (const dmb::SourceTrack& s) { return s.id == id; }),
                               importedSources.end());

        return importedSources.size() != before;
    }

    if (id == instanceId)
    {
        clearLocalCapture();
        return true;
    }

    return false;      // e.g. a capture published by another instance
}

dmb::SourceTrack DeepSeekMidiBridgeProcessor::getLocalCapture() const
{
    const juce::ScopedLock sl (sourceLock);
    return localSnapshot;
}

std::vector<dmb::SourceTrack> DeepSeekMidiBridgeProcessor::getImportedSources() const
{
    const juce::ScopedLock sl (sourceLock);
    return importedSources;
}

void DeepSeekMidiBridgeProcessor::addImportedSources (std::vector<dmb::SourceTrack> sources)
{
    const auto info = getProjectForWrite();

    if (! info.isValid())
        return;

    bool stored = false;

    for (auto& source : sources)
    {
        // remember where it came from, then keep it in the persistent library
        if (source.origin.isEmpty())
            source.origin = dmb::utf8 ("导入: ") + source.name;

        source.id = {};
        source.projectId = info.id;
        stored = dmb::SourceLibrary::save (info.libraryDirectory(), source).isNotEmpty() || stored;
    }

    if (stored)
        refreshLibrarySources();
}

void DeepSeekMidiBridgeProcessor::clearImportedSources()
{
    {
        const juce::ScopedLock sl (sourceLock);
        importedSources.clear();
    }

    const auto info = getProject();

    if (info.isValid())
        dmb::SourceLibrary::removeByOrigin (info.libraryDirectory(), dmb::utf8 ("导入"));

    refreshLibrarySources();
}

//==============================================================================
std::shared_ptr<const dmb::GeneratedClip> DeepSeekMidiBridgeProcessor::getGeneratedClip() const
{
    const juce::ScopedLock sl (clipLock);
    return generatedClip;
}

void DeepSeekMidiBridgeProcessor::clearGeneratedClip()
{
    {
        const juce::ScopedLock sl (clipLock);
        generatedClip.reset();
    }

    player.clearClip();
    ++clipVersion;
}

juce::String DeepSeekMidiBridgeProcessor::getPromptText() const
{
    const juce::ScopedLock sl (clipLock);
    return promptText;
}

void DeepSeekMidiBridgeProcessor::setPromptText (const juce::String& text)
{
    const juce::ScopedLock sl (clipLock);
    promptText = text;
}

//==============================================================================
dmb::GenerationSpec DeepSeekMidiBridgeProcessor::makeGenerationSpec() const
{
    const auto current = getSettings();

    dmb::GenerationSpec spec;

    spec.bars = getIntParam (dmb::params::generationBars);
    spec.beatsPerBar = getIntParam (dmb::params::beatsPerBar);
    spec.temperature = (double) getFloatParam (dmb::params::temperature);
    spec.maxNotes = getIntParam (dmb::params::maxNotes);
    spec.referenceBars = getIntParam (dmb::params::referenceBars);

    // "生成长度跟随宿主循环长度": take the bars straight from the host loop
    if (getBoolParam (dmb::params::matchLoop))
    {
        const auto loopBars = lastLoopBars.load();

        if (loopBars >= 1.0)
            spec.bars = juce::jlimit (1, 64, (int) std::lround (loopBars));
    }
    spec.channel = getIntParam (dmb::params::channel);
    spec.transpose = getIntParam (dmb::params::transpose);
    spec.style = current.style;
    spec.instrument = current.instrument;
    spec.key = current.key;
    spec.keepGroove = current.keepGroove;

    return spec;
}

void DeepSeekMidiBridgeProcessor::startGeneration (const juce::String& requirement,
                                                   const std::vector<dmb::SourceTrack>& sources)
{
    auto spec = makeGenerationSpec();

    const auto current = getSettings();

    // the .mid files of this project live in its own folder
    const auto info = getProjectForWrite();

    generation.start (current.api, spec, requirement, sources,
                      info.isValid() ? info.folder : getProjectsRoot());
}

void DeepSeekMidiBridgeProcessor::cancelGeneration()
{
    generation.cancel();
}

void DeepSeekMidiBridgeProcessor::setFreeRunPreview (bool shouldPlay)
{
    freeRunPreview.store (shouldPlay);
}

//==============================================================================
void DeepSeekMidiBridgeProcessor::publishCapture()
{
    auto current = getSettings();

    auto name = current.sourceName;

    if (name.isEmpty())
        name = getHostTrackName();                 // Live hands us the real track name

    if (name.isEmpty())
        name = juce::String (JucePlugin_Name) + dmb::utf8 (" 捕获 ") + instanceId;

    auto snapshot = capture.snapshot (instanceId, name);

    {
        const juce::ScopedLock sl (sourceLock);
        localSnapshot = snapshot;
    }

    if (! getBoolParam (dmb::params::sendToBus))
        return;

    // A heartbeat keeps the entry alive on the other instances while this plugin
    // is loaded but the transport is standing still.
    const auto lastPitch = snapshot.notes.empty() ? 0 : snapshot.notes.back().pitch;

    const auto signature = juce::String (snapshot.notes.size()) + "_"
                         + juce::String (snapshot.lastTick, 1) + "_"
                         + juce::String (lastPitch);

    const bool changed = (signature != publishSignature);
    const bool beat = (++publishHeartbeat % 8) == 0;

    if (! changed && ! beat)
        return;

    if (snapshot.notes.empty() && publishSignature.isEmpty())
        return;

    publishSignature = signature;

    // The bus is shared with the other instances of this host project only; entries
    // of a project that happens to be open in another process are ignored.
    const auto info = getProjectForWrite();

    if (! info.isValid())
        return;

    snapshot.projectId = info.id;

    dmb::CaptureBus::publish (snapshot);
}

void DeepSeekMidiBridgeProcessor::maybeAutoGenerate()
{
    const auto playing = lastHostPlaying.load();
    const auto wasPlayingBefore = wasPlaying;
    wasPlaying = playing;

    if (! getBoolParam (dmb::params::autoGenerate))
        return;

    // fire on the playing -> stopped edge only
    if (! wasPlayingBefore || playing)
        return;

    if (generation.isRunning() || getApiProbe().isRunning())
        return;

    const auto now = juce::Time::currentTimeMillis();

    if (now - lastAutoGenerateMs < 10000)      // never spam the API
        return;

    auto sources = getAvailableSources();

    sources.erase (std::remove_if (sources.begin(), sources.end(),
                                   [] (const dmb::SourceTrack& s) { return s.notes.size() < 4; }),
                   sources.end());

    if (sources.empty())
        return;

    const auto requirement = getPromptText().trim();

    if (requirement.isEmpty())
        return;

    lastAutoGenerateMs = now;
    generation.setPendingNote (dmb::utf8 ("自动生成：检测到走带停止，参考 ")
                              + juce::String ((int) sources.size()) + dmb::utf8 (" 条轨"));
    startGeneration (requirement, sources);
}

void DeepSeekMidiBridgeProcessor::refreshVisibleSources()
{
    const auto info = getProject();

    // Before the project is known nothing is shown: the material of the project that
    // was open before must never leak into this one.
    auto sources = info.isValid() ? dmb::CaptureBus::readAll (15 * 60 * 1000, instanceId, info.id)
                                  : std::vector<dmb::SourceTrack>();

    const juce::ScopedLock sl (sourceLock);
    busSources = std::move (sources);
}

void DeepSeekMidiBridgeProcessor::pollGenerationResult()
{
    dmb::GeneratedClip clip;

    if (! generation.takeResult (clip))
        return;

    auto shared = std::make_shared<const dmb::GeneratedClip> (std::move (clip));

    {
        const juce::ScopedLock sl (clipLock);
        generatedClip = shared;
    }

    player.setClip (shared);
    ++clipVersion;
}

//==============================================================================
DeepSeekMidiBridgeProcessor::BackgroundThread::BackgroundThread (DeepSeekMidiBridgeProcessor& ownerToUse)
    : juce::Thread ("dmb-background"), owner (ownerToUse)
{
}

void DeepSeekMidiBridgeProcessor::BackgroundThread::run()
{
    int tick = 0;

    while (! threadShouldExit())
    {
        if (! owner.legacyLibraryChecked)
        {
            owner.legacyLibraryChecked = true;      // once per instance, cheap when there is nothing to do
            owner.migrateLegacyLibrary();
        }

        owner.capture.update();
        owner.pollGenerationResult();
        owner.persistNewCaptures();
        owner.maybeAutoGenerate();

        ++tick;

        if ((tick % 2) == 0)
            owner.publishCapture();

        if ((tick % 2) == 0)
            owner.syncProjectFromRegistry();

        // heartbeat: keeps the project claim of this host session alive for
        // instances that run in another process (plugin sandboxing)
        if ((tick % 32) == 0)
            dmb::ProjectRegistry::touch();

        if ((tick % 8) == 0)
            owner.refreshVisibleSources();

        if ((tick % 512) == 0)
            dmb::CaptureBus::pruneExpired();

        if ((tick % 4096) == 0)
            dmb::ProjectSession::prune();

        wait (60);
    }
}

//==============================================================================
void DeepSeekMidiBridgeProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());

    if (xml == nullptr)
    {
        xml = std::make_unique<juce::XmlElement> ("DEEPSEEK_MIDI_BRIDGE");
    }

    auto* extra = new juce::DynamicObject();

    {
        const juce::ScopedLock sl (settingsLock);
        extra->setProperty ("settings", settings.toVar());
    }

    {
        const juce::ScopedLock sl (clipLock);
        extra->setProperty ("prompt", promptText);

        if (generatedClip != nullptr)
            extra->setProperty ("clip", dmb::generatedClipToVar (*generatedClip));
    }

    // Which host project this is. Resolving here means the project (and its folder)
    // is written down as soon as the host saves the project, so reopening it brings
    // back exactly this project's material.
    const auto info = getProjectForWrite();

    if (info.isValid())
    {
        extra->setProperty ("projectId", info.id);
        extra->setProperty ("projectName", info.name);
        extra->setProperty ("projectFolder", info.folder.getFullPathName());
        extra->setProperty ("projectCreated", (juce::int64) info.createdMs);
    }

    xml->setAttribute ("dmbState", juce::JSON::toString (juce::var (extra), false));

    copyXmlToBinary (*xml, destData);
}

void DeepSeekMidiBridgeProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml == nullptr)
        return;

    if (xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));

    const auto json = xml->getStringAttribute ("dmbState");

    if (json.isEmpty())
        return;

    juce::var parsed;

    if (! juce::JSON::parse (json, parsed).wasOk())
        return;

    if (auto* root = parsed.getDynamicObject())
    {
        // The project comes first: the library, the output folder and the capture bus
        // are all scoped to it.
        dmb::ProjectInfo stored;
        stored.id = root->getProperty ("projectId").toString().trim();
        stored.name = root->getProperty ("projectName").toString().trim();
        stored.createdMs = (juce::int64) root->getProperty ("projectCreated");

        const auto folder = root->getProperty ("projectFolder").toString().trim();

        if (stored.id.isNotEmpty() && folder.isNotEmpty())
        {
            stored.folder = juce::File (folder);
            adoptProject (stored, true);
        }

        {
            const juce::ScopedLock sl (settingsLock);
            settings = dmb::PluginSettings::fromVar (root->getProperty ("settings"));

            // A recalled project (or the Standalone's saved state) that has no key of
            // its own falls back to the key stored on this machine, so a configured
            // key is never lost just because an older state was saved.
            if (settings.api.apiKey.isEmpty())
            {
                const auto local = dmb::PluginSettings::load();

                settings.api.apiKey = local.api.apiKey;

                if (settings.api.endpoint.isEmpty())
                    settings.api.endpoint = local.api.endpoint;

                if (settings.api.model.isEmpty())
                    settings.api.model = local.api.model;

                if (settings.busDirectory.isEmpty())
                    settings.busDirectory = local.busDirectory;
            }

            if (! dmb::CaptureBus::isRedirectedByEnvironment() && settings.busDirectory.isNotEmpty())
                dmb::CaptureBus::setRootDirectory (juce::File (settings.busDirectory));

            capture.setWindowBars (settings.windowBars);
            capture.setBeatsPerBar (settings.beatsPerBar);
        }

        dmb::GeneratedClip clip;

        if (dmb::generatedClipFromVar (root->getProperty ("clip"), clip))
        {
            auto shared = std::make_shared<const dmb::GeneratedClip> (std::move (clip));

            {
                const juce::ScopedLock sl (clipLock);
                generatedClip = shared;
                promptText = root->getProperty ("prompt").toString();
            }

            player.setClip (shared);
            ++clipVersion;
        }
    }
}

//==============================================================================
juce::AudioProcessorEditor* DeepSeekMidiBridgeProcessor::createEditor()
{
    return new DeepSeekMidiBridgeEditor (*this);
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new DeepSeekMidiBridgeProcessor();
}
