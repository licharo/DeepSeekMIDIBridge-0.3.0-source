#include "DeepSeekClient.h"

namespace dmb
{

juce::String DeepSeekClient::buildRequestBodyJson (const ApiSettings& settings,
                                                   const juce::String& systemPrompt,
                                                   const juce::String& userPrompt,
                                                   double temperature)
{
    juce::Array<juce::var> messages;

    {
        auto* system = new juce::DynamicObject();
        system->setProperty ("role", "system");
        system->setProperty ("content", systemPrompt);
        messages.add (juce::var (system));
    }

    {
        auto* user = new juce::DynamicObject();
        user->setProperty ("role", "user");
        user->setProperty ("content", userPrompt);
        messages.add (juce::var (user));
    }

    auto* root = new juce::DynamicObject();
    root->setProperty ("model", settings.model.isNotEmpty() ? settings.model : juce::String ("deepseek-chat"));
    root->setProperty ("messages", messages);
    root->setProperty ("temperature", juce::jlimit (0.0, 2.0, temperature));
    root->setProperty ("max_tokens", juce::jlimit (256, 8192, settings.maxTokens));
    root->setProperty ("stream", false);

    if (settings.jsonMode)
    {
        auto* format = new juce::DynamicObject();
        format->setProperty ("type", "json_object");
        root->setProperty ("response_format", juce::var (format));
    }

    // Truly compact: "model":"x" rather than "model": "x" (fewer tokens on the wire).
    return juce::JSON::toString (juce::var (root),
                                 juce::JSON::FormatOptions().withSpacing (juce::JSON::Spacing::none));
}

//==============================================================================
DeepSeekResult DeepSeekClient::parseResponseBody (const juce::String& body, int statusCode)
{
    DeepSeekResult result;
    result.statusCode = statusCode;
    result.raw = body;

    juce::var root;
    const auto parseResult = juce::JSON::parse (body, root);

    if (! parseResult.wasOk())
    {
        result.error = dmb::utf8 ("服务器返回的不是 JSON (HTTP ") + juce::String (statusCode) + "): "
                     + body.substring (0, 300);
        return result;
    }

    if (auto* rootObject = root.getDynamicObject())
        if (auto* errorObject = rootObject->getProperty ("error").getDynamicObject())
            result.error = errorObject->getProperty ("message").toString();

    if (statusCode < 200 || statusCode >= 300)
    {
        if (result.error.isEmpty())
            result.error = "HTTP " + juce::String (statusCode) + ": " + body.substring (0, 300);

        return result;
    }

    if (auto* rootObject = root.getDynamicObject())
    {
        result.usage = rootObject->getProperty ("usage");

        if (auto* choices = rootObject->getProperty ("choices").getArray())
        {
            if (! choices->isEmpty())
            {
                const auto first = choices->getReference (0);

                if (auto* choice = first.getDynamicObject())
                {
                    result.finishReason = choice->getProperty ("finish_reason").toString();

                    if (auto* message = choice->getProperty ("message").getDynamicObject())
                    {
                        result.content = message->getProperty ("content").toString();
                        result.reasoning = message->getProperty ("reasoning_content").toString();
                    }
                }
            }
        }
    }

    if (result.content.trim().isEmpty())
    {
        if (result.error.isEmpty())
            result.error = dmb::utf8 ("模型返回了空内容 (finish_reason=") + result.finishReason + ")";

        return result;
    }

    result.ok = true;
    return result;
}

//==============================================================================
DeepSeekResult DeepSeekClient::call (const ApiSettings& settings,
                                     const juce::String& systemPrompt,
                                     const juce::String& userPrompt,
                                     double temperature,
                                     CancelCheck shouldCancel,
                                     ProgressCallback progress)
{
    const auto startTime = juce::Time::currentTimeMillis();

    auto finish = [&startTime] (DeepSeekResult r) -> DeepSeekResult
    {
        r.elapsedMs = juce::Time::currentTimeMillis() - startTime;
        return r;
    };

    if (settings.apiKey.trim().isEmpty())
    {
        DeepSeekResult result;
        result.error = dmb::utf8 ("没有设置 DeepSeek API Key (请在插件界面顶部填入)");
        return finish (result);
    }

    juce::URL url (settings.endpoint.trim());

    if (! url.isWellFormed())
    {
        DeepSeekResult result;
        result.error = dmb::utf8 ("API 地址不合法: ") + settings.endpoint;
        return finish (result);
    }

    const auto body = buildRequestBodyJson (settings, systemPrompt, userPrompt, temperature);
    url = url.withPOSTData (body);

    juce::WebInputStream stream (url, true);

    const auto headers = "Content-Type: application/json\r\n"
                         "Authorization: Bearer " + settings.apiKey.trim() + "\r\n"
                         "Accept: application/json";

    stream.withExtraHeaders (headers);
    stream.withConnectionTimeout (juce::jlimit (5000, 600000, settings.timeoutMs));
    stream.withNumRedirectsToFollow (5);

    if (! stream.connect (nullptr))
    {
        DeepSeekResult result;
        result.statusCode = stream.getStatusCode();
        result.error = dmb::utf8 ("无法连接 ") + url.getDomain() + dmb::utf8 (" (代码 ") + juce::String (result.statusCode) + ")";
        return finish (result);
    }

    juce::MemoryOutputStream response;

    {
        char buffer[8192];

        while (! stream.isExhausted())
        {
            if (shouldCancel && shouldCancel())
            {
                stream.cancel();
                DeepSeekResult result;
                result.statusCode = stream.getStatusCode();
                result.error = dmb::utf8 ("已取消");
                return finish (result);
            }

            const auto numRead = stream.read (buffer, (int) sizeof (buffer));

            if (numRead <= 0)
                break;

            response.write (buffer, (size_t) numRead);

            if (progress)
                progress ((juce::int64) response.getDataSize());
        }
    }

    auto result = parseResponseBody (response.toString(), stream.getStatusCode());
    return finish (result);
}

} // namespace dmb
