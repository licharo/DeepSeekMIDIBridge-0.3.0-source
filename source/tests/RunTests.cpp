/*
    Verification suite for the DeepSeek MIDI Bridge core.

    Everything the plugin does, except the host integration itself, is covered
    here: the cross-instance capture bus, the MIDI capture window, the prompt
    builder, the DeepSeek HTTP client (against a local mock server), the JSON ->
    notes parser, the .mid writer / reader, the live playback engine and the whole
    "capture -> API -> .mid file" pipeline.
*/

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <iostream>
#include <thread>

#include "core/ApiProbe.h"
#include "core/CaptureBus.h"
#include "core/DeepSeekClient.h"
#include "core/DiagnosticsLog.h"
#include "core/GenerationService.h"
#include "core/GeneratedMidiPlayer.h"
#include "core/MidiCapture.h"
#include "core/MidiFileUtil.h"
#include "core/MidiJson.h"
#include "core/ProjectStore.h"
#include "core/PromptBuilder.h"
#include "core/Settings.h"
#include "core/SourceLibrary.h"

namespace
{
int g_checks = 0;
int g_failures = 0;
juce::String g_currentTest;
juce::File g_logFile;
std::unique_ptr<juce::FileOutputStream> g_log;

/** Writes a line to the console and to a UTF-8 log file (the Windows console
    mangles non-ASCII text, so the file is the reliable record). */
void out (const juce::String& line)
{
    std::cout << line.toRawUTF8() << std::endl;

    if (g_log != nullptr)
    {
        g_log->writeText (line + "\n", false, false, "\n");
        g_log->flush();
    }
}

juce::String toHex (const juce::String& text)
{
    juce::String out;

    for (auto* p = text.toRawUTF8(); *p != 0; ++p)
        out << juce::String::toHexString ((int) (juce::uint8) *p).paddedLeft ('0', 2) << " ";

    return out;
}

void check (bool ok, const juce::String& what)
{
    ++g_checks;

    if (ok)
    {
        out ("    ok   " + what);
    }
    else
    {
        ++g_failures;
        out ("    FAIL " + what + "   (" + g_currentTest + ")");
    }
}

void beginTest (const juce::String& name)
{
    g_currentTest = name;
    out ("\n== " + name);
}

juce::File makeTempDirectory (const juce::String& name)
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("dmb_tests")
                   .getChildFile (name);

    dir.deleteRecursively();
    dir.createDirectory();

    return dir;
}

/** Unpacks a plugin state block (the same layout AudioProcessor::copyXmlToBinary
    writes: two ints, then the XML text). */
std::unique_ptr<juce::XmlElement> parseStateBlock (const juce::MemoryBlock& block)
{
    if (block.getSize() <= 8)
        return {};

    return juce::parseXML (juce::String::fromUTF8 (static_cast<const char*> (block.getData()) + 8,
                                                   (int) block.getSize() - 8));
}

/** The state a *hosted* VST3 instance hands out wraps the plugin's own block in a
    <VST3PluginState> xml with the component state base64 encoded inside - unwrap it
    until the plugin's own state block is reached. */
juce::MemoryBlock unwrapHostedState (const juce::MemoryBlock& block)
{
    auto xml = parseStateBlock (block);

    if (xml == nullptr)
        return block;

    for (auto* child = xml->getFirstChildElement(); child != nullptr; child = child->getNextElement())
    {
        juce::MemoryBlock inner;

        if (child->getTagName() == "IComponent" && inner.fromBase64Encoding (child->getAllSubText()))
            return inner;
    }

    return block;
}

/** Reads one string property out of a plugin state block's "dmbState" json. */
juce::String projectPropertyFromState (const juce::MemoryBlock& block, const juce::String& property)
{
    auto xml = parseStateBlock (unwrapHostedState (block));

    if (xml == nullptr)
        return {};

    juce::var parsed;

    if (! juce::JSON::parse (xml->getStringAttribute ("dmbState"), parsed).wasOk())
        return {};

    if (auto* root = parsed.getDynamicObject())
        return root->getProperty (property).toString();

    return {};
}

//==============================================================================
dmb::SourceTrack makeSource (const juce::String& id, const juce::String& name, int numNotes, int ppq = 960)
{
    dmb::SourceTrack source;
    source.id = id;
    source.name = name;
    source.ppq = ppq;
    source.bpm = 128.0;
    source.playing = true;
    source.firstTick = 0.0;

    for (int i = 0; i < numNotes; ++i)
    {
        dmb::Note note;
        note.tick = i * (ppq / 4);
        note.pitch = 36 + (i % 12);
        note.velocity = 80 + (i % 30);
        note.channel = 1;
        note.lengthTicks = ppq / 8;
        source.notes.push_back (note);
    }

    source.lastTick = source.notes.empty() ? 0.0 : (double) source.notes.back().endTick();
    return source;
}

//==============================================================================
void testCaptureBus()
{
    beginTest ("CaptureBus: publish / read / ignore self / atomic replace");

    const auto dir = makeTempDirectory ("bus");
    dmb::CaptureBus::setRootDirectory (dir);

    auto a = makeSource ("aaaa1111", dmb::utf8 ("鼓组轨"), 64);
    auto b = makeSource ("bbbb2222", juce::String::fromUTF8 (u8"贝斯 🎸"), 33);
    auto c = makeSource ("cccc3333", "Pad", 128);

    dmb::CaptureBus::publish (a);
    dmb::CaptureBus::publish (b);
    dmb::CaptureBus::publish (c);

    auto all = dmb::CaptureBus::readAll (60000, {});
    check ((int) all.size() == 3, dmb::utf8 ("三个源都被读到"));

    auto others = dmb::CaptureBus::readAll (60000, "bbbb2222");
    check ((int) others.size() == 2, dmb::utf8 ("可以忽略自己这个实例"));

    bool foundBass = false;

    for (const auto& source : all)
    {
        if (source.id == "bbbb2222")
        {
            foundBass = true;
            check (source.name == juce::String::fromUTF8 (u8"贝斯 🎸"),
                   dmb::utf8 ("unicode 名称往返正常  got=[") + source.name + "] hex=[" + toHex (source.name) + "]");
            check ((int) source.notes.size() == 33, dmb::utf8 ("音符数量往返正常"));
            check (std::abs (source.bpm - 128.0) < 0.001, dmb::utf8 ("bpm 往返正常"));
            check (source.notes.front().pitch == 36, dmb::utf8 ("音符内容往返正常"));
            check (source.notes.back().tick == 32 * 240, dmb::utf8 ("tick 位置往返正常"));
        }
    }

    check (foundBass, dmb::utf8 ("找到了贝斯轨"));

    // republish with new content: exactly one file, new content
    b.notes.resize (5);
    dmb::CaptureBus::publish (b);

    auto files = dir.findChildFiles (juce::File::findFiles, false, "*.dmb");
    check (files.size() == 3, dmb::utf8 ("重复发布不会产生多余文件 (原子替换)"));

    auto reread = dmb::CaptureBus::readAll (60000, {});
    int bassNotes = -1;

    for (const auto& source : reread)
        if (source.id == "bbbb2222")
            bassNotes = (int) source.notes.size();

    check (bassNotes == 5, dmb::utf8 ("重复发布后读到的是新内容"));

    auto stale = dmb::CaptureBus::readAll (1, {});
    check (stale.size() <= 3, dmb::utf8 ("过期过滤不会崩溃"));

    dmb::CaptureBus::remove ("cccc3333");
    auto afterRemove = dmb::CaptureBus::readAll (60000, {});
    check ((int) afterRemove.size() == 2, dmb::utf8 ("remove() 会删除对应条目"));

    // a corrupted file must be ignored, not crash
    dir.getChildFile ("broken.dmb").replaceWithText ("this is not a bus file");
    auto withBroken = dmb::CaptureBus::readAll (60000, {});
    check ((int) withBroken.size() == 2, dmb::utf8 ("损坏的文件被忽略"));
}

//==============================================================================
void testMidiCapture()
{
    beginTest ("MidiCapture: capture MIDI, pair note offs, window, transport restart");

    dmb::MidiCapture capture;
    capture.prepare (48000.0);
    capture.setWindowBars (2);
    capture.setBeatsPerBar (4);

    const double bpm = 120.0;
    const int blockSize = 512;
    const double blocksPerQuarter = (60.0 / bpm) * 48000.0 / blockSize;   // blocks per beat

    double ppq = 0.0;

    auto runBlock = [&] (int blockIndex, const juce::MidiBuffer& midi)
    {
        capture.processBlock (midi, blockSize, true, true, ppq, bpm);
        ppq += (double) blockSize / 48000.0 * (bpm / 60.0);
        juce::ignoreUnused (blockIndex);
    };

    juce::MidiBuffer first;
    first.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
    first.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 90), 10);
    runBlock (0, first);

    juce::MidiBuffer offs;
    offs.addEvent (juce::MidiMessage::noteOff (1, 60), 100);
    offs.addEvent (juce::MidiMessage::noteOff (1, 64), 200);
    runBlock (1, offs);

    for (int i = 0; i < 40; ++i)
        runBlock (i + 2, {});

    check (capture.update(), dmb::utf8 ("update() 报告有新事件"));
    auto snapshot = capture.snapshot ("test", dmb::utf8 ("本机"));

    check ((int) snapshot.notes.size() == 2, dmb::utf8 ("两个音符被捕获并配对"));
    check (snapshot.notes[0].pitch == 60, dmb::utf8 ("第一个音符音高正确"));
    check (snapshot.notes[0].lengthTicks > 0, dmb::utf8 ("音符时长为正数"));
    check (snapshot.notes[0].lengthTicks < 960 * 2, dmb::utf8 ("音符时长没有失控"));
    check (snapshot.notes[0].tick >= 0 && snapshot.notes[1].tick >= snapshot.notes[0].tick, dmb::utf8 ("音符按时间排序"));
    check (snapshot.name == dmb::utf8 ("本机"), dmb::utf8 ("snapshot 保留了名称  got=[") + snapshot.name + "] hex=[" + toHex (snapshot.name)
                                     + "] expected=[" + toHex (dmb::utf8 ("本机")) + "]");

    // window: move far ahead, notes should be trimmed away
    for (int i = 0; i < 4000; ++i)
        runBlock (100 + i, {});

    capture.update();
    auto trimmed = capture.snapshot ("test", dmb::utf8 ("本机"));
    check ((int) trimmed.notes.size() == 0, dmb::utf8 ("超出捕获窗口的音符被裁掉"));

    // transport restart clears the take
    juce::MidiBuffer note;
    note.addEvent (juce::MidiMessage::noteOn (2, 40, (juce::uint8) 100), 0);
    runBlock (600, note);

    capture.update();
    ppq = 0.0;   // host restarted / looped back
    juce::MidiBuffer nothing;
    runBlock (601, nothing);
    juce::MidiBuffer offAfterLoop;
    offAfterLoop.addEvent (juce::MidiMessage::noteOff (2, 40), 8);
    runBlock (602, offAfterLoop);

    capture.update();
    auto afterRestart = capture.snapshot ("test", dmb::utf8 ("本机"));
    check ((int) afterRestart.notes.size() <= 2, dmb::utf8 ("走带重启后不会留下错误的旧音符"));

    check (capture.getTotalEvents() > 0, dmb::utf8 ("事件计数器在工作"));
}

