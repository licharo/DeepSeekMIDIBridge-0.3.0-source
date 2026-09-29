#pragma once

#include "DmbTypes.h"

namespace dmb
{

/** One named "API + model" combination the user can switch to with one click. */
struct ApiProfile
{
    juce::String name;
    juce::String endpoint;
    juce::String apiKey;
    juce::String model;

    bool matches (const ApiSettings& settings) const
    {
        return endpoint == settings.endpoint && model == settings.model;
    }
};

/** Everything that the plugin remembers between sessions. */
struct PluginSettings
{
    ApiSettings api;
    std::vector<ApiProfile> apiProfiles;   // saved "API + model" presets
    juce::String outputDirectory;
    juce::String busDirectory;
    juce::String sourceName;             // label of this instance on the capture bus
    juce::String style;                  // free text hints handed to the model
    juce::String instrument;
    juce::String key;
    int windowBars = 8;
    int generationBars = 8;
    int beatsPerBar = 4;
    int channel = 1;
    double temperature = 0.7;
    int maxNotes = 2000;
    bool keepGroove = true;
    bool liveOutput = false;
    bool openFolderAfterGenerate = true;   // reveal the .mid in Explorer when it is written
    double uiScale = 1.0;                // 1.0 = 100%; 0 = follow the display scale

    static juce::File getAppDirectory();
    static juce::File getSettingsFile();

    static PluginSettings load();
    bool save() const;

    juce::var toVar() const;
    static PluginSettings fromVar (const juce::var& v);
};

} // namespace dmb
