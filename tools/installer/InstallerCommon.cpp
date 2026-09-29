#include "InstallerCommon.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <tlhelp32.h>
#endif

namespace dmbin
{

const juce::String kReadmeName      { u8 ("安装说明.txt") };
const juce::String kUninstallerName { u8 ("卸载 DeepSeek MIDI Bridge.exe") };

namespace
{
    constexpr int kMaxPayloadEntries = 4096;

    juce::String normalise (const juce::File& file)
    {
        auto path = file.getFullPathName().replaceCharacter ('/', '\\');

        while (path.endsWithChar ('\\') && path.length() > 3)
            path = path.dropLastCharacters (1);

        return path.toLowerCase();
    }

    juce::File exeFile()
    {
        return juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    }

    bool writeWholeFile (const juce::File& target, const void* data, size_t size, juce::String& error)
    {
        const auto parent = target.getParentDirectory();

        if (! parent.isDirectory() && ! parent.createDirectory())
        {
            error = u8 ("无法创建目录: ") + parent.getFullPathName();
            return false;
        }

        // remove a read-only file that is in the way (never a directory)
        if (target.existsAsFile() && ! target.hasWriteAccess())
            target.setReadOnly (false);

        juce::TemporaryFile temporary (target);
        auto stream = temporary.getFile().createOutputStream();

        if (stream == nullptr)
        {
            error = u8 ("无法写入: ") + target.getFullPathName();
            return false;
        }

        stream->write (data, size);
        stream->flush();
        stream.reset();

        if (! temporary.overwriteTargetFileWithTemporary())
        {
            error = u8 ("无法替换文件: ") + target.getFullPathName();
            return false;
        }

        return true;
    }
}

//==============================================================================
bool Payload::pack (const juce::File& contentDir, const juce::File& selfExe, const juce::File& outExe,
                    juce::StringArray& log, juce::String& error)
{
    error.clear();
    log.clear();

    if (! contentDir.isDirectory())
    {
        error = u8 ("要打包的目录不存在: ") + contentDir.getFullPathName();
        return false;
    }

    juce::MemoryOutputStream buffer;

    {
        juce::FileInputStream self (selfExe);

        if (! self.openedOk())
        {
            error = u8 ("无法读取安装程序自身: ") + selfExe.getFullPathName();
            return false;
        }

        buffer.writeFromInputStream (self, -1);
    }

    std::vector<PayloadEntry> entries;

    for (const auto& entry : juce::RangedDirectoryIterator (contentDir, true, "*", juce::File::findFiles))
    {
        const auto file = entry.getFile();
        const auto relative = file.getRelativePathFrom (contentDir).replaceCharacter ('\\', '/');

        juce::MemoryOutputStream compressed;

        {
            juce::FileInputStream in (file);

            if (! in.openedOk())
            {
                error = u8 ("无法读取: ") + file.getFullPathName();
                return false;
            }

            juce::GZIPCompressorOutputStream gzip (compressed, 6);
            gzip.writeFromInputStream (in, -1);
        }

        PayloadEntry item;
        item.name = relative;
        item.offset = (juce::int64) buffer.getDataSize();
        item.compressedSize = (juce::int64) compressed.getDataSize();
        item.originalSize = file.getSize();

        if (item.originalSize <= 0 || item.compressedSize <= 0)
        {
            error = u8 ("空文件，拒绝打包: ") + file.getFullPathName();
            return false;
        }

        buffer.write (compressed.getData(), compressed.getDataSize());
        entries.push_back (item);

        log.add (relative + "  " + juce::String (item.originalSize / 1024) + " KB -> "
                  + juce::String (item.compressedSize / 1024) + " KB");
    }

    if (entries.empty())
    {
        error = u8 ("目录里没有文件: ") + contentDir.getFullPathName();
        return false;
    }

    const auto indexOffset = (juce::int64) buffer.getDataSize();

    buffer.writeInt ((int) entries.size());

    for (const auto& item : entries)
    {
        const auto nameBytes = item.name.toRawUTF8();
        const auto nameLen = (int) std::strlen (nameBytes);

        buffer.writeInt (nameLen);
        buffer.write (nameBytes, (size_t) nameLen);
        buffer.writeInt64 (item.offset);
        buffer.writeInt64 (item.compressedSize);
        buffer.writeInt64 (item.originalSize);
    }

    buffer.writeInt64 (indexOffset);
    buffer.write (kPayloadMagic, 16);

    if (outExe.existsAsFile())
        outExe.deleteFile();

    {
        auto stream = outExe.createOutputStream();

        if (stream == nullptr)
        {
            error = u8 ("无法写入 ") + outExe.getFullPathName();
            return false;
        }

        stream->write (buffer.getData(), buffer.getDataSize());
        stream->flush();
    }

    log.add (u8 ("总共 ") + juce::String (entries.size()) + u8 (" 个文件, 安装程序 ")
             + juce::String (outExe.getSize() / 1024) + " KB");

    return true;
}

bool Payload::readFrom (const juce::File& selfExe, juce::String& error)
{
    entries.clear();
    error.clear();

    juce::FileInputStream in (selfExe);

    if (! in.openedOk())
    {
        error = u8 ("无法读取安装程序自身: ") + selfExe.getFullPathName();
        return false;
    }

    const auto totalSize = in.getTotalLength();

    if (totalSize < kPayloadTrailerSize)
    {
        error = u8 ("这个 exe 里没有安装内容（需要用打包过的安装程序）");
        return false;
    }

    in.setPosition (totalSize - kPayloadTrailerSize);

    const auto indexOffset = in.readInt64();
    char magic[17] = {};
    in.read (magic, 16);

    if (juce::String (magic, 16) != juce::String (kPayloadMagic, 16))
    {
        error = u8 ("安装内容损坏（尾部标记不对）");
        return false;
    }

    if (indexOffset <= 0 || indexOffset >= totalSize - kPayloadTrailerSize)
    {
        error = u8 ("安装内容损坏（索引位置不对）");
        return false;
    }

    in.setPosition (indexOffset);

    const auto count = (int) in.readInt();

    if (count <= 0 || count > kMaxPayloadEntries)
    {
        error = u8 ("安装内容损坏（文件数量不对）");
        return false;
    }

    for (int i = 0; i < count; ++i)
    {
        const auto nameLen = (int) in.readInt();

        if (nameLen <= 0 || nameLen > 4096)
        {
            error = u8 ("安装内容损坏（文件名长度不对）");
            return false;
        }

        juce::HeapBlock<char> name ((size_t) nameLen + 1, true);
        in.read (name.getData(), nameLen);

        PayloadEntry item;
        item.name = juce::String::fromUTF8 (name.getData(), nameLen);
        item.offset = in.readInt64();
        item.compressedSize = in.readInt64();
        item.originalSize = in.readInt64();

        if (item.name.contains ("..") || item.name.startsWithChar ('/')
            || item.offset <= 0 || item.compressedSize <= 0 || item.originalSize <= 0)
        {
            error = u8 ("安装内容损坏（条目无效: ") + item.name + ")";
            return false;
        }

        entries.push_back (item);
    }

    return true;
}

bool Payload::readEntry (const juce::File& selfExe, const PayloadEntry& entry,
                         juce::MemoryBlock& decompressed, juce::String& error)
{
    error.clear();

    juce::FileInputStream in (selfExe);

    if (! in.openedOk() || ! in.setPosition (entry.offset))
    {
        error = u8 ("无法读取安装内容: ") + entry.name;
        return false;
    }

    juce::MemoryBlock compressed ((size_t) entry.compressedSize);

    if (in.read (compressed.getData(), (int) entry.compressedSize) != (int) entry.compressedSize)
    {
        error = u8 ("安装内容不完整: ") + entry.name;
        return false;
    }

    juce::MemoryInputStream memory (compressed, false);
    juce::GZIPDecompressorInputStream gzip (memory);
    juce::MemoryOutputStream out (decompressed, false);
    out.writeFromInputStream (gzip, -1);
    out.flush();                                   // trims the block to what was written

    const auto written = (juce::int64) out.getDataSize();

    if (written != entry.originalSize)
    {
        error = u8 ("解压后大小不对: ") + entry.name
              + u8 ("  期望 ") + juce::String (entry.originalSize)
              + u8 (" 字节, 实际 ") + juce::String (written)
              + u8 (" 字节 (压缩后 ") + juce::String (entry.compressedSize) + u8 (")");
        return false;
    }

    return true;
}

//==============================================================================
juce::File getProgramFilesDirectory()
{
    return juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory);
}