//==============================================================================
void testJsonParsing()
{
    beginTest ("MidiJson: parsing model replies");

    int failuresBefore = g_failures;

    {
        const juce::String json = juce::String::fromUTF8 (u8R"({
            "name": "dnb drums",
            "explanation": "切分鼓组",
            "bpm": 174,
            "bars": 4,
            "unit": "beats",
            "notes": [
                {"start": 0.0, "pitch": 36, "length": 0.25, "velocity": 120},
                {"start": 0.5, "pitch": 38, "length": 0.25, "velocity": 100},
                {"start": 1, "pitch": 42, "length": 0.125, "velocity": 80}
            ]
        })");

        dmb::GeneratedClip clip;
        juce::String error;
        const bool ok = dmb::parseGenerationResult (json, 960, 8, 4, 1, 0, clip, error);
        check (ok, dmb::utf8 ("beats 格式解析成功"));
        check (clip.tracks.size() == 1, dmb::utf8 ("单轨结果"));
        check (clip.totalNotes() == 3, dmb::utf8 ("三个音符"));
        check (clip.bpm == 174.0, dmb::utf8 ("bpm 解析"));
        check (clip.ppq == 960, dmb::utf8 ("默认 ppq"));
        check (! clip.tracks.empty() && clip.tracks[0].notes[1].tick == 480, dmb::utf8 ("start 拍 -> tick 换算正确"));
        check (! clip.tracks.empty() && clip.tracks[0].notes[2].lengthTicks == 120, dmb::utf8 ("length 拍 -> tick 换算正确"));
        check (clip.title == "dnb drums", dmb::utf8 ("标题解析"));
    }

    {
        const juce::String json = R"({"title":"t","unit":"ticks","ppq":480,
            "notes":[{"start":240,"pitch":60,"length":120,"velocity":90}]})";

        dmb::GeneratedClip clip;
        juce::String error;
        check (dmb::parseGenerationResult (json, 960, 4, 4, 1, 0, clip, error), dmb::utf8 ("ticks 单位解析成功"));
        check (clip.ppq == 480, dmb::utf8 ("ppq 字段被采用"));
        check (clip.tracks[0].notes[0].tick == 240, dmb::utf8 ("tick 值原样保留"));
        check (clip.tracks[0].notes[0].lengthTicks == 120, dmb::utf8 ("length tick 原样保留"));
    }

    {
        const juce::String json = R"({"name":"steps","unit":"steps","bars":2,
            "notes":[{"start":4,"pitch":50,"length":2,"velocity":70}]})";

        dmb::GeneratedClip clip;
        juce::String error;
        check (dmb::parseGenerationResult (json, 960, 4, 4, 1, 0, clip, error), dmb::utf8 ("steps 单位解析成功"));
        check (clip.tracks[0].notes[0].tick == 960, dmb::utf8 ("step(十六分) -> tick 换算正确"));
        check (clip.tracks[0].notes[0].lengthTicks == 480, dmb::utf8 ("step 时值换算正确"));
    }

    {
        const juce::String json = R"({"name":"bars","unit":"beats","bars":2,
            "notes":[{"start":"1:2:0","pitch":48,"length":"0:1:0","velocity":64}]})";

        dmb::GeneratedClip clip;
        juce::String error;
        check (dmb::parseGenerationResult (json, 960, 4, 4, 1, 0, clip, error), dmb::utf8 ("bar:beat:sixteenth 位置解析成功"));
        check (clip.tracks[0].notes[0].tick == 6 * 960, dmb::utf8 ("小节:拍:十六分 -> tick 正确"));
        check (clip.tracks[0].notes[0].lengthTicks == 960, dmb::utf8 ("时值字符串解析正确"));
    }

    {
        const juce::String json = R"({"name":"multi","bars":4,"bpm":100,
            "tracks":[
              {"name":"Drums","channel":10,"notes":[{"start":0,"pitch":36,"length":0.25,"velocity":127}]},
              {"name":"Bass","channel":2,"notes":[{"start":0.5,"pitch":40,"length":0.5,"velocity":90}]}
            ]})";

        dmb::GeneratedClip clip;
        juce::String error;
        check (dmb::parseGenerationResult (json, 960, 4, 4, 1, 0, clip, error), dmb::utf8 ("多轨结果解析成功"));
        check (clip.tracks.size() == 2, dmb::utf8 ("两条轨"));
        check (clip.tracks[0].channel == 10, dmb::utf8 ("通道被保留"));
        check (clip.tracks[1].notes[0].pitch == 40, dmb::utf8 ("第二轨音符正确"));
    }

    {
        const juce::String fenced = dmb::utf8 ("好的，这是你要的鼓组：\n```json\n{\"name\":\"fenced\",\"notes\":"
                                    "[{\"start\":0,\"pitch\":36,\"length\":0.25,\"velocity\":100}]}\n```\n希望喜欢！");

        dmb::GeneratedClip clip;
        juce::String error;
        check (dmb::parseGenerationResult (fenced, 960, 4, 4, 1, 0, clip, error), dmb::utf8 ("带 markdown 包裹的回复仍能解析"));
        check (clip.title == "fenced", dmb::utf8 ("标题正确"));
    }

    {
        // every pitch out of range -> the note is dropped
        const juce::String json = R"({"name":"clamp","notes":[
            {"start":-5,"pitch":200,"length":0,"velocity":999}]})";

        dmb::GeneratedClip clip;
        juce::String error;
        check (! dmb::parseGenerationResult (json, 960, 4, 4, 1, 0, clip, error), dmb::utf8 ("越界音高被丢弃"));
    }

    {
        const juce::String json = R"({"name":"clamp2","notes":[
            {"start":-5,"pitch":60,"length":0,"velocity":999},
            {"start":0,"pitch":61,"length":100000,"velocity":0}]})";

        dmb::GeneratedClip clip;
        juce::String error;
        check (dmb::parseGenerationResult (json, 960, 4, 4, 1, 0, clip, error), dmb::utf8 ("越界数值仍可解析"));
        check (clip.tracks.size() == 1 && clip.tracks[0].notes.size() == 2, dmb::utf8 ("两个音符都保留"));
        check (! clip.tracks.empty() && clip.tracks[0].notes[0].tick == 0, dmb::utf8 ("负 tick 被夹到 0"));
        check (! clip.tracks.empty() && clip.tracks[0].notes[0].velocity == 127, dmb::utf8 ("过大的力度被夹到 127"));
        check (! clip.tracks.empty() && clip.tracks[0].notes[1].velocity == 1, dmb::utf8 ("0 力度被夹到 1"));
        check (! clip.tracks.empty() && clip.tracks[0].notes[1].lengthTicks <= 64 * 960, dmb::utf8 ("超长时值被夹紧"));
    }

    {
        const juce::String json = R"({"name":"tr","notes":[{"start":0,"pitch":60,"length":0.5,"velocity":100}]})";

        dmb::GeneratedClip clip;
        juce::String error;
        check (dmb::parseGenerationResult (json, 960, 4, 4, 1, 12, clip, error), dmb::utf8 ("移调解析成功"));
        check (clip.tracks[0].notes[0].pitch == 72, dmb::utf8 ("移调 +12 生效"));
    }

    {
        dmb::GeneratedClip clip;
        juce::String error;
        check (! dmb::parseGenerationResult (dmb::utf8 ("这不是 JSON"), 960, 4, 4, 1, 0, clip, error), dmb::utf8 ("非 JSON 被拒绝"));
        check (error.isNotEmpty(), dmb::utf8 ("给出了错误说明"));

        check (! dmb::parseGenerationResult (R"({"name":"empty","notes":[]})", 960, 4, 4, 1, 0, clip, error),
               dmb::utf8 ("空 notes 被拒绝"));

        check (! dmb::parseGenerationResult ("", 960, 4, 4, 1, 0, clip, error), dmb::utf8 ("空字符串被拒绝"));
    }

    // serialisation round trip (used for saving the clip inside the host project)
    {
        auto clip = dmb::GeneratedClip();
        clip.title = "roundtrip";
        clip.ppq = 960;
        clip.bpm = 123.0;
        clip.bars = 3;
        clip.beatsPerBar = 4;
        dmb::GeneratedTrack track;
        track.name = "Bass";
        track.channel = 3;
        dmb::Note note;
        note.tick = 240;
        note.pitch = 41;
        note.velocity = 99;
        note.lengthTicks = 120;
        note.channel = 3;
        track.notes.push_back (note);
        clip.tracks.push_back (track);

        dmb::GeneratedClip restored;
        check (dmb::generatedClipFromVar (dmb::generatedClipToVar (clip), restored), dmb::utf8 ("clip 序列化往返成功"));
        check (restored.title == "roundtrip" && restored.totalNotes() == 1, dmb::utf8 ("往返内容一致"));
        check (restored.tracks[0].notes[0].pitch == 41, dmb::utf8 ("往返音符一致"));
    }

    check (g_failures == failuresBefore, dmb::utf8 ("解析测试全部通过"));
}

//==============================================================================
void testMidiFiles()
{
    beginTest ("MidiFileUtil: writing, splitting and re-importing .mid files");

    auto dir = makeTempDirectory ("midi");

    dmb::GeneratedClip clip;
    clip.title = dmb::utf8 ("测试 Drum & Bass / 鼓组");
    clip.ppq = 960;
    clip.bpm = 174.0;
    clip.bars = 2;
    clip.beatsPerBar = 4;

    for (int t = 0; t < 2; ++t)
    {
        dmb::GeneratedTrack track;
        track.name = (t == 0 ? "Drums" : "Bass");
        track.channel = (t == 0 ? 10 : 2);

        for (int i = 0; i < 8; ++i)
        {
            dmb::Note note;
            note.tick = i * 240;
            note.pitch = 36 + i + t * 12;
            note.velocity = 100;
            note.lengthTicks = 120;
            note.channel = track.channel;
            track.notes.push_back (note);
        }

        clip.tracks.push_back (track);
    }

    juce::String error;
    check (dmb::writeClipToDisk (clip, dir, error), dmb::utf8 ("写盘成功: ") + error);
    check (clip.file.existsAsFile(), dmb::utf8 ("合并文件存在"));
    check (clip.file.getFileName().contains ("Drum") || clip.file.getFileName().contains ("_"), dmb::utf8 ("文件名被清洗过"));
    check (clip.perTrackFiles.size() == 2, dmb::utf8 ("多轨时额外写出每轨文件"));

    juce::MidiFile midi;
    juce::FileInputStream stream (clip.file);
    check (stream.openedOk() && midi.readFrom (stream), dmb::utf8 ("写出的文件能被读回"));
    check (midi.getNumTracks() == 2, dmb::utf8 ("包含两条 MIDI 轨"));
    check (midi.getTimeFormat() == 960, dmb::utf8 ("ppq 正确"));

    int noteCount = 0;

    for (int t = 0; t < midi.getNumTracks(); ++t)
    {
        auto* sequence = midi.getTrack (t);

        for (int i = 0; i < sequence->getNumEvents(); ++i)
            if (sequence->getEventPointer (i)->message.isNoteOn())
                ++noteCount;
    }

    check (noteCount == 16, dmb::utf8 ("16 个音符全部写入"));

    // ---- strict checks: a real player stops at the first end-of-track marker ----
    {
        int allNoteOns = 0;
        int allNoteOffs = 0;
        bool endOfTrackIsLast = true;
        bool endOfTrackIsLatest = true;

        for (int t = 0; t < midi.getNumTracks(); ++t)
        {
            auto* sequence = midi.getTrack (t);
            const auto numEvents = sequence->getNumEvents();

            for (int i = 0; i < numEvents; ++i)
            {
                const auto& message = sequence->getEventPointer (i)->message;

                if (message.isNoteOn())  ++allNoteOns;
                if (message.isNoteOff()) ++allNoteOffs;

                if (message.isEndOfTrackMetaEvent() && i != numEvents - 1)
                    endOfTrackIsLast = false;
            }

            const auto* last = numEvents > 0 ? sequence->getEventPointer (numEvents - 1) : nullptr;

            if (last == nullptr || ! last->message.isEndOfTrackMetaEvent())
            {
                endOfTrackIsLast = false;
            }
            else
            {
                for (int i = 0; i < numEvents; ++i)
                    if (sequence->getEventPointer (i)->message.getTimeStamp() > last->message.getTimeStamp())
                        endOfTrackIsLatest = false;
            }
        }

        check (endOfTrackIsLast, dmb::utf8 ("end-of-track 必须是每轨最后一个事件（否则播放器会当成空文件）"));
        check (endOfTrackIsLatest, dmb::utf8 ("end-of-track 的时间戳必须晚于所有音符"));
        check (allNoteOns == allNoteOffs, dmb::utf8 ("note on / note off 数量相等 (")
                                            + juce::String (allNoteOns) + " / " + juce::String (allNoteOffs) + ")");

        // and at the raw byte level: the file must end with the end-of-track marker
        juce::MemoryBlock raw;

        if (clip.file.loadFileAsData (raw))
        {
            const auto* bytes = static_cast<const juce::uint8*> (raw.getData());
            const auto size = (int) raw.getSize();
            check (size > 3 && bytes[size - 3] == 0xff && bytes[size - 2] == 0x2f && bytes[size - 1] == 0x00,
                   dmb::utf8 ("文件最后三个字节是 FF 2F 00 (end of track)"));
        }
        else
        {
            check (false, dmb::utf8 ("读不回刚写的文件"));
        }
    }

    auto imported = dmb::importMidiFile (clip.file, error);
    check (imported.size() == 2, dmb::utf8 ("重新导入得到两条轨"));
    check (! imported.empty() && imported[0].notes.size() == 8, dmb::utf8 ("重新导入的音符数量正确"));

    if (imported.size() >= 2)
    {
        check (imported[0].notes[0].pitch == 36, dmb::utf8 ("重新导入的音高正确"));

        bool foundLength = false;

        for (const auto& note : imported[0].notes)
            if (note.lengthTicks == 120)
                foundLength = true;

        check (foundLength, dmb::utf8 ("重新导入的时值正确 (120 ticks)"));

        auto bass = dmb::importMidiFile (clip.perTrackFiles[1], error);
        check (! bass.empty() && bass[0].notes[0].pitch == 48, dmb::utf8 ("每轨单独文件内容正确"));
    }

    check (dmb::sanitiseFileName ("a/b\\c:d*e?f\"g<h>i|j") == "a_b_c_d_e_f_g_h_i_j", dmb::utf8 ("文件名清洗正确"));
    check (dmb::sanitiseFileName ("!!!").isNotEmpty(), dmb::utf8 ("全非法字符时给出兜底文件名"));
    check (dmb::sanitiseFileName ("").isNotEmpty(), dmb::utf8 ("空文件名时给出兜底"));
}

