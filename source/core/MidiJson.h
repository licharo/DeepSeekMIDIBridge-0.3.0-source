#pragma once

#include "DmbTypes.h"

namespace dmb
{

/** Pulls the first complete JSON object out of a model reply. Language models
    sometimes wrap their answer in markdown fences or add a sentence before or
    after the JSON, so this has to be tolerant.
*/
juce::String extractJsonObject (const juce::String& text);

/** Converts a model reply (JSON) into a GeneratedClip.
    start/length values are interpreted as beats unless the payload says
    otherwise ("unit" may be "beats", "ticks", "steps" or "bars", and an
    individual note may use an explicit "tick" field).
*/
bool parseGenerationResult (const juce::String& text,
                            int defaultPpq,
                            int defaultBars,
                            int defaultBeatsPerBar,
                            int defaultChannel,
                            int transposeSemitones,
                            GeneratedClip& result,
                            juce::String& error);

/** Serialisation used to keep the generated material inside the host project. */
juce::var generatedClipToVar (const GeneratedClip& clip);
bool generatedClipFromVar (const juce::var& value, GeneratedClip& clip);

} // namespace dmb
