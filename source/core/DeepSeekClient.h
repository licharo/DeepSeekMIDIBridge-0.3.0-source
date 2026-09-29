#pragma once

#include "DmbTypes.h"

#include <functional>

namespace dmb
{

struct DeepSeekResult
{
    bool ok = false;
    int statusCode = 0;
    juce::String content;        // the assistant message
    juce::String reasoning;      // deepseek-reasoner chain of thought, if any
    juce::String error;
    juce::String raw;
    juce::String finishReason;
    juce::var usage;
    juce::int64 elapsedMs = 0;
};

/** Minimal, dependency free DeepSeek (OpenAI compatible) chat client. */
class DeepSeekClient
{
public:
    using CancelCheck = std::function<bool()>;
    using ProgressCallback = std::function<void (juce::int64 bytesReceived)>;

    /** Blocking; call from a background thread. */
    static DeepSeekResult call (const ApiSettings& settings,
                                const juce::String& systemPrompt,
                                const juce::String& userPrompt,
                                double temperature,
                                CancelCheck shouldCancel = {},
                                ProgressCallback progress = {});

    static juce::String buildRequestBodyJson (const ApiSettings& settings,
                                              const juce::String& systemPrompt,
                                              const juce::String& userPrompt,
                                              double temperature);

    /** Split out so it can be unit tested without a network. */
    static DeepSeekResult parseResponseBody (const juce::String& body, int statusCode);
};

} // namespace dmb
