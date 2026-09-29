#include "DiagnosticsLog.h"
#include "Settings.h"

namespace dmb
{

namespace
{
    constexpr juce::int64 kMaxLogSize = 256 * 1024;

    juce::CriticalSection& logLock()
    {
        static juce::CriticalSection lock;
        return lock;
    }
}

juce::File getDiagnosticsLogFile()
{
    return PluginSettings::getAppDirectory().getChildFile ("diagnostics.log");
}

void logDiagnostic (const juce::String& category, const juce::String& message)
{
    const juce::ScopedLock sl (logLock());

    const auto file = getDiagnosticsLogFile();

    if (file.existsAsFile() && file.getSize() > kMaxLogSize)
        file.deleteFile();

    const auto line = juce::Time::getCurrentTime().toString (true, true, true, true)
                    + "  [" + category + "]  " + message + "\n";

    file.appendText (line, false, false);
}

juce::String readDiagnosticsLog (int maxLines)
{
    const juce::ScopedLock sl (logLock());

    const auto file = getDiagnosticsLogFile();

    if (! file.existsAsFile())
        return {};

    auto lines = juce::StringArray::fromLines (file.loadFileAsString());
    lines.removeEmptyStrings();

    if (maxLines > 0 && lines.size() > maxLines)
        lines.removeRange (0, lines.size() - maxLines);

    return lines.joinIntoString ("\n");
}

void clearDiagnosticsLog()
{
    const juce::ScopedLock sl (logLock());
    getDiagnosticsLogFile().deleteFile();
}

} // namespace dmb