juce::File getDefaultVst3Directory()
{
    return getProgramFilesDirectory().getChildFile ("Common Files").getChildFile ("VST3");
}

juce::File getPerUserVst3Directory()
{
    return juce::File::getSpecialLocation (juce::File::windowsLocalAppData)
               .getChildFile ("Programs").getChildFile ("Common").getChildFile ("VST3");
}

juce::File getAppDataDirectory()
{
    return juce::File::getSpecialLocation (juce::File::windowsLocalAppData).getChildFile ("DeepSeekMidiBridge");
}

juce::File getUserMediaDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile (kProductName);
}

bool canWriteInto (const juce::File& directory)
{
    auto probeDir = directory;

    while (! probeDir.isDirectory() && probeDir.getParentDirectory() != probeDir)
        probeDir = probeDir.getParentDirectory();

    if (! probeDir.isDirectory())
        return false;

    const auto probe = probeDir.getNonexistentChildFile (".dmb-write-test", ".tmp", false);

    if (! probe.create())
        return false;

    probe.deleteFile();
    return true;
}

juce::File getDefaultAppDirectory()
{
    const auto programFiles = getProgramFilesDirectory().getChildFile (kProductName);

    if (canWriteInto (programFiles))
        return programFiles;

    return juce::File::getSpecialLocation (juce::File::windowsLocalAppData)
               .getChildFile ("Programs").getChildFile (kProductName);
}

