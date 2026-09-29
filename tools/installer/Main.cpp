/*
    DeepSeek MIDI Bridge - installer and uninstaller (one program, two jobs).

        DeepSeekMidiBridgeSetup.exe                 install (GUI)
        DeepSeekMidiBridgeSetup.exe --silent [--dir <path>] [--force] [--keep-stale]
        DeepSeekMidiBridgeSetup.exe --uninstall [--silent] [--keep-user-data] [--delete-user-media]
        DeepSeekMidiBridgeSetup.exe --pack <contentDir> <outExe> [--log <file>]
        DeepSeekMidiBridgeSetup.exe --cleanup-self <exeToDelete> [--dir <appDir>]

    The uninstaller that is installed next to the plugin is a copy of this
    executable; it uninstalls because of its file name (or because of --uninstall).
*/

#include "InstallerCommon.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

using namespace dmbin;

namespace
{
    const juce::Colour kBackground { 0xff15171c };
    const juce::Colour kPanelLight { 0xff282c34 };
    const juce::Colour kAccent     { 0xff4f9df7 };
    const juce::Colour kText       { 0xffd8dee9 };
    const juce::Colour kTextDim    { 0xff8b95a5 };
    const juce::Colour kWarning    { 0xffe0a34a };
    const juce::Colour kDanger     { 0xffe06c75 };

    bool isUninstallerName (const juce::File& exe)
    {
        const auto name = exe.getFileNameWithoutExtension();

        return name.contains (u8 ("卸载")) || name.equalsIgnoreCase ("uninstall")
            || name.containsIgnoreCase ("uninstall-deepseek");
    }

    /** Splits the command line and drops the quotes the host puts around paths. */
    juce::StringArray tokeniseCommandLine (const juce::String& commandLine)
    {
        auto tokens = juce::StringArray::fromTokens (commandLine, true);

        for (auto& token : tokens)
            token = token.unquoted().trim();

        return tokens;
    }

    /** Copies this executable to %TEMP% and lets the copy delete the real one, so
        the uninstaller can remove itself. A fixed file name keeps %TEMP% tidy. */
    void spawnSelfCleanup (const juce::File& exeToDelete)
    {
        const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory);
        auto temp = tempDir.getChildFile ("DeepSeekMidiBridge-cleanup.exe");

        if (temp.existsAsFile() && ! temp.deleteFile())
            temp = tempDir.getChildFile ("DeepSeekMidiBridge-cleanup-"
                                         + juce::String (juce::Time::currentTimeMillis()) + ".exe");

        const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

        if (! self.copyFileTo (temp))
            return;

