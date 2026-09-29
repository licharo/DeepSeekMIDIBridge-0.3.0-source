#include "ProjectStore.h"

#include "MidiFileUtil.h"
#include "Settings.h"

#include <unordered_map>

#if JUCE_WINDOWS
 #include <windows.h>
 #include <tlhelp32.h>
#endif

namespace dmb
{

namespace
{
    juce::CriticalSection& registryLock()
    {
        static juce::CriticalSection lock;
        return lock;
    }

    ProjectRegistry::Snapshot& registrySlot()
    {
        static ProjectRegistry::Snapshot slot;
        return slot;
    }

#if JUCE_WINDOWS
    struct WindowSearch
    {
        unsigned long pid = 0;
        juce::String best;
        int bestLength = -1;
    };

    BOOL CALLBACK collectWindowTitle (HWND hwnd, LPARAM param)
    {
        auto* search = reinterpret_cast<WindowSearch*> (param);

        DWORD pid = 0;
        GetWindowThreadProcessId (hwnd, &pid);

        if (pid != search->pid || ! IsWindowVisible (hwnd))
            return TRUE;

        const int length = GetWindowTextLengthW (hwnd);

        if (length <= 0)
            return TRUE;

        juce::HeapBlock<wchar_t> buffer ((size_t) length + 1, true);

        if (GetWindowTextW (hwnd, buffer.getData(), length + 1) <= 0)
            return TRUE;

        const juce::String title (buffer.getData(), (size_t) length);

        // the main window is the one with the longest title
        if (title.length() > search->bestLength)
        {
            search->bestLength = title.length();
            search->best = title;
        }

        return TRUE;
    }
#endif
}

//==============================================================================
juce::File ProjectInfo::libraryDirectory() const
{
    return folder.getChildFile (ProjectStore::kLibraryFolderName);
}

juce::File ProjectInfo::markerFile() const
{
    return folder.getChildFile (ProjectStore::kMarkerFileName);
}

//==============================================================================
namespace
{
    /** pid -> (parent pid, executable name) for every running process. */
    struct ProcessInfo
    {
        unsigned long parent = 0;
        juce::String name;
    };

   #if JUCE_WINDOWS
    std::unordered_map<unsigned long, ProcessInfo> snapshotProcesses()
    {
        std::unordered_map<unsigned long, ProcessInfo> map;

        const auto snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);

        if (snapshot == INVALID_HANDLE_VALUE)
            return map;

        PROCESSENTRY32W entry = {};
        entry.dwSize = sizeof (entry);

        if (Process32FirstW (snapshot, &entry))
        {
            do
            {
                ProcessInfo info;
                info.parent = (unsigned long) entry.th32ParentProcessID;
                info.name = juce::String (entry.szExeFile).toLowerCase();
                map[(unsigned long) entry.th32ProcessID] = info;
            }
            while (Process32NextW (snapshot, &entry));
        }

        CloseHandle (snapshot);
        return map;
    }

    bool isShellOrLauncher (const juce::String& name)
    {
        static const char* shells[] = { "explorer.exe", "services.exe", "wininit.exe", "svchost.exe",
                                        "cmd.exe", "powershell.exe", "pwsh.exe", "windowsterminal.exe",
                                        "conhost.exe", "wscript.exe", "cscript.exe", "devenv.exe",
                                        "code.exe", "steam.exe", "taskeng.exe", "runtimebroker.exe" };

        for (const auto* shell : shells)
            if (name == shell)
                return true;

        return false;
    }

    juce::String processCreationStamp (unsigned long pid)
    {
        const auto handle = OpenProcess (PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);

        if (handle == nullptr)
            return {};

        FILETIME creation = {}, exit = {}, kernel = {}, user = {};
        auto ok = GetProcessTimes (handle, &creation, &exit, &kernel, &user) != 0;
        CloseHandle (handle);

        if (! ok)
            return {};

        return juce::String (((juce::int64) creation.dwHighDateTime << 32) | (juce::int64) creation.dwLowDateTime);
    }
   #endif
}

juce::String ProjectSession::hostSessionKey()
{
   #if JUCE_WINDOWS
    const auto processes = snapshotProcesses();
    auto pid = (unsigned long) GetCurrentProcessId();
    auto hostPid = pid;

    // Walk up the parent chain to the application that started this process (the
    // plugin host); stop before the shell that launched it.
    for (int hop = 0; hop < 12; ++hop)
    {
        const auto entry = processes.find (pid);

        if (entry == processes.end() || entry->second.parent == 0 || entry->second.parent == pid)
            break;

        const auto parent = processes.find (entry->second.parent);

        if (parent == processes.end() || isShellOrLauncher (parent->second.name))
            break;

        hostPid = entry->second.parent;
        pid = entry->second.parent;
    }

    const auto stamp = processCreationStamp (hostPid);
    return (stamp.isNotEmpty() ? stamp : juce::String (hostPid));
   #else
    return juce::String ((int) juce::Time::currentTimeMillis());
   #endif
}

