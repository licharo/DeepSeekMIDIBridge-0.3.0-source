#pragma once

#include "DeepSeekClient.h"
#include "DmbTypes.h"

namespace dmb
{

/** Runs one "capture -> prompt -> DeepSeek -> .mid file" job on a background
    thread. The UI polls the state; nothing here blocks the audio thread.
*/
class GenerationService : public juce::Thread
{
public:
    enum class State { idle, running, done, failed, cancelled };

    GenerationService();
    ~GenerationService() override;

    void start (const ApiSettings& api,
                const GenerationSpec& spec,
                const juce::String& requirement,
                const std::vector<SourceTrack>& sources,
                const juce::File& outputDirectory);

    void cancel();

    bool isRunning() const noexcept;
    State getState() const noexcept;

    juce::String getStatusMessage() const;
    juce::String getLog() const;
    juce::int64 getLogVersion() const noexcept;

    /** Returns true (once) when a fresh result is available. */
    bool takeResult (GeneratedClip& clip);

    void clearResult();

    /** A line to put at the top of the next run's log (e.g. why it started). */
    void setPendingNote (const juce::String& note);

    /** The exact prompt that was / would be sent, for the dmb::utf8 ("预览提示词") button. */
    juce::String getLastPrompt() const;

    static juce::String stateToString (State state);

private:
    void run() override;
    void log (const juce::String& line);
    void setStatus (const juce::String& text);
    void setState (State newState);

    mutable juce::CriticalSection lock;

    State state = State::idle;
    juce::String statusMessage;
    juce::String logText;
    juce::int64 logVersion = 0;

    ApiSettings api;
    GenerationSpec spec;
    juce::String requirement;
    std::vector<SourceTrack> sources;
    juce::File outputDirectory;

    juce::String systemPrompt;
    juce::String pendingNote;
    juce::String userPrompt;

    GeneratedClip result;
    bool resultAvailable = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GenerationService)
};

} // namespace dmb
