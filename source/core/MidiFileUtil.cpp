#include "MidiFileUtil.h"

namespace dmb
{

juce::File getDefaultOutputDirectory()
{
    auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);

    if (! docs.isDirectory())
        docs = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    return docs.getChildFile ("DeepSeek MIDI Bridge");
}

//==============================================================================
juce::MidiFile buildMidiFile (const GeneratedClip& clip, int onlyTrackIndex)
{
    juce::MidiFile midi;
    midi.setTicksPerQuarterNote (juce::jlimit (24, 9600, clip.ppq));

    const auto tempo = (int) std::lround (60000000.0 / juce::jlimit (20.0, 400.0, clip.bpm));

    const int firstTrack = (onlyTrackIndex >= 0 ? onlyTrackIndex : 0);
    const int lastTrack  = (onlyTrackIndex >= 0 ? onlyTrackIndex + 1 : (int) clip.tracks.size());

    for (int t = firstTrack; t < lastTrack && t < (int) clip.tracks.size(); ++t)
    {
        const auto& source = clip.tracks[(size_t) t];

        juce::MidiMessageSequence sequence;

        auto header = juce::MidiMessage::textMetaEvent (3, source.name.isNotEmpty() ? source.name : clip.title);
        header.setTimeStamp (0.0);
        sequence.addEvent (header);

        auto tempoEvent = juce::MidiMessage::tempoMetaEvent (tempo);
        tempoEvent.setTimeStamp (0.0);
        sequence.addEvent (tempoEvent);

        auto timeSig = juce::MidiMessage::timeSignatureMetaEvent (clip.beatsPerBar, 4);
        timeSig.setTimeStamp (0.0);
        sequence.addEvent (timeSig);

        for (const auto& note : source.notes)
        {
            const auto channel = juce::jlimit (1, 16, note.channel);
            const auto pitch = juce::jlimit (0, 127, note.pitch);
            const auto velocity = juce::jlimit (1, 127, note.velocity);

            auto on = juce::MidiMessage::noteOn (channel, pitch, (juce::uint8) velocity);
            on.setTimeStamp ((double) juce::jmax (0, note.tick));
            sequence.addEvent (on);

            auto off = juce::MidiMessage::noteOff (channel, pitch);
            off.setTimeStamp ((double) juce::jmax (note.tick + 1, note.endTick()));
            sequence.addEvent (off);
        }

        sequence.updateMatchedPairs();

        // The end-of-track marker HAS to sit after every other event: a marker at
        // tick 0 makes players (and Live) treat the clip as empty, which is exactly
        // what happened before this fix.
        sequence.addEvent (juce::MidiMessage::endOfTrack(), sequence.getEndTime() + 1.0);
        midi.addTrack (sequence);
    }

    return midi;
}

//==============================================================================
bool writeClipToDisk (GeneratedClip& clip, const juce::File& directory, juce::String& error)
{
    if (! directory.isDirectory() && ! directory.createDirectory())
    {
        error = dmb::utf8 ("无法创建输出目录: ") + dmb::displayPath (directory);
        return false;
    }

    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S");
    const auto base = sanitiseFileName (clip.title, "AI") + "_" + stamp;

    auto writeOne = [&clip] (int trackIndex, const juce::File& target) -> bool
    {
        const auto midi = buildMidiFile (clip, trackIndex);

        juce::TemporaryFile temporary (target);

        {
            juce::FileOutputStream stream (temporary.getFile());

            if (! stream.openedOk())
                return false;

            if (! midi.writeTo (stream))
                return false;

            stream.flush();
        }

        return temporary.overwriteTargetFileWithTemporary();
    };

    clip.perTrackFiles.clear();

    const auto combined = directory.getChildFile (base + ".mid");

    if (! writeOne (-1, combined))
    {
        error = dmb::utf8 ("写入 MIDI 文件失败: ") + dmb::displayPath (combined);
        return false;
    }

    clip.file = combined;

    if (clip.tracks.size() > 1)
    {
        int index = 0;

        for (const auto& track : clip.tracks)
        {
            ++index;

            const auto trackName = sanitiseFileName (track.name.isNotEmpty() ? track.name : ("Track" + juce::String (index)),
                                                     "Track" + juce::String (index));
            const auto name = base + "__" + juce::String (index) + "_" + trackName + ".mid";
            const auto file = directory.getChildFile (name);

            if (writeOne (index - 1, file))
                clip.perTrackFiles.push_back (file);
        }
    }

    return true;
}

