#include "CaptureBus.h"

#include <cstring>

namespace dmb
{

namespace
{
    juce::File defaultRoot()
    {
        // Lets tests (and users with unusual setups) redirect the bus elsewhere.
        const auto redirected = juce::SystemStats::getEnvironmentVariable ("DMB_BUS_DIR", {}).trim();

        if (redirected.isNotEmpty())
            return juce::File (redirected);

        auto base = juce::File::getSpecialLocation (juce::File::windowsLocalAppData);

        if (! base.isDirectory())
            base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);

        return base.getChildFile ("DeepSeekMidiBridge").getChildFile ("bus");
    }

    juce::File& rootSlot()
    {
        static juce::File root = defaultRoot();
        return root;
    }

    const char* magic = "DMBC";
}

//==============================================================================
juce::File CaptureBus::getRootDirectory() { return getRoot(); }

void CaptureBus::setRootDirectory (const juce::File& dir)
{
    if (dir != juce::File())
        rootSlot() = dir;
}

juce::File CaptureBus::getRoot() { return rootSlot(); }

juce::String CaptureBus::makeInstanceId()
{
    return juce::Uuid().toDashedString().substring (0, 8);
}

bool CaptureBus::isRedirectedByEnvironment()
{
    return juce::SystemStats::getEnvironmentVariable ("DMB_BUS_DIR", {}).trim().isNotEmpty();
}

juce::File CaptureBus::fileForId (const juce::File& dir, const juce::String& id)
{
    return dir.getChildFile (sanitiseFileName (id, "instance") + ".dmb");
}

//==============================================================================
juce::MemoryBlock CaptureBus::serialiseTrack (const SourceTrack& s)
{
    juce::MemoryOutputStream out;

    out.write (magic, 4);
    out.writeInt (kFormatVersion);
    out.writeInt (0);                                  // flags, reserved
    out.writeInt (s.ppq);
    out.writeDouble (s.bpm);
    out.writeDouble (s.firstTick);
    out.writeDouble (s.lastTick);
    out.writeBool (s.playing);
    out.writeInt64 (s.stampMs);

    const auto idBytes = s.id.toRawUTF8();
    const auto idLen = (int) std::strlen (idBytes);
    out.writeInt (idLen);
    out.write (idBytes, (size_t) idLen);

    const auto nameBytes = s.name.toRawUTF8();
    const auto nameLen = (int) std::strlen (nameBytes);
    out.writeInt (nameLen);
    out.write (nameBytes, (size_t) nameLen);

    const auto originBytes = s.origin.toRawUTF8();
    const auto originLen = (int) std::strlen (originBytes);
    out.writeInt (originLen);
    out.write (originBytes, (size_t) originLen);

    const auto projectBytes = s.projectId.toRawUTF8();
    const auto projectLen = (int) std::strlen (projectBytes);
    out.writeInt (projectLen);
    out.write (projectBytes, (size_t) projectLen);

    const auto count = juce::jmin ((int) s.notes.size(), kMaxNotesPerSource);
    out.writeInt (count);

    for (int i = 0; i < count; ++i)
    {
        const auto& n = s.notes[(size_t) i];
        out.writeInt (n.tick);
        out.writeShort ((juce::int16) juce::jlimit (0, 127, n.pitch));
        out.writeByte ((char) juce::jlimit (1, 127, n.velocity));
        out.writeByte ((char) juce::jlimit (1, 16, n.channel));
        out.writeInt (juce::jmax (1, n.lengthTicks));
    }

    return out.getMemoryBlock();
}