//==============================================================================
void testPromptBuilder()
{
    beginTest ("PromptBuilder: prompt construction");

    dmb::GenerationSpec spec;
    spec.bars = 8;
    spec.beatsPerBar = 4;
    spec.maxNotes = 500;
    spec.style = "Drum & Bass";
    spec.instrument = dmb::utf8 ("鼓组");
    spec.key = "F# minor";

    auto source = makeSource ("id1", dmb::utf8 ("贝斯轨"), 400);

    const auto system = dmb::buildSystemPrompt (spec);
    const auto user = dmb::buildUserPrompt ({ source }, spec, dmb::utf8 ("写一段鼓组"));

    check (system.contains ("JSON"), dmb::utf8 ("system 提示词要求 JSON"));
    check (system.contains ("tracks"), dmb::utf8 ("system 提示词描述了 tracks 结构"));
    check (system.contains ("Drum & Bass"), dmb::utf8 ("风格被写入提示词"));
    check (system.contains ("F# minor"), dmb::utf8 ("调性被写入提示词"));
    check (user.contains (dmb::utf8 ("贝斯轨")), dmb::utf8 ("user 提示词包含源轨名称"));
    check (user.contains (dmb::utf8 ("写一段鼓组")), dmb::utf8 ("user 提示词包含用户要求"));
    check (user.contains ("8"), dmb::utf8 ("user 提示词包含小节数"));
    check (user.length() < 40000, dmb::utf8 ("提示词长度可控 (实际 ") + juce::String (user.length()) + ")");

    const auto formatted = dmb::formatSourceNotes (source, 100);
    check (formatted.contains (":"), dmb::utf8 ("音符格式为 start:pitch:length:velocity"));
    check (formatted.length() < 4000, dmb::utf8 ("抽样后长度受限"));

    auto second = makeSource ("id2", dmb::utf8 ("鼓组轨"), 10);
    const auto multi = dmb::buildUserPrompt ({ source, second }, spec, dmb::utf8 ("融合一下"));
    check (multi.contains (dmb::utf8 ("贝斯轨")) && multi.contains (dmb::utf8 ("鼓组轨")), dmb::utf8 ("多轨素材都出现在提示词中"));

    // ready-made requests (used by the "常用要求" menu in the plugin)
    {
        const auto templates = dmb::requestTemplates();
        check (templates.size() >= 5, "预置要求至少有 5 条 (实际 " + juce::String ((int) templates.size()) + ")");

        bool allGood = true;
        bool hasDrums = false, hasBass = false;

        for (const auto& item : templates)
        {
            if (item.label.isEmpty() || item.prompt.isEmpty() || item.bars < 1)
                allGood = false;

            if (item.instrument.contains (dmb::utf8 ("鼓")) || item.prompt.contains ("kick"))
                hasDrums = true;

            if (item.instrument.contains (dmb::utf8 ("贝斯")))
                hasBass = true;
        }

        check (allGood, "每条预置要求都有名称/文案/小节数");
        check (hasDrums, "预置里包含鼓组类要求");
        check (hasBass, "预置里包含贝斯类要求");

        // the templates must produce a sane spec for the prompt builder
        dmb::GenerationSpec spec;
        spec.bars = templates.front().bars;
        spec.style = templates.front().style;
        spec.instrument = templates.front().instrument;
        const auto built = dmb::buildUserPrompt ({ source }, spec, templates.front().prompt);
        check (built.contains (templates.front().prompt.substring (0, 8)), "预置要求会被带进提示词");
    }

    const auto summary = dmb::summariseSources ({ source, second });
    check (summary.contains (dmb::utf8 ("贝斯轨")) && summary.contains ("400"), dmb::utf8 ("摘要包含音符数"));
}

//==============================================================================
/** A tiny HTTP server used to exercise the client without the real API. */
class MockServer
{
public:
    MockServer (juce::String responseBody, int status, bool trickle = false)
        : body (std::move (responseBody)), statusCode (status), slow (trickle)
    {
    }

    ~MockServer() { stop(); }

    bool start()
    {
        if (! listener.createListener (0, "127.0.0.1"))
            return false;

        port = listener.getBoundPort();
        thread = std::thread ([this] { serve(); });
        return true;
    }

    void stop()
    {
        listener.close();

        if (thread.joinable())
            thread.join();
    }

    int getPort() const { return port; }

    juce::String getReceivedRequest() const
    {
        const juce::ScopedLock sl (lock);
        return request;
    }

private:
    void serve()
    {
        std::unique_ptr<juce::StreamingSocket> connection (listener.waitForNextConnection());

        if (connection == nullptr)
            return;

        juce::String headerText;
        char buffer[2048];

        while (! headerText.contains ("\r\n\r\n"))
        {
            if (! connection->waitUntilReady (true, 5000))
                break;

            const auto numRead = connection->read (buffer, (int) sizeof (buffer) - 1, false);

            if (numRead <= 0)
                break;

            headerText += juce::String::fromUTF8 (buffer, numRead);
        }

        int contentLength = 0;

        for (const auto& line : juce::StringArray::fromLines (headerText))
            if (line.startsWithIgnoreCase ("content-length:"))
                contentLength = line.fromFirstOccurrenceOf (":", false, false).trim().getIntValue();

        auto headerEnd = headerText.indexOf ("\r\n\r\n");
        juce::String bodyText = headerEnd >= 0 ? headerText.substring (headerEnd + 4) : juce::String();

        while (bodyText.getNumBytesAsUTF8() < contentLength)
        {
            if (! connection->waitUntilReady (true, 5000))
                break;

            const auto numRead = connection->read (buffer, (int) sizeof (buffer) - 1, false);

            if (numRead <= 0)
                break;

            bodyText += juce::String::fromUTF8 (buffer, numRead);
        }

        {
            const juce::ScopedLock sl (lock);
            request = headerText + bodyText;
        }

        const auto statusText = juce::String (statusCode) + (statusCode == 200 ? " OK" : " Error");

        juce::String response = "HTTP/1.1 " + statusText + "\r\n"
                                "Content-Type: application/json\r\n"
                                "Content-Length: " + juce::String (body.getNumBytesAsUTF8()) + "\r\n"
                                "Connection: close\r\n\r\n" + body;

        if (slow)
        {
            // send the head only, then wait for the client to give up
            const auto head = response.upToFirstOccurrenceOf ("\r\n\r\n", true, false);
            connection->write (head.toRawUTF8(), head.getNumBytesAsUTF8());
            juce::Thread::sleep (2000);
            return;
        }

        connection->write (response.toRawUTF8(), response.getNumBytesAsUTF8());
        connection->close();
    }

    juce::StreamingSocket listener;
    std::thread thread;
    juce::String body;
    int statusCode = 200;
    bool slow = false;
    int port = 0;

    mutable juce::CriticalSection lock;
    juce::String request;
};

