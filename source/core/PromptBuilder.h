#pragma once

#include "DmbTypes.h"

namespace dmb
{

/** The instructions that turn the API into a MIDI generator. */
juce::String buildSystemPrompt (const GenerationSpec& spec);

/** A compact textual description of the captured tracks, plus the user's request. */
juce::String buildUserPrompt (const std::vector<SourceTrack>& sources,
                              const GenerationSpec& spec,
                              const juce::String& userRequirement);

/** Text form of a single captured source ("0.00:36:0.25:100 ..."). */
juce::String formatSourceNotes (const SourceTrack& source, int maxNotes);

/** Compact report used by the UI log (dmb::utf8 ("贝斯: 128 notes, 8 bars")). */
juce::String summariseSources (const std::vector<SourceTrack>& sources);

/** Copies a source but keeps only its first maxBars bars, and at most
    maxNotesPerSource notes (uniformly sampled). Without this a long reference file
    makes the request enormous and expensive. */
SourceTrack limitSourceForPrompt (const SourceTrack& source, int maxBars, int maxNotesPerSource, int beatsPerBar);

/** Rough token estimate for a mixed Chinese/English prompt. */
int estimateTokenCount (const juce::String& text);

//==============================================================================
/** A ready-made request the user can pick from a menu instead of typing. */
struct RequestTemplate
{
    juce::String label;        // shown in the menu
    juce::String style;
    juce::String instrument;
    juce::String keyHint;
    juce::String prompt;       // the text dropped into the request box
    int bars = 0;              // 0 = leave the current setting alone
};

std::vector<RequestTemplate> requestTemplates();

} // namespace dmb