juce::File& sessionDirectorySlot()
{
    static juce::File dir = PluginSettings::getAppDirectory().getChildFile ("sessions");
    return dir;
}

juce::File ProjectSession::getDirectory() { return sessionDirectorySlot(); }

void ProjectSession::setDirectory (const juce::File& directory)
{
    if (directory != juce::File())
        sessionDirectorySlot() = directory;
}

void ProjectSession::resetForTests()
{
    sessionDirectorySlot() = PluginSettings::getAppDirectory().getChildFile ("sessions");
}

juce::File sessionFileFor (const juce::String& key)
{
    return ProjectSession::getDirectory().getChildFile ("session-" + sanitiseFileName (key, "host") + ".dmb");
}

ProjectInfo ProjectSession::readFresh (juce::int64 maxAgeMs, juce::int64* claimedAt, juce::String* claimedBy)
{
    ProjectInfo info;

    if (claimedAt != nullptr) *claimedAt = 0;
    if (claimedBy != nullptr) claimedBy->clear();

    const auto file = sessionFileFor (hostSessionKey());

    if (! file.existsAsFile())
        return info;

    const auto text = file.loadFileAsString();
    juce::int64 alive = 0;
    juce::int64 claimed = 0;
    juce::String hostFolder, name, id, by;

    for (const auto& line : juce::StringArray::fromLines (text))
    {
        const auto trimmed = line.trim();

        if (! trimmed.contains ("="))
            continue;

        const auto key = trimmed.upToFirstOccurrenceOf ("=", false, false).trim();
        const auto value = trimmed.fromFirstOccurrenceOf ("=", false, false).trim();

        if (key == "id")              id = value;
        else if (key == "name")       name = value;
        else if (key == "folder")     hostFolder = value;
        else if (key == "claimed")    claimed = value.getLargeIntValue();
        else if (key == "alive")      alive = value.getLargeIntValue();
        else if (key == "by")         by = value;
    }

    if (id.isEmpty() || hostFolder.isEmpty())
        return info;

    if (alive <= 0 || juce::Time::currentTimeMillis() - alive > maxAgeMs)
        return info;                       // that host session is gone

    info.id = id;
    info.name = name;
    info.folder = juce::File (hostFolder);

    if (claimedAt != nullptr) *claimedAt = claimed;
    if (claimedBy != nullptr) *claimedBy = by;

    return info;
}

void ProjectSession::write (const ProjectInfo& info, juce::int64 claimedAtMs, const juce::String& instanceId)
{
    if (! info.isValid())
        return;

    const auto dir = getDirectory();

    if (! dir.isDirectory() && ! dir.createDirectory())
        return;

    juce::StringArray lines;
    lines.add ("id=" + info.id);
    lines.add ("name=" + info.name);
    lines.add ("folder=" + info.folder.getFullPathName());
    lines.add ("host=" + hostSessionKey());
    lines.add ("claimed=" + juce::String (claimedAtMs));
    lines.add ("alive=" + juce::String (juce::Time::currentTimeMillis()));
    lines.add ("by=" + instanceId);

    auto file = sessionFileFor (hostSessionKey());
    juce::TemporaryFile temporary (file);
    auto stream = temporary.getFile().createOutputStream();

    if (stream == nullptr)
        return;

    stream->writeText (lines.joinIntoString ("\n") + "\n", false, false, "\n");
    stream->flush();
    stream.reset();
    temporary.overwriteTargetFileWithTemporary();
}

void ProjectSession::touch()
{
    ProjectRegistry::touch();
}

int ProjectSession::prune (juce::int64 maxAgeMs)
{
    const auto dir = getDirectory();

    if (! dir.isDirectory())
        return 0;

    const auto now = juce::Time::currentTimeMillis();
    int removed = 0;

    for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "session-*.dmb"))
    {
        if (now - entry.getFile().getLastModificationTime().toMilliseconds() > maxAgeMs)
            if (entry.getFile().deleteFile())
                ++removed;
    }

    return removed;
}

//==============================================================================
ProjectRegistry::Snapshot ProjectRegistry::get()
{
    {
        const juce::ScopedLock sl (registryLock());
        const auto& slot = registrySlot();

        if (slot.info.isValid())
            return slot;
    }

    // nothing in this process yet: has another process of this host decided?
    juce::int64 claimed = 0;
    juce::String by;
    const auto fromSession = ProjectSession::readFresh (8000, &claimed, &by);

    if (! fromSession.isValid())
    {
        const juce::ScopedLock sl (registryLock());
        return registrySlot();
    }

    const juce::ScopedLock sl (registryLock());
    auto& slot = registrySlot();

    if (! slot.info.isValid() || claimed > slot.changedAtMs)
    {
        slot.info = fromSession;
        slot.changedAtMs = claimed > 0 ? claimed : juce::Time::currentTimeMillis();
        slot.changedBy = by;
    }

    return slot;
}

