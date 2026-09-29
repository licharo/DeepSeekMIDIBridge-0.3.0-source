#include "GenerationService.h"
#include "MidiFileUtil.h"
#include "MidiJson.h"
#include "PromptBuilder.h"

namespace dmb
{

GenerationService::GenerationService()
    : juce::Thread ("dmb-generation")
{
    result = GeneratedClip();
}

GenerationService::~GenerationService()
{
    cancel();
    stopThread (5000);
}

//==============================================================================
juce::String GenerationService::stateToString (State s)
{
    switch (s)
    {
        case State::idle:      return dmb::utf8 ("空闲");
        case State::running:   return dmb::utf8 ("生成中");
        case State::done:      return dmb::utf8 ("完成");
        case State::failed:    return dmb::utf8 ("失败");
        case State::cancelled: return dmb::utf8 ("已取消");
    }

    return {};
}

//==============================================================================
void GenerationService::start (const ApiSettings& newApi,
                               const GenerationSpec& newSpec,
                               const juce::String& newRequirement,
                               const std::vector<SourceTrack>& newSources,
                               const juce::File& newOutputDirectory)
{
    if (isThreadRunning())
        return;

    {
        const juce::ScopedLock sl (lock);
        api = newApi;
        spec = newSpec;
        requirement = newRequirement;
        sources = newSources;
        outputDirectory = newOutputDirectory;
        state = State::running;
        statusMessage = dmb::utf8 ("正在构建提示词...");
        logText.clear();
        result = GeneratedClip();
        resultAvailable = false;
        ++logVersion;
    }

    startThread();
}

void GenerationService::cancel()
{
    signalThreadShouldExit();
}

bool GenerationService::isRunning() const noexcept
{
    return isThreadRunning();
}

GenerationService::State GenerationService::getState() const noexcept
{
    const juce::ScopedLock sl (lock);
    return state;
}

juce::String GenerationService::getStatusMessage() const
{
    const juce::ScopedLock sl (lock);
    return statusMessage;
}

juce::String GenerationService::getLog() const
{
    const juce::ScopedLock sl (lock);
    return logText;
}

juce::int64 GenerationService::getLogVersion() const noexcept
{
    const juce::ScopedLock sl (lock);
    return logVersion;
}

juce::String GenerationService::getLastPrompt() const
{
    const juce::ScopedLock sl (lock);
    return "=== system ===\n" + systemPrompt + "\n\n=== user ===\n" + userPrompt;
}

bool GenerationService::takeResult (GeneratedClip& clip)
{
    const juce::ScopedLock sl (lock);

    if (! resultAvailable)
        return false;

    clip = result;
    resultAvailable = false;
    return true;
}

void GenerationService::setPendingNote (const juce::String& note)
{
    const juce::ScopedLock sl (lock);
    pendingNote = note;
}

void GenerationService::clearResult()
{
    const juce::ScopedLock sl (lock);
    resultAvailable = false;
    result = GeneratedClip();
}

//==============================================================================
void GenerationService::log (const juce::String& line)
{
    const juce::ScopedLock sl (lock);
    logText << line << "\n";
    ++logVersion;
}

void GenerationService::setStatus (const juce::String& text)
{
    const juce::ScopedLock sl (lock);
    statusMessage = text;
    ++logVersion;
}

void GenerationService::setState (State newState)
{
    const juce::ScopedLock sl (lock);
    state = newState;
    ++logVersion;
}

//==============================================================================
void GenerationService::run()
{
    const auto started = juce::Time::currentTimeMillis();

    ApiSettings localApi;
    GenerationSpec localSpec;
    juce::String localRequirement;
    std::vector<SourceTrack> localSources;
    juce::File localOutput;

    {
        const juce::ScopedLock sl (lock);
        localApi = api;
        localSpec = spec;
        localRequirement = requirement;
        localSources = sources;
        localOutput = outputDirectory;
        systemPrompt = buildSystemPrompt (localSpec);
        userPrompt = buildUserPrompt (localSources, localSpec, localRequirement);
    }

    if (pendingNote.isNotEmpty())
    {
        log (pendingNote);
        pendingNote.clear();
    }

    log (dmb::utf8 ("模型: ") + localApi.model + dmb::utf8 ("    地址: ") + localApi.endpoint);
    log (dmb::utf8 ("参考素材: ") + summariseSources (localSources).replace ("\n", " | "));
    log (dmb::utf8 ("提示词长度: ") + juce::String (systemPrompt.length() + userPrompt.length()) + dmb::utf8 (" 字符"));

    setStatus (dmb::utf8 ("正在请求 DeepSeek..."));

    auto response = DeepSeekClient::call (localApi, systemPrompt, userPrompt, localSpec.temperature,
                                          [this] { return threadShouldExit(); },
                                          [this] (juce::int64 bytes)
                                          {
                                              setStatus (dmb::utf8 ("正在接收回复... ") + juce::String (bytes / 1024.0, 1) + " KB");
                                          });

    if (threadShouldExit())
    {
        log (dmb::utf8 ("已取消"));
        setStatus (dmb::utf8 ("已取消"));
        setState (State::cancelled);
        return;
    }

    log ("HTTP " + juce::String (response.statusCode)
         + dmb::utf8 ("   耗时 ") + juce::String (response.elapsedMs / 1000.0, 1) + dmb::utf8 (" 秒"));

    if (auto* usage = response.usage.getDynamicObject())
    {
        const auto prompt = (int) usage->getProperty ("prompt_tokens");
        const auto completion = (int) usage->getProperty ("completion_tokens");
        const auto total = (int) usage->getProperty ("total_tokens");

        if (total > 0)
            log ("Token: prompt " + juce::String (prompt) + " + completion " + juce::String (completion)
                 + " = " + juce::String (total));
    }

    if (! response.ok)
    {
        log (dmb::utf8 ("请求失败: ") + response.error);
        setStatus (dmb::utf8 ("请求失败: ") + response.error);
        setState (State::failed);
        return;
    }

    setStatus (dmb::utf8 ("正在解析 MIDI..."));

    GeneratedClip clip;
    juce::String parseError;

    if (! parseGenerationResult (response.content, kTicksPerQuarter, localSpec.bars,
                                 localSpec.beatsPerBar, localSpec.channel, localSpec.transpose,
                                 clip, parseError))
    {
        log (dmb::utf8 ("解析失败: ") + parseError);
        log (dmb::utf8 ("模型原文前 500 字符: ") + response.content.substring (0, 500));
        setStatus (dmb::utf8 ("解析失败: ") + parseError);
        setState (State::failed);
        return;
    }

    clip.model = localApi.model;
    clip.usageSummary = "HTTP " + juce::String (response.statusCode) + dmb::utf8 (" · ")
                      + juce::String (response.elapsedMs / 1000.0, 1) + dmb::utf8 (" 秒 · ")
                      + juce::String (clip.totalNotes()) + dmb::utf8 (" 音符");

    if (clip.explanation.isNotEmpty())
        log (dmb::utf8 ("说明: ") + clip.explanation);

    log (dmb::utf8 ("生成 ") + juce::String (clip.tracks.size()) + dmb::utf8 (" 条轨, 共 ") + juce::String (clip.totalNotes())
         + dmb::utf8 (" 个音符, ") + juce::String (clip.bars) + dmb::utf8 (" 小节, ") + juce::String (clip.bpm, 1) + " BPM");

    setStatus (dmb::utf8 ("正在写入 MIDI 文件..."));

    juce::String writeError;

    if (! writeClipToDisk (clip, localOutput, writeError))
    {
        log (dmb::utf8 ("写文件失败: ") + writeError);
        setStatus (dmb::utf8 ("写文件失败: ") + writeError);
        setState (State::failed);
        return;
    }

    log (dmb::utf8 ("已保存: ") + dmb::displayPath (clip.file));

    for (const auto& file : clip.perTrackFiles)
        log ("         " + file.getFileName());

    {
        const juce::ScopedLock sl (lock);
        result = clip;
        resultAvailable = true;
        state = State::done;
        statusMessage = dmb::utf8 ("完成: ") + juce::String (clip.totalNotes()) + dmb::utf8 (" 个音符, 已保存 .mid (")
                      + juce::String ((juce::Time::currentTimeMillis() - started) / 1000.0, 1) + dmb::utf8 (" 秒)");
        ++logVersion;
    }
}

} // namespace dmb
