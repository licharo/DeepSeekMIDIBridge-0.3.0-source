#include "ApiProbe.h"

namespace dmb
{

ApiProbe::ApiProbe()
    : juce::Thread ("dmb-api-probe")
{
}

ApiProbe::~ApiProbe()
{
    signalThreadShouldExit();
    stopThread (5000);
}

//==============================================================================
juce::String ApiProbe::describeKey (const juce::String& apiKey)
{
    auto key = apiKey.trim();

    if (key.isEmpty())
        return utf8 ("未设置 Key");

    if (key.length() <= 12)
        return utf8 ("Key: ") + key;

    return utf8 ("Key: ") + key.substring (0, 6) + "..." + key.substring (key.length() - 4);
}

//==============================================================================
void ApiProbe::start (const ApiSettings& settings)
{
    if (isThreadRunning())
        return;

    {
        const juce::ScopedLock sl (lock);
        api = settings;
        api.maxTokens = juce::jlimit (16, 64, api.maxTokens);
        api.timeoutMs = juce::jlimit (5000, 60000, api.timeoutMs);
        result = Result();
    }

    startThread();
}

void ApiProbe::cancel()
{
    signalThreadShouldExit();
}

ApiProbe::Result ApiProbe::getResult() const
{
    const juce::ScopedLock sl (lock);
    return result;
}

void ApiProbe::clearResult()
{
    const juce::ScopedLock sl (lock);
    result = Result();
}

//==============================================================================
void ApiProbe::run()
{
    ApiSettings local;

    {
        const juce::ScopedLock sl (lock);
        local = api;
    }

    ApiSettings probe = local;
    probe.maxTokens = 32;
    probe.jsonMode = false;      // keep the check as cheap as possible

    const auto response = DeepSeekClient::call (probe,
                                                utf8 ("你是连接测试助手，只回复一个词。"),
                                                utf8 ("回复：OK"),
                                                0.0,
                                                [this] { return threadShouldExit(); });

    Result newResult;
    newResult.valid = true;
    newResult.statusCode = response.statusCode;
    newResult.model = local.model;
    newResult.elapsedMs = response.elapsedMs;
    newResult.content = response.content.substring (0, 80).trim();
    newResult.ok = response.ok;

    if (response.ok)
    {
        newResult.message = utf8 ("连接正常 · ") + local.model
                          + " · " + juce::String (response.elapsedMs / 1000.0, 1) + "s"
                          + (newResult.content.isNotEmpty() ? (" · " + newResult.content) : juce::String());
    }
    else
    {
        newResult.message = (threadShouldExit() ? utf8 ("已取消") : response.error.isNotEmpty()
                                                                     ? response.error
                                                                     : utf8 ("未知错误"));
    }

    {
        const juce::ScopedLock sl (lock);
        result = newResult;
    }
}

} // namespace dmb
