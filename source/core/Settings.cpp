#include "Settings.h"
#include "MidiFileUtil.h"

namespace dmb
{

juce::File PluginSettings::getAppDirectory()
{
    auto base = juce::File::getSpecialLocation (juce::File::windowsLocalAppData);

    if (! base.isDirectory())
        base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);

    auto dir = base.getChildFile ("DeepSeekMidiBridge");

    if (! dir.isDirectory())
        dir.createDirectory();

    return dir;
}

juce::File PluginSettings::getSettingsFile()
{
    return getAppDirectory().getChildFile ("settings.json");
}

//==============================================================================
juce::var PluginSettings::toVar() const
{
    auto* root = new juce::DynamicObject();

    root->setProperty ("endpoint", api.endpoint);
    root->setProperty ("apiKey", api.apiKey);
    root->setProperty ("model", api.model);
    root->setProperty ("timeoutMs", api.timeoutMs);
    root->setProperty ("maxTokens", api.maxTokens);
    root->setProperty ("jsonMode", api.jsonMode);

    root->setProperty ("outputDirectory", outputDirectory);
    root->setProperty ("busDirectory", busDirectory);
    root->setProperty ("sourceName", sourceName);
    root->setProperty ("style", style);
    root->setProperty ("instrument", instrument);
    root->setProperty ("key", key);
    root->setProperty ("windowBars", windowBars);
    root->setProperty ("generationBars", generationBars);
    root->setProperty ("beatsPerBar", beatsPerBar);
    root->setProperty ("channel", channel);
    root->setProperty ("temperature", temperature);
    root->setProperty ("maxNotes", maxNotes);
    root->setProperty ("keepGroove", keepGroove);
    root->setProperty ("liveOutput", liveOutput);
    root->setProperty ("openFolderAfterGenerate", openFolderAfterGenerate);
    root->setProperty ("uiScale", uiScale);

    // saved "API + model" presets
    if (! apiProfiles.empty())
    {
        juce::Array<juce::var> profiles;

        for (const auto& profile : apiProfiles)
        {
            auto* object = new juce::DynamicObject();
            object->setProperty ("name", profile.name);
            object->setProperty ("endpoint", profile.endpoint);
            object->setProperty ("apiKey", profile.apiKey);
            object->setProperty ("model", profile.model);
            profiles.add (juce::var (object));
        }

        root->setProperty ("apiProfiles", profiles);
    }

    return juce::var (root);
}

PluginSettings PluginSettings::fromVar (const juce::var& v)
{
    PluginSettings s;
    s.outputDirectory = getDefaultOutputDirectory().getFullPathName();

    auto* root = v.getDynamicObject();

    if (root == nullptr)
        return s;

    auto getString = [root] (const char* name, const juce::String& fallback)
    {
        const auto value = root->getProperty (name);
        return value.isVoid() ? fallback : value.toString();
    };

    auto getInt = [root] (const char* name, int fallback)
    {
        const auto value = root->getProperty (name);
        return (value.isVoid() || value.isString()) ? fallback : (int) value;
    };

    auto getDouble = [root] (const char* name, double fallback)
    {
        const auto value = root->getProperty (name);
        return (value.isVoid() || value.isString()) ? fallback : (double) value;
    };

    auto getBool = [root] (const char* name, bool fallback)
    {
        const auto value = root->getProperty (name);
        return value.isVoid() ? fallback : (bool) value;
    };

    s.api.endpoint = getString ("endpoint", s.api.endpoint);
    s.api.apiKey = getString ("apiKey", {});
    s.api.model = getString ("model", s.api.model);
    s.api.timeoutMs = juce::jlimit (5000, 600000, getInt ("timeoutMs", s.api.timeoutMs));
    s.api.maxTokens = juce::jlimit (256, 8192, getInt ("maxTokens", s.api.maxTokens));
    s.api.jsonMode = getBool ("jsonMode", s.api.jsonMode);

    s.outputDirectory = getString ("outputDirectory", s.outputDirectory);
    s.busDirectory = getString ("busDirectory", {});
    s.sourceName = getString ("sourceName", {});
    s.style = getString ("style", {});
    s.instrument = getString ("instrument", {});
    s.key = getString ("key", {});
    s.windowBars = juce::jlimit (1, 64, getInt ("windowBars", s.windowBars));
    s.generationBars = juce::jlimit (1, 128, getInt ("generationBars", s.generationBars));
    s.beatsPerBar = juce::jlimit (1, 16, getInt ("beatsPerBar", s.beatsPerBar));
    s.channel = juce::jlimit (1, 16, getInt ("channel", s.channel));
    s.temperature = juce::jlimit (0.0, 2.0, getDouble ("temperature", s.temperature));
    s.maxNotes = juce::jlimit (16, 8000, getInt ("maxNotes", s.maxNotes));
    s.keepGroove = getBool ("keepGroove", s.keepGroove);
    s.liveOutput = getBool ("liveOutput", s.liveOutput);
    s.openFolderAfterGenerate = getBool ("openFolderAfterGenerate", s.openFolderAfterGenerate);
    s.uiScale = juce::jlimit (0.0, 3.0, getDouble ("uiScale", s.uiScale));

    if (auto* profiles = root->getProperty ("apiProfiles").getArray())
    {
        for (const auto& item : *profiles)
        {
            auto* object = item.getDynamicObject();

            if (object == nullptr)
                continue;

            ApiProfile profile;
            profile.name = object->getProperty ("name").toString();
            profile.endpoint = object->getProperty ("endpoint").toString();
            profile.apiKey = object->getProperty ("apiKey").toString();
            profile.model = object->getProperty ("model").toString();

            if (profile.name.isEmpty())
                profile.name = profile.endpoint + " · " + profile.model;

            s.apiProfiles.push_back (std::move (profile));
        }
    }

    return s;
}

//==============================================================================
PluginSettings PluginSettings::load()
{
    PluginSettings settings;

    const auto file = getSettingsFile();

    if (file.existsAsFile())
    {
        juce::var parsed;

        if (juce::JSON::parse (file.loadFileAsString(), parsed).wasOk())
            settings = fromVar (parsed);
    }

    // An environment variable provides a convenient way to avoid storing the key
    // in a plain text file.
    if (settings.api.apiKey.isEmpty())
        settings.api.apiKey = juce::SystemStats::getEnvironmentVariable ("DEEPSEEK_API_KEY", {}).trim();

    if (settings.outputDirectory.isEmpty())
        settings.outputDirectory = getDefaultOutputDirectory().getFullPathName();

    return settings;
}

bool PluginSettings::save() const
{
    const auto file = getSettingsFile();

    if (! file.getParentDirectory().isDirectory())
        file.getParentDirectory().createDirectory();

    return file.replaceWithText (juce::JSON::toString (toVar(), false));
}

} // namespace dmb
