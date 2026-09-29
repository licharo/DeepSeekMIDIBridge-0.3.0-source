#include "MidiJson.h"

#include <cmath>

namespace dmb
{

namespace
{
    int findStringEnd (const juce::String& text, int start)
    {
        // start points at the opening quote
        for (int i = start + 1; i < text.length(); ++i)
        {
            if (text[i] == '\\')
            {
                ++i;
                continue;
            }

            if (text[i] == '"')
                return i;
        }

        return text.length() - 1;
    }

    juce::var getAny (const juce::var& object, const juce::StringArray& keys)
    {
        if (auto* dyn = object.getDynamicObject())
            for (const auto& key : keys)
                if (dyn->hasProperty (key))
                    return dyn->getProperty (key);

        return {};
    }

    bool isMissing (const juce::var& v) { return v.isVoid() || v.isUndefined(); }

    //--------------------------------------------------------------------------
    /** Parses a position that may be a number, or a string such as "3:2:0"
        (bar:beat:sixteenth) or "12.5". Sets wasBarBeatString when the value was
        written in the bar:beat:sixteenth form (which is always in beats).
    */
    double parsePositionValue (const juce::var& value, int beatsPerBar, bool& wasBarBeatString)
    {
        wasBarBeatString = false;

        if (value.isString())
        {
            const auto text = value.toString().trim();

            if (text.containsChar (':'))
            {
                auto parts = juce::StringArray::fromTokens (text, ":", "");
                parts.removeEmptyStrings();

                double beats = 0.0;

                if (parts.size() >= 3)
                    beats = parts[0].getDoubleValue() * (double) beatsPerBar
                          + parts[1].getDoubleValue()
                          + parts[2].getDoubleValue() / 4.0;   // sixteenths
                else if (parts.size() == 2)
                    beats = parts[0].getDoubleValue() * (double) beatsPerBar + parts[1].getDoubleValue();

                wasBarBeatString = true;
                return beats;
            }

            return text.getDoubleValue();
        }

        return (double) value;
    }

    void appendNotesFromArray (const juce::var& array,
                               double ticksPerUnit,
                               int ppq,
                               int beatsPerBar,
                               int defaultChannel,
                               int transpose,
                               GeneratedTrack& track,
                               juce::String& error)
    {
        if (! array.isArray())
        {
            error = dmb::utf8 ("notes 字段不是数组");
            return;
        }

        for (const auto& item : *array.getArray())
        {
            if (! item.isObject())
                continue;

            Note note;
            note.channel = defaultChannel;

            const auto pitchVar = getAny (item, { "pitch", "note", "key", "midi", "n", "p" });

            if (isMissing (pitchVar))
                continue;

            note.pitch = (int) std::lround ((double) pitchVar) + transpose;

            if (note.pitch < 0 || note.pitch > 127)
                continue;

            double startTicks = 0.0;

            const auto startTickVar = getAny (item, { "tick", "startTick", "start_tick" });

            if (! isMissing (startTickVar))
            {
                startTicks = (double) startTickVar;
            }
            else
            {
                const auto startVar = getAny (item, { "start", "startBeat", "start_beats", "beat", "beats",
                                                      "time", "position", "pos", "at", "t" });

                if (! isMissing (startVar))
                {
                    bool wasBarBeat = false;
                    const auto value = parsePositionValue (startVar, beatsPerBar, wasBarBeat);
                    startTicks = wasBarBeat ? value * (double) ppq : value * ticksPerUnit;
                }
            }

            double lengthTicks = ticksPerUnit * 0.25;

            const auto lenTickVar = getAny (item, { "lengthTicks", "durationTicks", "lenTicks", "duration_ticks" });

            if (! isMissing (lenTickVar))
            {
                lengthTicks = (double) lenTickVar;
            }
            else
            {
                const auto lenVar = getAny (item, { "length", "len", "duration", "dur", "lengthBeats",
                                                    "sustain", "l" });

                if (! isMissing (lenVar))
                {
                    bool wasBarBeat = false;
                    const auto value = parsePositionValue (lenVar, beatsPerBar, wasBarBeat);
                    lengthTicks = wasBarBeat ? value * (double) ppq : value * ticksPerUnit;
                }
            }

            const auto velocityVar = getAny (item, { "velocity", "vel", "volume", "v", "dynamic", "velo" });
            note.velocity = isMissing (velocityVar) ? 100 : (int) std::lround ((double) velocityVar);

            const auto channelVar = getAny (item, { "channel", "ch", "midiChannel", "c" });
            note.channel = isMissing (channelVar) ? defaultChannel : (int) std::lround ((double) channelVar);

            note.tick = juce::jmax (0, (int) std::lround (startTicks));
            note.lengthTicks = juce::jlimit (1, 64 * ppq, (int) std::lround (lengthTicks));
            note.velocity = juce::jlimit (1, 127, note.velocity);
            note.channel = juce::jlimit (1, 16, note.channel);

            track.notes.push_back (note);
        }
    }