//==============================================================================
void testDeepSeekClient()
{
    beginTest ("DeepSeekClient: request shape, success, error and cancel paths");

    const juce::String successBody = R"({
        "id": "chat-1",
        "choices": [{"index":0,"finish_reason":"stop","message":{"role":"assistant",
            "content":"{\"name\":\"mocked\",\"bpm\":128,\"bars\":2,\"unit\":\"beats\",
            \"notes\":[{\"start\":0,\"pitch\":36,\"length\":0.25,\"velocity\":110}]}"}}],
        "usage": {"prompt_tokens": 120, "completion_tokens": 40, "total_tokens": 160}
    })";

    {
        MockServer server (successBody, 200);
        check (server.start(), dmb::utf8 ("mock 服务器启动"));

        dmb::ApiSettings api;
        api.endpoint = "http://127.0.0.1:" + juce::String (server.getPort()) + "/chat/completions";
        api.apiKey = "test-key-123";
        api.model = "deepseek-chat";
        api.timeoutMs = 15000;

        const auto result = dmb::DeepSeekClient::call (api, "SYSTEM-PROMPT", "USER-PROMPT", 0.7);

        check (result.ok, dmb::utf8 ("HTTP 200 请求成功: ") + result.error);
        check (result.statusCode == 200, dmb::utf8 ("状态码为 200"));
        check (result.content.contains ("mocked"), dmb::utf8 ("取回了 assistant content"));
        check ((int) result.usage.getDynamicObject()->getProperty ("total_tokens") == 160, dmb::utf8 ("解析出 token 用量"));

        const auto request = server.getReceivedRequest();
        out ("    info 收到请求 " + juce::String (request.length()) + " 字符, body 片段: "
             + request.fromLastOccurrenceOf ("\r\n\r\n", false, false).substring (0, 420));
        check (request.contains ("POST /chat/completions") || request.contains ("POST /chat/completions HTTP"),
               dmb::utf8 ("使用了 POST 与正确的路径"));
        check (request.contains ("Authorization: Bearer test-key-123"), dmb::utf8 ("带上了 API Key"));
        check (request.contains ("application/json"), dmb::utf8 ("Content-Type 为 application/json"));
        check (request.contains ("\"model\":\"deepseek-chat\""), dmb::utf8 ("请求体包含模型名"));
        check (request.contains ("SYSTEM-PROMPT") && request.contains ("USER-PROMPT"), dmb::utf8 ("请求体包含两条消息"));
        check (request.contains ("json_object"), dmb::utf8 ("请求体要求 JSON 输出"));
        check (request.contains ("\"temperature\":0.7"), dmb::utf8 ("请求体包含 temperature"));

        dmb::GeneratedClip clip;
        juce::String error;
        check (dmb::parseGenerationResult (result.content, 960, 8, 4, 1, 0, clip, error),
               dmb::utf8 ("mock 返回的内容可以解析成音符"));
        check (clip.tracks[0].notes[0].pitch == 36, dmb::utf8 ("解析出的音高正确"));

        server.stop();
    }

    {
        MockServer server (R"({"error":{"message":"Authentication Fails","type":"authentication_error"}})", 401);
        check (server.start(), dmb::utf8 ("错误场景 mock 服务器启动"));

        dmb::ApiSettings api;
        api.endpoint = "http://127.0.0.1:" + juce::String (server.getPort()) + "/chat/completions";
        api.apiKey = "bad-key";

        const auto result = dmb::DeepSeekClient::call (api, "s", "u", 0.7);
        check (! result.ok, dmb::utf8 ("401 被判定为失败"));
        check (result.statusCode == 401, dmb::utf8 ("状态码 401"));
        check (result.error.contains ("Authentication"), dmb::utf8 ("错误信息来自服务器: ") + result.error);

        server.stop();
    }

    {
        MockServer server (successBody, 200);
        check (server.start(), dmb::utf8 ("取消场景 mock 服务器启动"));

        dmb::ApiSettings api;
        api.endpoint = "http://127.0.0.1:" + juce::String (server.getPort()) + "/chat/completions";
        api.apiKey = "k";

        const auto result = dmb::DeepSeekClient::call (api, "s", "u", 0.7, [] { return true; });
        check (! result.ok, dmb::utf8 ("取消后不会返回成功"));
        check (result.error.contains (juce::String::fromUTF8 (u8"取消")), dmb::utf8 ("取消有明确说明: ") + result.error);

        server.stop();
    }

    {
        dmb::ApiSettings api;
        api.apiKey = {};
        const auto result = dmb::DeepSeekClient::call (api, "s", "u", 0.7);
        check (! result.ok, dmb::utf8 ("缺少 API Key 时直接失败"));
        check (result.error.contains ("API Key"), dmb::utf8 ("提示缺少 API Key"));
    }

    {
        dmb::ApiSettings api;
        api.apiKey = "k";
        api.endpoint = "http://127.0.0.1:1/chat/completions";   // nothing listens here
        api.timeoutMs = 5000;
        const auto result = dmb::DeepSeekClient::call (api, "s", "u", 0.7);
        check (! result.ok, dmb::utf8 ("连接失败时返回失败而不是崩溃"));
        check (result.error.isNotEmpty(), dmb::utf8 ("连接失败有错误信息"));
    }
}

