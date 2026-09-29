#include "PromptBuilder.h"

namespace dmb
{

namespace
{
    juce::String pitchName (int pitch)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        const auto octave = pitch / 12 - 1;
        return juce::String (names[pitch % 12]) + juce::String (octave);
    }
}

//==============================================================================
juce::String formatSourceNotes (const SourceTrack& source, int maxNotes)
{
    if (source.notes.empty())
        return dmb::utf8 ("(无音符)");

    const auto ppq = (double) juce::jmax (1, source.ppq);

    const int total = (int) source.notes.size();
    const int step = (maxNotes > 0 && total > maxNotes) ? juce::jmax (1, total / maxNotes) : 1;

    juce::StringArray chunks;
    chunks.ensureStorageAllocated (juce::jmin (total, maxNotes) + 4);

    for (int i = 0; i < total; i += step)
    {
        const auto& n = source.notes[(size_t) i];

        chunks.add (juce::String (n.tick / ppq, 3) + ":"
                    + juce::String (n.pitch) + ":"
                    + juce::String (n.lengthTicks / ppq, 3) + ":"
                    + juce::String (n.velocity));
    }

    juce::String out;

    for (int i = 0; i < chunks.size(); ++i)
    {
        out << chunks[i];

        if (i + 1 < chunks.size())
            out << " ";
    }

    if (step > 1)
        out << dmb::utf8 ("   (每 ") << step << dmb::utf8 (" 个音符抽样一次, 共 ") << total << dmb::utf8 (" 个)");

    return out;
}

//==============================================================================
juce::String summariseSources (const std::vector<SourceTrack>& sources)
{
    if (sources.empty())
        return dmb::utf8 ("没有捕获到任何音轨");

    juce::StringArray lines;

    for (const auto& s : sources)
        lines.add (s.describe());

    return lines.joinIntoString ("\n");
}

//==============================================================================
juce::String buildSystemPrompt (const GenerationSpec& spec)
{
    juce::String p;

    p << dmb::utf8 ("你是一位专业的 MIDI 编曲助手。用户会给你一段或多段已经存在的 MIDI 轨道数据"
         "(格式为 start:音高:时值:力度, 单位为四分音符/拍, start 相对于片段开头), "
"以及一段自然语言要求。请根据这些素材创作新的 MIDI 内容。\n\n");

    p << dmb::utf8 ("只输出一个 JSON 对象, 不要输出解释文字, 不要使用 markdown 代码块。JSON 结构:\n");
    p << dmb::utf8 ("{\n"
                    "  \"name\": \"简短的中文或英文文件名(不要包含空格和斜杠)\",\n"
                    "  \"explanation\": \"一句话说明你的编曲思路\",\n"
                    "  \"bpm\": 120,\n"
                    "  \"bars\": 8,\n"
                    "  \"unit\": \"beats\",\n"
                    "  \"tracks\": [\n"
                    "    {\n"
                    "      \"name\": \"轨道名\",\n"
                    "      \"channel\": 1,\n"
                    "      \"notes\": [\n"
                    "        { \"start\": 0.0, \"pitch\": 36, \"length\": 0.25, \"velocity\": 110 }\n"
                    "      ]\n"
                    "    }\n"
                    "  ]\n"
                    "}\n\n");

    p << dmb::utf8 ("规则:\n");
    p << dmb::utf8 ("- start/length 的单位是拍(四分音符), length 必须大于 0; 也可以给 \"unit\": \"ticks\" 并把 start/length 写成 tick。\n");
    p << dmb::utf8 ("- pitch 使用标准 MIDI 编号 0-127。鼓组请使用 GM 映射: 36=底鼓, 38=军鼓, 42=闭镲, 46=开镲, 39=拍手, 49=吊镲, 45/47/50=桶鼓。\n");
    p << dmb::utf8 ("- 音符按时间排序, 不要有重复或重叠到不合理的音符; 力度 1-127。\n");
    p << dmb::utf8 ("- 生成的小节数必须等于 ") << spec.bars << dmb::utf8 (" 小节, 每小节 ") << spec.beatsPerBar << dmb::utf8 (" 拍, 不要超出该长度。\n");
    p << dmb::utf8 ("- 最多 ") << spec.maxNotes << dmb::utf8 (" 个音符。\n");
    p << dmb::utf8 ("- 如果要求里包含多条轨(例如鼓 + 贝斯), 就把它们放在 tracks 数组里, 每条轨用不同的 name 和 channel。\n");

    if (spec.keepGroove)
        p << dmb::utf8 ("- 保持源轨道的律动、节奏密度和音高走向, 让新内容能与之配合。\n");

    if (spec.key.isNotEmpty())
        p << dmb::utf8 ("- 调性/音阶: ") << spec.key << dmb::utf8 ("。\n");

    if (spec.instrument.isNotEmpty())
        p << dmb::utf8 ("- 目标乐器/音色: ") << spec.instrument << dmb::utf8 ("。\n");

    if (spec.style.isNotEmpty())
        p << dmb::utf8 ("- 风格: ") << spec.style << dmb::utf8 ("。\n");

    return p;
}

