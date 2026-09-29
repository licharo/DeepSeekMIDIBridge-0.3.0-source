#pragma once

#include <juce_core/juce_core.h>

namespace dmb
{

/** A tiny append-only log used to diagnose things that cannot be seen from the UI
    (drag & drop between the plugin and the host, host quirks, ...).

    The file lives next to the other settings so it can be inspected afterwards:
    %LOCALAPPDATA%\DeepSeekMidiBridge\diagnostics.log
*/
void logDiagnostic (const juce::String& category, const juce::String& message);

juce::File getDiagnosticsLogFile();

/** Returns the last maxLines lines (for showing them in the UI). */
juce::String readDiagnosticsLog (int maxLines = 200);

void clearDiagnosticsLog();

} // namespace dmb