//==============================================================================
void testGenerationPipeline()
{
    beginTest ("Pipeline: capture -> prompt -> HTTP -> parse -> .mid files");

    auto outputDir = makeTempDirectory ("pipeline");

    const juce::String responseBody = juce::String::fromUTF8 (u8R"({
        "choices": [{"finish_reason":"stop","message":{"content":
          "{\"name\":\"AI 鼓组\",\"explanation\":\"跟进贝斯的切分\",\"bpm\":174,\"bars\":4,\"unit\":\"beats\",
            \"tracks\":[{\"name\":\"Drums\",\"channel\":10,\"notes\":[
              {\"start\":0,\"pitch\":36,\"length\":0.25,\"velocity\":120},
              {\"start\":0.5,\"pitch\":38,\"length\":0.25,\"velocity\":110},
              {\"start\":1.0,\"pitch\":42,\"length\":0.125,\"velocity\":80}]}]}"}}],
        "usage": {"prompt_tokens": 300, "completion_tokens": 90, "total_tokens": 390}
    })");

    MockServer server (responseBody, 200);
    check (server.start(), dmb::utf8 ("mock 服务器启动"));

    dmb::ApiSettings api;
    api.endpoint = "http://127.0.0.1:" + juce::String (server.getPort()) + "/chat/completions";
    api.apiKey = "pipeline-key";
    api.model = "deepseek-chat";

    dmb::GenerationSpec spec;
    spec.bars = 4;
    spec.beatsPerBar = 4;
    spec.temperature = 0.8;
    spec.maxNotes = 400;
    spec.channel = 1;

    auto source = makeSource ("bass", juce::String::fromUTF8 (u8"贝斯轨"), 64);

    dmb::GenerationService service;
    service.start (api, spec, dmb::utf8 ("根据贝斯写一段鼓组"), { source }, outputDir);

    int guard = 0;

    while (service.isRunning() && guard++ < 300)
        juce::Thread::sleep (50);

    check (! service.isRunning(), dmb::utf8 ("生成线程结束"));
    check (service.getState() == dmb::GenerationService::State::done,
           dmb::utf8 ("状态为完成 (实际: ") + dmb::GenerationService::stateToString (service.getState()) + ")");

    dmb::GeneratedClip clip;
    check (service.takeResult (clip), dmb::utf8 ("取到了结果"));
    check (clip.totalNotes() == 3, dmb::utf8 ("结果包含 3 个音符"));
    check (clip.bars == 4, dmb::utf8 ("小节数正确"));
    check (clip.file.existsAsFile(), dmb::utf8 (".mid 文件已经写到磁盘"));
    check (clip.title.contains (juce::String::fromUTF8 (u8"鼓组")) || clip.title.contains ("AI"), dmb::utf8 ("标题来自模型"));

    check (service.getLog().contains ("HTTP 200"), dmb::utf8 ("日志记录了 HTTP 状态"));
    check (service.getLog().contains (juce::String::fromUTF8 (u8"贝斯轨")), dmb::utf8 ("日志记录了参考素材"));
    check (! service.takeResult (clip), dmb::utf8 ("结果只交付一次"));

    const auto request = server.getReceivedRequest();
    check (request.contains ("pipeline-key"), dmb::utf8 ("请求使用了配置的 key"));
    check (request.contains (juce::String::fromUTF8 (u8"贝斯轨")), dmb::utf8 ("提示词里带上了捕获到的源轨"));
    check (request.contains (dmb::utf8 ("根据贝斯写一段鼓组")), dmb::utf8 ("提示词里带上了用户要求"));

    server.stop();

    // failure path: the model returns garbage
    MockServer badServer (juce::String::fromUTF8 (
                              u8R"({"choices":[{"message":{"content":"我觉得这样比较好，你自己编吧"}}]})"), 200);
    check (badServer.start(), dmb::utf8 ("第二个 mock 服务器启动"));

    api.endpoint = "http://127.0.0.1:" + juce::String (badServer.getPort()) + "/chat/completions";

    dmb::GenerationService failing;
    failing.start (api, spec, dmb::utf8 ("再来一次"), { source }, outputDir);

    guard = 0;

    while (failing.isRunning() && guard++ < 300)
        juce::Thread::sleep (50);

    check (failing.getState() == dmb::GenerationService::State::failed, dmb::utf8 ("无法解析的回复被标记为失败"));
    check (failing.getLog().contains (juce::String::fromUTF8 (u8"解析失败")), dmb::utf8 ("日志说明了失败原因"));

    badServer.stop();
}

//==============================================================================
void testLivePlayback()
{
    beginTest ("GeneratedMidiPlayer: note scheduling, looping and stopping");

    auto clip = std::make_shared<dmb::GeneratedClip>();
    clip->title = "player";
    clip->ppq = 960;
    clip->bpm = 120.0;
    clip->bars = 1;
    clip->beatsPerBar = 4;

    dmb::GeneratedTrack track;
    track.name = "test";
    track.channel = 1;

    {
        dmb::Note note;
        note.tick = 0;
        note.pitch = 60;
        note.lengthTicks = 480;
        note.velocity = 100;
        track.notes.push_back (note);
    }

    {
        dmb::Note note;
        note.tick = 960;          // beat 2
        note.pitch = 64;
        note.lengthTicks = 240;
        note.velocity = 90;
        track.notes.push_back (note);
    }

    clip->tracks.push_back (track);

    dmb::GeneratedMidiPlayer player;
    player.prepare (48000.0);
    player.setClip (clip);
    check (player.hasClip(), dmb::utf8 ("player 拿到了片段"));

    // 120 bpm @ 48k: one beat = 24000 samples. Use 512 sample blocks like a host would.
    const int blockSize = 512;
    const double ppqPerBlock = (double) blockSize / 48000.0 * 2.0;   // 120bpm -> 2 quarters/second

    int noteOns = 0;
    int noteOffs = 0;
    double ppq = 0.0;

    for (int block = 0; block < 1000; ++block)
    {
        juce::MidiBuffer midi;
        player.processBlock (midi, blockSize, true, true, ppq, 120.0, true, true, false);

        for (const auto metadata : midi)
        {
            if (metadata.getMessage().isNoteOn())
                ++noteOns;
            else if (metadata.getMessage().isNoteOff())
                ++noteOffs;
        }

        ppq += ppqPerBlock;
    }

    check (noteOns >= 8, dmb::utf8 ("循环播放时音符被反复触发 (实际 ") + juce::String (noteOns) + ")");
    check (noteOffs >= 8, dmb::utf8 ("每个音符都有对应的 note off (实际 ") + juce::String (noteOffs) + ")");

    // transport stopped -> everything must be released
    {
        juce::MidiBuffer midi;
        player.processBlock (midi, blockSize, true, false, ppq, 120.0, true, true, false);

        int offs = 0;

        for (const auto metadata : midi)
            if (metadata.getMessage().isNoteOff())
                ++offs;

        check (offs >= 0, dmb::utf8 ("停止时不崩溃"));
    }

    // one shot: the note must not repeat forever
    dmb::GeneratedMidiPlayer oneShot;
    oneShot.prepare (48000.0);
    oneShot.setClip (clip);

    int oneShotOns = 0;
    ppq = 0.0;

    for (int block = 0; block < 400; ++block)
    {
        juce::MidiBuffer midi;
        oneShot.processBlock (midi, blockSize, true, true, ppq, 120.0, true, false, false);

        for (const auto metadata : midi)
            if (metadata.getMessage().isNoteOn())
                ++oneShotOns;

        ppq += ppqPerBlock;
    }

    check (oneShotOns == 2, dmb::utf8 ("不循环时每个音符只触发一次 (实际 ") + juce::String (oneShotOns) + ")");

    // free running preview without a host transport
    dmb::GeneratedMidiPlayer freeRun;
    freeRun.prepare (48000.0);
    freeRun.setClip (clip);

    int freeOns = 0;

    for (int block = 0; block < 100; ++block)
    {
        juce::MidiBuffer midi;
        freeRun.processBlock (midi, blockSize, false, false, 0.0, 120.0, false, true, true);

        for (const auto metadata : midi)
            if (metadata.getMessage().isNoteOn())
                ++freeOns;
    }

    check (freeOns >= 2, dmb::utf8 ("无宿主走带时也能试听 (实际 ") + juce::String (freeOns) + ")");
}

//==============================================================================
void testRealApiIfAvailable()
{
    beginTest ("Optional: real DeepSeek API call");

    const auto key = juce::SystemStats::getEnvironmentVariable ("DEEPSEEK_API_KEY", {}).trim();

    if (key.isEmpty())
    {
        out (dmb::utf8 ("    skip 未设置 DEEPSEEK_API_KEY, 跳过真实 API 测试"));
        return;
    }

    dmb::ApiSettings api;
    api.apiKey = key;
    api.model = "deepseek-chat";
    api.timeoutMs = 120000;
    api.maxTokens = 1200;

    dmb::GenerationSpec spec;
    spec.bars = 1;
    spec.beatsPerBar = 4;
    spec.maxNotes = 16;

    auto source = makeSource ("bass", dmb::utf8 ("贝斯轨"), 16);

    const auto system = dmb::buildSystemPrompt (spec);
    const auto user = dmb::buildUserPrompt ({ source }, spec, juce::String::fromUTF8 (u8"生成一小节简单的四分音符底鼓"));

    const auto result = dmb::DeepSeekClient::call (api, system, user, 0.3);
    check (result.ok, dmb::utf8 ("真实 API 调用成功: ") + result.error);

    // the plugin's "测试连接" button uses ApiProbe - check it against the real API too
    {
        dmb::ApiProbe probe;
        probe.start (api);

        int guard = 0;

        while (probe.isRunning() && guard++ < 400)
            juce::Thread::sleep (50);

        const auto probeResult = probe.getResult();
        check (probeResult.valid && probeResult.ok,
               dmb::utf8 ("测试连接按钮对真实 API 判定正确: ") + probeResult.message);
    }

    if (result.ok)
    {
        dmb::GeneratedClip clip;
        juce::String error;
        check (dmb::parseGenerationResult (result.content, 960, 1, 4, 1, 0, clip, error),
               dmb::utf8 ("真实回复可解析: ") + error);
        check (clip.totalNotes() > 0, dmb::utf8 ("真实回复包含音符"));
        out (dmb::utf8 ("    info 真实模型返回 ") + juce::String (clip.totalNotes()) + dmb::utf8 (" 个音符: ") + clip.explanation);
    }
}

//==============================================================================
void testCaptureSurvivesStop()
{
    beginTest ("Capture survives stopping / looping (regression test)");

    dmb::MidiCapture capture;
    capture.prepare (48000.0);
    capture.setWindowBars (2);
    capture.setBeatsPerBar (4);

    const double bpm = 120.0;
    const int blockSize = 512;
    const double beatsPerBlock = (double) blockSize / 48000.0 * (bpm / 60.0);

    double ppq = 0.0;
    const int blocksPerBar = (int) std::ceil (4.0 / beatsPerBlock);

    auto runPlaying = [&] (int blocks)
    {
        for (int i = 0; i < blocks; ++i)
        {
            juce::MidiBuffer midi;

            if ((i % 8) == 0)
                midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);

            if ((i % 8) == 4)
                midi.addEvent (juce::MidiMessage::noteOff (1, 60), 0);

            capture.processBlock (midi, blockSize, true, true, ppq, bpm);
            ppq += beatsPerBlock;
            capture.update();
        }
    };

    auto runStopped = [&] (int blocks)
    {
        for (int i = 0; i < blocks; ++i)
        {
            juce::MidiBuffer midi;
            capture.processBlock (midi, blockSize, true, false, ppq, bpm);   // host stopped
            capture.update();
        }
    };

    // ---- record two bars, then stop ----------------------------------------
    runPlaying (blocksPerBar * 2);

    auto whilePlaying = capture.snapshot ("test", dmb::utf8 ("本机"));
    check ((int) whilePlaying.notes.size() >= 4,
           "播放时抓到了音符 (实际 " + juce::String ((int) whilePlaying.notes.size()) + ")");

    runStopped (blocksPerBar);

    auto afterStop = capture.snapshot ("test", dmb::utf8 ("本机"));
    check (afterStop.notes.size() == whilePlaying.notes.size(),
           "停止后音符还在（这正是之前的 bug：被窗口裁掉了）");

    runStopped (blocksPerBar * 8);

    auto afterLongPause = capture.snapshot ("test", dmb::utf8 ("本机"));
    check (afterLongPause.notes.size() == whilePlaying.notes.size(),
           "长时间暂停也不丢 (实际 " + juce::String ((int) afterLongPause.notes.size()) + " 个)");

    // ---- the stopped take gets put aside automatically ----------------------
    auto kept = capture.getKeptTakes ("kept-", dmb::utf8 ("本机 保留"));
    check (! kept.empty(), "停止后自动保留了一段素材");
    check (capture.getKeptTakeCount() == (int) kept.size(), "保留段数与统计一致");

    if (! kept.empty())
    {
        check (kept[0].notes.size() == whilePlaying.notes.size(), "保留段里的音符数量正确");
        check (kept[0].name.contains (dmb::utf8 ("保留")), "保留段有可读名字: " + kept[0].name);
        check (kept[0].id.startsWith ("kept-"), "保留段有稳定 id");
    }

    // ---- restart from the beginning: the old material must survive ----------
    ppq = 0.0;
    runPlaying (blocksPerBar);

    check (! capture.getKeptTakes ("kept-", dmb::utf8 ("本机 保留")).empty(),
           "从头重播后，之前保留的素材还在");

    // ---- repeated stop/loop must not pile up duplicates ---------------------
    for (int loop = 0; loop < 4; ++loop)
    {
        ppq = 0.0;
        runPlaying (blocksPerBar);
        runStopped (4);
    }

    check (capture.getKeptTakeCount() <= 6, "反复循环/停止不会无限堆积保留段");

    // ---- rename one kept take, then delete just that one --------------------
    {
        ppq = 0.0;
        runPlaying (blocksPerBar);      // a second take (different start -> kept as well)
        runStopped (4);

        auto takes = capture.getKeptTakes ("kept-", dmb::utf8 ("本机 保留"));
        check (takes.size() >= 2, "两段不同起点的素材都保留了 (实际 "
                                    + juce::String ((int) takes.size()) + ")");

        if (takes.size() >= 2)
        {
            const auto firstId = takes[0].id;
            const auto secondId = takes[1].id;

            check (firstId != secondId, "每段有各自稳定的 id");

            check (capture.renameKeptTake (firstId, dmb::utf8 ("我的贝斯动机")), "重命名成功");
            check (! capture.renameKeptTake ("kept-99999", "x"), "未知 id 重命名会失败");

            auto renamed = capture.getKeptTakes ("kept-", dmb::utf8 ("本机 保留"));
            check (! renamed.empty() && renamed[0].name == dmb::utf8 ("我的贝斯动机"),
                   "重命名后名字生效: " + (renamed.empty() ? juce::String ("<空>") : renamed[0].name));

            const auto countBefore = capture.getKeptTakeCount();
            check (capture.removeKeptTake (secondId), "按 id 删除其中一段成功");
            check (capture.getKeptTakeCount() == countBefore - 1, "只删掉了那一段，其它还在");

            auto remaining = capture.getKeptTakes ("kept-", dmb::utf8 ("本机 保留"));

            bool stillHasRenamed = false;
            bool stillHasDeleted = false;

            for (const auto& take : remaining)
            {
                if (take.id == firstId)  stillHasRenamed = true;
                if (take.id == secondId) stillHasDeleted = true;
            }

            check (stillHasRenamed, "改过名的那段还在");
            check (! stillHasDeleted, "被删掉的段确实不在了");
            check (! capture.removeKeptTake ("kept-99999"), "未知 id 删除会失败");
        }
    }

    // ---- explicit clear wipes both the live take and the kept ones ----------
    capture.clear();
    check (capture.snapshot ("test", dmb::utf8 ("本机")).notes.empty(), "clear() 清掉当前捕获");
    check (capture.getKeptTakeCount() == 0, "clear() 同时清掉保留段");
}

//==============================================================================
void testSourceLibrary()
{
    beginTest ("Persistent source library (stuff survives a host restart)");

    const auto dir = makeTempDirectory ("library");
    dmb::SourceLibrary::setDirectory (dir);

    check (dmb::SourceLibrary::loadAll().empty(), "刚开始库是空的");

    // ---- a captured take -----------------------------------------------
    auto capture = makeSource ("cap", dmb::utf8 ("Bass 保留 21:00:00"), 24);
    capture.origin = dmb::utf8 ("捕获");

    const auto captureId = dmb::SourceLibrary::save (capture);
    check (captureId.startsWith ("lib-"), "保存后拿到库 id: " + captureId);

    auto all = dmb::SourceLibrary::loadAll();
    check (all.size() == 1, "保存后能读回 1 条");
    check (! all.empty() && all[0].notes.size() == 24, "音符数量一致");
    check (! all.empty() && all[0].origin == dmb::utf8 ("捕获"), "来源标记记住了 (捕获)");
    check (! all.empty() && all[0].name == dmb::utf8 ("Bass 保留 21:00:00"), "名字记住了（含中文）");
    check (! all.empty() && all[0].notes.front().pitch == 36, "音符内容一致");

    // re-loading (as a fresh plugin instance would) still finds it
    check (dmb::SourceLibrary::loadAll().size() == 1, "重新加载仍然在（模拟重开宿主）");

    // ---- an imported file reference -------------------------------------
    auto imported = makeSource ("imp", dmb::utf8 ("导入: bass.mid"), 8);
    imported.origin = dmb::utf8 ("导入: bass.mid");
    const auto importedId = dmb::SourceLibrary::save (imported);

    check (dmb::SourceLibrary::loadAll().size() == 2, "导入的素材也进去了");

    // ---- rename just one entry ------------------------------------------
    check (dmb::SourceLibrary::rename (importedId, dmb::utf8 ("我的贝斯")), "库里的条目可以改名");

    auto renamed = dmb::SourceLibrary::loadAll();
    bool foundRenamed = false;
    bool otherIntact = false;

    for (const auto& source : renamed)
    {
        if (source.id == importedId && source.name == dmb::utf8 ("我的贝斯"))
            foundRenamed = true;

        if (source.id == captureId && source.name == capture.name)
            otherIntact = true;
    }

    check (foundRenamed, "改名生效");
    check (otherIntact, "改名不影响别的条目");
    check (! dmb::SourceLibrary::rename ("lib-nope", "x"), "未知条目改名会失败");

    // ---- delete just one entry ------------------------------------------
    check (dmb::SourceLibrary::remove (importedId), "可以单独删除一条");
    check (dmb::SourceLibrary::loadAll().size() == 1, "另一条还在");
    check (! dmb::SourceLibrary::remove ("lib-nope"), "未知条目删除会失败");

    // ---- clear by origin -------------------------------------------------
    dmb::SourceLibrary::save (imported);
    check (dmb::SourceLibrary::loadAll().size() == 2, "又存回一条用于测试");

    const auto removedCaptures = dmb::SourceLibrary::removeByOrigin (dmb::utf8 ("捕获"));
    check (removedCaptures == 1, "按来源批量删除（捕获）删掉了 1 条");
    check (dmb::SourceLibrary::loadAll().size() == 1, "只剩导入的那条");
    check (dmb::SourceLibrary::loadAll().front().origin.startsWith (dmb::utf8 ("导入")), "剩下的确实是导入的");

    // ---- pruning keeps the library bounded -------------------------------
    for (int i = 0; i < 40; ++i)
    {
        auto extra = makeSource ("x" + juce::String (i), "take " + juce::String (i), 4);
        extra.origin = dmb::utf8 ("捕获");
        extra.stampMs = juce::Time::currentTimeMillis() + i;
        dmb::SourceLibrary::save (extra);
    }

    const auto afterPrune = dmb::SourceLibrary::loadAll();
    check (afterPrune.size() <= 32, "库不会无限变大 (实际 " + juce::String ((int) afterPrune.size()) + ")");

    dmb::SourceLibrary::setDirectory (juce::File());
}

//==============================================================================
void testProjects()
{
    beginTest ("Projects (one folder per host project, material never mixes)");

    const auto root = makeTempDirectory ("projects");

    check (dmb::getDefaultProjectsRoot().getFileName() == "DeepSeek MIDI Bridge",
           "默认工程根目录是 文档\\DeepSeek MIDI Bridge");

    // ---- creating a project description writes nothing ----------------------
    auto a = dmb::ProjectStore::create (root, dmb::utf8 ("我的歌"));

    check (a.isValid(), "新建工程拿到 id: " + a.id);
    check (a.folder.getParentDirectory() == root, "工程文件夹直接放在根目录下面");
    check (a.folder.getFileName() == dmb::utf8 ("我的歌"), "文件夹就是工程名（中文没问题）");
    check (! a.folder.exists(), "只是描述，还没往磁盘写");

    check (dmb::ProjectStore::ensureFolder (a), "第一次要写东西时才建立文件夹");
    check (a.folder.isDirectory(), "文件夹建好了");
    check (a.markerFile().existsAsFile(), "工程标记文件写好了");

    const auto readBack = dmb::ProjectStore::readMarker (a.folder);
    check (readBack.id == a.id && readBack.name == a.name, "标记文件里的 id / 名字一致");

    // ---- a second project with the same name is still a separate folder -----
    auto b = dmb::ProjectStore::create (root, dmb::utf8 ("我的歌"));

    check (b.folder != a.folder, "重名工程用另一个文件夹");
    check (b.folder.getFileName() == dmb::utf8 ("我的歌 (2)"),
           "重名自动加序号: " + b.folder.getFileName());
    check (dmb::ProjectStore::ensureFolder (b), "第二个工程也能建好");

    // never take over a folder that belongs to another project
    auto intruder = a;
    intruder.id = "p-intruder";
    check (! dmb::ProjectStore::ensureFolder (intruder), "不会抢占别的工程的文件夹");

    // ---- renaming moves the whole folder -----------------------------------
    const auto sample = a.folder.getChildFile ("take.mid");
    sample.replaceWithText ("MThd");

    juce::String error;
    const auto renamed = dmb::ProjectStore::rename (a, dmb::utf8 ("新名字"), root, error);

    check (error.isEmpty(), "改名没有报错");
    check (renamed.isValid() && renamed.id == a.id, "改名保留 id（素材还对得上）");
    check (renamed.folder.getFileName() == dmb::utf8 ("新名字"), "文件夹换成新名字");
    check (! a.folder.exists() && renamed.folder.isDirectory(), "旧文件夹没了，新文件夹在");
    check (renamed.folder.getChildFile ("take.mid").existsAsFile(), "里面的文件一起搬过去了");
    check (dmb::ProjectStore::readMarker (renamed.folder).id == a.id, "标记文件也跟着更新");
    check (dmb::ProjectStore::rename (a, "", root, error).isValid() == false && error.isNotEmpty(),
           "空名字会被拒绝");

    const auto listed = dmb::ProjectStore::list (root);
    check (listed.size() == 2, "list() 找到 2 个工程 (实际 " + juce::String ((int) listed.size()) + ")");

    // ---- process wide registry (how instances of one host agree) ------------
    dmb::ProjectRegistry::resetForTests();
    check (! dmb::ProjectRegistry::get().info.isValid(), "一开始没有当前工程");

    dmb::ProjectRegistry::publish (renamed, "inst-1");

    const auto snapshot = dmb::ProjectRegistry::get();
    check (snapshot.info.id == renamed.id, "登记之后别的实例能读到同一个工程");
    check (snapshot.changedBy == "inst-1" && snapshot.changedAtMs > 0, "登记带实例 id 和时间戳");
    dmb::ProjectRegistry::resetForTests();

    // ---- reference material is per project ---------------------------------
    auto sourceA = makeSource ("a", dmb::utf8 ("贝斯 A"), 8);
    sourceA.projectId = renamed.id;

    const auto idA = dmb::SourceLibrary::save (renamed.libraryDirectory(), sourceA);

    check (idA.startsWith ("lib-"), "素材存进当前工程的库");
    check (renamed.libraryDirectory().getParentDirectory() == renamed.folder,
           "库就在工程文件夹里面 (.dmb-library)");
    check (dmb::SourceLibrary::loadAll (renamed.libraryDirectory()).size() == 1, "本工程读得到");
    check (dmb::SourceLibrary::loadAll (b.libraryDirectory()).empty(), "另一个工程读不到（互不干扰）");

    const auto counts = dmb::SourceLibrary::countEntries (renamed.libraryDirectory());
    check (counts == 1, "countEntries 工作正常");

    // ---- the capture bus is filtered per project ---------------------------
    const auto busDir = makeTempDirectory ("projectbus");
    dmb::CaptureBus::setRootDirectory (busDir);

    auto publishedA = makeSource ("inst-a", "A", 4);
    publishedA.projectId = renamed.id;

    auto publishedB = makeSource ("inst-b", "B", 4);
    publishedB.projectId = b.id;

    dmb::CaptureBus::publish (publishedA);
    dmb::CaptureBus::publish (publishedB);

    const auto onlyA = dmb::CaptureBus::readAll (60000, {}, renamed.id);
    const auto onlyB = dmb::CaptureBus::readAll (60000, {}, b.id);
    const auto unfiltered = dmb::CaptureBus::readAll (60000, {});

    check (onlyA.size() == 1 && onlyA[0].id == "inst-a", "只看到自己工程的捕获");
    check (onlyB.size() == 1 && onlyB[0].id == "inst-b", "另一个工程只看到它自己的");
    check (unfiltered.size() == 2, "不筛工程时底层仍然能看到全部（给 CLI / 测试用）");
    check (dmb::CaptureBus::pruneExpired (6 * 60 * 60 * 1000) == 0, "新条目不会被当成过期清掉");
    check (dmb::CaptureBus::pruneExpired (-1) == 2, "过期清理能删掉旧条目");

    // ---- name suggestion straight from the host window title ----------------
    check (dmb::ProjectStore::projectNameFromHostTitle ("My Song.als - Ableton Live 12 Suite") == "My Song",
           "从 Live 窗口标题里取出 Set 名");
    check (dmb::ProjectStore::projectNameFromHostTitle (dmb::utf8 ("未命名.als - Ableton Live 12 Suite"))
             == dmb::utf8 ("未命名"), "中文 Set 名也能取出来");
    check (dmb::ProjectStore::projectNameFromHostTitle ("Ableton Live 12 Suite").isEmpty(),
           "只有宿主名时不算 Set 名");
    check (dmb::ProjectStore::projectNameFromHostTitle ("").isEmpty(), "空标题返回空");
}

//==============================================================================
void testOtherHostsAndPrivacy()
{
    beginTest ("Other hosts (project session across processes) + no local paths in the UI");

    const auto sessionDir = makeTempDirectory ("sessions");
    dmb::ProjectSession::setDirectory (sessionDir);
    dmb::ProjectRegistry::resetForTests();

    // ---- the host session key -----------------------------------------------
    const auto key = dmb::ProjectSession::hostSessionKey();
    check (key.isNotEmpty(), "能算出本宿主会话的 key: " + key);
    check (key == dmb::ProjectSession::hostSessionKey(), "同一个进程里 key 稳定");
    check (dmb::ProjectSession::getDirectory() == sessionDir, "会话目录可以重定向（测试用）");

    // ---- a project stored by another process is adopted ----------------------
    dmb::ProjectInfo shared;
    shared.id = "p-shared";
    shared.name = dmb::utf8 ("另一个进程的工程");
    shared.folder = sessionDir.getChildFile ("shared");

    dmb::ProjectSession::write (shared, 12345, "other-process");

    const auto snapshot = dmb::ProjectRegistry::get();
    check (snapshot.info.id == "p-shared", "本进程没有工程时，会采用别的进程写下的工程");
    check (snapshot.changedAtMs == 12345, "沿用别的进程声明的“声明时间”");
    check (snapshot.changedBy == "other-process", "记住是谁声明的（本进程自己的声明才会被忽略）");

    // stale sessions are ignored (that host is gone)
    juce::int64 claimed = 0;
    check (! dmb::ProjectSession::readFresh (-1, &claimed).isValid(), "过期的会话文件会被忽略");
    check (dmb::ProjectSession::readFresh (60000).isValid(), "没过期的会话文件仍然有效");

    // heartbeat keeps it alive without changing the claim
    dmb::ProjectRegistry::touch();
    juce::int64 claimedAfter = 0;
    juce::String byAfter;
    const auto reread = dmb::ProjectSession::readFresh (60000, &claimedAfter, &byAfter);
    check (reread.id == "p-shared" && claimedAfter == 12345 && byAfter == "other-process",
           "心跳只刷新存活时间，不改动声明");

    check (dmb::ProjectSession::prune (12 * 60 * 60 * 1000) == 0, "刚写的会话文件不会被当成垃圾清掉");

    dmb::ProjectRegistry::resetForTests();
    dmb::ProjectSession::setDirectory (juce::File());
    dmb::ProjectSession::resetForTests();

    // ---- project names of the other hosts ------------------------------------
    check (dmb::ProjectStore::projectNameFromHostTitle ("My Song.rpp - REAPER") == "My Song",
           "REAPER 的工程名");
    check (dmb::ProjectStore::projectNameFromHostTitle ("Project1 - Cubase") == "Project1",
           "Cubase 的工程名");
    check (dmb::ProjectStore::projectNameFromHostTitle ("Song.cpr - Nuendo") == "Song",
           "Nuendo 的工程名");
    check (dmb::ProjectStore::projectNameFromHostTitle ("My Track.bwproject - Bitwig Studio") == "My Track",
           "Bitwig 的工程名");
    check (dmb::ProjectStore::projectNameFromHostTitle ("* Demo - REAPER") == "Demo",
           "宿主标记的“已修改”星号会被去掉");
    check (dmb::ProjectStore::projectNameFromHostTitle ("Cubase").isEmpty(),
           "只有宿主名时不给名字（Cubase）");
    check (dmb::ProjectStore::projectNameFromHostTitle ("REAPER").isEmpty(),
           "只有宿主名时不给名字（REAPER）");
    check (dmb::ProjectStore::projectNameFromHostTitle ("Untitled - FL Studio") == "Untitled",
           "没保存过的工程也有名字");

    // ---- paths shown in the UI never contain the real account ----------------
    const auto documents = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    const auto insideDocuments = documents.getChildFile ("DeepSeek MIDI Bridge").getChildFile ("My Song");
    const auto shownDocuments = dmb::displayPath (insideDocuments);

    check (! shownDocuments.containsIgnoreCase (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getFileName()),
           "文档里的路径不再带用户名: " + shownDocuments);
    check (shownDocuments.startsWith (documents.getFileName()), "文档里的路径显示成 文档\\… : " + shownDocuments);

    const auto insideAppData = dmb::PluginSettings::getAppDirectory().getChildFile ("settings.json");
    check (dmb::displayPath (insideAppData) == "%LOCALAPPDATA%\\DeepSeekMidiBridge\\settings.json",
           "设置文件显示成 %LOCALAPPDATA%\\… : " + dmb::displayPath (insideAppData));

    check (dmb::displayPath ("C:\\Program Files\\Common Files\\VST3")
             == "C:\\Program Files\\Common Files\\VST3", "系统目录原样显示（不含个人信息）");
    check (dmb::displayPath (juce::String()).isEmpty(), "空路径还是空");

    // ---- a child process of this host resolves to the same session -----------
    // (this is what a sandboxing host does: every plug-in instance gets its own
    // process, whose parent is the host application)
    const auto probe = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("dmb_session_key_probe.txt");
    probe.deleteFile();

    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    const auto launched = juce::Process::openDocument (exe.getFullPathName(), "--session-key-probe");

    check (launched, "能启动一个子进程（模拟“每个实例一个进程”的宿主）");

    for (int i = 0; i < 80 && ! probe.existsAsFile(); ++i)
        juce::Thread::sleep (250);

    const auto childKey = probe.existsAsFile() ? probe.loadFileAsString().trim() : juce::String();

    check (childKey.isNotEmpty(), "子进程算出了自己的会话 key: " + childKey);
    check (childKey == dmb::ProjectSession::hostSessionKey(),
           "子进程认出的宿主会话与宿主进程一致（两个进程会落到同一个工程）");

    probe.deleteFile();
}

//==============================================================================
void testApiProfilesAndProbe()
{
    beginTest ("API profiles (quick switching) + connection probe");

    // ---- settings round trip, including the saved API/model presets ---------
    {
        dmb::PluginSettings settings;
        settings.api.endpoint = "https://api.deepseek.com/chat/completions";
        settings.api.model = "deepseek-chat";
        settings.api.apiKey = "sk-current";

        dmb::ApiProfile official;
        official.name = dmb::utf8 ("官方 · chat");
        official.endpoint = "https://api.deepseek.com/chat/completions";
        official.model = "deepseek-chat";
        official.apiKey = "sk-a";

        dmb::ApiProfile relay;
        relay.name = dmb::utf8 ("中转 · reasoner");
        relay.endpoint = "https://relay.example.com/v1/chat/completions";
        relay.model = "deepseek-reasoner";
        relay.apiKey = "sk-b";

        settings.apiProfiles = { official, relay };

        const auto restored = dmb::PluginSettings::fromVar (settings.toVar());

        check (restored.apiProfiles.size() == 2, "方案序列化往返数量正确");
        check (! restored.apiProfiles.empty() && restored.apiProfiles[0].name == official.name,
               "方案名称（含中文）往返正确");
        check (restored.apiProfiles.size() > 1 && restored.apiProfiles[1].apiKey == relay.apiKey
                 && restored.apiProfiles[1].model == "deepseek-reasoner",
               "方案内容往返正确");
        check (restored.apiProfiles[0].matches (settings.api), "matches() 能识别当前使用的方案");

        // UI scale is remembered too (0 = follow the display scale)
        settings.uiScale = 1.25;
        check (std::abs (dmb::PluginSettings::fromVar (settings.toVar()).uiScale - 1.25) < 0.001,
               "界面缩放设置可以保存/读回");
        settings.uiScale = 0.0;
        check (dmb::PluginSettings::fromVar (settings.toVar()).uiScale == 0.0,
               "自动缩放 (0) 可以保存/读回");

        // the "open Explorer after generating" switch is remembered too
        settings.openFolderAfterGenerate = false;
        check (! dmb::PluginSettings::fromVar (settings.toVar()).openFolderAfterGenerate,
               "生成后自动打开文件夹可以关掉并保存/读回");
        settings.openFolderAfterGenerate = true;
        check (dmb::PluginSettings::fromVar (settings.toVar()).openFolderAfterGenerate,
               "生成后自动打开文件夹默认/开启状态可以读回");
        check (dmb::PluginSettings().openFolderAfterGenerate, "新装的插件默认自动打开文件夹");
        check (! restored.apiProfiles[1].matches (settings.api), "matches() 不会误判别的方案");

        // a profile without a name is named after its endpoint
        auto* root = new juce::DynamicObject();
        juce::Array<juce::var> list;
        auto* item = new juce::DynamicObject();
        item->setProperty ("endpoint", "https://x.example.com/v1/chat/completions");
        item->setProperty ("model", "deepseek-chat");
        list.add (juce::var (item));
        root->setProperty ("apiProfiles", list);

        const auto withUnnamed = dmb::PluginSettings::fromVar (juce::var (root));
        check (withUnnamed.apiProfiles.size() == 1 && withUnnamed.apiProfiles[0].name.contains ("x.example.com"),
               "没有名字的方案会自动命名");
    }

    // ---- diagnostics log (used to trace drag & drop) ------------------------
    {
        dmb::clearDiagnosticsLog();
        check (! dmb::getDiagnosticsLogFile().existsAsFile(), "诊断日志可以清空");

        dmb::logDiagnostic ("test", dmb::utf8 ("第一行 中文"));
        dmb::logDiagnostic ("drag", "second line");

        const auto text = dmb::readDiagnosticsLog (10);
        check (text.contains (dmb::utf8 ("第一行 中文")), "诊断日志能写能读（含中文）");
        check (text.contains ("[drag]"), "诊断日志带分类标签");
        check (text.contains ("second line"), "诊断日志有多行");
        check (text.contains ("2026") || text.contains ("202"), "诊断日志带时间戳");

        const auto file = dmb::getDiagnosticsLogFile();
        check (file.getParentDirectory().isDirectory(), "诊断日志目录存在: " + file.getFullPathName());
    }

    // ---- key masking used by the status line --------------------------------
    check (dmb::ApiProbe::describeKey ("sk-95c94e65485a47c8b2eb7dfcc5dbd400") == "Key: sk-95c...d400",
           "长 Key 显示为掩码");
    check (dmb::ApiProbe::describeKey ("") == dmb::utf8 ("未设置 Key"), "空 Key 有提示");
    check (dmb::ApiProbe::describeKey ("short") == "Key: short", "短 Key 原样显示");

    // ---- probe against a healthy mock endpoint ------------------------------
    {
        MockServer server (R"({"choices":[{"finish_reason":"stop","message":{"content":"OK"}}],"usage":{"total_tokens":5}})", 200);
        check (server.start(), "probe mock 服务器启动");

        dmb::ApiSettings api;
        api.endpoint = "http://127.0.0.1:" + juce::String (server.getPort()) + "/v1/chat/completions";
        api.apiKey = "probe-key";
        api.model = "deepseek-chat";
        api.timeoutMs = 10000;

        dmb::ApiProbe probe;
        check (! probe.getResult().valid, "测试前没有结果");
        probe.start (api);

        int guard = 0;
        while (probe.isRunning() && guard++ < 300)
            juce::Thread::sleep (50);

        const auto result = probe.getResult();
        check (result.valid, "测试产生了结果");
        check (result.ok, "测试判定连接正常: " + result.message);
        check (result.statusCode == 200, "测试记下了 HTTP 状态码");
        check (result.message.contains ("deepseek-chat"), "测试结果里带模型名");
        check (! probe.isRunning(), "测试线程已结束");

        probe.clearResult();
        check (! probe.getResult().valid, "clearResult() 会清掉旧结果");

        server.stop();
    }

    // ---- probe against a bad key --------------------------------------------
    {
        MockServer server (R"({"error":{"message":"Authentication Fails"}})", 401);
        check (server.start(), "失败场景 mock 服务器启动");

        dmb::ApiSettings api;
        api.endpoint = "http://127.0.0.1:" + juce::String (server.getPort()) + "/v1/chat/completions";
        api.apiKey = "bad-key";
        api.timeoutMs = 10000;

        dmb::ApiProbe probe;
        probe.start (api);

        int guard = 0;
        while (probe.isRunning() && guard++ < 300)
            juce::Thread::sleep (50);

        const auto result = probe.getResult();
        check (result.valid && ! result.ok, "测试判定失败");
        check (result.statusCode == 401, "测试记下了 401");
        check (result.message.contains ("Authentication"), "测试带上了服务器的错误信息: " + result.message);

        server.stop();
    }

    // ---- starting twice in a row must be harmless ---------------------------
    {
        MockServer server (R"({"choices":[{"message":{"content":"OK"}}]})", 200);
        check (server.start(), "重复启动场景 mock 服务器启动");

        dmb::ApiSettings api;
        api.endpoint = "http://127.0.0.1:" + juce::String (server.getPort()) + "/v1/chat/completions";
        api.apiKey = "k";
        api.timeoutMs = 10000;

        dmb::ApiProbe probe;
        probe.start (api);
        probe.start (api);   // ignored while running

        int guard = 0;
        while (probe.isRunning() && guard++ < 300)
            juce::Thread::sleep (50);

        check (probe.getResult().valid, "重复调用 start() 不会出问题");

        server.stop();
    }
}

//==============================================================================
#if JUCE_PLUGINHOST_VST3
/** A minimal play head so the plugin sees a running transport. */
class TestPlayHead : public juce::AudioPlayHead
{
public:
    void setPosition (double newPpq, double newBpm, bool playing)
    {
        ppq = newPpq;
        bpm = newBpm;
        isPlaying = playing;
    }

    void advance (int numSamples, double sampleRate)
    {
        ppq += (double) numSamples / sampleRate * (bpm / 60.0);
    }

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setPpqPosition (ppq);
        info.setBpm (bpm);
        info.setIsPlaying (isPlaying);
        info.setTimeSignature (TimeSignature { 4, 4 });
        return info;
    }

private:
    double ppq = 0.0;
    double bpm = 120.0;
    bool isPlaying = true;
};

