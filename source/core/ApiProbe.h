#pragma once

#include "DeepSeekClient.h"

namespace dmb
{

/** Fires one tiny request at the API so the user can immediately tell whether the
    key / endpoint / model they just switched to actually works.
*/
class ApiProbe : public juce::Thread
{
public:
    struct Result
    {
        bool valid = false;          // a result has been produced
        bool ok = false;
        int statusCode = 0;
        juce::String message;        // human readable outcome
        juce::String model;
        juce::String content;
        juce::int64 elapsedMs = 0;
    };

    ApiProbe();
    ~ApiProbe() override;

    /** Starts a check (ignored when one is already running). */
    void start (const ApiSettings& settings);

    void cancel();

    bool isRunning() const noexcept { return isThreadRunning(); }

    /** Returns the last result; valid == false when nothing has been checked yet. */
    Result getResult() const;

    void clearResult();

    /** Wording used by the UI for the current key state. */
    static juce::String describeKey (const juce::String& apiKey);

private:
    void run() override;

    mutable juce::CriticalSection lock;
    ApiSettings api;
    Result result;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ApiProbe)
};

} // namespace dmb
