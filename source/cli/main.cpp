/*
    dmb-cli - the headless half of DeepSeek MIDI Bridge.

    Reads one or more reference .mid files, asks the DeepSeek API for new material
    that fits them, and writes the result as .mid files. Everything is done through
    the same core the plugin uses, so this is also a quick way to check the
    pipeline without a DAW.

    Example:
        dmb-cli --reference bass.mid --prompt "写 8 小节 Drum & Bass 鼓组" --out C:\midi
*/

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <iostream>

#include "core/DeepSeekClient.h"
#include "core/DmbTypes.h"
#include "core/MidiFileUtil.h"
#include "core/MidiJson.h"
#include "core/PromptBuilder.h"
#include "core/Settings.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace
{

struct Options
{
    juce::StringArray references;
    juce::String prompt;
    juce::File outputDirectory;
    dmb::GenerationSpec spec;
    dmb::ApiSettings api;
    bool printPromptOnly = false;
    bool resave = false;
    bool quiet = false;
};

void print (const juce::String& text)
{
    std::cout << text.toRawUTF8() << std::endl;
}

void printError (const juce::String& text)
{
    std::cerr << text.toRawUTF8() << std::endl;
}

juce::StringArray getUnicodeArguments (int argc, char* argv[])
{
   #if JUCE_WINDOWS
    int wideArgc = 0;

    if (auto** wideArgv = CommandLineToArgvW (GetCommandLineW(), &wideArgc))
    {
        juce::StringArray result;

        for (int i = 0; i < wideArgc; ++i)
            result.add (juce::String (wideArgv[i]));

        LocalFree (wideArgv);

        if (! result.isEmpty())
            return result;
    }
   #endif

    juce::StringArray result;

    for (int i = 0; i < argc; ++i)
        result.add (juce::String::fromUTF8 (argv[i]));

    return result;
}

void printUsage()
{
    print (juce::String::fromUTF8 (u8R"(
dmb-cli - 用 DeepSeek 根据参考 MIDI 生成新的 MIDI 文件

用法:
  dmb-cli --reference <输入.mid> [--reference <输入2.mid> ...] (--prompt <要求> | --prompt-file <utf8.txt>) [选项]

主要选项:
  --reference <file>      参考 MIDI 文件, 可重复多次
  --prompt <text>         你的要求
  --prompt-file <file>    从 UTF-8 文本文件读取要求 (中文更稳)
  --out <dir>             输出目录 (默认: 文档\DeepSeek MIDI Bridge)
  --bars <n>              生成小节数 (默认 8)
  --beats-per-bar <n>     每小节拍数 (默认 4)
  --style <text>          风格提示, 例如 "Drum & Bass"
  --instrument <text>     乐器/音色提示, 例如 "鼓组"
  --key <text>            调性/音阶, 例如 "F# minor"
  --channel <1-16>        默认 MIDI 通道 (默认 1)
  --transpose <n>         结果移调半音
  --temperature <f>       创意程度 0-2 (默认 0.7)
  --max-notes <n>         生成音符上限 (默认 2000)
  --reference-bars <n>    每条参考轨只取前 n 小节发给模型 (默认 16, 越小越省钱)
  --model <name>          模型 (默认 deepseek-chat)
  --endpoint <url>        接口地址 (默认 https://api.deepseek.com/chat/completions)
  --api-key <key>         API Key (默认取 DEEPSEEK_API_KEY 或插件保存的设置)
  --print-prompt          只打印将要发送的提示词, 不请求 API
  --resave                把 --reference 的 MIDI 重新写出（用于修复旧版写坏的 end-of-track）
  --quiet                 安静模式
  --help, -h              显示本帮助

退出码: 0 成功 / 1 参数错误 / 2 请求失败 / 3 解析或写盘失败
)"));
}

juce::String takeValue (const juce::StringArray& args, int& index, const juce::String& optionName)
{
    const auto& current = args[index];
    const auto eq = current.indexOfChar ('=');

    if (eq > 0)
        return current.substring (eq + 1);

    if (index + 1 >= args.size())
    {
        printError (juce::String::fromUTF8 (u8"选项缺少参数: ") + optionName);
        std::exit (1);
    }

    return args[++index];
}

bool matches (const juce::String& argument, const juce::String& longName, const juce::String& shortName = {})
{
    if (argument == longName)
        return true;

    if (shortName.isNotEmpty() && argument == shortName)
        return true;

    return argument.startsWith (longName + "=");
}

} // namespace

//==============================================================================
int main (int argc, char* argv[])
{
   #if JUCE_WINDOWS
    SetConsoleOutputCP (CP_UTF8);
   #endif

    const auto args = getUnicodeArguments (argc, argv);

    Options options;
    options.spec = dmb::GenerationSpec();
    options.api = dmb::ApiSettings();

    const auto savedSettings = dmb::PluginSettings::load();
    options.outputDirectory = dmb::getDefaultOutputDirectory();

    if (savedSettings.api.apiKey.isNotEmpty())
        options.api.apiKey = savedSettings.api.apiKey;

    if (savedSettings.api.endpoint.isNotEmpty())
        options.api.endpoint = savedSettings.api.endpoint;

    if (savedSettings.outputDirectory.isNotEmpty())
        options.outputDirectory = juce::File (savedSettings.outputDirectory);

    juce::File promptFile;
    juce::String bpmText;

    for (int i = 1; i < args.size(); ++i)
    {
        const auto argument = args[i];

        if (argument == "--help" || argument == "-h" || argument == "/?")
        {
            printUsage();
            return 0;
        }

        if (matches (argument, "--reference"))
            options.references.add (takeValue (args, i, "--reference"));
        else if (matches (argument, "--prompt"))
            options.prompt = takeValue (args, i, "--prompt");
        else if (matches (argument, "--prompt-file"))
            promptFile = juce::File (takeValue (args, i, "--prompt-file"));
        else if (matches (argument, "--out"))
            options.outputDirectory = juce::File (takeValue (args, i, "--out"));
        else if (matches (argument, "--bars"))
            options.spec.bars = juce::jlimit (1, 256, takeValue (args, i, "--bars").getIntValue());
        else if (matches (argument, "--beats-per-bar"))
            options.spec.beatsPerBar = juce::jlimit (1, 16, takeValue (args, i, "--beats-per-bar").getIntValue());
        else if (matches (argument, "--style"))
            options.spec.style = takeValue (args, i, "--style");
        else if (matches (argument, "--instrument"))
            options.spec.instrument = takeValue (args, i, "--instrument");
        else if (matches (argument, "--key"))
            options.spec.key = takeValue (args, i, "--key");
        else if (matches (argument, "--channel"))
            options.spec.channel = juce::jlimit (1, 16, takeValue (args, i, "--channel").getIntValue());
        else if (matches (argument, "--transpose"))
            options.spec.transpose = juce::jlimit (-48, 48, takeValue (args, i, "--transpose").getIntValue());
        else if (matches (argument, "--temperature"))
            options.spec.temperature = juce::jlimit (0.0, 2.0, takeValue (args, i, "--temperature").getDoubleValue());
        else if (matches (argument, "--max-notes"))
            options.spec.maxNotes = juce::jlimit (16, 8000, takeValue (args, i, "--max-notes").getIntValue());
        else if (matches (argument, "--reference-bars"))
            options.spec.referenceBars = juce::jlimit (1, 512, takeValue (args, i, "--reference-bars").getIntValue());
        else if (matches (argument, "--model"))
            options.api.model = takeValue (args, i, "--model");
        else if (matches (argument, "--endpoint"))
            options.api.endpoint = takeValue (args, i, "--endpoint");
        else if (matches (argument, "--api-key"))
            options.api.apiKey = takeValue (args, i, "--api-key");
        else if (matches (argument, "--bpm"))
            bpmText = takeValue (args, i, "--bpm");
        else if (argument == "--print-prompt")
            options.printPromptOnly = true;
        else if (argument == "--resave")
            options.resave = true;
        else if (argument == "--quiet")
            options.quiet = true;
        else
        {
            printError (juce::String::fromUTF8 (u8"无法识别的参数: ") + argument);
            printUsage();
            return 1;
        }
    }

    if (promptFile != juce::File())
    {
        if (! promptFile.existsAsFile())
        {
            printError (juce::String::fromUTF8 (u8"找不到提示词文件: ") + promptFile.getFullPathName());
            return 1;
        }

        options.prompt = promptFile.loadFileAsString();
    }

    if (options.references.isEmpty() && options.prompt.trim().isEmpty())
    {
        printUsage();
        return 1;
    }

    // ---- reference material ------------------------------------------------
    std::vector<dmb::SourceTrack> sources;

    for (const auto& path : options.references)
    {
        const juce::File file (path);
        juce::String error;
        auto imported = dmb::importMidiFile (file, error);

        if (imported.empty())
        {
            printError ((error.isNotEmpty() ? error : juce::String::fromUTF8 (u8"无法读取参考文件: ")) + file.getFullPathName());
            return 1;
        }

        if (! options.quiet)
            print (juce::String::fromUTF8 (u8"参考: ") + file.getFileName() + "  ->  "
                   + juce::String ((int) imported.size()) + juce::String::fromUTF8 (u8" 条轨"));

        for (auto& source : imported)
            sources.push_back (std::move (source));
    }

    const auto systemPrompt = dmb::buildSystemPrompt (options.spec);
    const auto userPrompt = dmb::buildUserPrompt (sources, options.spec, options.prompt);

    if (! options.quiet)
    {
        print (dmb::summariseSources (sources));
        const auto estimatedTokens = dmb::estimateTokenCount (systemPrompt) + dmb::estimateTokenCount (userPrompt);

        print (juce::String::fromUTF8 (u8"提示词: ") + juce::String (systemPrompt.length() + userPrompt.length())
               + juce::String::fromUTF8 (u8" 字符, 约 ") + juce::String (estimatedTokens)
               + juce::String::fromUTF8 (u8" tokens, 模型: ") + options.api.model);

        if (estimatedTokens > 12000)
            print (juce::String::fromUTF8 (u8"提示: 参考素材偏大, 可用 --reference-bars 减小开销 (参考轨只保留前 n 小节)"));
    }

    if (options.printPromptOnly)
    {
        print ("\n=== system ===\n" + systemPrompt + "\n=== user ===\n" + userPrompt);
        return 0;
    }

    if (options.resave)
    {
        // Re-write the reference files with the current writer. Used to repair .mid
        // files whose end-of-track marker was written at tick 0 by older builds.
        for (const auto& path : options.references)
        {
            const juce::File file (path);
            juce::String error;
            auto tracks = dmb::importMidiFile (file, error);

            if (tracks.empty())
            {
                printError ((error.isNotEmpty() ? error : juce::String::fromUTF8 (u8"无法读取: ")) + path);
                return 3;
            }

            dmb::GeneratedClip clip;
            clip.title = file.getFileNameWithoutExtension();
            clip.bpm = tracks.front().bpm > 1.0 ? tracks.front().bpm : 120.0;
            clip.ppq = tracks.front().ppq;
            clip.beatsPerBar = 4;
            clip.explanation = juce::String::fromUTF8 (u8"由 --resave 重新写出");

            double lastTick = 0.0;

            for (auto& track : tracks)
            {
                dmb::GeneratedTrack generated;
                generated.name = track.name.isNotEmpty() ? track.name : juce::String ("Track");
                generated.channel = track.notes.empty() ? 1 : track.notes.front().channel;
                generated.notes = track.notes;

                for (const auto& note : track.notes)
                    lastTick = juce::jmax (lastTick, (double) note.endTick());

                clip.tracks.push_back (std::move (generated));
            }

            clip.bars = juce::jmax (1, (int) std::ceil (lastTick / ((double) clip.ppq * 4.0)));

            juce::String writeError;

            if (! dmb::writeClipToDisk (clip, options.outputDirectory, writeError))
            {
                printError (writeError);
                return 3;
            }

            print (juce::String::fromUTF8 (u8"已重新写出: ") + clip.file.getFullPathName()
                   + "  (" + juce::String (clip.totalNotes()) + juce::String::fromUTF8 (u8" 个音符, ")
                   + juce::String (clip.bars) + juce::String::fromUTF8 (u8" 小节)"));
        }

        return 0;
    }

    if (options.api.apiKey.trim().isEmpty())
    {
        printError (juce::String::fromUTF8 (u8"没有 API Key: 请用 --api-key 传入, 或设置环境变量 DEEPSEEK_API_KEY"));
        return 1;
    }

    // ---- request -----------------------------------------------------------
    const auto started = juce::Time::currentTimeMillis();
    auto cancelled = false;

    const auto response = dmb::DeepSeekClient::call (
        options.api, systemPrompt, userPrompt, options.spec.temperature,
        [&cancelled] { return cancelled; },
        [&options] (juce::int64 bytes)
        {
            if (! options.quiet)
                std::cout << "\r" << juce::String::fromUTF8 (u8"接收中... ").toRawUTF8()
                          << (int) (bytes / 1024) << " KB" << std::flush;
        });

    if (! options.quiet)
        std::cout << "\r" << std::string (40, ' ') << "\r" << std::flush;

    if (! response.ok)
    {
        printError (juce::String::fromUTF8 (u8"请求失败 (HTTP ") + juce::String (response.statusCode)
                    + "): " + response.error);
        return 2;
    }

    if (! options.quiet)
    {
        if (auto* usage = response.usage.getDynamicObject())
        {
            const auto total = (int) usage->getProperty ("total_tokens");

            if (total > 0)
                print (juce::String::fromUTF8 (u8"token: ") + juce::String (total));
        }

        print (juce::String::fromUTF8 (u8"耗时: ") + juce::String ((juce::Time::currentTimeMillis() - started) / 1000.0, 1)
               + juce::String::fromUTF8 (u8" 秒"));
    }

    // ---- parse + write -----------------------------------------------------
    dmb::GeneratedClip clip;
    juce::String parseError;

    if (! dmb::parseGenerationResult (response.content, dmb::kTicksPerQuarter, options.spec.bars,
                                      options.spec.beatsPerBar, options.spec.channel, options.spec.transpose,
                                      clip, parseError))
    {
        printError (juce::String::fromUTF8 (u8"解析失败: ") + parseError);
        printError (juce::String::fromUTF8 (u8"模型原文前 600 字符:\n") + response.content.substring (0, 600));
        return 3;
    }

    if (bpmText.isNotEmpty())
        clip.bpm = juce::jlimit (20.0, 400.0, bpmText.getDoubleValue());
    else if (std::abs (clip.bpm - 120.0) < 0.001 && ! sources.empty() && sources.front().bpm > 1.0)
        clip.bpm = sources.front().bpm;   // the model did not state a tempo: follow the reference

    clip.model = options.api.model;

    juce::String writeError;

    if (! dmb::writeClipToDisk (clip, options.outputDirectory, writeError))
    {
        printError (writeError);
        return 3;
    }

    print (juce::String::fromUTF8 (u8"标题: ") + clip.title);
    print (juce::String::fromUTF8 (u8"说明: ") + clip.explanation);
    print (juce::String::fromUTF8 (u8"生成: ") + juce::String (clip.totalNotes())
           + juce::String::fromUTF8 (u8" 个音符, ") + juce::String (clip.tracks.size())
           + juce::String::fromUTF8 (u8" 条轨, ") + juce::String (clip.bars)
           + juce::String::fromUTF8 (u8" 小节, ") + juce::String (clip.bpm, 1) + " BPM");
    print (juce::String::fromUTF8 (u8"文件: ") + clip.file.getFullPathName());

    for (const auto& file : clip.perTrackFiles)
        print ("      " + file.getFullPathName());

    return 0;
}