    /** Returns how many ticks one unit is worth. The default (and "beats") is a
        quarter note. */
    double detectUnitScale (const juce::var& root, int ppq, int beatsPerBar)
    {
        auto unit = getAny (root, { "unit", "timeUnit", "time_unit", "units" }).toString().trim().toLowerCase();

        if (unit.startsWith ("tick"))
            return 1.0;

        if (unit.startsWith ("step") || unit.startsWith ("sixteenth") || unit == "16th")
            return (double) ppq / 4.0;

        if (unit.startsWith ("bar") || unit.startsWith ("measure"))
            return (double) ppq * (double) beatsPerBar;

        return (double) ppq;   // beats (also the fallback)
    }
}

//==============================================================================
juce::String extractJsonObject (const juce::String& text)
{
    auto trimmed = text.trim();

    if (trimmed.startsWith ("```"))
    {
        const auto firstNewline = trimmed.indexOfChar ('\n');

        if (firstNewline >= 0)
            trimmed = trimmed.substring (firstNewline + 1);

        if (trimmed.endsWith ("```"))
            trimmed = trimmed.dropLastCharacters (3);

        trimmed = trimmed.trim();
    }

    const int start = trimmed.indexOfChar ('{');

    if (start < 0)
        return {};

    int depth = 0;
    bool inString = false;

    for (int i = start; i < trimmed.length(); ++i)
    {
        const auto c = trimmed[i];

        if (inString)
        {
            if (c == '\\')
            {
                ++i;
                continue;
            }

            if (c == '"')
                inString = false;

            continue;
        }

        if (c == '"')
        {
            inString = true;
            continue;
        }

        if (c == '{')
        {
            ++depth;
        }
        else if (c == '}')
        {
            if (--depth == 0)
                return trimmed.substring (start, i + 1);
        }
    }

    juce::ignoreUnused (findStringEnd);
    return {};
}

//==============================================================================
bool parseGenerationResult (const juce::String& text,
                            int defaultPpq,
                            int defaultBars,
                            int defaultBeatsPerBar,
                            int defaultChannel,
                            int transposeSemitones,
                            GeneratedClip& result,
                            juce::String& error)
{
    const auto json = extractJsonObject (text);

    if (json.isEmpty())
    {
        error = dmb::utf8 ("模型回复中没有找到 JSON 对象");
        return false;
    }

    juce::var root;

    {
        const auto parseResult = juce::JSON::parse (json, root);

        if (! parseResult.wasOk())
        {
            error = dmb::utf8 ("JSON 解析失败: ") + parseResult.getErrorMessage();
            return false;
        }
    }

    if (! root.isObject())
    {
        error = dmb::utf8 ("JSON 根节点不是对象");
        return false;
    }

    result = GeneratedClip();
    result.rawJson = json;

    const auto ppqVar = getAny (root, { "ppq", "resolution", "ticksPerQuarter" });
    result.ppq = isMissing (ppqVar) ? defaultPpq : (int) std::lround ((double) ppqVar);
    result.ppq = juce::jlimit (24, 9600, result.ppq);

    const auto bpmVar = getAny (root, { "bpm", "tempo" });
    result.bpm = isMissing (bpmVar) ? 0.0 : juce::jlimit (20.0, 400.0, (double) bpmVar);

    const auto beatsVar = getAny (root, { "beatsPerBar", "beats_per_bar", "timeSignatureNumerator" });
    result.beatsPerBar = isMissing (beatsVar) ? defaultBeatsPerBar
                                              : juce::jlimit (1, 16, (int) std::lround ((double) beatsVar));

    auto titleVar = getAny (root, { "name", "title", "clipName", "trackName" });
    result.title = titleVar.isVoid() ? juce::String ("AI") : titleVar.toString();

    auto explanationVar = getAny (root, { "explanation", "reason", "comment", "description", "notes_text", "summary" });
    result.explanation = explanationVar.isVoid() ? juce::String() : explanationVar.toString();

    const auto ticksPerUnit = detectUnitScale (root, result.ppq, result.beatsPerBar);

    const auto tracksVar = getAny (root, { "tracks", "parts", "layers" });

    if (tracksVar.isArray())
    {
        int index = 0;

        for (const auto& item : *tracksVar.getArray())
        {
            if (! item.isObject())
                continue;

            GeneratedTrack track;
            track.name = getAny (item, { "name", "title", "instrument", "part" }).toString();

            if (track.name.isEmpty())
                track.name = "Track " + juce::String (++index);
            else
                ++index;

            const auto channelVar = getAny (item, { "channel", "ch", "midiChannel" });
            track.channel = isMissing (channelVar) ? defaultChannel : juce::jlimit (1, 16, (int) std::lround ((double) channelVar));

            auto notesVar = getAny (item, { "notes", "events", "noteList" });

            if (notesVar.isArray())
                appendNotesFromArray (notesVar, ticksPerUnit, result.ppq, result.beatsPerBar,
                                      track.channel, transposeSemitones, track, error);

            std::sort (track.notes.begin(), track.notes.end(),
                       [] (const Note& a, const Note& b) { return a.tick < b.tick; });

            if (! track.notes.empty())
                result.tracks.push_back (std::move (track));
        }
    }

    if (result.tracks.empty())
    {
        auto notesVar = getAny (root, { "notes", "events", "noteList", "midi" });

        if (notesVar.isArray())
        {
            GeneratedTrack track;
            track.name = result.title.isEmpty() ? juce::String ("AI") : result.title;
            track.channel = defaultChannel;

            appendNotesFromArray (notesVar, ticksPerUnit, result.ppq, result.beatsPerBar,
                                  defaultChannel, transposeSemitones, track, error);

            std::sort (track.notes.begin(), track.notes.end(),
                       [] (const Note& a, const Note& b) { return a.tick < b.tick; });

            if (! track.notes.empty())
                result.tracks.push_back (std::move (track));
        }
    }

    if (result.tracks.empty())
    {
        error = dmb::utf8 ("模型回复里没有可用的音符 (notes 为空)");
        return false;
    }

    int totalNotes = 0;

    for (auto& track : result.tracks)
        totalNotes += (int) track.notes.size();

    if (totalNotes > kMaxGeneratedNotes)
    {
        // keep the earliest notes only, to stay sane
        for (auto& track : result.tracks)
            if ((int) track.notes.size() > kMaxGeneratedNotes)
                track.notes.resize ((size_t) kMaxGeneratedNotes);
    }

    const auto barsVar = getAny (root, { "bars", "length", "lengthBars" });

    if (! isMissing (barsVar))
        result.bars = juce::jlimit (1, 256, (int) std::lround ((double) barsVar));
    else
        result.bars = defaultBars;

    double lastTick = 0.0;

    for (auto& track : result.tracks)
        for (auto& note : track.notes)
            lastTick = juce::jmax (lastTick, (double) note.endTick());

    const auto neededBars = (int) std::ceil (lastTick / ((double) result.ppq * (double) result.beatsPerBar));

    result.bars = juce::jlimit (1, 512, juce::jmax (result.bars, neededBars));

    if (result.bpm <= 0.0)
        result.bpm = 120.0;

    return true;
}

//==============================================================================
juce::var generatedClipToVar (const GeneratedClip& clip)
{
    auto* root = new juce::DynamicObject();

    root->setProperty ("title", clip.title);
    root->setProperty ("explanation", clip.explanation);
    root->setProperty ("model", clip.model);
    root->setProperty ("ppq", clip.ppq);
    root->setProperty ("bpm", clip.bpm);
    root->setProperty ("bars", clip.bars);
    root->setProperty ("beatsPerBar", clip.beatsPerBar);
    root->setProperty ("file", clip.file.getFullPathName());

    juce::Array<juce::var> tracks;

    for (const auto& track : clip.tracks)
    {
        auto* trackObject = new juce::DynamicObject();
        trackObject->setProperty ("name", track.name);
        trackObject->setProperty ("channel", track.channel);

        juce::Array<juce::var> notes;

        for (const auto& note : track.notes)
        {
            auto* noteObject = new juce::DynamicObject();
            noteObject->setProperty ("t", note.tick);
            noteObject->setProperty ("p", note.pitch);
            noteObject->setProperty ("v", note.velocity);
            noteObject->setProperty ("c", note.channel);
            noteObject->setProperty ("l", note.lengthTicks);
            notes.add (juce::var (noteObject));
        }

        trackObject->setProperty ("notes", notes);
        tracks.add (juce::var (trackObject));
    }

    root->setProperty ("tracks", tracks);

    return juce::var (root);
}

bool generatedClipFromVar (const juce::var& value, GeneratedClip& clip)
{
    auto* root = value.getDynamicObject();

    if (root == nullptr)
        return false;

    GeneratedClip result;
    result.title = root->getProperty ("title").toString();
    result.explanation = root->getProperty ("explanation").toString();
    result.model = root->getProperty ("model").toString();
    result.ppq = juce::jlimit (24, 9600, (int) root->getProperty ("ppq"));
    result.bpm = juce::jlimit (20.0, 400.0, (double) root->getProperty ("bpm"));
    result.bars = juce::jlimit (1, 512, (int) root->getProperty ("bars"));
    result.beatsPerBar = juce::jlimit (1, 16, (int) root->getProperty ("beatsPerBar"));

    if (result.ppq <= 0)  result.ppq = kTicksPerQuarter;
    if (result.bpm <= 0.0) result.bpm = 120.0;
    if (result.bars <= 0) result.bars = 1;
    if (result.beatsPerBar <= 0) result.beatsPerBar = 4;

    const auto filePath = root->getProperty ("file").toString();

    if (filePath.isNotEmpty())
    {
        const juce::File file (filePath);

        if (file.existsAsFile())
            result.file = file;
    }

    if (auto* tracks = root->getProperty ("tracks").getArray())
    {
        for (const auto& trackVar : *tracks)
        {
            auto* trackObject = trackVar.getDynamicObject();

            if (trackObject == nullptr)
                continue;

            GeneratedTrack track;
            track.name = trackObject->getProperty ("name").toString();
            track.channel = juce::jlimit (1, 16, (int) trackObject->getProperty ("channel"));

            if (track.channel <= 0)
                track.channel = 1;

            if (auto* notes = trackObject->getProperty ("notes").getArray())
            {
                for (const auto& noteVar : *notes)
                {
                    auto* noteObject = noteVar.getDynamicObject();

                    if (noteObject == nullptr)
                        continue;

                    Note note;
                    note.tick = juce::jmax (0, (int) noteObject->getProperty ("t"));
                    note.pitch = juce::jlimit (0, 127, (int) noteObject->getProperty ("p"));
                    note.velocity = juce::jlimit (1, 127, (int) noteObject->getProperty ("v"));
                    note.channel = juce::jlimit (1, 16, (int) noteObject->getProperty ("c"));
                    note.lengthTicks = juce::jmax (1, (int) noteObject->getProperty ("l"));
                    track.notes.push_back (note);
                }
            }

            if (! track.notes.empty())
                result.tracks.push_back (std::move (track));
        }
    }

    if (result.tracks.empty())
        return false;

    clip = std::move (result);
    return true;
}

} // namespace dmb