        juce::Process::openDocument (temp.getFullPathName(),
                                     "--cleanup-self \"" + exeToDelete.getFullPathName() + "\""
                                     + " --dir \"" + exeToDelete.getParentDirectory().getFullPathName() + "\"");
    }

    /** True when this executable is the installed uninstaller (not the setup exe). */
    bool isRunFromAppFolder()
    {
        const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

        return isUninstallerName (self)
            || self.getParentDirectory().getChildFile (kManifestName).existsAsFile();
    }

    /** Runs one of the long operations on a background thread. */
    class JobThread : public juce::Thread    {
    public:
        JobThread (const juce::String& name, std::function<void()> workToDo)
            : juce::Thread (name), work (std::move (workToDo)) {}

        void run() override { work(); }

    private:
        std::function<void()> work;
    };

    //==========================================================================
    class MainComponent : public juce::Component
    {
    public:
        MainComponent (bool uninstallMode, const juce::StringArray& args)
            : uninstalling (uninstallMode)
        {
            juce::ignoreUnused (args);

            title.setFont (juce::Font (juce::FontOptions (21.0f, juce::Font::bold)));
            title.setColour (juce::Label::textColourId, kText);
            addAndMakeVisible (title);

            subtitle.setFont (juce::Font (juce::FontOptions (12.0f)));
            subtitle.setColour (juce::Label::textColourId, kTextDim);
            addAndMakeVisible (subtitle);

            status.setFont (juce::Font (juce::FontOptions (12.5f)));
            status.setColour (juce::Label::textColourId, kText);
            status.setMinimumHorizontalScale (0.8f);
            addAndMakeVisible (status);

            progress.setColour (juce::ProgressBar::backgroundColourId, kPanelLight);
            progress.setColour (juce::ProgressBar::foregroundColourId, kAccent);
            addAndMakeVisible (progress);

            primaryButton.setColour (juce::TextButton::buttonColourId, kAccent);
            addAndMakeVisible (primaryButton);
            addAndMakeVisible (closeButton);

            closeButton.onClick = [this]
            {
                if (auto* app = juce::JUCEApplication::getInstance())
                    app->systemRequestedQuit();
            };

            if (uninstalling)
                setUpUninstall();
            else
                setUpInstall();

            primaryButton.onClick = [this] { if (uninstalling) startUninstall(); else startInstall(); };

            setSize (700, uninstalling ? 440 : 400);
        }

        ~MainComponent() override { waitForJob(); }

        bool isBusy() const noexcept { return job != nullptr; }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (kBackground);
            g.setColour (uninstalling ? kDanger : kAccent);
            g.fillRect (0, 0, getWidth(), 3);
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (26);

            auto header = area.removeFromTop (56);
            title.setBounds (header.removeFromTop (30));
            subtitle.setBounds (header);

            area.removeFromTop (10);

            if (! uninstalling)
            {
                auto row = area.removeFromTop (30);
                vst3Label.setBounds (row.removeFromLeft (110));
                browseButton.setBounds (row.removeFromRight (96).reduced (0, 3));
                row.removeFromRight (8);
                vst3Editor.setBounds (row.reduced (0, 3));

                area.removeFromTop (10);

                if (staleToggle != nullptr)
                    staleToggle->setBounds (area.removeFromTop (26));

                area.removeFromTop (6);

                if (warningLabel != nullptr)
                    warningLabel->setBounds (area.removeFromTop (36));

                if (installInfoLabel != nullptr)
                    installInfoLabel->setBounds (area.removeFromTop (installInfoHeight));
            }
            else
            {
                if (infoLabel != nullptr)
                    infoLabel->setBounds (area.removeFromTop (120));

                area.removeFromTop (6);

                if (appDataToggle != nullptr)
                    appDataToggle->setBounds (area.removeFromTop (28));

                if (mediaToggle != nullptr)
                    mediaToggle->setBounds (area.removeFromTop (28));
            }

            auto footer = getLocalBounds().removeFromBottom (112).reduced (26, 0);
            auto buttons = footer.removeFromBottom (34);
            closeButton.setBounds (buttons.removeFromRight (110));
            buttons.removeFromRight (10);
            primaryButton.setBounds (buttons.removeFromRight (150));

            footer.removeFromBottom (10);
            status.setBounds (footer.removeFromBottom (24));
            progress.setBounds (footer.removeFromBottom (18));

            if (juce::SystemStats::getEnvironmentVariable ("DMB_UI_DEBUG", {}).isNotEmpty())
            {
                juce::StringArray lines;
                lines.add ("localBounds=" + getLocalBounds().toString()
                           + " globalScale=" + juce::String (juce::Desktop::getInstance().getGlobalScaleFactor(), 3)
                           + " peerScale=" + juce::String (getPeer() != nullptr ? getPeer()->getPlatformScaleFactor() : 0.0, 3));
                lines.add ("  title=" + title.getBounds().toString());
                lines.add ("  vst3Label=" + vst3Label.getBounds().toString());
                lines.add ("  vst3Editor=" + vst3Editor.getBounds().toString());
                lines.add ("  browse=" + browseButton.getBounds().toString());
                lines.add ("  info=" + (installInfoLabel != nullptr ? installInfoLabel->getBounds().toString() : juce::String ("-")));
                lines.add ("  status=" + status.getBounds().toString());
                lines.add ("  progress=" + progress.getBounds().toString());
                lines.add ("  primary=" + primaryButton.getBounds().toString());
                lines.add ("  close=" + closeButton.getBounds().toString());

                juce::File::getSpecialLocation (juce::File::tempDirectory)
                    .getChildFile ("dmb-installer-ui.log").appendText (lines.joinIntoString ("\n") + "\n\n");
            }
        }

        void reportProgress (int step, int total, const juce::String& what)
        {
            const auto percent = total > 0 ? juce::jlimit (0.0, 1.0, (double) step / (double) total) : 0.0;

            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this), percent, what]
            {
                if (safe == nullptr)
                    return;

                safe->progressValue = percent;
                safe->status.setText (what, juce::dontSendNotification);
            });
        }

    private:
        //======================================================================
        void setUpInstall()
        {
            title.setText (u8 ("安装 DeepSeek MIDI Bridge"), juce::dontSendNotification);
            subtitle.setText (u8 ("VST3 插件 · 版本 ") + kPluginVersion
                              + u8 (" · 不需要额外的运行库，自带卸载程序"), juce::dontSendNotification);

            vst3Label.setText (u8 ("插件安装位置"), juce::dontSendNotification);
            vst3Label.setFont (juce::Font (juce::FontOptions (12.0f)));
            vst3Label.setColour (juce::Label::textColourId, kTextDim);
            addAndMakeVisible (vst3Label);

            const auto registered = getRegisteredVst3Directory();
            vst3Editor.setText ((registered != juce::File() ? registered : getDefaultVst3Directory()).getFullPathName(), false);
            vst3Editor.setFont (juce::Font (juce::FontOptions (12.5f)));
            addAndMakeVisible (vst3Editor);

            browseButton.onClick = [this] { chooseVst3Folder(); };
            addAndMakeVisible (browseButton);

            auto* info = new juce::Label();
            info->setFont (juce::Font (juce::FontOptions (11.5f)));
            info->setColour (juce::Label::textColourId, kTextDim);
            info->setJustificationType (juce::Justification::topLeft);
            installInfo.reset (info);
            installInfoLabel = info;
            addAndMakeVisible (info);

            juce::String host;

            if (isHostRunning (host))
            {
                auto* warn = new juce::Label();
                warn->setFont (juce::Font (juce::FontOptions (12.0f)));
                warn->setColour (juce::Label::textColourId, kWarning);
                warn->setText (u8 ("⚠ 检测到 ") + host + u8 (" 正在运行：请先关闭它，否则插件文件被占用。"),
                               juce::dontSendNotification);
                warning.reset (warn);
                warningLabel = warn;
                addAndMakeVisible (warn);
            }

            auto* midi = new juce::ToggleButton (
                u8 ("同时安装纯 MIDI 版本（Cubase / Bitwig 等宿主的 MIDI 插槽用；Ableton Live 无法加载它）"));
            midi->setToggleState (false, juce::dontSendNotification);
            midi->setColour (juce::ToggleButton::textColourId, kTextDim);
            staleToggle.reset (midi);
            addAndMakeVisible (midi);

            installInfoLabel->setText (installInfoText(), juce::dontSendNotification);

            primaryButton.setButtonText (u8 ("开始安装"));
            closeButton.setButtonText (u8 ("退出"));
            status.setText (u8 ("准备就绪，点“开始安装”。"), juce::dontSendNotification);
        }

        juce::String installInfoText() const
        {
            return u8 ("· Ableton Live 只扫描 C:\\Program Files\\Common Files\\VST3，默认就装在那里（其他目录需要在 Live 里手动添加）。");
        }

        //======================================================================
        void setUpUninstall()
        {
            title.setText (u8 ("卸载 DeepSeek MIDI Bridge"), juce::dontSendNotification);
            subtitle.setText (u8 ("只删除本插件自己的文件；其它插件和你的音乐文件都不会动"),
                              juce::dontSendNotification);

            const auto manifest = readOwnManifest();
            juce::StringArray summary;

            juce::String host;

            if (isHostRunning (host))
                summary.add (u8 ("⚠ 检测到 ") + host + u8 (" 正在运行：请先关闭它再卸载。"));

            if (manifest.isValid())
            {
                summary.add (u8 ("插件目录：") + displayPath (manifest.vst3Root));
                summary.add (u8 ("程序目录：") + displayPath (manifest.appDir));
                summary.add (u8 ("安装清单记录了 ") + juce::String (manifest.files.size())
                             + u8 (" 个文件，只会删这些（以及它们留下的空文件夹）。"));
            }
            else
            {
                const auto found = findUnregisteredBundles();

                if (found.isEmpty())
                {
                    summary.add (u8 ("没有找到安装记录，标准目录里也没有本插件。"));
                    summary.add (u8 ("（如果是手动复制进去的，请自己删掉那个 DeepSeek MIDI Bridge.vst3 文件夹。）"));
                }
                else
                {
                    summary.add (u8 ("没有安装记录，但按“文件夹名 + 内容”确认下面这些是本插件，将删除："));

                    for (const auto& bundle : found)
                        summary.add ("    " + displayPath (bundle));
                }
            }

            auto* info = new juce::Label();
            info->setFont (juce::Font (juce::FontOptions (11.5f)));
            info->setColour (juce::Label::textColourId, kText);
            info->setJustificationType (juce::Justification::topLeft);
            info->setText (summary.joinIntoString ("\n"), juce::dontSendNotification);
            infoText.reset (info);
            infoLabel = info;
            addAndMakeVisible (info);

            auto* dataToggle = new juce::ToggleButton (
                u8 ("同时删除本机设置 / 素材库 / 共享总线（%LOCALAPPDATA%\\DeepSeekMidiBridge）"));
            dataToggle->setToggleState (true, juce::dontSendNotification);
            appDataToggle.reset (dataToggle);
            addAndMakeVisible (dataToggle);

            auto* media = new juce::ToggleButton (
                u8 ("同时删除我生成的 MIDI 与工程文件夹（文档\\DeepSeek MIDI Bridge）"));
            media->setToggleState (false, juce::dontSendNotification);
            media->setColour (juce::ToggleButton::textColourId, kWarning);
            mediaToggle.reset (media);
            addAndMakeVisible (media);

            primaryButton.setButtonText (u8 ("开始卸载"));
            primaryButton.setColour (juce::TextButton::buttonColourId, kDanger);
            closeButton.setButtonText (u8 ("取消"));
            status.setText (u8 ("默认保留你生成的 MIDI 文件；确认无误后点“开始卸载”。"),
                            juce::dontSendNotification);
        }

        void chooseVst3Folder()
        {
            chooser = std::make_unique<juce::FileChooser> (u8 ("选择 VST3 插件目录"),
                                                           juce::File (vst3Editor.getText()), juce::String());

            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [this] (const juce::FileChooser& fc)
                                  {
                                      const auto result = fc.getResult();

                                      if (result != juce::File())
                                          vst3Editor.setText (result.getFullPathName(), false);
                                  });
        }

        //======================================================================
        void startInstall()
        {
            const juce::File target (vst3Editor.getText().trim());

            if (target.getFullPathName().isEmpty())
            {
                setStatus (u8 ("请先选择安装位置。"), kWarning);
                return;
            }

            juce::String host;

            if (isHostRunning (host))
            {
                setStatus (host + u8 (" 正在运行：请先关闭它，再点一次“开始安装”。"), kWarning);
                return;
            }

            InstallOptions options;
            options.vst3Root = target;
            options.includeMidiVariant = staleToggle != nullptr && staleToggle->getToggleState();

            setBusy (true);

            auto* self = this;

            job = std::make_unique<JobThread> ("install", [self, options]
            {
                auto result = runInstall (options, [self] (int step, int total, const juce::String& what)
                {
                    self->reportProgress (step, total, what);
                });

                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (self), result]
                {
                    if (safe == nullptr)
                        return;

                    safe->waitForJob();
                    safe->finishInstall (result);
                });
            });

            job->startThread();
        }

        void finishInstall (const dmbin::InstallResult& result)
        {
            setBusy (false);

            if (! result.ok)
            {
                setStatus (result.error, kWarning);
                progressValue = 0.0;
                return;
            }

            progressValue = 1.0;
            setStatus (u8 ("安装完成：写入 ") + juce::String (result.filesWritten) + u8 (" 个文件 → ")
                       + displayPath (result.vst3Root), kText);

            if (installInfoLabel != nullptr)
            {
                installInfoLabel->setColour (juce::Label::textColourId, kText);
                installInfoLabel->setText (u8 ("下一步：打开 Ableton Live → 设置 → Plug-Ins → 点 Rescan（按住 Alt 点 = 完整重扫），\n")
                                           + u8 ("然后在浏览器 Plug-Ins → VST3 里找到 DeepSeek MIDI Bridge。\n")
                                           + u8 ("卸载：") + displayPath (result.appDir.getChildFile (kUninstallerName))
                                           + u8 ("，或系统“应用和功能”里的 DeepSeek MIDI Bridge。"),
                                           juce::dontSendNotification);

                installInfoHeight = 76;      // three lines after a successful install
                resized();
            }

            if (warningLabel != nullptr)
                warningLabel->setVisible (false);

            if (staleToggle != nullptr)
                staleToggle->setVisible (false);

            vst3Editor.setEnabled (false);
            browseButton.setEnabled (false);

            const auto folder = result.vst3Root;
            primaryButton.setButtonText (u8 ("打开插件文件夹"));
            primaryButton.onClick = [folder]
            {
                if (folder.isDirectory())
                    folder.startAsProcess();
            };

            closeButton.setButtonText (u8 ("完成"));
        }

        //======================================================================
        void startUninstall()
        {
            UninstallOptions options;
            options.removeAppData = appDataToggle == nullptr || appDataToggle->getToggleState();
            options.removeUserMedia = mediaToggle != nullptr && mediaToggle->getToggleState();

            setBusy (true);

            auto* self = this;

            job = std::make_unique<JobThread> ("uninstall", [self, options]
            {
                auto result = runUninstall (options, [self] (int step, int total, const juce::String& what)
                {
                    self->reportProgress (step, total, what);
                });

                juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (self), result]
                {
                    if (safe == nullptr)
                        return;

                    safe->waitForJob();
                    safe->finishUninstall (result);
                });
            });

            job->startThread();
        }

        void finishUninstall (const dmbin::UninstallResult& result)
        {
            setBusy (false);
            closeButton.setButtonText (u8 ("关闭"));

            if (! result.ok)
            {
                setStatus (result.error, kWarning);
                return;
            }

            progressValue = 1.0;
            setStatus (u8 ("卸载完成：删除 ") + juce::String (result.filesRemoved) + u8 (" 个文件")
                       + (result.filesKept > 0 ? (u8 ("，保留 ") + juce::String (result.filesKept) + u8 (" 个不属于本插件的文件"))
                                               : juce::String()),
                       kText);

            if (infoLabel != nullptr)
            {
                juce::StringArray lines;
                lines.add (u8 ("已清理的目录："));

                if (result.removedRoots.isEmpty())
                {
                    lines.add (u8 ("    （无）"));
                }
                else
                {
                    for (const auto& root : result.removedRoots)
                        lines.add (displayPath (root));
                }

                if (! result.keptPaths.isEmpty())
                {
                    lines.add (u8 ("保留（没动）："));

                    for (const auto& kept : result.keptPaths)
                        lines.add (displayPath (kept));
                }

                infoLabel->setText (lines.joinIntoString ("\n"), juce::dontSendNotification);
            }

            primaryButton.setEnabled (false);
            primaryButton.setButtonText (u8 ("已卸载"));

            // the running uninstaller can only delete itself from a temporary copy
            spawnSelfCleanup();
        }

        //======================================================================
        void spawnSelfCleanup()
        {
            const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

            if (isRunFromAppFolder())
                ::spawnSelfCleanup (self);
        }

        void setBusy (bool busy)
        {
            primaryButton.setEnabled (! busy);
            closeButton.setEnabled (! busy);
            vst3Editor.setEnabled (! busy && ! uninstalling);
            browseButton.setEnabled (! busy && ! uninstalling);

            if (busy)
                closeButton.setButtonText (u8 ("请稍候…"));
            else
                closeButton.setButtonText (uninstalling ? u8 ("取消") : u8 ("退出"));
        }

        void setStatus (const juce::String& text, juce::Colour colour)
        {
            status.setColour (juce::Label::textColourId, colour);
            status.setText (text, juce::dontSendNotification);
        }

        void waitForJob()
        {
            if (job == nullptr)
                return;

            job->signalThreadShouldExit();

            if (job->isThreadRunning())
                job->stopThread (20000);

            job.reset();
        }

        //======================================================================
        const bool uninstalling;

        juce::Label title, subtitle, status, vst3Label;
        juce::TextEditor vst3Editor;
        juce::TextButton browseButton { u8 ("浏览...") };
        double progressValue = 0.0;
        juce::ProgressBar progress { progressValue };
        juce::TextButton primaryButton, closeButton;

        std::unique_ptr<juce::Label> installInfo, infoText, warning;
        juce::Label* installInfoLabel = nullptr;
        juce::Label* infoLabel = nullptr;
        juce::Label* warningLabel = nullptr;
        int installInfoHeight = 26;
        std::unique_ptr<juce::ToggleButton> staleToggle, appDataToggle, mediaToggle;

        std::unique_ptr<juce::FileChooser> chooser;
        std::unique_ptr<JobThread> job;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
    };

    //==========================================================================
    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow (bool uninstallMode, const juce::StringArray& args)
            : juce::DocumentWindow (uninstallMode ? u8 ("卸载 DeepSeek MIDI Bridge")
                                                  : u8 ("DeepSeek MIDI Bridge 安装程序"),
                                    kBackground,
                                    juce::DocumentWindow::closeButton)
        {
            setUsingNativeTitleBar (true);
            auto* content = new MainComponent (uninstallMode, args);
            setContentOwned (content, true);
            setResizable (false, false);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override
        {
            if (auto* content = dynamic_cast<MainComponent*> (getContentComponent()))
                if (content->isBusy())
                    return;      // never leave a half finished install behind

            if (auto* app = juce::JUCEApplication::getInstance())
                app->systemRequestedQuit();
        }
    };
}