void ProjectRegistry::publish (const ProjectInfo& info, const juce::String& instanceId)
{
    const auto now = juce::Time::currentTimeMillis();

    {
        const juce::ScopedLock sl (registryLock());

        auto& slot = registrySlot();
        slot.info = info;
        slot.changedAtMs = now;
        slot.changedBy = instanceId;
    }

    ProjectSession::write (info, now, instanceId);
}

void ProjectRegistry::touch()
{
    ProjectInfo info;
    juce::int64 claimed = 0;
    juce::String by;

    {
        const juce::ScopedLock sl (registryLock());
        info = registrySlot().info;
        claimed = registrySlot().changedAtMs;
        by = registrySlot().changedBy;
    }

    if (info.isValid())
        ProjectSession::write (info, claimed, by);
}

void ProjectRegistry::resetForTests()
{
    {
        const juce::ScopedLock sl (registryLock());
        registrySlot() = {};
    }

    sessionFileFor (ProjectSession::hostSessionKey()).deleteFile();
}

//==============================================================================
juce::File getDefaultProjectsRoot()
{
    return getDefaultOutputDirectory();
}

//==============================================================================
juce::String ProjectStore::makeProjectId()
{
    return "p-" + juce::Uuid().toDashedString().substring (0, 8);
}

ProjectInfo ProjectStore::create (const juce::File& root, const juce::String& desiredName)
{
    ProjectInfo info;

    info.id = makeProjectId();
    info.name = desiredName.trim();

    if (info.name.isEmpty())
        info.name = juce::String ("Project ") + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M");

    info.folder = root.getChildFile (makeFolderName (root, info.name));
    info.createdMs = juce::Time::currentTimeMillis();

    return info;
}

juce::String ProjectStore::makeFolderName (const juce::File& root, const juce::String& desiredName,
                                           const juce::File& ignoreFolder)
{
    auto base = sanitiseFileName (desiredName, "Project").trim();

    if (base.isEmpty())
        base = "Project";

    auto candidate = root.getChildFile (base);
    int counter = 2;

    while (candidate.isDirectory() && candidate != ignoreFolder)
    {
        candidate = root.getChildFile (base + " (" + juce::String (counter) + ")");
        ++counter;
    }

    return candidate.getFileName();
}

bool ProjectStore::writeMarker (const ProjectInfo& info)
{
    if (! info.folder.isDirectory() && ! info.folder.createDirectory())
        return false;

    auto* object = new juce::DynamicObject();
    object->setProperty ("id", info.id);
    object->setProperty ("name", info.name);
    object->setProperty ("created", (juce::int64) info.createdMs);

    const auto text = juce::JSON::toString (juce::var (object), false);

    juce::TemporaryFile temporary (info.markerFile());

    {
        auto stream = temporary.getFile().createOutputStream();

        if (stream == nullptr)
            return false;

        stream->writeText (text, false, false, "\n");
        stream->flush();
    }

    return temporary.overwriteTargetFileWithTemporary();
}

ProjectInfo ProjectStore::readMarker (const juce::File& folder)
{
    ProjectInfo info;

    const auto file = folder.getChildFile (kMarkerFileName);

    if (! file.existsAsFile())
        return info;

    const auto parsed = juce::JSON::parse (file.loadFileAsString());

    if (auto* object = parsed.getDynamicObject())
    {
        info.id = object->getProperty ("id").toString().trim();
        info.name = object->getProperty ("name").toString().trim();
        info.createdMs = (juce::int64) object->getProperty ("created");

        if (info.id.isNotEmpty())
        {
            info.folder = folder;

            if (info.name.isEmpty())
                info.name = folder.getFileName();
        }
    }

    return info;
}

bool ProjectStore::ensureFolder (const ProjectInfo& info)
{
    if (! info.isValid())
        return false;

    if (! info.folder.isDirectory() && ! info.folder.createDirectory())
        return false;

    // never take over a folder that already belongs to another project
    const auto existing = readMarker (info.folder);

    if (existing.isValid() && existing.id != info.id)
        return false;

    return writeMarker (info);
}

std::vector<ProjectInfo> ProjectStore::list (const juce::File& root)
{
    std::vector<ProjectInfo> result;

    if (! root.isDirectory())
        return result;

    for (const auto& entry : juce::RangedDirectoryIterator (root, false, "*", juce::File::findDirectories))
    {
        const auto folder = entry.getFile();

        if (! folder.isDirectory())
            continue;

        auto info = readMarker (folder);

        if (info.isValid())
            result.push_back (std::move (info));
    }

    std::sort (result.begin(), result.end(), [] (const ProjectInfo& a, const ProjectInfo& b)
    {
        return a.name.compareIgnoreCase (b.name) < 0;
    });

    return result;
}