bool isElevated()
{
   #if JUCE_WINDOWS
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid (&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0, &adminGroup))
    {
        CheckTokenMembership (nullptr, adminGroup, &isAdmin);
        FreeSid (adminGroup);
    }

    return isAdmin != FALSE;
   #else
    return true;
   #endif
}

bool relaunchElevated (const juce::String& extraArguments, juce::String& error)
{
   #if JUCE_WINDOWS
    const auto exe = exeFile().getFullPathName();
    const auto arguments = extraArguments;
    const auto verb = juce::String ("runas");
    const auto directory = exeFile().getParentDirectory().getFullPathName();

    SHELLEXECUTEINFOW info = {};
    info.cbSize = sizeof (info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = verb.toWideCharPointer();
    info.lpFile = exe.toWideCharPointer();
    info.lpParameters = arguments.toWideCharPointer();
    info.lpDirectory = directory.toWideCharPointer();
    info.nShow = SW_SHOWNORMAL;

    if (! ShellExecuteExW (&info))
    {
        error = u8 ("无法请求管理员权限（可能被取消，或者系统策略禁止）");
        return false;
    }

    if (info.hProcess != nullptr)
        CloseHandle (info.hProcess);

    return true;
   #else
    juce::ignoreUnused (extraArguments);
    error = u8 ("这个平台不支持提权");
    return false;
   #endif
}

bool isHostRunning (juce::String& hostName)
{
    hostName.clear();

   #if JUCE_WINDOWS
    const auto snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPPROCESS, 0);

    if (snapshot == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof (entry);

    if (Process32FirstW (snapshot, &entry))
    {
        do
        {
            const juce::String name (entry.szExeFile);

            if (name.containsIgnoreCase ("ableton") || name.equalsIgnoreCase ("Live.exe"))
            {
                hostName = name;
                break;
            }
        }
        while (Process32NextW (snapshot, &entry));
    }

    CloseHandle (snapshot);
   #endif

    return hostName.isNotEmpty();
}

//==============================================================================
juce::String displayPath (const juce::String& path)
{
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    if (home == juce::File())
        return path;

    // keep the drive / folder the profile actually lives in, only the account
    // folder name is replaced ("C:\Users\somebody" -> "C:\Users\administrator")
    const auto generic = home.getParentDirectory().getFullPathName() + juce::String ("\\administrator");

    return path.replace (home.getFullPathName(), generic, true);
}

juce::String displayPath (const juce::File& path)
{
    return displayPath (path.getFullPathName());
}

bool pathIsInside (const juce::File& candidate, const juce::File& root)
{
    if (root == juce::File() || candidate == juce::File())
        return false;

    const auto child = normalise (candidate);
    const auto parent = normalise (root);

    return child == parent || child.startsWith (parent + "\\");
}

bool looksLikeOurBundle (const juce::File& bundleFolder)
{
    if (! bundleFolder.isDirectory())
        return false;

    const auto info = bundleFolder.getChildFile ("Contents").getChildFile ("Resources").getChildFile ("moduleinfo.json");

    if (info.existsAsFile() && info.loadFileAsString().contains (kProductName))
        return true;

    // fall back: the binary itself must carry our name in its version info or path
    for (const auto& file : bundleFolder.findChildFiles (juce::File::findFiles, true, "*.vst3"))
        if (file.getFileNameWithoutExtension().startsWith (kProductName))
            return true;

    return false;
}

int removeEmptyDirectories (const juce::File& dir)
{
    if (! dir.isDirectory())
        return 0;

    int removed = 0;

    for (const auto& child : dir.findChildFiles (juce::File::findDirectories, false, "*"))
        removed += removeEmptyDirectories (child);

    if (dir.getNumberOfChildFiles (juce::File::findFilesAndDirectories, "*") == 0 && dir.deleteFile())
        ++removed;

    return removed;
}

//==============================================================================
juce::String Manifest::toString() const
{
    juce::StringArray lines;
    lines.add ("format=" + juce::String (format));
    lines.add ("product=" + juce::String (kProductName));
    lines.add ("pluginVersion=" + pluginVersion);
    lines.add ("installedAt=" + installedAt);
    lines.add ("machineWide=" + juce::String (machineWide ? 1 : 0));
    lines.add ("vst3Root=" + vst3Root.getFullPathName());
    lines.add ("appDir=" + appDir.getFullPathName());

    for (const auto& file : files)
        lines.add ("file=" + file);

    return lines.joinIntoString ("\n") + "\n";
}

Manifest Manifest::fromString (const juce::String& text)
{
    Manifest manifest;

    for (const auto& line : juce::StringArray::fromLines (text))
    {
        const auto trimmed = line.trim();

        if (trimmed.isEmpty() || ! trimmed.contains ("="))
            continue;

        const auto key = trimmed.upToFirstOccurrenceOf ("=", false, false).trim();
        const auto value = trimmed.fromFirstOccurrenceOf ("=", false, false);

        if (key == "format")             manifest.format = value.getIntValue();
        else if (key == "pluginVersion") manifest.pluginVersion = value.trim();
        else if (key == "installedAt")   manifest.installedAt = value.trim();
        else if (key == "machineWide")   manifest.machineWide = value.getIntValue() != 0;
        else if (key == "vst3Root")      manifest.vst3Root = juce::File (value.trim());
        else if (key == "appDir")        manifest.appDir = juce::File (value.trim());
        else if (key == "file" && value.trim().isNotEmpty()) manifest.files.add (value.trim());
    }

    return manifest;
}

Manifest readOwnManifest()
{
    const auto next = exeFile().getParentDirectory().getChildFile (kManifestName);
    const auto inParent = exeFile().getParentDirectory().getParentDirectory().getChildFile (kManifestName);

    for (const auto& candidate : { next, inParent })
    {
        if (candidate.existsAsFile())
        {
            auto manifest = Manifest::fromString (candidate.loadFileAsString());

            if (manifest.isValid())
            {
                if (manifest.files.isEmpty())
                    manifest.files.add (candidate.getFullPathName());

                return manifest;
            }
        }
    }

    return {};
}

juce::File getRegisteredVst3Directory()
{
    for (const auto hive : { juce::String ("HKEY_LOCAL_MACHINE\\"), juce::String ("HKEY_CURRENT_USER\\") })
    {
        const auto value = juce::WindowsRegistry::getValue (hive + kRegInfoKey + "\\Vst3Dir").trim();

        if (value.isNotEmpty())
        {
            const juce::File folder (value);

            if (folder.isDirectory())
                return folder;
        }
    }

    return {};
}

juce::Array<juce::File> findUnregisteredBundles()
{
    juce::Array<juce::File> found;

    const juce::File roots[] = { getDefaultVst3Directory(), getPerUserVst3Directory(),
                                 getProgramFilesDirectory().getChildFile ("Common Files").getChildFile ("VST3") };

    const juce::StringArray names { kBundleName, kStaleBundleName };

    for (const auto& root : roots)
    {
        for (const auto& name : names)
        {
            const auto bundle = root.getChildFile (name);

            if (bundle.isDirectory() && looksLikeOurBundle (bundle) && ! found.contains (bundle))
                found.add (bundle);
        }
    }

    return found;
}

//==============================================================================
bool writeUninstallRegistry (bool machineWide, const Manifest& manifest, const juce::File& pluginFile, juce::String& error)
{
    const auto hive = machineWide ? juce::String ("HKEY_LOCAL_MACHINE\\") : juce::String ("HKEY_CURRENT_USER\\");
    const auto uninstallKey = hive + kRegUninstallKey;
    const auto infoKey = hive + kRegInfoKey;

    const auto uninstaller = manifest.appDir.getChildFile (kUninstallerName).getFullPathName();
    const auto quoted = "\"" + uninstaller + "\"";

    bool ok = true;

    ok = juce::WindowsRegistry::setValue (uninstallKey + "\\DisplayName", juce::String (kProductName) + u8 (" (VST3 插件)")) && ok;
    ok = juce::WindowsRegistry::setValue (uninstallKey + "\\DisplayVersion", juce::String (kPluginVersion)) && ok;
    ok = juce::WindowsRegistry::setValue (uninstallKey + "\\Publisher", juce::String (kProductName)) && ok;
    ok = juce::WindowsRegistry::setValue (uninstallKey + "\\InstallLocation", manifest.appDir.getFullPathName()) && ok;
    ok = juce::WindowsRegistry::setValue (uninstallKey + "\\UninstallString", quoted + " --uninstall") && ok;
    ok = juce::WindowsRegistry::setValue (uninstallKey + "\\QuietUninstallString", quoted + " --uninstall --silent") && ok;
    ok = juce::WindowsRegistry::setValue (uninstallKey + "\\URLInfoAbout", juce::String ("https://api-docs.deepseek.com/")) && ok;
    ok = juce::WindowsRegistry::setValue (uninstallKey + "\\NoModify", (juce::uint32) 1) && ok;
    ok = juce::WindowsRegistry::setValue (uninstallKey + "\\NoRepair", (juce::uint32) 1) && ok;

    if (pluginFile.existsAsFile())
        ok = juce::WindowsRegistry::setValue (uninstallKey + "\\DisplayIcon", pluginFile.getFullPathName()) && ok;

    // estimated size in KB, for the "Apps & features" list
    juce::int64 total = 0;

    for (const auto& file : manifest.files)
        total += juce::File (file).getSize();

    ok = juce::WindowsRegistry::setValue (uninstallKey + "\\EstimatedSize", (juce::uint32) juce::jmax ((juce::int64) 1, total / 1024)) && ok;

    ok = juce::WindowsRegistry::setValue (infoKey + "\\InstallDir", manifest.appDir.getFullPathName()) && ok;
    ok = juce::WindowsRegistry::setValue (infoKey + "\\Vst3Dir", manifest.vst3Root.getFullPathName()) && ok;
    ok = juce::WindowsRegistry::setValue (infoKey + "\\Version", juce::String (kPluginVersion)) && ok;

    if (! ok)
        error = u8 ("写入注册表失败（没有管理员权限时会退回到当前用户）");

    return ok;
}

void removeUninstallRegistry (juce::StringArray& removedFrom)
{
    for (const auto hive : { juce::String ("HKEY_LOCAL_MACHINE\\"), juce::String ("HKEY_CURRENT_USER\\") })
    {
        const auto uninstallKey = hive + kRegUninstallKey;
        const auto infoKey = hive + kRegInfoKey;

        if (juce::WindowsRegistry::keyExists (uninstallKey))
        {
            juce::WindowsRegistry::deleteKey (uninstallKey);
            removedFrom.add (uninstallKey);
        }

        if (juce::WindowsRegistry::keyExists (infoKey))
        {
            juce::WindowsRegistry::deleteKey (infoKey);
            removedFrom.add (infoKey);
        }
    }
}

//==============================================================================
InstallResult runInstall (const InstallOptions& options, const ProgressFn& progress)
{
    InstallResult result;
    result.vst3Root = options.vst3Root;
    result.appDir = options.appDir != juce::File() ? options.appDir : getDefaultAppDirectory();

    const auto self = exeFile();
    const auto totalSteps = 6;
    int step = 0;

    const auto report = [&] (const juce::String& what)
    {
        if (progress != nullptr)
            progress (step, totalSteps, what);
    };

    // ---- 1. the plugin file must not be in use ---------------------------------
    report (u8 ("检查宿主程序..."));

    juce::String host;

    if (isHostRunning (host) && ! options.allowHostRunning)
    {
        result.error = u8 ("请先关闭 ") + host + u8 ("：插件文件正在被占用，无法覆盖。关闭后点“重试”。");
        return result;
    }

    // ---- 2. read our own payload ----------------------------------------------
    ++step;
    report (u8 ("读取安装内容..."));

    Payload payload;
    juce::String error;

    if (! payload.readFrom (self, error))
    {
        result.error = error;
        return result;
    }

    if (! result.vst3Root.isDirectory() && ! result.vst3Root.createDirectory())
    {
        result.error = u8 ("无法创建插件目录: ") + displayPath (result.vst3Root)
                     + u8 ("（可能需要以管理员身份运行）");
        return result;
    }

    if (! canWriteInto (result.vst3Root))
    {
        result.error = u8 ("没有权限写入 ") + displayPath (result.vst3Root)
                     + u8 ("。请用“以管理员身份运行”重新打开安装程序，或者换一个目录。");
        return result;
    }

    // ---- 3. unpack into the plugin folder and the program folder ---------------
    Manifest manifest;
    manifest.pluginVersion = kPluginVersion;
    manifest.installedAt = juce::Time::getCurrentTime().toString (true, true, false, true);
    manifest.vst3Root = result.vst3Root;
    manifest.appDir = result.appDir;

    if (! result.appDir.isDirectory() && ! result.appDir.createDirectory())
    {
        result.error = u8 ("无法创建程序目录: ") + displayPath (result.appDir);
        return result;
    }

    const auto& entries = payload.getEntries();
    int index = 0;

    for (const auto& entry : entries)
    {
        ++index;
        report (u8 ("正在安装 ") + juce::String (index) + "/" + juce::String ((int) entries.size())
                + ": " + entry.name);

        juce::File target;

        // both bundles are packed with the .vst3 folder as their first path segment,
        // so the rest of the relative path goes straight into the plug-in folder
        if (entry.name.startsWith ("vst3/"))
            target = result.vst3Root.getChildFile (entry.name.fromFirstOccurrenceOf ("/", false, false));
        else if (entry.name.startsWith ("vst3-midi/"))
        {
            // the MIDI-only flavour is optional (hosts with a MIDI effect slot)
            if (! options.includeMidiVariant)
                continue;

            target = result.vst3Root.getChildFile (entry.name.fromFirstOccurrenceOf ("/", false, false));
        }
        else if (entry.name.startsWith ("app/"))
            target = result.appDir.getChildFile (entry.name.fromFirstOccurrenceOf ("/", false, false));
        else
            continue;

        if (target.isDirectory())
        {
            result.error = u8 ("目标位置已经有一个同名文件夹: ") + displayPath (target);
            return result;
        }

        juce::MemoryBlock content;

        if (! Payload::readEntry (self, entry, content, error))
        {
            result.error = error;
            return result;
        }

        if (! writeWholeFile (target, content.getData(), content.getSize(), error))
        {
            result.error = error;
            return result;
        }

        manifest.files.add (target.getFullPathName());
        ++result.filesWritten;
    }

    // ---- 4. the uninstaller is a copy of this program -------------------------
    ++step;
    report (u8 ("安装卸载程序..."));

    const auto uninstaller = result.appDir.getChildFile (kUninstallerName);

    if (uninstaller.getFullPathName() != self.getFullPathName())
    {
        if (uninstaller.existsAsFile())
            uninstaller.deleteFile();

        if (! self.copyFileTo (uninstaller))
        {
            result.error = u8 ("无法写入卸载程序: ") + displayPath (uninstaller);
            return result;
        }

        manifest.files.add (uninstaller.getFullPathName());
    }
    else
    {
        manifest.files.add (uninstaller.getFullPathName());
    }

    // ---- 5. the manifest itself ------------------------------------------------
    ++step;
    report (u8 ("记录安装清单..."));

    const auto manifestFile = result.appDir.getChildFile (kManifestName);
    manifest.files.add (manifestFile.getFullPathName());

    if (! writeWholeFile (manifestFile, manifest.toString().toRawUTF8(),
                          (size_t) manifest.toString().getNumBytesAsUTF8(), error))
    {
        result.error = error;
        return result;
    }

    // ---- 6. registry -----------------------------------------------------------
    ++step;
    report (u8 ("登记到“应用和功能”..."));

    manifest.machineWide = isElevated();

    const auto pluginFile = result.vst3Root.getChildFile (kBundleName)
                                        .getChildFile ("Contents").getChildFile ("x86_64-win")
                                        .getChildFile (kBundleName);

    if (! writeUninstallRegistry (manifest.machineWide, manifest, pluginFile, error))
    {
        // not fatal: the plugin works, it is only missing from "Apps & features"
        manifest.machineWide = false;

        writeWholeFile (manifestFile, manifest.toString().toRawUTF8(),
                        (size_t) manifest.toString().getNumBytesAsUTF8(), error);
    }

    result.machineWide = manifest.machineWide;

    // ---- housekeeping ---------------------------------------------------------
    // Without the optional MIDI-only flavour, a leftover copy is removed: Ableton
    // Live refuses to load it ("no valid audio input bus"), so keeping it would only
    // make Live's browser show a plug-in that cannot be instantiated.
    if (! options.includeMidiVariant)
    {
        for (const auto& root : { result.vst3Root, getPerUserVst3Directory() })
        {
            const auto stale = root.getChildFile (kStaleBundleName);

            if (stale.isDirectory() && looksLikeOurBundle (stale))
                stale.deleteRecursively();
        }
    }

    // ---- housekeeping: an earlier install into a different folder ----------------
    const auto previous = readOwnManifest();

    if (previous.isValid() && previous.vst3Root != juce::File() && previous.vst3Root != result.vst3Root)
    {
        const auto oldBundle = previous.vst3Root.getChildFile (kBundleName);

        if (oldBundle.isDirectory() && looksLikeOurBundle (oldBundle))
        {
            oldBundle.deleteRecursively();
            removeEmptyDirectories (previous.vst3Root);
        }
    }

    ++step;
    report (u8 ("完成"));

    result.ok = true;
    return result;
}

//==============================================================================
UninstallResult runUninstall (const UninstallOptions& options, const ProgressFn& progress)
{
    UninstallResult result;

    // the plugin file cannot be removed while the host has it loaded
    juce::String host;

    if (isHostRunning (host) && ! options.allowHostRunning)
    {
        result.error = u8 ("请先关闭 ") + host + u8 ("，再点“重试”。\n")
                     + u8 ("（Live 正在使用时插件文件被占用，删不掉。）");
        return result;
    }

    auto manifest = readOwnManifest();

    // ---- plan ------------------------------------------------------------------
    juce::Array<juce::File> files;
    juce::Array<juce::File> roots;

    for (const auto& path : manifest.files)
    {
        const juce::File file (path);

        // hard safety net: only files inside the folders we recorded
        if (! pathIsInside (file, manifest.vst3Root) && ! pathIsInside (file, manifest.appDir))
        {
            ++result.filesKept;
            result.keptPaths.add (file.getFullPathName());
            continue;
        }

        files.add (file);
    }

    if (manifest.vst3Root != juce::File())
    {
        // never wider than our own bundle folders - even with a tampered manifest
        // nothing else in the VST3 folder can be reached
        roots.add (manifest.vst3Root.getChildFile (kBundleName));
        roots.add (manifest.vst3Root.getChildFile (kStaleBundleName));
    }

    if (manifest.appDir != juce::File())
        roots.add (manifest.appDir);

    if (options.removeUnregistered)
    {
        for (const auto& bundle : findUnregisteredBundles())
        {
            if (bundle.isDirectory())
            {
                roots.add (bundle.getParentDirectory());

                for (const auto& file : bundle.findChildFiles (juce::File::findFiles, true, "*"))
                    files.addIfNotAlreadyThere (file);
            }
        }
    }

    if (files.isEmpty() && manifest.appDir == juce::File() && ! options.removeUnregistered)
    {
        result.error = u8 ("没有找到安装清单，也没有找到本插件的文件夹");
        return result;
    }

    const auto total = files.size() + 4;
    int step = 0;

    const auto report = [&] (const juce::String& what)
    {
        if (progress != nullptr)
            progress (step, total, what);
    };

    // ---- 1. delete the recorded files ------------------------------------------
    for (const auto& file : files)
    {
        ++step;
        report (u8 ("删除 ") + file.getFileName());

        if (! file.existsAsFile())
            continue;

        const bool insideKnownRoot = std::any_of (roots.begin(), roots.end(),
                                                  [&file] (const juce::File& root) { return pathIsInside (file, root); });

        if (! insideKnownRoot)
        {
            ++result.filesKept;
            result.keptPaths.add (file.getFullPathName());
            continue;
        }

        file.setReadOnly (false);

        if (file.deleteFile())
            ++result.filesRemoved;
        else
        {
            ++result.filesKept;
            result.keptPaths.add (file.getFullPathName());
        }
    }

    // ---- 2. remove the folders we created, but only while they are empty --------
    ++step;
    report (u8 ("清理空目录..."));

    const auto bundleFolder = manifest.vst3Root.getChildFile (kBundleName);
    const auto staleFolder = manifest.vst3Root.getChildFile (kStaleBundleName);

    for (const auto& folder : { bundleFolder, staleFolder })
        if (folder.isDirectory())
            removeEmptyDirectories (folder);

    if (manifest.appDir.isDirectory())
    {
        removeEmptyDirectories (manifest.appDir);

        if (manifest.appDir.isDirectory())
        {
            // our own folder may still hold files we could not delete
            const auto left = manifest.appDir.getNumberOfChildFiles (juce::File::findFilesAndDirectories, "*");

            if (left > 0)
                result.keptPaths.add (manifest.appDir.getFullPathName() + u8 ("（还有 ")
                                      + juce::String (left) + u8 (" 个文件没删掉，可能是正在运行的程序）"));
        }
    }

    // look at the standard folders as well (a hand-made install has no manifest)
    for (const auto& bundle : findUnregisteredBundles())
    {
        removeEmptyDirectories (bundle);

        if (! bundle.isDirectory())
            result.removedRoots.add (bundle.getFullPathName());
    }

    if (bundleFolder.isDirectory())
    {
        const auto left = bundleFolder.getNumberOfChildFiles (juce::File::findFilesAndDirectories, "*");

        if (left > 0)
            result.keptPaths.add (bundleFolder.getFullPathName() + u8 ("（保留了 ")
                                  + juce::String (left) + u8 (" 个不属于本插件的文件）"));
    }

    // ---- 3. registry ------------------------------------------------------------
    ++step;
    report (u8 ("清理注册表..."));

    removeUninstallRegistry (result.removedRoots);

    // ---- 4. optional user data --------------------------------------------------
    ++step;
    report (u8 ("清理可选数据..."));

    if (options.removeAppData)
    {
        const auto data = getAppDataDirectory();

        if (data.isDirectory() && data.getFileName() == "DeepSeekMidiBridge")
        {
            data.deleteRecursively();
            result.removedRoots.add (data.getFullPathName());
        }
    }

    if (options.removeUserMedia)
    {
        const auto media = getUserMediaDirectory();

        if (media.isDirectory() && media.getFileName() == kProductName)
        {
            media.deleteRecursively();
            result.removedRoots.add (media.getFullPathName());
        }
    }

    result.ok = true;
    return result;
}

} // namespace dmbin