//==============================================================================
std::vector<SourceTrack> importMidiFile (const juce::File& file, juce::String& error)
{
    std::vector<SourceTrack> result;

    if (! file.existsAsFile())
    {
        error = dmb::utf8 ("文件不存在: ") + dmb::displayPath (file);
        return result;
    }

    juce::MidiFile midi;
    juce::FileInputStream stream (file);

    if (! stream.openedOk() || ! midi.readFrom (stream))
    {
        error = dmb::utf8 ("无法读取 MIDI 文件: ") + dmb::displayPath (file);
        return result;
    }

    int ppq = 960;

    if (midi.getTimeFormat() > 0)
        ppq = juce::jlimit (24, 9600, (int) midi.getTimeFormat());

    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        const auto* sourceSequence = midi.getTrack (t);

        if (sourceSequence == nullptr)
            continue;

        juce::MidiMessageSequence sequenceCopy (*sourceSequence);
        auto* sequence = &sequenceCopy;

        if (sequence == nullptr)
            continue;

        sequence->updateMatchedPairs();

        SourceTrack source;
        source.id = file.getFileNameWithoutExtension() + "_" + juce::String (t);
        source.name = file.getFileNameWithoutExtension();

        if (midi.getNumTracks() > 1)
            source.name += " #" + juce::String (t + 1);

        source.ppq = ppq;
        source.bpm = 120.0;
        source.playing = false;
        source.stampMs = juce::Time::currentTimeMillis();

        double firstTick = 1.0e18;
        double lastTick = 0.0;

        for (int i = 0; i < sequence->getNumEvents(); ++i)
        {
            const auto* holder = sequence->getEventPointer (i);

            if (holder == nullptr)
                continue;

            const auto& message = holder->message;

            if (message.isTempoMetaEvent() && source.bpm == 120.0)
                source.bpm = juce::jlimit (20.0, 400.0, 60.0 / juce::jmax (1.0e-6, message.getTempoSecondsPerQuarterNote()));

            if (! message.isNoteOn())
                continue;

            Note note;
            note.pitch = message.getNoteNumber();
            note.velocity = juce::jlimit (1, 127, (int) message.getVelocity());
            note.channel = juce::jlimit (1, 16, message.getChannel());
            note.tick = (int) std::lround (message.getTimeStamp());

            if (holder->noteOffObject != nullptr)
                note.lengthTicks = (int) std::lround (holder->noteOffObject->message.getTimeStamp() - message.getTimeStamp());
            else
                note.lengthTicks = ppq;

            note.lengthTicks = juce::jlimit (1, 64 * ppq, note.lengthTicks);

            firstTick = juce::jmin (firstTick, (double) note.tick);
            lastTick = juce::jmax (lastTick, (double) note.endTick());

            source.notes.push_back (note);
        }

        if (source.notes.empty())
            continue;

        const auto shift = (int) firstTick;

        for (auto& note : source.notes)
            note.tick -= shift;

        source.firstTick = 0.0;
        source.lastTick = juce::jmax (0.0, lastTick - firstTick);
        source.name += " (" + file.getFileExtension() + ")";

        result.push_back (std::move (source));
    }

    if (result.empty())
        error = dmb::utf8 ("这个 MIDI 文件里没有音符");

    return result;
}

} // namespace dmb
