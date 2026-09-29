#pragma once

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace dmb
{

/** juce::String's `const char*` constructor interprets the input as ASCII, which
    turns every UTF-8 literal into mojibake. All text in this project is UTF-8, so
    non-ASCII literals are wrapped with this helper.
*/
inline juce::String utf8 (const char* text)
{
    return juce::String (juce::CharPointer_UTF8 (text));
}

//==============================================================================
/** Turns an absolute path into something that can be shown without revealing the
    account name or the folder layout of this machine:

        C:\Users\anyone\OneDrive\文档\DeepSeek MIDI Bridge\My Song
            -> 文档\DeepSeek MIDI Bridge\My Song
        C:\Users\anyone\AppData\Local\DeepSeekMidiBridge\settings.json
            -> %LOCALAPPDATA%\DeepSeekMidiBridge\settings.json
        C:\Users\anyone\Music\midi\out
            -> %USERPROFILE%\Music\midi\out

    Everything outside the user profile (C:\Program Files\..., D:\Samples\...) is
    returned unchanged - such a path says nothing about the person using the plugin.
*/
inline juce::String displayPath (const juce::String& pathText)
{
    if (pathText.isEmpty())
        return pathText;

    const juce::File path (pathText);

    if (path == juce::File())
        return pathText;

    struct Root
    {
        juce::File folder;
        juce::String token;
    };

    const juce::File documents (juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));
    const juce::File localAppData (juce::File::getSpecialLocation (juce::File::windowsLocalAppData));
    const juce::File roamingAppData (juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory));
    const juce::File home (juce::File::getSpecialLocation (juce::File::userHomeDirectory));

    const Root roots[] =
    {
        { documents,      documents.getFileName().isNotEmpty() ? documents.getFileName() : juce::String ("Documents") },
        { localAppData,   "%LOCALAPPDATA%" },
        { roamingAppData, "%APPDATA%" },
        { home,           "%USERPROFILE%" },
    };

    for (const auto& root : roots)
    {
        if (root.folder.isDirectory() && path.isAChildOf (root.folder))
        {
            const auto relative = path.getRelativePathFrom (root.folder);

            if (relative.isNotEmpty())
                return root.token + "\\" + relative;
        }
    }

    return pathText;
}

inline juce::String displayPath (const juce::File& path)
{
    return displayPath (path.getFullPathName());
}

/** Resolution used for every note that travels through the plugin. */
constexpr int kTicksPerQuarter = 960;
constexpr int kMaxNotesPerSource = 30000;
constexpr int kMaxGeneratedNotes = 20000;

//==============================================================================
/** A single note, stored in ticks relative to the start of the captured window. */
struct Note
{
    int tick = 0;
    int pitch = 60;
    int velocity = 100;
    int channel = 1;
    int lengthTicks = kTicksPerQuarter / 4;

    int endTick() const noexcept { return tick + juce::jmax (1, lengthTicks); }
};

//==============================================================================
/** One captured MIDI track (usually another host track, seen through an instance
    of this plugin that was placed on that track).
*/
struct SourceTrack
{
    juce::String id;                 // stable id of the publishing instance
    juce::String name;               // user visible label
    juce::String origin;             // dmb::utf8 ("捕获") / dmb::utf8 ("导入: x.mid")
    juce::String projectId;          // host project this source belongs to (see ProjectStore)
    int ppq = kTicksPerQuarter;
    double bpm = 120.0;
    bool playing = false;
    juce::int64 stampMs = 0;         // last time this source was updated
    double firstTick = 0.0;
    double lastTick = 0.0;
    std::vector<Note> notes;

    int barCount (double beatsPerBar = 4.0) const noexcept
    {
        if (notes.empty())
            return 0;

        const auto span = (lastTick - firstTick) / (double) juce::jmax (1, ppq);

        return juce::jmax (1, (int) std::ceil (span / juce::jmax (0.25, beatsPerBar)));
    }

    int lowestPitch() const noexcept
    {
        int v = 127;
        for (auto& n : notes) v = juce::jmin (v, n.pitch);
        return notes.empty() ? 60 : v;
    }

    int highestPitch() const noexcept
    {
        int v = 0;
        for (auto& n : notes) v = juce::jmax (v, n.pitch);
        return notes.empty() ? 72 : v;
    }

    juce::String describe() const
    {
        if (notes.empty())
            return name + dmb::utf8 ("  (空)");

        return name + "  " + juce::String (notes.size()) + dmb::utf8 (" 个音符, ")
             + juce::String (barCount()) + dmb::utf8 (" 小节, ") + juce::String (bpm, 1) + " BPM";
    }
};

//==============================================================================
/** A generated musical part. A response from the language model may describe
    several tracks at once (e.g. drums + bass).
*/
struct GeneratedTrack
{
    juce::String name;
    int channel = 1;
    std::vector<Note> notes;
};

struct GeneratedClip
{
    juce::String title = "AI";
    juce::String explanation;
    juce::String rawJson;
    juce::String model;
    juce::String usageSummary;
    int ppq = kTicksPerQuarter;
    double bpm = 120.0;
    int bars = 4;
    int beatsPerBar = 4;
    std::vector<GeneratedTrack> tracks;
    juce::File file;                                  // combined multi-track .mid
    std::vector<juce::File> perTrackFiles;            // one .mid per track

    int totalNotes() const noexcept
    {
        int n = 0;
        for (auto& t : tracks) n += (int) t.notes.size();
        return n;
    }

    bool isEmpty() const noexcept { return totalNotes() == 0; }

    double lengthInBeats() const noexcept
    {
        double last = 0.0;

        for (auto& t : tracks)
            for (auto& n : t.notes)
                last = juce::jmax (last, n.endTick() / (double) juce::jmax (1, ppq));

        return last;
    }
};

//==============================================================================
/** Everything the plugin needs to talk to the DeepSeek API. */
struct ApiSettings
{
    juce::String endpoint = "https://api.deepseek.com/chat/completions";
    juce::String apiKey;
    juce::String model = "deepseek-chat";
    int timeoutMs = 240000;
    int maxTokens = 8192;
    bool jsonMode = true;
};

//==============================================================================
/** Musical options that are handed to the language model. */
struct GenerationSpec
{
    int bars = 8;
    int beatsPerBar = 4;
    double temperature = 0.7;
    int maxNotes = 2000;
    int referenceBars = 16;     // only the first N bars of each reference are sent
    juce::String style;         // free text, e.g. dmb::utf8 ("Drum & Bass 鼓组")
    juce::String instrument;    // e.g. dmb::utf8 ("贝斯 / 合成器 / 鼓")
    juce::String key;           // e.g. "F# minor" or dmb::utf8 ("跟随源轨道")
    int transpose = 0;
    int channel = 1;
    bool keepGroove = true;
};

//==============================================================================
inline juce::String sanitiseFileName (const juce::String& raw, const juce::String& fallback = "AI")
{
    auto s = raw.trim();

    juce::String out;
    for (auto c : s)
    {
        if (juce::CharacterFunctions::isLetterOrDigit (c) || c > 127)
            out << juce::String::charToString (c);
        else
            out << "_";
    }
    while (out.contains ("__"))
        out = out.replace ("__", "_");

    out = out.trimCharactersAtStart ("_").trimCharactersAtEnd ("_");

    if (out.isEmpty())
        out = fallback;

    return out.substring (0, 80);
}

} // namespace dmb