bool CaptureBus::deserialiseTrack (const juce::MemoryBlock& block, SourceTrack& result)
{
    if (block.getSize() < 8)
        return false;

    juce::MemoryInputStream in (block, false);

    char m[4] = {};

    if (in.read (m, 4) != 4 || std::memcmp (m, magic, 4) != 0)
        return false;

    const auto version = in.readInt();

    if (version != kFormatVersion)
        return false;

    in.readInt();                                     // flags
    result.ppq = juce::jlimit (24, 9600, in.readInt());
    result.bpm = in.readDouble();
    result.firstTick = in.readDouble();
    result.lastTick = in.readDouble();
    result.playing = in.readBool();
    result.stampMs = in.readInt64();

    if (result.bpm < 1.0 || result.bpm > 1000.0)
        result.bpm = 120.0;

    auto readString = [&in]() -> juce::String
    {
        const auto len = in.readInt();

        if (len <= 0 || len > 4096 || (juce::int64) len > in.getNumBytesRemaining())
            return {};

        juce::HeapBlock<char> buffer ((size_t) len + 1, true);
        in.read (buffer.getData(), len);
        return juce::String::fromUTF8 (buffer.getData(), len);
    };

    result.id = readString();
    result.name = readString();
    result.origin = readString();
    result.projectId = readString();

    const auto count = in.readInt();

    if (count < 0 || count > kMaxNotesPerSource)
        return false;

    if ((juce::int64) count * 11 > in.getNumBytesRemaining())
        return false;

    result.notes.clear();
    result.notes.reserve ((size_t) count);

    for (int i = 0; i < count; ++i)
    {
        Note n;
        n.tick = in.readInt();
        n.pitch = (int) (juce::int16) in.readShort();
        n.velocity = (int) (juce::uint8) in.readByte();
        n.channel = (int) (juce::uint8) in.readByte();
        n.lengthTicks = in.readInt();

        n.pitch = juce::jlimit (0, 127, n.pitch);
        n.velocity = juce::jlimit (1, 127, n.velocity);
        n.channel = juce::jlimit (1, 16, n.channel);
        n.lengthTicks = juce::jmax (1, n.lengthTicks);

        result.notes.push_back (n);
    }

    return true;
}

//==============================================================================
void CaptureBus::publish (const SourceTrack& source)
{
    auto dir = getRoot();

    if (! dir.isDirectory() && ! dir.createDirectory())
        return;

    const auto block = serialiseTrack (source);
    const auto target = fileForId (dir, source.id);
    const auto tmp = dir.getChildFile (target.getFileName() + ".tmp");

    {
        juce::FileOutputStream stream (tmp, 65536);

        if (! stream.openedOk())
            return;

        stream.write (block.getData(), block.getSize());
        stream.flush();
    }

    if (! tmp.moveFileTo (target))
    {
        // Move failed (e.g. the target is locked by a reader): fall back to a copy.
        tmp.copyFileTo (target);
        tmp.deleteFile();
    }
}

void CaptureBus::remove (const juce::String& instanceId)
{
    const auto dir = getRoot();

    if (! dir.isDirectory())
        return;

    fileForId (dir, instanceId).deleteFile();
}

std::vector<SourceTrack> CaptureBus::readAll (juce::int64 maxAgeMs, const juce::String& ignoreInstanceId,
                                              const juce::String& projectId)
{
    std::vector<SourceTrack> result;

    const auto dir = getRoot();

    if (! dir.isDirectory())
        return result;

    const auto now = juce::Time::currentTimeMillis();

    for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*.dmb"))
    {
        const auto file = entry.getFile();

        if (entry.getFile().getFileName().endsWithIgnoreCase (".tmp"))
            continue;

        if (now - entry.getModificationTime().toMilliseconds() > maxAgeMs)
            continue;

        SourceTrack source;

        juce::MemoryBlock block;

        if (! file.loadFileAsData (block))
            continue;

        if (! deserialiseTrack (block, source))
            continue;

        if (source.id.isEmpty())
            source.id = file.getFileNameWithoutExtension();

        if (source.id == ignoreInstanceId)
            continue;

        // Material that belongs to another host project is not shown at all.
        if (projectId.isNotEmpty() && source.projectId != projectId)
            continue;

        source.stampMs = entry.getModificationTime().toMilliseconds();
        result.push_back (std::move (source));
    }

    std::sort (result.begin(), result.end(),
               [] (const SourceTrack& a, const SourceTrack& b) { return a.name < b.name; });

    return result;
}

int CaptureBus::pruneExpired (juce::int64 maxAgeMs)
{
    const auto dir = getRoot();

    if (! dir.isDirectory())
        return 0;

    const auto now = juce::Time::currentTimeMillis();
    int removed = 0;

    for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*.dmb"))
    {
        if (now - entry.getModificationTime().toMilliseconds() > maxAgeMs)
            if (entry.getFile().deleteFile())
                ++removed;
    }

    return removed;
}

} // namespace dmb