juce::Array<juce::File> findBuiltVst3Bundles()
{
    juce::Array<juce::File> result;

    auto searchRoot = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

    for (int level = 0; level < 6 && result.isEmpty(); ++level)
    {
        if (searchRoot.getChildFile ("CMakeCache.txt").existsAsFile())
        {
            for (const auto& entry : juce::RangedDirectoryIterator (searchRoot, true, "*_artefacts",
                                                                   juce::File::findDirectories))
                result.addArray (entry.getFile().findChildFiles (juce::File::findDirectories, true, "*.vst3"));

            break;
        }

        searchRoot = searchRoot.getParentDirectory();
    }

    return result;
}

/** Loads the compiled .vst3 in a real VST3 host (JUCE's host code), feeds it MIDI
    and audio, and checks that it behaves like a plugin and publishes its capture.
*/
void testVst3HostLoad (const juce::StringArray& bundlePathsFromArgs)
{
    beginTest ("VST3: load the built plugin in a real VST3 host");

    juce::Array<juce::File> bundles;

    for (const auto& path : bundlePathsFromArgs)
    {
        juce::File file (path);

        if (file.isDirectory())
            bundles.add (file);
    }

    if (bundles.isEmpty())
        bundles = findBuiltVst3Bundles();

    if (bundles.isEmpty())
    {
        out ("    skip 没有找到 .vst3 产物 (先运行 scripts/build.ps1)");
        return;
    }

    // The plugin publishes its captures through the shared bus. Point that bus at a
    // throw-away folder, using the plugin's own settings file (the DLL has its own
    // copy of the bus, so an in-process override would not reach it). The user's
    // settings are backed up and restored afterwards.
    const auto busDir = makeTempDirectory ("hostbus");
    const auto settingsFile = dmb::PluginSettings::getSettingsFile();
    const auto settingsBackup = settingsFile.existsAsFile() ? settingsFile.loadFileAsString() : juce::String();
    const bool hadSettings = settingsFile.existsAsFile();

    {
        dmb::PluginSettings testSettings;
        testSettings.busDirectory = busDir.getFullPathName();
        testSettings.outputDirectory = busDir.getFullPathName();
        testSettings.sourceName = dmb::utf8 ("测试实例");
        testSettings.save();
    }

    dmb::CaptureBus::setRootDirectory (busDir);

    juce::AudioPluginFormatManager manager;
    juce::addDefaultFormatsToManager (manager);

    for (const auto& bundle : bundles)
    {
        const bool isMidiOnly = bundle.getFileNameWithoutExtension().containsIgnoreCase ("(MIDI)");

        out ("    info 测试插件: " + bundle.getFullPathName());

        juce::OwnedArray<juce::PluginDescription> types;

        for (auto* format : manager.getFormats())
            if (format->getName().containsIgnoreCase ("VST3"))
                format->findAllTypesForFile (types, bundle.getFullPathName());

        check (! types.isEmpty(), dmb::utf8 ("宿主扫描到了插件类型: ") + bundle.getFileNameWithoutExtension());

        if (types.isEmpty())
            continue;

        juce::String error;
        auto instance = manager.createPluginInstance (*types[0], 48000.0, 512, error);

        check (instance != nullptr, dmb::utf8 ("插件实例创建成功: ") + error);

        if (instance == nullptr)
            continue;

        const auto instanceName = instance->getName();
        out ("    info 插件名: " + instanceName
             + " / 音频输出 " + juce::String (instance->getTotalNumOutputChannels())
             + " / 参数 " + juce::String (instance->getParameters().size()));

        check (instanceName.containsIgnoreCase ("DeepSeek"), dmb::utf8 ("插件名包含 DeepSeek"));
        check (instance->acceptsMidi(), dmb::utf8 ("插件接受 MIDI 输入"));
        check (instance->producesMidi(), dmb::utf8 ("插件可以输出 MIDI"));
        check (instance->getParameters().size() >= 8, dmb::utf8 ("参数通过 VST3 暴露出来"));

        if (isMidiOnly)
        {
            check (instance->getTotalNumOutputChannels() == 0, dmb::utf8 ("纯 MIDI 版本没有音频输出总线"));
        }
        else
        {
            check (instance->getTotalNumOutputChannels() == 2, dmb::utf8 ("Fx 版本是 2 声道输出"));
        }

        TestPlayHead playHead;
        instance->setPlayHead (&playHead);
        instance->prepareToPlay (48000.0, 512);

        {
            // state round trip through the host API - this is how a Live project
            // saves and restores the plugin
            juce::MemoryBlock state;
            instance->getStateInformation (state);
            out ("    info 状态 " + juce::String ((int) state.getSize()) + " 字节");
            check (state.getSize() > 100, dmb::utf8 ("插件把设置/结果交给宿主保存"));

            auto& params = instance->getParameters();
            int transposeIndex = -1;

            for (int i = 0; i < params.size(); ++i)
                if (params[i]->getName (64).contains (dmb::utf8 ("移调")))
                    transposeIndex = i;

            check (transposeIndex >= 0, dmb::utf8 ("参数名通过 VST3 正确暴露 (移调)"));

            if (transposeIndex >= 0)
            {
                params[transposeIndex]->setValueNotifyingHost (0.75f);
                const auto savedValue = params[transposeIndex]->getValue();

                instance->getStateInformation (state);

                juce::String secondError;
                auto second = manager.createPluginInstance (*types[0], 48000.0, 512, secondError);

                check (second != nullptr, dmb::utf8 ("第二个实例也创建成功"));

                if (second != nullptr)
                {
                    second->setStateInformation (state.getData(), (int) state.getSize());

                    auto& secondParams = second->getParameters();
                    const auto restored = secondParams[juce::jmin (transposeIndex, secondParams.size() - 1)]->getValue();

                    check (std::abs (restored - savedValue) < 1.0e-3f,
                           dmb::utf8 ("工程状态里的参数被正确恢复 (") + juce::String (restored, 4) + ")");

                    second.reset();
                }
            }
        }

        juce::AudioBuffer<float> buffer (juce::jmax (1, instance->getTotalNumInputChannels()), 512);
        bool passthroughOk = true;

        for (int block = 0; block < 40; ++block)
        {
            buffer.clear();

            for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    buffer.setSample (channel, i, 0.25f);

            juce::MidiBuffer midi;

            if (block == 1)
                midi.addEvent (juce::MidiMessage::noteOn (1, 61, (juce::uint8) 100), 0);

            if (block == 5)
                midi.addEvent (juce::MidiMessage::noteOff (1, 61), 0);

            playHead.advance (buffer.getNumSamples(), 48000.0);
            instance->processBlock (buffer, midi);

            if (block == 20 && buffer.getNumChannels() > 0)
                for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                    for (int i = 0; i < buffer.getNumSamples(); ++i)
                        if (std::abs (buffer.getSample (channel, i) - 0.25f) > 1.0e-6f)
                            passthroughOk = false;
        }

        if (! isMidiOnly)
            check (passthroughOk, dmb::utf8 ("音频直通没有被破坏"));

        // give the plugin's background thread time to publish the capture
        juce::Thread::sleep (900);

        auto published = dmb::CaptureBus::readAll (120000, {});

        check (! published.empty(), dmb::utf8 ("插件实例把捕获结果发布到了共享总线"));

        bool foundNote = false;

        for (const auto& source : published)
            for (const auto& note : source.notes)
                if (note.pitch == 61)
                    foundNote = true;

        check (foundNote, dmb::utf8 ("总线里的音符就是宿主送进去的那个 (pitch 61)"));

        // ---- the project the plugin saved in the host state --------------------
        juce::MemoryBlock projectState;
        instance->getStateInformation (projectState);

        const auto projectId = projectPropertyFromState (projectState, "projectId");
        const auto projectName = projectPropertyFromState (projectState, "projectName");
        const auto projectFolder = projectPropertyFromState (projectState, "projectFolder");

        check (projectId.startsWith ("p-"), dmb::utf8 ("宿主状态里存下了工程 id: ") + projectId);
        check (projectName.isNotEmpty(), dmb::utf8 ("宿主状态里存下了工程名: ") + projectName);

        const juce::File projectDir (projectFolder);
        check (projectDir.isDirectory(), dmb::utf8 ("工程文件夹已经建立: ") + projectFolder);
        check (projectDir.getChildFile (dmb::ProjectStore::kMarkerFileName).existsAsFile(),
               dmb::utf8 ("工程文件夹里有标记文件（重开宿主能认回来）"));

        const auto marker = dmb::ProjectStore::readMarker (projectDir);
        check (marker.id == projectId && projectId.isNotEmpty(), dmb::utf8 ("标记文件里的 id 和状态里的一致"));

        // the bus only shows this project's material
        const auto sameProject = dmb::CaptureBus::readAll (120000, {}, projectId);
        const auto otherProject = dmb::CaptureBus::readAll (120000, {}, "p-somewhere-else");

        check (sameProject.size() == published.size(), dmb::utf8 ("按工程 id 读总线能读到自己的捕获"));
        check (otherProject.empty(), dmb::utf8 ("另一个工程读总线什么也看不到"));

        // a second instance that is handed the same project state joins it
        {
            juce::String secondError;
            auto reopened = manager.createPluginInstance (*types[0], 48000.0, 512, secondError);

            if (reopened != nullptr)
            {
                reopened->setStateInformation (projectState.getData(), (int) projectState.getSize());

                juce::MemoryBlock reopenedState;
                reopened->getStateInformation (reopenedState);

                check (projectPropertyFromState (reopenedState, "projectId") == projectId,
                       dmb::utf8 ("重开工程后还是同一个工程（素材不会丢）"));

                reopened.reset();
            }
        }

        // The editor is what the user actually touches: build it, let its timers
        // run, and render it once. This catches layout/paint crashes that a
        // compile-only check would miss.
        if (instance->hasEditor())
        {
            auto* editor = instance->createEditorIfNeeded();
            check (editor != nullptr, dmb::utf8 ("插件界面可以创建"));

            if (editor != nullptr)
            {
                editor->setSize (1060, 760);

                if (auto* peer = editor->getPeer())
                    juce::ignoreUnused (peer);

                juce::MessageManager::getInstance()->runDispatchLoopUntil (400);

                juce::Image image (juce::Image::ARGB, 1060, 760, true);
                juce::Graphics g (image);
                editor->paintEntireComponent (g, true);

                check (true, dmb::utf8 ("插件界面可以渲染 (含 5 Hz 刷新)"));

                // NOTE: a *hosted* VST3 editor paints into its own native window, so an
                // offscreen render only proves that the layout code runs without
                // crashing - the real look is captured from the Standalone build.
                juce::ignoreUnused (image);

                // Optional: dump the offscreen render (main view + settings panel) so the
                // layout can be eyeballed without opening a host. Off by default.
                if (juce::SystemStats::getEnvironmentVariable ("DMB_UI_SNAPSHOT", {}).isNotEmpty())
                {
                    const auto outDir = juce::File::getCurrentWorkingDirectory().getChildFile ("build");

                    auto renderTo = [editor] (const juce::File& target)
                    {
                        juce::Image shot (juce::Image::ARGB, 1060, 760, true);
                        juce::Graphics sg (shot);
                        editor->paintEntireComponent (sg, true);
                        juce::PNGImageFormat png;
                        target.deleteFile();
                        if (auto stream = target.createOutputStream())
                            png.writeImageToStream (shot, *stream);
                    };

                    renderTo (outDir.getChildFile ("editor_main.png"));

                    // find the "⚙ 设置" button anywhere in the tree and click it
                    std::function<juce::TextButton* (juce::Component&)> findSettingsButton =
                        [&findSettingsButton] (juce::Component& parent) -> juce::TextButton*
                        {
                            for (auto* child : parent.getChildren())
                            {
                                if (auto* button = dynamic_cast<juce::TextButton*> (child))
                                    if (button->getButtonText().contains (dmb::utf8 ("设置")))
                                        return button;

                                if (auto* found = findSettingsButton (*child))
                                    return found;
                            }

                            return nullptr;
                        };

                    if (auto* settingsButton = findSettingsButton (*editor))
                    {
                        settingsButton->triggerClick();
                        juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
                        renderTo (outDir.getChildFile ("editor_settings.png"));
                        dmb::logDiagnostic ("ui", "snapshot: settings panel rendered");
                    }
                }

                instance->editorBeingDeleted (editor);
                delete editor;
            }
        }

        instance->setPlayHead (nullptr);
        instance.reset();

        juce::Thread::sleep (200);

        for (const auto& file : busDir.findChildFiles (juce::File::findFiles, false, "*.dmb"))
            file.deleteFile();
    }

    // restore whatever settings the user had
    if (hadSettings)
        settingsFile.replaceWithText (settingsBackup);
    else
        settingsFile.deleteFile();
}
#endif

} // namespace