ProjectInfo ProjectStore::rename (const ProjectInfo& info, const juce::String& newName,
                                  const juce::File& root, juce::String& error)
{
    error.clear();

    const auto name = newName.trim();

    if (name.isEmpty())
    {
        error = dmb::utf8 ("工程名不能为空");
        return {};
    }

    if (! info.isValid())
    {
        error = dmb::utf8 ("当前还没有工程");
        return {};
    }

    ProjectInfo renamed;
    renamed.id = info.id;
    renamed.name = name;
    renamed.createdMs = info.createdMs != 0 ? info.createdMs : juce::Time::currentTimeMillis();

    const bool livesInRoot = (info.folder.getParentDirectory() == root);
    const auto wanted = root.getChildFile (makeFolderName (root, name, info.folder));

    // Only move the folder when it already lives directly inside the projects root;
    // otherwise (a root the user changed in the meantime) the folder keeps its place
    // and only the display name changes.
    if (! livesInRoot)
    {
        renamed.folder = info.folder;

        if (info.folder.isDirectory() && ! writeMarker (renamed))
        {
            error = dmb::utf8 ("写工程标记失败: ") + dmb::displayPath (info.folder);
            return {};
        }

        return renamed;
    }

    renamed.folder = wanted;

    if (wanted != info.folder && info.folder.isDirectory())
    {
        if (! info.folder.moveFileTo (wanted))
        {
            error = dmb::utf8 ("移动工程文件夹失败（文件可能正被宿主占用）: ") + dmb::displayPath (info.folder);
            return {};
        }
    }
    else if (! wanted.isDirectory() && ! wanted.createDirectory())
    {
        error = dmb::utf8 ("创建工程文件夹失败: ") + dmb::displayPath (wanted);
        return {};
    }

    if (! writeMarker (renamed))
    {
        error = dmb::utf8 ("写工程标记失败: ") + dmb::displayPath (wanted);
        return {};
    }

    return renamed;
}

//==============================================================================
juce::String ProjectStore::hostProjectTitle()
{
#if JUCE_WINDOWS
    WindowSearch search;
    search.pid = (unsigned long) GetCurrentProcessId();

    EnumWindows (&collectWindowTitle, reinterpret_cast<LPARAM> (&search));

    return search.best;
#else
    return {};
#endif
}

juce::String ProjectStore::projectNameFromHostTitle (const juce::String& hostTitle)
{
    auto title = hostTitle.trim();

    if (title.isEmpty())
        return {};

    // "My Song.als - Ableton Live 12 Suite" -> "My Song.als"
    // "Song.rpp - REAPER"                  -> "Song.rpp"
    // "Project1 - Cubase"                  -> "Project1"
    if (title.contains (" - "))
        title = title.upToFirstOccurrenceOf (" - ", false, true).trim();

    title = title.trimCharactersAtStart ("*").trimCharactersAtEnd ("*").trim();

    // every DAW marks a modified project in its own way
    if (title.startsWithChar ('[') && title.contains ("]"))
        title = title.fromFirstOccurrenceOf ("]", false, false).trim();

    // drop the project file extension of the common hosts
    static const char* extensions[] = { ".als", ".als.bak", ".rpp", ".rpp-bak", ".cpr", ".npr", ".cpr.bak",
                                        ".song", ".dawproject", ".flp", ".bwproject", ".ptx", ".ptf",
                                        ".logicx", ".wrk", ".mx4", ".vpr", ".sib", ".mus", ".band" };

    for (const auto* extension : extensions)
    {
        if (title.endsWithIgnoreCase (extension))
        {
            title = title.dropLastCharacters ((int) std::strlen (extension)).trim();
            break;
        }
    }

    if (title.isEmpty())
        return {};

    // a brand new project may only show the application name - that is not a name
    const auto lower = title.toLowerCase();

    static const char* hostOnlyNames[] = { "ableton live", "live", "cubase", "nuendo", "reaper",
                                           "bitwig studio", "studio one", "fl studio", "fruity loops",
                                           "waveform", "tracktion", "mixcraft", "samplitude", "cakewalk",
                                           "sonar", "digital performer", "reason", "renoise", "ardour",
                                           "harrison mixbus", "vegas pro", "acoustica", "sound forge" };

    for (const auto* host : hostOnlyNames)
    {
        if (lower == host || lower.startsWith (juce::String (host) + " "))
            return {};
    }

    if (lower.contains ("deepseek midi bridge"))
        return {};

    return title;
}

} // namespace dmb