//==============================================================================
juce::String buildUserPrompt (const std::vector<SourceTrack>& sources,
                              const GenerationSpec& spec,
                              const juce::String& userRequirement)
{
    juce::String p;

    p << dmb::utf8 ("=== 宿主里其他音轨的 MIDI 素材 (start:音高:时值:力度, 单位=拍) ===\n");

    if (sources.empty())
    {
        p << dmb::utf8 ("(没有捕获到其他音轨, 请根据下面的要求自由创作)\n");
    }
    else
    {
        const int perSourceBudget = juce::jmax (64, spec.maxNotes / juce::jmax (1, (int) sources.size()));

        for (const auto& original : sources)
        {
            const auto s = limitSourceForPrompt (original, spec.referenceBars, perSourceBudget, spec.beatsPerBar);
            const bool trimmed = s.notes.size() != original.notes.size();

            p << dmb::utf8 ("\n[音轨] ") << s.name
              << dmb::utf8 (" | 速度 ") << juce::String (s.bpm, 1) << " BPM"
              << dmb::utf8 (" | 长度 ") << juce::String (s.barCount()) << dmb::utf8 (" 小节")
              << dmb::utf8 (" | 音符 ") << (int) s.notes.size()
              << dmb::utf8 (" | 音高范围 ") << pitchName (s.lowestPitch()) << "-" << pitchName (s.highestPitch());

            if (trimmed)
                p << dmb::utf8 (" (原文件 ") << (int) original.notes.size()
                  << dmb::utf8 (" 个音符, 这里只取前 ") << juce::jmax (1, spec.referenceBars) << dmb::utf8 (" 小节)");

            p << "\n";
            p << formatSourceNotes (s, perSourceBudget) << "\n";
        }
    }

    p << dmb::utf8 ("\n=== 用户要求 ===\n");
    p << (userRequirement.trim().isEmpty() ? juce::String (dmb::utf8 ("请创作一段能与上面素材配合的内容。"))
                                           : userRequirement.trim()) << "\n";

    p << dmb::utf8 ("\n=== 输出参数 ===\n");
    p << dmb::utf8 ("小节数: ") << spec.bars << dmb::utf8 (", 拍号: ") << spec.beatsPerBar << "/4"
      << dmb::utf8 (", 默认通道: ") << spec.channel
      << dmb::utf8 (", 最多音符数: ") << spec.maxNotes << "\n";

    p << dmb::utf8 ("\n请只返回 JSON。");

    return p;
}

//==============================================================================
SourceTrack limitSourceForPrompt (const SourceTrack& source, int maxBars, int maxNotesPerSource, int beatsPerBar)
{
    SourceTrack copy = source;

    const auto ppq = (double) juce::jmax (1, copy.ppq);
    const auto limitTicks = (double) juce::jmax (1, maxBars) * (double) juce::jmax (1, beatsPerBar) * ppq;

    copy.notes.erase (std::remove_if (copy.notes.begin(), copy.notes.end(),
                                      [limitTicks] (const Note& n) { return (double) n.tick >= limitTicks; }),
                      copy.notes.end());

    if (maxNotesPerSource > 0 && (int) copy.notes.size() > maxNotesPerSource)
    {
        const auto step = juce::jmax (1, (int) std::ceil ((double) copy.notes.size() / (double) maxNotesPerSource));
        std::vector<Note> sampled;
        sampled.reserve (copy.notes.size() / (size_t) step + 1);

        for (size_t i = 0; i < copy.notes.size(); i += (size_t) step)
            sampled.push_back (copy.notes[i]);

        copy.notes = std::move (sampled);
    }

    copy.lastTick = juce::jmin (copy.lastTick, limitTicks);

    if (copy.notes.empty())
        copy.lastTick = 0.0;

    return copy;
}

int estimateTokenCount (const juce::String& text)
{
    // Very rough: DeepSeek's tokenizer spends about one token per CJK character and
    // about a quarter of a token per ASCII character. Good enough for a warning.
    double tokens = 0.0;

    for (auto c : text)
        tokens += (c > 127 ? 1.0 : 0.25);

    return (int) std::ceil (tokens);
}

//==============================================================================
std::vector<RequestTemplate> requestTemplates()
{
    std::vector<RequestTemplate> templates;

    auto add = [&templates] (const juce::String& label, const juce::String& style,
                             const juce::String& instrument, const juce::String& keyHint,
                             const juce::String& prompt, int bars)
    {
        RequestTemplate item;
        item.label = label;
        item.style = style;
        item.instrument = instrument;
        item.keyHint = keyHint;
        item.prompt = prompt;
        item.bars = bars;
        templates.push_back (std::move (item));
    };

    add (utf8 ("鼓组 · Drum & Bass"), "Drum & Bass", utf8 ("鼓组"), {},
         utf8 ("根据参考素材写鼓组：kick 对齐参考轨的根音，2、4 拍放军鼓，"
               "加入鬼音军鼓和 offbeat 开镲，保持切分感，不要盖住中频"), 4);

    add (utf8 ("鼓组 · House/Tech"), "House", utf8 ("鼓组"), {},
         utf8 ("根据参考素材写鼓组：四踩底鼓，2、4 拍拍手，offbeat 开镲，"
               "偶尔加一个 1/16 的闭合踩镲做律动"), 4);

    add (utf8 ("贝斯线"), {}, utf8 ("贝斯"), {},
         utf8 ("根据参考素材写贝斯线：贴合和声根音，音符不要与参考轨冲突，"
               "加入切分和八度跳进，留出呼吸空间"), 4);

    add (utf8 ("和弦铺底 / Pad"), {}, utf8 ("Pad 弦乐"), {},
         utf8 ("根据参考素材写和弦铺底：跟随和声走向，以长音为主，"
               "避免与主旋律同音区打架，整体音量温和"), 8);

    add (utf8 ("加花 / 过门"), {}, utf8 ("鼓组"), {},
         utf8 ("写 1 小节过门 fill：在最后一拍加入密集鼓点，"
               "力度做渐强，用来衔接下一段"), 1);

    add (utf8 ("对位旋律"), {}, utf8 ("主音"), {},
         utf8 ("根据参考素材写一条对位旋律：与参考动机形成呼应，"
               "节奏上错开，句尾留空，音域高一个八度"), 4);

    return templates;
}

} // namespace dmb