//==============================================================================
int main (int argc, char** argv)
{
    // helper mode: a child process reports the host session it resolved to, which
    // the suite uses to check the sandboxing case (one process per plug-in instance).
    // The report goes to a fixed file: command line arguments are ANSI on Windows and
    // a user name with non-ASCII characters would come through mangled.
    for (int i = 1; i < argc; ++i)
    {
        if (juce::String (argv[i]).contains ("--session-key-probe"))
        {
            juce::File::getSpecialLocation (juce::File::tempDirectory)
                .getChildFile ("dmb_session_key_probe.txt")
                .replaceWithText (dmb::ProjectSession::hostSessionKey());

            return 0;
        }
    }

    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    g_logFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("dmb_tests.log");
    g_logFile.deleteFile();
    g_log = std::make_unique<juce::FileOutputStream> (g_logFile);

    out ("DeepSeek MIDI Bridge - core verification suite");
    out ("JUCE " + juce::SystemStats::getJUCEVersion());
    out ("log file: " + g_logFile.getFullPathName());

    testCaptureBus();
    testMidiCapture();
    testJsonParsing();
    testMidiFiles();
    testPromptBuilder();
    testDeepSeekClient();
    testGenerationPipeline();
    testLivePlayback();
    testCaptureSurvivesStop();
    testSourceLibrary();
    testProjects();
    testOtherHostsAndPrivacy();
    testApiProfilesAndProbe();

#if JUCE_PLUGINHOST_VST3
    {
        juce::StringArray bundlePaths;

        for (int i = 1; i < argc; ++i)
            bundlePaths.add (juce::String (argv[i]));

        testVst3HostLoad (bundlePaths);
    }
#endif

    testRealApiIfAvailable();

    out ("\n----------------------------------------");
    out (juce::String (g_checks - g_failures) + " / " + juce::String (g_checks) + " checks passed");

    if (g_failures > 0)
        out (juce::String (g_failures) + " FAILURES");

    g_log.reset();

    return g_failures == 0 ? 0 : 1;
}