//==============================================================================
class DmbInstallerApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return kProductName; }
    const juce::String getApplicationVersion() override { return kPluginVersion; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        const juce::StringArray args = tokeniseCommandLine (commandLine);

        lookAndFeel.setColourScheme (juce::LookAndFeel_V4::getDarkColourScheme());
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        if (args.contains ("--pack"))         { runPack (args); return; }
        if (args.contains ("--cleanup-self")) { runCleanup (args); return; }

        const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        const bool uninstallMode = args.contains ("--uninstall") || isUninstallerName (self);

        if (args.contains ("--silent"))
        {
            runSilent (uninstallMode, args);
            return;
        }

        // Writing into (or deleting from) Program Files needs administrator rights.
        // Ask for them right away - but only when this machine really cannot do it.
        if (! isElevated() && ! args.contains ("--elevated"))
        {
            bool needsElevation = false;
            juce::File probe;

            if (uninstallMode)
            {
                const auto manifest = readOwnManifest();
                probe = manifest.vst3Root.getChildFile (kBundleName);

                if (! probe.isDirectory())
                    probe = manifest.vst3Root.isDirectory() ? manifest.vst3Root : getDefaultVst3Directory();

                needsElevation = ! canWriteInto (probe);
            }
            else
            {
                probe = getRegisteredVst3Directory();

                if (probe == juce::File())
                    probe = getDefaultVst3Directory();

                needsElevation = ! canWriteInto (probe);
            }

            if (needsElevation)
            {
                juce::String error;

                if (relaunchElevated (commandLine + " --elevated", error))
                {
                    quit();
                    return;
                }

                if (! uninstallMode)
                    juce::NativeMessageBox::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        u8 ("没有管理员权限"),
                        u8 ("无法写入 ") + displayPath (probe) + u8 ("。\n\n")
                        + u8 ("可以：\n· 右键安装程序 → 以管理员身份运行；或者\n")
                        + u8 ("· 把安装位置改成用户目录（例如 ") + displayPath (getPerUserVst3Directory())
                        + u8 ("），装好后在 Live 的 Plug-Ins 设置里手动添加该目录。"));
            }
        }

        window = std::make_unique<MainWindow> (uninstallMode, args);
    }

    void shutdown() override
    {
        window.reset();
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override { quit(); }

private:
    //==========================================================================
    void runPack (const juce::StringArray& args)
    {
        const auto index = args.indexOf ("--pack");
        const juce::File contentDir (index >= 0 && index + 1 < args.size() ? args[index + 1] : juce::String());
        const juce::File outExe (index >= 0 && index + 2 < args.size() ? args[index + 2] : juce::String());

        const auto logFile = args.contains ("--log") ? juce::File (args[args.indexOf ("--log") + 1])
                                                     : outExe.withFileExtension ("pack.log");

        const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile);

        juce::StringArray log;
        juce::String error;

        const bool ok = Payload::pack (contentDir, self, outExe, log, error);

        if (! ok)
            log.add (u8 ("打包失败: ") + error);

        logFile.replaceWithText (log.joinIntoString ("\n") + "\n");

        setApplicationReturnValue (ok ? 0 : 1);
        quit();
    }

    /** Runs from a copy in %TEMP%: deletes the real uninstaller, then itself. */
    void runCleanup (const juce::StringArray& args)
    {
        const auto index = args.indexOf ("--cleanup-self");
        const juce::File target (index >= 0 && index + 1 < args.size() ? args[index + 1] : juce::String());
        const juce::File appDir (args.contains ("--dir") ? args[args.indexOf ("--dir") + 1] : juce::String());

        if (target != juce::File())
        {
            for (int attempt = 0; attempt < 60 && target.existsAsFile(); ++attempt)
            {
                target.setReadOnly (false);

                if (target.deleteFile())
                    break;

                juce::Thread::sleep (250);
            }
        }

        if (appDir.isDirectory())
            removeEmptyDirectories (appDir);

        // older builds left timestamped copies behind - they are ours by name
        const auto tempDir = juce::File::getSpecialLocation (juce::File::tempDirectory);

        for (const auto& leftover : tempDir.findChildFiles (juce::File::findFiles, false,
                                                            "DeepSeekMidiBridge-uninstall-*.exe"))
            leftover.deleteFile();

       #if JUCE_WINDOWS
        const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        MoveFileExW (self.getFullPathName().toWideCharPointer(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
       #endif

        quit();
    }

    //==========================================================================
    void runSilent (bool uninstallMode, const juce::StringArray& args)
    {
        const auto logFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("DeepSeekMidiBridge-install.log");

        const auto logLine = [&logFile] (const juce::String& text)
        {
            logFile.appendText (juce::Time::getCurrentTime().toString (true, true, true, true) + "  " + text + "\n");
        };

        logLine (juce::String (uninstallMode ? "uninstall" : "install") + " args: " + args.joinIntoString (" | "));

        if (uninstallMode)
        {
            UninstallOptions options;
            options.removeAppData = ! args.contains ("--keep-user-data");
            options.removeUserMedia = args.contains ("--delete-user-media");
            options.allowHostRunning = args.contains ("--force");

            const auto result = runUninstall (options, nullptr);

            logLine (result.ok ? ("uninstall ok, removed " + juce::String (result.filesRemoved)
                                  + " files, kept " + juce::String (result.filesKept))
                               : ("uninstall failed: " + result.error));

            // same self-cleanup as in the GUI
            if (result.ok && isRunFromAppFolder())
                spawnSelfCleanup (juce::File::getSpecialLocation (juce::File::currentExecutableFile));

            setApplicationReturnValue (result.ok ? 0 : 1);
            quit();
            return;
        }

        InstallOptions options;
        options.vst3Root = args.contains ("--dir") ? juce::File (args[args.indexOf ("--dir") + 1])
                                                   : getDefaultVst3Directory();
        options.appDir = args.contains ("--app-dir") ? juce::File (args[args.indexOf ("--app-dir") + 1])
                                                     : juce::File();
        options.includeMidiVariant = args.contains ("--with-midi");
        options.allowHostRunning = args.contains ("--force");

        const auto result = runInstall (options, nullptr);

        logLine (result.ok ? ("install ok: " + juce::String (result.filesWritten) + " files -> "
                              + result.vst3Root.getFullPathName() + "  appDir=" + result.appDir.getFullPathName())
                           : ("install failed: " + result.error));

        setApplicationReturnValue (result.ok ? 0 : (result.error.contains (u8 ("请先关闭")) ? 2 : 1));
        quit();
    }

    juce::LookAndFeel_V4 lookAndFeel;
    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (DmbInstallerApplication)
