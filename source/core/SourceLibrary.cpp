#include "SourceLibrary.h"
#include "CaptureBus.h"
#include "Settings.h"

namespace dmb
{

namespace
{
    constexpr const char* kFileSuffix = ".dmbsrc";

    juce::File fileForId (const juce::File& dir, const juce::String& id)
    {
        return dir.getChildFile (sanitiseFileName (id, "entry") + kFileSuffix);
    }

    bool writeBlockAtomically (const juce::File& dir, const juce::File& target, const juce::MemoryBlock& block)
    {
        if (! dir.isDirectory() && ! dir.createDirectory())
            return false;

        const auto temp = dir.getChildFile (target.getFileName() + ".tmp");

        {
            juce::FileOutputStream stream (temp, 65536);

            if (! stream.openedOk())
                return false;

            stream.write (block.getData(), block.getSize());
            stream.flush();
        }

        if (! temp.moveFileTo (target))
        {
            temp.copyFileTo (target);
            temp.deleteFile();
        }

        return true;
    }
}

juce::File& SourceLibrary::defaultSlot()
{
    static juce::File dir = getLegacyDirectory();
    return dir;
}

juce::File SourceLibrary::getLegacyDirectory()
{
    return PluginSettings::getAppDirectory().getChildFile ("library");
}

juce::File SourceLibrary::getDirectory() { return defaultSlot(); }

void SourceLibrary::setDirectory (const juce::File& directory)
{
    if (directory != juce::File())
        defaultSlot() = directory;
}

juce::String SourceLibrary::idFromFile (const juce::File& file)
{
    return file.getFileNameWithoutExtension();
}

//==============================================================================
juce::String SourceLibrary::save (SourceTrack source)
{
    return save (getDirectory(), std::move (source));
}

juce::String SourceLibrary::save (const juce::File& directory, SourceTrack source)
{
    if (! directory.isDirectory() && ! directory.createDirectory())
        return {};

    if (source.id.isEmpty() || ! source.id.startsWith ("lib-"))
        source.id = "lib-" + juce::Uuid().toDashedString().substring (0, 8);

    if (source.stampMs == 0)
        source.stampMs = juce::Time::currentTimeMillis();

    const auto block = CaptureBus::serialiseTrack (source);
    const auto target = fileForId (directory, source.id);

    if (! writeBlockAtomically (directory, target, block))
        return {};

    prune (directory);

    return source.id;
}

bool SourceLibrary::remove (const juce::String& id)
{
    return remove (getDirectory(), id);
}

bool SourceLibrary::remove (const juce::File& directory, const juce::String& id)
{
    if (! directory.isDirectory())
        return false;

    const auto file = fileForId (directory, id);

    // File::deleteFile() also reports success for files that never existed
    return file.existsAsFile() && file.deleteFile();
}

bool SourceLibrary::rename (const juce::String& id, const juce::String& newName)
{
    return rename (getDirectory(), id, newName);
}

bool SourceLibrary::rename (const juce::File& directory, const juce::String& id, const juce::String& newName)
{
    if (! directory.isDirectory())
        return false;

    const auto file = fileForId (directory, id);

    if (! file.existsAsFile())
        return false;

    SourceTrack source;
    juce::MemoryBlock block;

    if (! file.loadFileAsData (block) || ! CaptureBus::deserialiseTrack (block, source))
        return false;

    source.name = newName.trim();

    if (source.name.isEmpty())
        return false;

    return writeBlockAtomically (directory, file, CaptureBus::serialiseTrack (source));
}

std::vector<SourceTrack> SourceLibrary::loadAll()
{
    return loadAll (getDirectory());
}

std::vector<SourceTrack> SourceLibrary::loadAll (const juce::File& directory)
{
    std::vector<SourceTrack> result;

    if (! directory.isDirectory())
        return result;

    for (const auto& entry : juce::RangedDirectoryIterator (directory, false, juce::String ("*") + kFileSuffix))
    {
        const auto file = entry.getFile();
        juce::MemoryBlock block;

        if (! file.loadFileAsData (block))
            continue;

        SourceTrack source;

        if (! CaptureBus::deserialiseTrack (block, source))
            continue;

        if (source.notes.empty())
            continue;

        if (source.id.isEmpty())
            source.id = idFromFile (file);

        if (source.stampMs == 0)
            source.stampMs = entry.getModificationTime().toMilliseconds();

        result.push_back (std::move (source));
    }

    std::sort (result.begin(), result.end(),
               [] (const SourceTrack& a, const SourceTrack& b) { return a.stampMs < b.stampMs; });

    return result;
}

int SourceLibrary::removeByOrigin (const juce::String& originPrefix)
{
    return removeByOrigin (getDirectory(), originPrefix);
}

int SourceLibrary::removeByOrigin (const juce::File& directory, const juce::String& originPrefix)
{
    if (! directory.isDirectory())
        return 0;

    int removed = 0;

    for (const auto& entry : juce::RangedDirectoryIterator (directory, false, juce::String ("*") + kFileSuffix))
    {
        const auto file = entry.getFile();
        SourceTrack source;
        juce::MemoryBlock block;

        if (! file.loadFileAsData (block) || ! CaptureBus::deserialiseTrack (block, source))
            continue;

        if (source.origin.startsWith (originPrefix) && file.deleteFile())
            ++removed;
    }

    return removed;
}

void SourceLibrary::prune (int maxEntries)
{
    prune (getDirectory(), maxEntries);
}

void SourceLibrary::prune (const juce::File& directory, int maxEntries)
{
    if (! directory.isDirectory())
        return;

    struct Entry
    {
        juce::File file;
        juce::int64 time = 0;
    };

    std::vector<Entry> entries;

    for (const auto& item : juce::RangedDirectoryIterator (directory, false, juce::String ("*") + kFileSuffix))
        entries.push_back ({ item.getFile(), item.getModificationTime().toMilliseconds() });

    if ((int) entries.size() <= maxEntries)
        return;

    std::sort (entries.begin(), entries.end(),
               [] (const Entry& a, const Entry& b) { return a.time < b.time; });

    for (int i = 0; i < (int) entries.size() - maxEntries; ++i)
        entries[(size_t) i].file.deleteFile();
}

//==============================================================================
int SourceLibrary::countEntries (const juce::File& directory)
{
    if (! directory.isDirectory())
        return 0;

    int count = 0;

    for (const auto& entry : juce::RangedDirectoryIterator (directory, false, juce::String ("*") + kFileSuffix))
    {
        juce::ignoreUnused (entry);
        ++count;
    }

    return count;
}

int SourceLibrary::moveEntriesTo (const juce::File& from, const juce::File& to)
{
    if (! from.isDirectory())
        return 0;

    if (! to.isDirectory() && ! to.createDirectory())
        return 0;

    int moved = 0;

    for (const auto& entry : juce::RangedDirectoryIterator (from, false, juce::String ("*") + kFileSuffix))
    {
        const auto file = entry.getFile();
        const auto target = to.getChildFile (file.getFileName());

        if (target.existsAsFile())
            target.deleteFile();

        if (file.moveFileTo (target) || (file.copyFileTo (target) && file.deleteFile()))
            ++moved;
    }

    return moved;
}

} // namespace dmb
