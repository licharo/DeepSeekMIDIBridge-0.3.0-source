#include "PluginEditor.h"
#include "PluginParameters.h"

#include "core/DiagnosticsLog.h"
#include "core/MidiFileUtil.h"
#include "core/MidiJson.h"
#include "core/PromptBuilder.h"

namespace
{
    const juce::Colour kBackground { 0xff15171c };
    const juce::Colour kPanel { 0xff1e2127 };
    const juce::Colour kPanelLight { 0xff282c34 };
    const juce::Colour kAccent { 0xff4f9df7 };
    const juce::Colour kText { 0xffd8dee9 };
    const juce::Colour kTextDim { 0xff8b95a5 };
}

//==============================================================================
DeepSeekMidiBridgeEditor::ResultChip::ResultChip (const juce::String& captionText)
    : caption (captionText)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void DeepSeekMidiBridgeEditor::ResultChip::setFile (const juce::File& newFile)
{
    file = newFile;
    repaint();
}

void DeepSeekMidiBridgeEditor::ResultChip::setCaption (const juce::String& text)
{
    caption = text;
    repaint();
}

void DeepSeekMidiBridgeEditor::ResultChip::paint (juce::Graphics& g)
{
    auto area = getLocalBounds().toFloat().reduced (1.0f);

    const bool ready = file.existsAsFile();

    g.setColour (ready ? kAccent.withAlpha (0.22f) : kPanelLight);
    g.fillRoundedRectangle (area, 5.0f);

    g.setColour (ready ? kAccent : kTextDim);
    g.drawRoundedRectangle (area, 5.0f, 1.0f);

    g.setColour (ready ? kText : kTextDim);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.drawText (caption, area.reduced (8.0f, 0.0f), juce::Justification::centredLeft, true);

    if (ready)
    {
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.setColour (kAccent);
        g.drawText (dmb::utf8 ("点这里打开文件夹 →"), area.reduced (8.0f, 0.0f),
                    juce::Justification::centredRight, false);
    }
}

void DeepSeekMidiBridgeEditor::ResultChip::mouseDown (const juce::MouseEvent&)
{
    if (onClick != nullptr)
        onClick();
}

//==============================================================================
int DeepSeekMidiBridgeEditor::SourceListModel::getNumRows()
{
    return (int) owner.sourceItems.size();
}

void DeepSeekMidiBridgeEditor::SourceListModel::paintListBoxItem (int rowNumber, juce::Graphics& g,
                                                                  int width, int height, bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= (int) owner.sourceItems.size())
        return;

    const auto& item = owner.sourceItems[(size_t) rowNumber];

    if (rowIsSelected)
    {
        g.setColour (kAccent.withAlpha (0.25f));
        g.fillRect (0, 0, width, height);
    }

    g.setColour (item.included ? kAccent : kTextDim);
    g.setFont (juce::Font (juce::FontOptions (13.0f)));
    g.drawText (item.included ? juce::String::fromUTF8 ("\xe2\x9c\x94") : juce::String::fromUTF8 ("\xe2\x97\x8b"),
                4, 0, 18, height, juce::Justification::centred, false);

    g.setColour (kText);
    g.setFont (juce::Font (juce::FontOptions (12.5f)));
    g.drawText (item.source.describe(), 24, 0, width - 28, height, juce::Justification::centredLeft, true);
}

void DeepSeekMidiBridgeEditor::SourceListModel::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (row < 0 || row >= (int) owner.sourceItems.size())
        return;

    if (e.mods.isPopupMenu())
    {
        owner.sourcesList.selectRow (row);
        owner.showSourceMenu (row);
        return;
    }

    auto& item = owner.sourceItems[(size_t) row];
    item.included = ! item.included;

    owner.previewSourceId = item.source.id;
    owner.sourceRoll.setSource (item.source);
    owner.sourcesList.repaintRow (row);
}

//==============================================================================
DeepSeekMidiBridgeEditor::DeepSeekMidiBridgeEditor (DeepSeekMidiBridgeProcessor& p)
    : juce::AudioProcessorEditor (&p), processor (p)
{
    listModel = std::make_unique<SourceListModel> (*this);

    auto setupSectionLabel = [this] (juce::Label& label, const juce::String& text, float size)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::Font (juce::FontOptions (size, juce::Font::bold)));
        label.setColour (juce::Label::textColourId, kText);
        content.addAndMakeVisible (label);
    };

    auto setupSmallLabel = [this] (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::Font (juce::FontOptions (11.5f)));
        label.setColour (juce::Label::textColourId, kTextDim);
        content.addAndMakeVisible (label);
    };

    // ---- top bar ------------------------------------------------------------
    setupSmallLabel (profileLabel, dmb::utf8 ("API 方案"));
    setupSmallLabel (apiKeyLabel, "DeepSeek API Key");
    setupSmallLabel (modelLabel, dmb::utf8 ("模型"));
    setupSmallLabel (endpointLabel, dmb::utf8 ("接口地址"));

    profileBox.setTextWhenNothingSelected (dmb::utf8 ("（还没有保存的方案）"));
    profileBox.onChange = [this] { applyProfile (profileBox.getSelectedId() - 1); };
    settingsPanel.addAndMakeVisible (profileBox);

    saveProfileButton.onClick = [this] { saveCurrentAsProfile(); };
    settingsPanel.addAndMakeVisible (saveProfileButton);

    deleteProfileButton.onClick = [this] { deleteSelectedProfile(); };
    settingsPanel.addAndMakeVisible (deleteProfileButton);

    testApiButton.onClick = [this] { runApiTest(); };
    settingsPanel.addAndMakeVisible (testApiButton);

    apiStateLabel.setFont (juce::Font (juce::FontOptions (11.5f)));
    apiStateLabel.setColour (juce::Label::textColourId, kTextDim);
    apiStateLabel.setJustificationType (juce::Justification::centredLeft);
    settingsPanel.addAndMakeVisible (apiStateLabel);

    apiKeyEditor.setPasswordCharacter (juce::juce_wchar (0x2022));
    apiKeyEditor.setTextToShowWhenEmpty ("sk-...", kTextDim);
    apiKeyEditor.onReturnKey = [this]
    {
        applySettingsFromFields();
        setStatus (dmb::utf8 ("API Key 已应用"));
    };
    apiKeyEditor.onFocusLost = [this] { applySettingsFromFields(); };
    settingsPanel.addAndMakeVisible (apiKeyEditor);

    endpointEditor.setTextToShowWhenEmpty ("https://api.deepseek.com/chat/completions", kTextDim);
    endpointEditor.onReturnKey = [this]
    {
        applySettingsFromFields();
        setStatus (dmb::utf8 ("接口地址已应用"));
    };
    endpointEditor.onFocusLost = [this] { applySettingsFromFields(); };
    settingsPanel.addAndMakeVisible (endpointEditor);

    modelBox.addItemList ({ "deepseek-chat", "deepseek-reasoner" }, 1);
    modelBox.setEditableText (true);
    modelBox.onChange = [this]
    {
        applySettingsFromFields();
        setStatus (dmb::utf8 ("已切换模型: ") + processor.getSettings().api.model);
    };
    settingsPanel.addAndMakeVisible (modelBox);

    saveApiButton.onClick = [this] { applySettingsFromFields(); setStatus (dmb::utf8 ("设置已保存")); };
    settingsPanel.addAndMakeVisible (saveApiButton);

    outputDirButton.onClick = [this] { chooseOutputDirectory(); };
    settingsPanel.addAndMakeVisible (outputDirButton);

    openOutputButton.onClick = [this]
    {
        const auto dir = processor.getOutputDirectory();

        if (dir.isDirectory())
            dir.startAsProcess();
        else if (dir.getParentDirectory().isDirectory())
            dir.getParentDirectory().startAsProcess();
    };
    settingsPanel.addAndMakeVisible (openOutputButton);

    // ---- left column --------------------------------------------------------
    setupSectionLabel (sourcesLabel, dmb::utf8 ("① 其它音轨的 MIDI（自动捕获）"), 13.5f);

    sourcesList.setModel (listModel.get());
    sourcesList.setRowHeight (22);
    sourcesList.setColour (juce::ListBox::backgroundColourId, kPanel);
    sourcesList.setColour (juce::ListBox::outlineColourId, kPanelLight);
    sourcesList.setOutlineThickness (1);
    content.addAndMakeVisible (sourcesList);

    instanceNameEditor.setTextToShowWhenEmpty (dmb::utf8 ("留空则自动用宿主轨道名；也可以自己起名"), kTextDim);
    content.addAndMakeVisible (instanceNameEditor);

    applyNameButton.onClick = [this] { applySettingsFromFields(); refreshSources (true); };
    content.addAndMakeVisible (applyNameButton);

    importButton.onClick = [this] { chooseMidiFileToImport(); };
    content.addAndMakeVisible (importButton);

    clearRefButton.onClick = [this] { processor.clearImportedSources(); refreshSources (true); };
    content.addAndMakeVisible (clearRefButton);

    clearCaptureButton.onClick = [this]
    {
        processor.clearLocalCapture();
        previewSourceId.clear();
        setStatus (dmb::utf8 ("已清空本机捕获（包括自动保留的段落）"));
        refreshSources (true);
    };
    content.addAndMakeVisible (clearCaptureButton);

    renameSourceButton.onClick = [this] { beginRenameSelected(); };
    content.addAndMakeVisible (renameSourceButton);

    deleteSourceButton.onClick = [this] { deleteSelectedSource(); };
    content.addAndMakeVisible (deleteSourceButton);

    renameEditor.setVisible (false);
    renameEditor.setFont (juce::Font (juce::FontOptions (12.5f)));
    renameEditor.onReturnKey = [this] { commitRename(); };
    renameEditor.onEscapeKey = [this] { cancelRename(); };
    renameEditor.onFocusLost = [this] { commitRename(); };
    content.addChildComponent (renameEditor);

    content.addAndMakeVisible (sourceRoll);

    usageHint.setText (dmb::utf8 ("用法：把本插件放在目标轨，用 Live 的 MIDI From 选择源轨；"
                       "或在源音轨上再放一个实例并勾选共享，它就会出现在 ① 里。"
                       "生成完成后会自动打开文件夹并选中 .mid，把它拖进 Live 即可；"
                       "也可以直接把 .mid 拖到本窗口当作参考素材。"),
                       juce::dontSendNotification);
    usageHint.setFont (juce::Font (juce::FontOptions (11.0f)));

    localStatusLabel.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
    localStatusLabel.setColour (juce::Label::textColourId, kTextDim);
    content.addAndMakeVisible (localStatusLabel);
    usageHint.setColour (juce::Label::textColourId, kTextDim);
    usageHint.setJustificationType (juce::Justification::topLeft);
    content.addAndMakeVisible (usageHint);

    // ---- right column -------------------------------------------------------
    setupSectionLabel (promptLabel, dmb::utf8 ("② 你的要求（自然语言）"), 13.5f);

    quickFillBox.setTextWhenNothingSelected (dmb::utf8 ("常用要求 ▾"));
    quickFillBox.onChange = [this] { applyQuickFill (quickFillBox.getSelectedId() - 1); };
    content.addAndMakeVisible (quickFillBox);

    {
        const auto templates = dmb::requestTemplates();

        for (int i = 0; i < (int) templates.size(); ++i)
            quickFillBox.addItem (templates[(size_t) i].label, i + 1);
    }

    promptEditor.setMultiLine (true, true);
    promptEditor.setReturnKeyStartsNewLine (true);
    promptEditor.setTextToShowWhenEmpty (dmb::utf8 ("例如：根据贝斯轨写一段 8 小节的 Drum & Bass 鼓组，保持切分感，"
                                         "加一些鬼音军鼓和开放式踩镲"), kTextDim);
    content.addAndMakeVisible (promptEditor);

    setupSmallLabel (styleLabel, dmb::utf8 ("风格"));
    setupSmallLabel (instrumentLabel, dmb::utf8 ("乐器/音色"));
    setupSmallLabel (keyLabel, dmb::utf8 ("调性/音阶"));

    styleEditor.setTextToShowWhenEmpty (dmb::utf8 ("如 Drum & Bass / Lo-fi"), kTextDim);
    instrumentEditor.setTextToShowWhenEmpty (dmb::utf8 ("如 鼓组 / 贝斯 / 弦乐"), kTextDim);
    keyEditor.setTextToShowWhenEmpty (dmb::utf8 ("如 F# minor 或留空"), kTextDim);
    content.addAndMakeVisible (styleEditor);
    content.addAndMakeVisible (instrumentEditor);
    content.addAndMakeVisible (keyEditor);

    setupSmallLabel (barsLabel, dmb::utf8 ("小节数"));
    setupSmallLabel (tempLabel, dmb::utf8 ("创意程度"));
    setupSmallLabel (channelLabel, dmb::utf8 ("通道"));
    setupSmallLabel (transposeLabel, dmb::utf8 ("移调"));
    setupSmallLabel (maxNotesLabel, dmb::utf8 ("最多音符"));
    setupSmallLabel (referenceBarsLabel, dmb::utf8 ("参考上限(小节)"));

    barsSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    barsSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 18);
    tempSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    tempSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 18);
    transposeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    transposeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 18);
    maxNotesSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    maxNotesSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 18);
    referenceBarsSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    referenceBarsSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 18);

    content.addAndMakeVisible (barsSlider);
    content.addAndMakeVisible (tempSlider);
    content.addAndMakeVisible (transposeSlider);
    settingsPanel.addAndMakeVisible (maxNotesSlider);
    settingsPanel.addAndMakeVisible (referenceBarsSlider);

    juce::StringArray channels;

    for (int i = 1; i <= 16; ++i)
        channels.add (juce::String (i));

    channelBox.addItemList (channels, 1);
    content.addAndMakeVisible (channelBox);

    settingsPanel.addAndMakeVisible (sendToBusButton);
    settingsPanel.addAndMakeVisible (liveOutputButton);
    settingsPanel.addAndMakeVisible (syncButton);
    settingsPanel.addAndMakeVisible (loopButton);
    settingsPanel.addAndMakeVisible (matchLoopButton);
    settingsPanel.addAndMakeVisible (autoGenerateButton);

    generateButton.onClick = [this] { startGeneration(); };
    generateButton.setColour (juce::TextButton::buttonColourId, kAccent.withAlpha (0.85f));
    content.addAndMakeVisible (generateButton);

    cancelButton.onClick = [this] { processor.cancelGeneration(); };
    content.addAndMakeVisible (cancelButton);

    previewPromptButton.onClick = [this] { showPromptPreview(); };
    content.addAndMakeVisible (previewPromptButton);

    statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    statusLabel.setColour (juce::Label::textColourId, kText);
    content.addAndMakeVisible (statusLabel);

    logEditor.setMultiLine (true);
    logEditor.setReadOnly (true);
    logEditor.setScrollbarsShown (true);
    logEditor.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.5f,
                                                      juce::Font::plain)));
    logEditor.setColour (juce::TextEditor::backgroundColourId, kPanel);
    content.addAndMakeVisible (logEditor);

    setupSectionLabel (resultLabel, dmb::utf8 ("③ 结果（已保存为 .mid，点它打开文件夹）"), 13.5f);

    resultChip.onClick = [this] { revealResultInFolder(); };
    content.addAndMakeVisible (resultChip);

    listenButton.setClickingTogglesState (true);
    listenButton.onClick = [this] { togglePreview(); };
    content.addAndMakeVisible (listenButton);

    saveAsButton.onClick = [this] { saveResultAs(); };
    content.addAndMakeVisible (saveAsButton);

    revealResultButton.onClick = [this] { revealResultInFolder(); };
    content.addAndMakeVisible (revealResultButton);

    clearResultButton.onClick = [this] { processor.clearGeneratedClip(); refreshResult (true); };
    content.addAndMakeVisible (clearResultButton);

    content.addAndMakeVisible (resultRoll);

    // ---- parameter attachments ---------------------------------------------
    auto& state = processor.getValueTreeState();

    barsAttachment = std::make_unique<SliderAttachment> (state, dmb::params::generationBars, barsSlider);
    tempAttachment = std::make_unique<SliderAttachment> (state, dmb::params::temperature, tempSlider);
    transposeAttachment = std::make_unique<SliderAttachment> (state, dmb::params::transpose, transposeSlider);
    maxNotesAttachment = std::make_unique<SliderAttachment> (state, dmb::params::maxNotes, maxNotesSlider);
    referenceBarsAttachment = std::make_unique<SliderAttachment> (state, dmb::params::referenceBars, referenceBarsSlider);
    channelAttachment = std::make_unique<ComboBoxAttachment> (state, dmb::params::channel, channelBox);
    sendToBusAttachment = std::make_unique<ButtonAttachment> (state, dmb::params::sendToBus, sendToBusButton);
    liveOutputAttachment = std::make_unique<ButtonAttachment> (state, dmb::params::liveOutput, liveOutputButton);
    syncAttachment = std::make_unique<ButtonAttachment> (state, dmb::params::syncToHost, syncButton);
    loopAttachment = std::make_unique<ButtonAttachment> (state, dmb::params::loopPlayback, loopButton);
    matchLoopAttachment = std::make_unique<ButtonAttachment> (state, dmb::params::matchLoop, matchLoopButton);
    autoGenerateAttachment = std::make_unique<ButtonAttachment> (state, dmb::params::autoGenerate, autoGenerateButton);

    tempSlider.setTextValueSuffix (dmb::utf8 (" °"));
    transposeSlider.setTextValueSuffix (" st");
    barsSlider.setTextValueSuffix (dmb::utf8 (" 小节"));
    referenceBarsSlider.setTextValueSuffix (dmb::utf8 (" 小节"));

    // Keep the manual bar count in charge: when "长度=循环长度" is on, the manual
    // slider is greyed out; switch it off and manual input is back immediately.
    matchLoopButton.onClick = [this]
    {
        const auto automatic = matchLoopButton.getToggleState();
        barsSlider.setEnabled (! automatic);

        setStatus (automatic ? dmb::utf8 ("生成长度将跟随宿主循环长度（要手填就把它关掉）")
                             : dmb::utf8 ("已切回手动小节数"));
    };

    barsSlider.setEnabled (! matchLoopButton.getToggleState());

    // ---- settings panel -----------------------------------------------------
    addAndMakeVisible (content);
    content.addAndMakeVisible (settingsPanel);
    settingsPanel.setVisible (false);

    // a few labels are created by the helper lambdas above, so re-parent them here
    for (auto* label : { &profileLabel, &apiKeyLabel, &modelLabel, &endpointLabel,
                         &maxNotesLabel, &referenceBarsLabel, &uiScaleLabel })
        settingsPanel.addAndMakeVisible (*label);

    settingsButton.onClick = [this] { toggleSettingsPanel (! showSettings); };
    content.addAndMakeVisible (settingsButton);

    closeSettingsButton.onClick = [this] { toggleSettingsPanel (false); };
    settingsPanel.addAndMakeVisible (closeSettingsButton);

    openFolderAfterGenerateButton.setToggleState (processor.getSettings().openFolderAfterGenerate,
                                                  juce::dontSendNotification);
    openFolderAfterGenerateButton.onClick = [this]
    {
        auto settings = processor.getSettings();
        settings.openFolderAfterGenerate = openFolderAfterGenerateButton.getToggleState();
        processor.setSettings (settings);

        setStatus (settings.openFolderAfterGenerate
                     ? dmb::utf8 ("生成完成后会自动打开文件夹并选中文件")
                     : dmb::utf8 ("已关闭：生成后不再自动打开文件夹（③ 的“打开文件夹”按钮仍可用）"));
    };
    settingsPanel.addAndMakeVisible (openFolderAfterGenerateButton);

    dragLogButton.onClick = [this]
    {
        logEditor.setText (dmb::readDiagnosticsLog (60));
        logEditor.moveCaretToEnd();
        dmb::getDiagnosticsLogFile().revealToUser();
    };
    settingsPanel.addAndMakeVisible (dragLogButton);

    // ---- project ------------------------------------------------------------
    projectSectionTitle.setText (dmb::utf8 ("工程（每个宿主工程一个文件夹：素材 + 生成的 .mid 都在里面）"),
                                 juce::dontSendNotification);
    projectSectionTitle.setFont (juce::Font (juce::FontOptions (11.5f, juce::Font::bold)));
    projectSectionTitle.setColour (juce::Label::textColourId, kText);
    settingsPanel.addAndMakeVisible (projectSectionTitle);

    projectNameLabel.setText (dmb::utf8 ("工程名称"), juce::dontSendNotification);
    projectNameLabel.setFont (juce::Font (juce::FontOptions (11.5f)));
    projectNameLabel.setColour (juce::Label::textColourId, kTextDim);
    settingsPanel.addAndMakeVisible (projectNameLabel);

    projectNameEditor.setFont (juce::Font (juce::FontOptions (12.5f)));
    projectNameEditor.setTextToShowWhenEmpty (dmb::utf8 ("例如：我的歌"), kTextDim);
    projectNameEditor.onReturnKey = [this] { applyProjectName(); };
    projectNameEditor.onFocusLost = [this] { applyProjectName(); };
    settingsPanel.addAndMakeVisible (projectNameEditor);

    renameProjectButton.onClick = [this] { applyProjectName(); };
    settingsPanel.addAndMakeVisible (renameProjectButton);

    newProjectButton.onClick = [this] { createNewProject(); };
    settingsPanel.addAndMakeVisible (newProjectButton);

    projectPickLabel.setText (dmb::utf8 ("当前工程"), juce::dontSendNotification);
    projectPickLabel.setFont (juce::Font (juce::FontOptions (11.5f)));
    projectPickLabel.setColour (juce::Label::textColourId, kTextDim);
    settingsPanel.addAndMakeVisible (projectPickLabel);

    projectBox.setTextWhenNothingSelected (dmb::utf8 ("（还没有工程）"));
    projectBox.onChange = [this] { selectProjectFromBox(); };
    settingsPanel.addAndMakeVisible (projectBox);

    openProjectFolderButton.onClick = [this]
    {
        processor.ensureProjectFolder();

        const auto dir = processor.getProjectFolder();

        if (dir.isDirectory())
            dir.startAsProcess();
        else if (processor.getProjectsRoot().isDirectory())
            processor.getProjectsRoot().startAsProcess();
    };
    settingsPanel.addAndMakeVisible (openProjectFolderButton);

    mergeDetachedButton.onClick = [this]
    {
        juce::String error;

        if (processor.mergeDetachedLibrary (error))
            setStatus (dmb::utf8 ("已把「未归类」的素材并入当前工程"));
        else
            setStatus (error);

        refreshProjectFields();
        refreshSources (true);
    };
    settingsPanel.addAndMakeVisible (mergeDetachedButton);

    projectPathLabel.setFont (juce::Font (juce::FontOptions (10.5f)));
    projectPathLabel.setColour (juce::Label::textColourId, kTextDim);
    projectPathLabel.setJustificationType (juce::Justification::centredLeft);
    settingsPanel.addAndMakeVisible (projectPathLabel);

    settingsTitle.setText (dmb::utf8 ("设置"), juce::dontSendNotification);
    settingsTitle.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
    settingsTitle.setColour (juce::Label::textColourId, kText);
    settingsPanel.addAndMakeVisible (settingsTitle);

    uiScaleLabel.setText (dmb::utf8 ("界面缩放"), juce::dontSendNotification);
    uiScaleLabel.setFont (juce::Font (juce::FontOptions (11.5f)));
    uiScaleLabel.setColour (juce::Label::textColourId, kTextDim);
    settingsPanel.addAndMakeVisible (uiScaleLabel);

    uiScaleBox.addItemList ({ dmb::utf8 ("自动（跟随系统）"), "80%", "90%", "100%", "110%",
                              "125%", "150%", "175%", "200%" }, 1);
    uiScaleBox.onChange = [this]
    {
        const auto id = uiScaleBox.getSelectedId();
        static const double scales[] = { 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0 };

        if (id <= 1)   // auto: follow the display scale
            setUiScale (getDisplayScale());
        else
            setUiScale (scales[juce::jlimit (0, 7, id - 2)]);

        setStatus (dmb::utf8 ("界面缩放已设为 ") + juce::String (juce::roundToInt (uiScale * 100.0))
                   + "%（窗口会一起变大；想更清晰就选大一点）");
    };
    settingsPanel.addAndMakeVisible (uiScaleBox);

    pushSettingsToFields();

    setResizable (true, true);
    setWantsKeyboardFocus (true);

    // start with the scale stored in the settings (0 = follow the display)
    uiScale = getPreferredScaleFromSettings();
    setSize (juce::roundToInt (kBaseWidth * uiScale), juce::roundToInt (kBaseHeight * uiScale));
    setResizeLimits (juce::roundToInt (900 * uiScale), juce::roundToInt (600 * uiScale),
                     juce::roundToInt (1400 * uiScale), juce::roundToInt (1100 * uiScale));

    // pick the matching entry in the UI scale menu
    {
        const auto percent = juce::roundToInt (uiScale * 100.0);
        static const int candidates[] = { 80, 90, 100, 110, 125, 150, 175, 200 };
        int best = 1;   // "auto"

        for (int i = 0; i < 8; ++i)
            if (percent == candidates[i])
                best = i + 2;

        uiScaleBox.setSelectedId (best, juce::dontSendNotification);
    }

    startTimerHz (5);

    refreshSources (true);
    refreshResult (true, false);   // don't pop Explorer open just for showing an old result
    updateButtons();

    // dev helper: start with the settings panel already open (used for screenshots)
    if (juce::SystemStats::getEnvironmentVariable ("DMB_OPEN_SETTINGS", {}).isNotEmpty())
        toggleSettingsPanel (true);
}

DeepSeekMidiBridgeEditor::~DeepSeekMidiBridgeEditor()
{
    stopTimer();
    processor.setPromptText (promptEditor.getText());
    processor.setFreeRunPreview (false);
}

//==============================================================================
void DeepSeekMidiBridgeEditor::pushSettingsToFields()
{
    const auto settings = processor.getSettings();

    apiKeyEditor.setText (settings.api.apiKey, false);
    endpointEditor.setText (settings.api.endpoint, false);

    const auto models = juce::StringArray { "deepseek-chat", "deepseek-reasoner" };
    const auto index = models.indexOf (settings.api.model);

    if (index >= 0)
        modelBox.setSelectedId (index + 1, juce::dontSendNotification);
    else
        modelBox.setText (settings.api.model, juce::dontSendNotification);

    styleEditor.setText (settings.style, false);
    instrumentEditor.setText (settings.instrument, false);
    keyEditor.setText (settings.key, false);
    instanceNameEditor.setText (settings.sourceName, false);

    outputDirButton.setButtonText (dmb::utf8 ("输出根目录: ") + processor.getProjectsRoot().getFileName());

    refreshProfiles (true);
    updateApiStateLabel();
    refreshProjectFields();
}

void DeepSeekMidiBridgeEditor::applySettingsFromFields()
{
    auto settings = processor.getSettings();

    settings.api.apiKey = apiKeyEditor.getText().trim();
    settings.api.endpoint = endpointEditor.getText().trim();

    if (settings.api.endpoint.isEmpty())
        settings.api.endpoint = "https://api.deepseek.com/chat/completions";

    settings.api.model = modelBox.getText().trim().isEmpty() ? juce::String ("deepseek-chat")
                                                             : modelBox.getText().trim();
    settings.style = styleEditor.getText().trim();
    settings.instrument = instrumentEditor.getText().trim();
    settings.key = keyEditor.getText().trim();

    if (instanceNameEditor.getText().trim().isNotEmpty())
        settings.sourceName = instanceNameEditor.getText().trim();

    settings.windowBars = (int) std::lround (processor.getValueTreeState()
                                                 .getRawParameterValue (dmb::params::windowBars)->load());
    settings.beatsPerBar = (int) std::lround (processor.getValueTreeState()
                                                  .getRawParameterValue (dmb::params::beatsPerBar)->load());

    processor.setSettings (settings);
    outputDirButton.setButtonText (dmb::utf8 ("输出根目录: ") + processor.getProjectsRoot().getFileName());

    // any change invalidates a previous connection test
    processor.getApiProbe().clearResult();
    refreshProfiles (true);
    updateApiStateLabel();
}

void DeepSeekMidiBridgeEditor::applyQuickFill (int index)
{
    const auto templates = dmb::requestTemplates();

    if (index < 0 || index >= (int) templates.size())
        return;

    const auto& item = templates[(size_t) index];

    if (item.style.isNotEmpty())
        styleEditor.setText (item.style, false);

    if (item.instrument.isNotEmpty())
        instrumentEditor.setText (item.instrument, false);

    if (item.keyHint.isNotEmpty())
        keyEditor.setText (item.keyHint, false);

    if (item.bars > 0)
    {
        const juce::ScopedValueSetter<bool> guard (updatingFieldsProgrammatically, true);
        barsSlider.setValue ((double) item.bars, juce::sendNotificationSync);
    }

    // The request box is always yours to edit: a template is only *added*, never
    // replaces something you already typed (and a too-short prompt is kept too).
    const auto existing = promptEditor.getText();

    if (existing.trim().isEmpty())
        promptEditor.setText (item.prompt);
    else
        promptEditor.setText (existing.trimEnd() + "\n" + item.prompt);

    promptEditor.moveCaretToEnd();
    processor.setPromptText (promptEditor.getText());

    setStatus (dmb::utf8 ("已套用常用要求: ") + item.label + dmb::utf8 ("（可以直接改，或再点生成 MIDI）"));
}

//==============================================================================
void DeepSeekMidiBridgeEditor::revealResultInFolder()
{
    auto clip = processor.getGeneratedClip();

    if (clip == nullptr || ! clip->file.existsAsFile())
    {
        setStatus (dmb::utf8 ("还没有生成结果文件"));
        return;
    }

    dmb::logDiagnostic ("ui", "reveal result in folder: " + dmb::displayPath (clip->file));
    clip->file.revealToUser();

    setStatus (dmb::utf8 ("已打开文件夹并选中: ") + clip->file.getFileName()
               + dmb::utf8 ("  —— 把它拖进 Live 的轨道或 Session 格即可"));
}

//==============================================================================
juce::String DeepSeekMidiBridgeEditor::getSelectedSourceId() const
{
    const auto row = sourcesList.getSelectedRow();

    if (row < 0 || row >= (int) sourceItems.size())
        return {};

    return sourceItems[(size_t) row].source.id;
}

void DeepSeekMidiBridgeEditor::beginRenameSelected()
{
    const auto row = sourcesList.getSelectedRow();

    if (row < 0 || row >= (int) sourceItems.size())
    {
        setStatus (dmb::utf8 ("请先在 ① 列表里点选一条素材，再点重命名"));
        return;
    }

    const auto& item = sourceItems[(size_t) row];
    renamingSourceId = item.source.id;

    // an inline editor appears right on top of the row
    auto area = sourcesList.getBounds().withHeight (juce::jmin (22, sourcesList.getHeight()));
    area.translate (0, juce::jlimit (0, juce::jmax (0, sourcesList.getHeight() - area.getHeight()),
                                     row * sourcesList.getRowHeight()));

    renameEditor.setBounds (area);
    renameEditor.setText (item.source.name, false);
    renameEditor.setVisible (true);
    renameEditor.toFront (true);
    renameEditor.grabKeyboardFocus();
    renameEditor.selectAll();
}

void DeepSeekMidiBridgeEditor::commitRename()
{
    if (renamingSourceId.isEmpty())
        return;

    const auto id = renamingSourceId;
    const auto name = renameEditor.getText().trim();
    renamingSourceId.clear();
    renameEditor.setVisible (false);

    if (name.isEmpty())
        return;

    if (processor.renameSource (id, name))
    {
        setStatus (dmb::utf8 ("已重命名为: ") + name);
        refreshSources (true);
    }
    else
    {
        setStatus (dmb::utf8 ("这条素材不能在这里改名（可能来自其它插件实例）"));
    }
}

void DeepSeekMidiBridgeEditor::cancelRename()
{
    renamingSourceId.clear();
    renameEditor.setVisible (false);
}

void DeepSeekMidiBridgeEditor::deleteSelectedSource()
{
    const auto row = sourcesList.getSelectedRow();

    if (row < 0 || row >= (int) sourceItems.size())
    {
        setStatus (dmb::utf8 ("请先在 ① 列表里点选一条素材，再点删除"));
        return;
    }

    const auto id = sourceItems[(size_t) row].source.id;
    const auto name = sourceItems[(size_t) row].source.name;

    if (processor.removeSource (id))
    {
        if (previewSourceId == id)
            previewSourceId.clear();

        setStatus (dmb::utf8 ("已删除素材: ") + name);
        refreshSources (true);
    }
    else
    {
        setStatus (dmb::utf8 ("这条素材属于其它插件实例，请在那个实例上清除（或等它过期）"));
    }
}

void DeepSeekMidiBridgeEditor::showSourceMenu (int row)
{
    if (row < 0 || row >= (int) sourceItems.size())
        return;

    const auto& item = sourceItems[(size_t) row];

    juce::PopupMenu menu;
    menu.addSectionHeader (item.source.describe());
    menu.addItem (1, dmb::utf8 ("用作参考 / 取消参考"));
    menu.addItem (2, dmb::utf8 ("重命名..."));
    menu.addItem (3, dmb::utf8 ("删除这一段"));
    menu.addSeparator();
    menu.addItem (4, dmb::utf8 ("清空本机捕获（含保留段）"));

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&sourcesList),
                        [this, row] (int result)
                        {
                            if (result == 1)
                            {
                                if (row < (int) sourceItems.size())
                                {
                                    sourceItems[(size_t) row].included = ! sourceItems[(size_t) row].included;
                                    sourcesList.repaintRow (row);
                                }
                            }
                            else if (result == 2)
                            {
                                beginRenameSelected();
                            }
                            else if (result == 3)
                            {
                                deleteSelectedSource();
                            }
                            else if (result == 4)
                            {
                                processor.clearLocalCapture();
                                previewSourceId.clear();
                                setStatus (dmb::utf8 ("已清空本机捕获（包括自动保留的段落）"));
                                refreshSources (true);
                            }
                        });
}

//==============================================================================
void DeepSeekMidiBridgeEditor::SettingsBackdrop::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.55f));
}

void DeepSeekMidiBridgeEditor::SettingsBackdrop::mouseDown (const juce::MouseEvent&)
{
    owner.toggleSettingsPanel (false);
}

void DeepSeekMidiBridgeEditor::SettingsPanel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();

    g.setColour (juce::Colour (0xff23262d));
    g.fillRoundedRectangle (bounds, 8.0f);
    g.setColour (kAccent.withAlpha (0.7f));
    g.drawRoundedRectangle (bounds.reduced (0.5f), 8.0f, 1.5f);
}

void DeepSeekMidiBridgeEditor::SettingsPanel::mouseDown (const juce::MouseEvent&)
{
    // clicking the dimmed area outside the panel closes it
}

void DeepSeekMidiBridgeEditor::SettingsPanel::resized()
{
    auto area = getLocalBounds().reduced (16);

    auto titleRow = area.removeFromTop (26);
    owner.settingsTitle.setBounds (titleRow.removeFromLeft (200));
    owner.closeSettingsButton.setBounds (titleRow.removeFromRight (80).reduced (0, 1));
    area.removeFromTop (10);

    // ---- API ---------------------------------------------------------------
    auto row = area.removeFromTop (24);
    owner.profileLabel.setBounds (row.removeFromLeft (66));
    owner.profileBox.setBounds (row.removeFromLeft (240).reduced (0, 1));
    row.removeFromLeft (6);
    owner.saveProfileButton.setBounds (row.removeFromLeft (96).reduced (0, 1));
    row.removeFromLeft (6);
    owner.deleteProfileButton.setBounds (row.removeFromLeft (86).reduced (0, 1));
    row.removeFromLeft (6);
    owner.testApiButton.setBounds (row.removeFromLeft (88).reduced (0, 1));

    area.removeFromTop (6);
    row = area.removeFromTop (24);
    owner.apiKeyLabel.setBounds (row.removeFromLeft (104));
    owner.apiKeyEditor.setBounds (row.removeFromLeft (300).reduced (0, 1));
    row.removeFromLeft (8);
    owner.saveApiButton.setBounds (row.removeFromLeft (86).reduced (0, 1));
    row.removeFromLeft (10);
    owner.modelLabel.setBounds (row.removeFromLeft (34));
    owner.modelBox.setBounds (row.removeFromLeft (170).reduced (0, 1));

    area.removeFromTop (6);
    row = area.removeFromTop (24);
    owner.endpointLabel.setBounds (row.removeFromLeft (104));
    owner.endpointEditor.setBounds (row.removeFromLeft (300).reduced (0, 1));
    row.removeFromLeft (8);
    owner.outputDirButton.setBounds (row.removeFromLeft (140).reduced (0, 1));
    row.removeFromLeft (6);
    owner.openOutputButton.setBounds (row.removeFromLeft (118).reduced (0, 1));

    area.removeFromTop (12);

    // ---- generation limits --------------------------------------------------
    row = area.removeFromTop (40);
    auto cell = row.removeFromLeft (170);
    owner.maxNotesLabel.setBounds (cell.removeFromTop (14));
    owner.maxNotesSlider.setBounds (cell);
    row.removeFromLeft (10);
    cell = row.removeFromLeft (170);
    owner.referenceBarsLabel.setBounds (cell.removeFromTop (14));
    owner.referenceBarsSlider.setBounds (cell);

    area.removeFromTop (8);

    // ---- playback / capture switches ---------------------------------------
    row = area.removeFromTop (24);
    owner.sendToBusButton.setBounds (row.removeFromLeft (150).reduced (0, 2));
    owner.liveOutputButton.setBounds (row.removeFromLeft (136).reduced (0, 2));
    owner.syncButton.setBounds (row.removeFromLeft (100).reduced (0, 2));
    owner.loopButton.setBounds (row.removeFromLeft (76).reduced (0, 2));

    area.removeFromTop (4);
    row = area.removeFromTop (24);
    owner.matchLoopButton.setBounds (row.removeFromLeft (150).reduced (0, 2));
    owner.autoGenerateButton.setBounds (row.removeFromLeft (160).reduced (0, 2));

    area.removeFromTop (12);

    // ---- interface ----------------------------------------------------------
    row = area.removeFromTop (24);
    owner.uiScaleLabel.setBounds (row.removeFromLeft (104));
    owner.uiScaleBox.setBounds (row.removeFromLeft (150).reduced (0, 1));
    row.removeFromLeft (12);
    owner.dragLogButton.setBounds (row.removeFromLeft (110).reduced (0, 1));

    area.removeFromTop (8);
    row = area.removeFromTop (24);
    owner.openFolderAfterGenerateButton.setBounds (row.removeFromLeft (250).reduced (0, 2));

    area.removeFromTop (14);

    // ---- project (one folder per host project) -------------------------------
    row = area.removeFromTop (26);
    owner.projectSectionTitle.setBounds (row);

    area.removeFromTop (4);
    row = area.removeFromTop (24);
    owner.projectNameLabel.setBounds (row.removeFromLeft (104));
    owner.projectNameEditor.setBounds (row.removeFromLeft (240).reduced (0, 1));
    row.removeFromLeft (8);
    owner.renameProjectButton.setBounds (row.removeFromLeft (140).reduced (0, 1));
    row.removeFromLeft (6);
    owner.newProjectButton.setBounds (row.removeFromLeft (90).reduced (0, 1));

    area.removeFromTop (6);
    row = area.removeFromTop (24);
    owner.projectPickLabel.setBounds (row.removeFromLeft (104));
    owner.projectBox.setBounds (row.removeFromLeft (240).reduced (0, 1));
    row.removeFromLeft (8);
    owner.openProjectFolderButton.setBounds (row.removeFromLeft (140).reduced (0, 1));
    row.removeFromLeft (6);
    owner.mergeDetachedButton.setBounds (row.removeFromLeft (150).reduced (0, 1));

    area.removeFromTop (4);
    owner.projectPathLabel.setBounds (area.removeFromTop (16));
}

//==============================================================================
void DeepSeekMidiBridgeEditor::toggleSettingsPanel (bool shouldShow)
{
    showSettings = shouldShow;
    settingsBackdrop.setVisible (shouldShow);
    settingsPanel.setVisible (shouldShow);

    if (shouldShow)
    {
        settingsBackdrop.setBounds (0, 0, kBaseWidth, kBaseHeight);
        settingsBackdrop.toFront (false);
        settingsPanel.setBounds (juce::Rectangle<int> (kBaseWidth / 2 - 390, 34, 780, 560));
        settingsPanel.toFront (true);

        refreshProjectFields();
    }
    else
    {
        applySettingsFromFields();
    }

    content.repaint();
}

double DeepSeekMidiBridgeEditor::getDisplayScale() const
{
    if (auto* peer = getPeer())
        return juce::jlimit (1.0, 2.0, (double) peer->getPlatformScaleFactor());

    return 1.0;
}

double DeepSeekMidiBridgeEditor::getPreferredScaleFromSettings() const
{
    const auto stored = processor.getSettings().uiScale;

    if (stored > 0.5 && stored < 3.0)
        return stored;

    // "auto" (0): follow the display scale so the UI matches the rest of the system
    return getDisplayScale();
}

void DeepSeekMidiBridgeEditor::setUiScale (double newScale)
{
    uiScale = juce::jlimit (0.75, 2.0, newScale);

    if (auto* parent = getParentComponent())
        juce::ignoreUnused (parent);

    setSize (juce::roundToInt (kBaseWidth * uiScale), juce::roundToInt (kBaseHeight * uiScale));
    setResizeLimits (juce::roundToInt (900 * uiScale), juce::roundToInt (600 * uiScale),
                     juce::roundToInt (1400 * uiScale), juce::roundToInt (1100 * uiScale));

    auto settings = processor.getSettings();
    settings.uiScale = uiScale;
    processor.setSettings (settings);

    resized();
}

//==============================================================================
void DeepSeekMidiBridgeEditor::refreshProfiles (bool selectCurrent)
{
    const auto settings = processor.getSettings();

    profileBox.clear (juce::dontSendNotification);

    for (int i = 0; i < (int) settings.apiProfiles.size(); ++i)
    {
        const auto& profile = settings.apiProfiles[(size_t) i];

        profileBox.addItem (profile.name + "  (" + profile.model + ")", i + 1);

        if (selectCurrent && profile.matches (settings.api))
            profileBox.setSelectedId (i + 1, juce::dontSendNotification);
    }

    if (profileBox.getNumItems() == 0)
        profileBox.setSelectedId (0, juce::dontSendNotification);
}

void DeepSeekMidiBridgeEditor::applyProfile (int profileIndex)
{
    const auto settings = processor.getSettings();

    if (profileIndex < 0 || profileIndex >= (int) settings.apiProfiles.size())
        return;

    const auto profile = settings.apiProfiles[(size_t) profileIndex];

    auto updated = settings;
    updated.api.endpoint = profile.endpoint;
    updated.api.apiKey = profile.apiKey;
    updated.api.model = profile.model;

    processor.setSettings (updated);
    pushSettingsToFields();
    setStatus (dmb::utf8 ("已切换到方案: ") + profile.name + "  ·  " + profile.model);
}

void DeepSeekMidiBridgeEditor::saveCurrentAsProfile()
{
    applySettingsFromFields();

    auto settings = processor.getSettings();

    const auto domain = juce::URL (settings.api.endpoint).getDomain();

    dmb::ApiProfile profile;
    profile.name = (domain.isNotEmpty() ? domain : settings.api.endpoint) + " · " + settings.api.model;
    profile.endpoint = settings.api.endpoint;
    profile.apiKey = settings.api.apiKey;
    profile.model = settings.api.model;

    bool replaced = false;

    for (auto& existing : settings.apiProfiles)
    {
        if (existing.endpoint == profile.endpoint && existing.model == profile.model)
        {
            existing = profile;
            replaced = true;
            break;
        }
    }

    if (! replaced)
        settings.apiProfiles.push_back (profile);

    processor.setSettings (settings);
    refreshProfiles (true);
    setStatus ((replaced ? dmb::utf8 ("已更新方案: ") : dmb::utf8 ("已保存方案: ")) + profile.name);
}

void DeepSeekMidiBridgeEditor::deleteSelectedProfile()
{
    const auto index = profileBox.getSelectedId() - 1;
    auto settings = processor.getSettings();

    if (index < 0 || index >= (int) settings.apiProfiles.size())
    {
        setStatus (dmb::utf8 ("请先在上面的下拉框里选中一个方案"));
        return;
    }

    const auto name = settings.apiProfiles[(size_t) index].name;

    settings.apiProfiles.erase (settings.apiProfiles.begin() + index);
    processor.setSettings (settings);
    refreshProfiles (false);
    setStatus (dmb::utf8 ("已删除方案: ") + name);
}

void DeepSeekMidiBridgeEditor::runApiTest()
{
    applySettingsFromFields();

    const auto settings = processor.getSettings();

    if (settings.api.apiKey.trim().isEmpty())
    {
        setStatus (dmb::utf8 ("请先填入 API Key"));
        return;
    }

    processor.getApiProbe().start (settings.api);
    setStatus (dmb::utf8 ("正在测试 ") + settings.api.endpoint + "  ·  " + settings.api.model + " ...");
    updateApiStateLabel();
}

void DeepSeekMidiBridgeEditor::updateApiStateLabel()
{
    const auto settings = processor.getSettings();
    const auto probe = processor.getApiProbe().getResult();

    juce::String text;
    juce::Colour colour = kTextDim;

    if (processor.getApiProbe().isRunning())
    {
        text = dmb::utf8 ("● 正在测试 ") + settings.api.model + " ...";
        colour = kAccent;
    }
    else if (probe.valid)
    {
        text = (probe.ok ? dmb::utf8 ("✔ ") : dmb::utf8 ("✘ ")) + probe.message;
        colour = probe.ok ? juce::Colour (0xff5ddc7f) : juce::Colour (0xffffa64d);
    }
    else
    {
        const auto hasKey = settings.api.apiKey.trim().isNotEmpty();
        text = (hasKey ? dmb::utf8 ("● ") : dmb::utf8 ("○ "))
             + dmb::ApiProbe::describeKey (settings.api.apiKey) + "  ·  "
             + juce::URL (settings.api.endpoint).getDomain() + "  ·  " + settings.api.model;
        colour = hasKey ? kText : kTextDim;
    }

    apiStateLabel.setText (text, juce::dontSendNotification);
    apiStateLabel.setColour (juce::Label::textColourId, colour);
}

//==============================================================================
std::vector<dmb::SourceTrack> DeepSeekMidiBridgeEditor::getSelectedSources() const
{
    std::vector<dmb::SourceTrack> result;

    for (const auto& item : sourceItems)
        if (item.included)
            result.push_back (item.source);

    return result;
}

juce::String DeepSeekMidiBridgeEditor::buildRequirementText() const
{
    auto text = promptEditor.getText().trim();

    if (text.isEmpty())
        text = dmb::utf8 ("请根据上面的素材创作一段能与之配合的 MIDI。");

    return text;
}

void DeepSeekMidiBridgeEditor::setStatus (const juce::String& text)
{
    statusLabel.setText (text, juce::dontSendNotification);
}

//==============================================================================
void DeepSeekMidiBridgeEditor::startGeneration()
{
    processor.setPromptText (promptEditor.getText());
    applySettingsFromFields();

    auto sources = getSelectedSources();

    if (sources.empty())
        setStatus (dmb::utf8 ("没有选中任何参考轨道，将让模型自由创作..."));

    processor.startGeneration (buildRequirementText(), sources);
    lastLogVersion = -1;
    updateButtons();
}

void DeepSeekMidiBridgeEditor::togglePreview()
{
    const bool playing = listenButton.getToggleState();
    processor.setFreeRunPreview (playing);
    listenButton.setButtonText (playing ? dmb::utf8 ("停止试听") : dmb::utf8 ("试听"));
}

void DeepSeekMidiBridgeEditor::showPromptPreview()
{
    const auto spec = processor.makeGenerationSpec();
    const auto sources = getSelectedSources();

    const auto text = "=== system ===\n" + dmb::buildSystemPrompt (spec)
                    + "\n=== user ===\n" + dmb::buildUserPrompt (sources, spec, buildRequirementText());

    logEditor.setText (text);
    setStatus (dmb::utf8 ("已在下方显示将发送给 DeepSeek 的完整提示词"));
}

//==============================================================================
void DeepSeekMidiBridgeEditor::chooseOutputDirectory()
{
    fileChooser = std::make_unique<juce::FileChooser> (dmb::utf8 ("选择工程根目录（每个工程一个子文件夹）"),
                                                       processor.getProjectsRoot(),
                                                       juce::String());

    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& chooser)
                              {
                                  const auto result = chooser.getResult();

                                  if (result == juce::File())
                                      return;

                                  processor.setOutputDirectory (result);
                                  outputDirButton.setButtonText (dmb::utf8 ("输出根目录: ") + result.getFileName());
                                  refreshProjectFields();
                                  setStatus (dmb::utf8 ("工程根目录已改为 ") + dmb::displayPath (result)
                                             + dmb::utf8 ("（当前工程的文件夹位置不变）"));
                              });
}

void DeepSeekMidiBridgeEditor::importMidiFiles (const juce::StringArray& paths)
{
    std::vector<dmb::SourceTrack> imported;
    juce::String lastError;

    for (const auto& path : paths)
    {
        const juce::File file (path);

        if (! file.existsAsFile())
            continue;

        juce::String error;
        auto tracks = dmb::importMidiFile (file, error);

        if (error.isNotEmpty())
            lastError = error;

        for (auto& track : tracks)
            imported.push_back (std::move (track));
    }

    if (imported.empty())
    {
        setStatus (lastError.isNotEmpty() ? lastError : juce::String (dmb::utf8 ("没有导入任何音符")));
        return;
    }

    const auto count = (int) imported.size();

    processor.addImportedSources (std::move (imported));
    setStatus (dmb::utf8 ("已导入 ") + juce::String (count) + dmb::utf8 (" 条参考轨"));
    refreshSources (true);
}

void DeepSeekMidiBridgeEditor::chooseMidiFileToImport()
{
    fileChooser = std::make_unique<juce::FileChooser> (dmb::utf8 ("选择要作为参考素材的 MIDI 文件"),
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       "*.mid;*.midi");

    fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                | juce::FileBrowserComponent::canSelectFiles
                                | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& chooser)
                              {
                                  juce::StringArray paths;

                                  for (const auto& file : chooser.getResults())
                                      paths.add (file.getFullPathName());

                                  importMidiFiles (paths);
                              });
}

//==============================================================================
bool DeepSeekMidiBridgeEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& path : files)
    {
        const auto extension = juce::File (path).getFileExtension().toLowerCase();

        if (extension == ".mid" || extension == ".midi")
            return true;
    }

    return false;
}

void DeepSeekMidiBridgeEditor::fileDragEnter (const juce::StringArray&, int, int)
{
    fileDragActive = true;
    repaint();
}

void DeepSeekMidiBridgeEditor::fileDragExit (const juce::StringArray&)
{
    fileDragActive = false;
    repaint();
}

void DeepSeekMidiBridgeEditor::filesDropped (const juce::StringArray& files, int, int)
{
    fileDragActive = false;
    repaint();

    importMidiFiles (files);
}

void DeepSeekMidiBridgeEditor::saveResultAs()
{
    auto clip = processor.getGeneratedClip();

    if (clip == nullptr || clip->isEmpty())
    {
        setStatus (dmb::utf8 ("还没有生成结果"));
        return;
    }

    const auto suggested = clip->file.existsAsFile()
                             ? clip->file
                             : processor.getOutputDirectory().getChildFile (dmb::sanitiseFileName (clip->title) + ".mid");

    fileChooser = std::make_unique<juce::FileChooser> (dmb::utf8 ("另存为 MIDI 文件"), suggested, "*.mid");

    fileChooser->launchAsync (juce::FileBrowserComponent::saveMode
                                | juce::FileBrowserComponent::canSelectFiles
                                | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, clip] (const juce::FileChooser& chooser)
                              {
                                  const auto target = chooser.getResult();

                                  if (target == juce::File())
                                      return;

                                  auto midi = dmb::buildMidiFile (*clip, -1);
                                  juce::FileOutputStream stream (target);

                                  if (stream.openedOk() && midi.writeTo (stream))
                                  {
                                      stream.flush();
                                      setStatus (dmb::utf8 ("已保存 ") + dmb::displayPath (target));
                                  }
                                  else
                                  {
                                      setStatus (dmb::utf8 ("保存失败: ") + dmb::displayPath (target));
                                  }
                              });
}

//==============================================================================
void DeepSeekMidiBridgeEditor::updateProjectHeader()
{
    processor.ensureProject();

    sourcesLabel.setText (dmb::utf8 ("① 其它音轨的 MIDI（自动捕获）  ·  工程：") + processor.getProjectName(),
                          juce::dontSendNotification);
}

void DeepSeekMidiBridgeEditor::refreshProjectFields()
{
    processor.ensureProject();

    const auto info = processor.getProject();

    projectNameEditor.setText (info.name, juce::dontSendNotification);

    projectPathLabel.setText (info.folder == juce::File()
                                 ? dmb::utf8 ("工程文件夹会在保存工程 / 生成 / 捕获素材时自动建立")
                                 : dmb::displayPath (info.folder)
                                       + (info.folder.isDirectory()
                                              ? juce::String()
                                              : dmb::utf8 ("   （还没建立：保存工程或生成一次就会出现）")),
                              juce::dontSendNotification);

    projectEntries = processor.listProjects();
    projectBox.clear (juce::dontSendNotification);

    int id = 0;
    int selected = 0;

    for (const auto& entry : projectEntries)
    {
        ++id;
        projectBox.addItem (entry.name + (entry.id == info.id ? dmb::utf8 ("  ● 当前") : juce::String()), id);

        if (entry.id == info.id)
            selected = id;
    }

    if (projectEntries.empty())
        projectBox.addItem (dmb::utf8 ("（还没有工程）"), 1);

    projectBox.setSelectedId (selected > 0 ? selected : 1, juce::dontSendNotification);

    const auto detached = processor.getDetachedLibraryCount();

    mergeDetachedButton.setButtonText (detached > 0
                                          ? dmb::utf8 ("并入未归类素材 (") + juce::String (detached) + ")"
                                          : dmb::utf8 ("并入未归类素材"));
    mergeDetachedButton.setEnabled (detached > 0);

    updateProjectHeader();
}

void DeepSeekMidiBridgeEditor::applyProjectName()
{
    const auto wanted = projectNameEditor.getText().trim();
    const auto current = processor.getProject();

    if (wanted.isEmpty() || wanted == current.name)
        return;

    juce::String error;

    if (processor.renameProject (wanted, error))
    {
        setStatus (dmb::utf8 ("工程已改名为「") + wanted + dmb::utf8 ("」；文件夹也移动了，生成的文件一起搬过去"));
        refreshProjectFields();
        refreshSources (true);
    }
    else
    {
        setStatus (error);
        projectNameEditor.setText (current.name, juce::dontSendNotification);
    }
}

void DeepSeekMidiBridgeEditor::selectProjectFromBox()
{
    const auto index = projectBox.getSelectedId() - 1;

    if (index < 0 || index >= (int) projectEntries.size())
        return;

    const auto wanted = projectEntries[(size_t) index];
    const auto current = processor.getProject();

    if (wanted.id == current.id)
        return;

    if (! wanted.folder.isDirectory())
    {
        setStatus (dmb::utf8 ("这个工程的文件夹已经不在了: ") + dmb::displayPath (wanted.folder));
        refreshProjectFields();
        return;
    }

    juce::String error;

    if (processor.switchProject (wanted.folder, error))
    {
        setStatus (dmb::utf8 ("已切到工程「") + wanted.name + dmb::utf8 ("」——① 里只显示这个工程的素材"));
        refreshProjectFields();
        refreshSources (true);
        refreshResult (true, false);
    }
    else
    {
        setStatus (error);
        refreshProjectFields();
    }
}

void DeepSeekMidiBridgeEditor::createNewProject()
{
    auto name = projectNameEditor.getText().trim();

    if (name.isEmpty() || name == processor.getProject().name)
        name = processor.getProjectName() + " " + juce::Time::getCurrentTime().formatted ("%m-%d %H-%M");

    juce::String error;

    if (processor.createProject (name, error))
    {
        setStatus (dmb::utf8 ("已新建空工程「") + name + dmb::utf8 ("」，之后的生成结果和素材都放进它自己的文件夹"));
        refreshProjectFields();
        refreshSources (true);
        refreshResult (true, false);
    }
    else
    {
        setStatus (error);
    }
}

//==============================================================================
void DeepSeekMidiBridgeEditor::refreshSources (bool force)
{
    auto sources = processor.getAvailableSources();

    juce::String signature;

    for (const auto& source : sources)
        signature << source.id << ":" << (int) source.notes.size() << ":" << (int) source.lastTick << ";";

    if (! force && signature == sourceSignature)
        return;

    sourceSignature = signature;

    std::vector<SourceItem> items;

    for (auto& source : sources)
    {
        SourceItem item;
        item.source = std::move (source);

        for (const auto& old : sourceItems)
            if (old.source.id == item.source.id)
                item.included = old.included;

        items.push_back (std::move (item));
    }

    sourceItems = std::move (items);
    sourcesList.updateContent();

    if (sourceItems.empty())
    {
        previewSourceId.clear();
        sourceRoll.clear();
        return;
    }

    const auto found = std::find_if (sourceItems.begin(), sourceItems.end(),
                                     [this] (const SourceItem& item) { return item.source.id == previewSourceId; });

    if (found == sourceItems.end())
    {
        previewSourceId = sourceItems.front().source.id;
        sourceRoll.setSource (sourceItems.front().source);
    }
    else
    {
        sourceRoll.setSource (found->source);
    }
}

void DeepSeekMidiBridgeEditor::refreshResult (bool force, bool autoReveal)
{
    const auto version = processor.getClipVersion();

    if (! force && version == lastClipVersion)
        return;

    const bool isNewClip = (version != lastClipVersion);

    lastClipVersion = version;

    auto clip = processor.getGeneratedClip();

    if (clip == nullptr || clip->isEmpty())
    {
        resultChip.setFile ({});
        resultChip.setCaption (dmb::utf8 ("还没有生成结果"));
        trackChips.clear();
        resultRoll.clear();
        resized();
        return;
    }

    resultChip.setFile (clip->file);
    resultChip.setCaption (clip->title + dmb::utf8 ("  ·  ") + juce::String (clip->totalNotes()) + dmb::utf8 (" 音符 · ")
                           + juce::String (clip->bars) + dmb::utf8 (" 小节 · ") + juce::String (clip->bpm, 1) + " BPM"
                           + (clip->explanation.isNotEmpty() ? (dmb::utf8 ("  —  ") + clip->explanation) : juce::String()));

    trackChips.clear();

    int index = 0;

    for (const auto& file : clip->perTrackFiles)
    {
        if (index >= 8)
            break;

        auto* chip = new ResultChip (dmb::utf8 ("轨 ") + juce::String (index + 1) + ": " + file.getFileNameWithoutExtension());
        chip->setFile (file);
        content.addAndMakeVisible (chip);
        trackChips.add (chip);
        ++index;
    }

    resultRoll.setClip (*clip);

    dmb::logDiagnostic ("result", "new clip: " + clip->title + "  notes=" + juce::String (clip->totalNotes())
                                  + "  file=" + dmb::displayPath (clip->file)
                                  + (clip->file.existsAsFile() ? "  (exists)" : "  (MISSING)"));

    // default behaviour: put the freshly written .mid in front of the user, ready to
    // be dragged into the host from Explorer (can be switched off in the settings)
    if (autoReveal && isNewClip && processor.getSettings().openFolderAfterGenerate)
        revealResultInFolder();

    resized();
}

void DeepSeekMidiBridgeEditor::refreshLogAndStatus()
{
    auto& service = processor.getGenerationService();

    const auto version = service.getLogVersion();

    if (version != lastLogVersion)
    {
        lastLogVersion = version;
        logEditor.setText (service.getLog());
        logEditor.moveCaretToEnd();
    }

    setStatus (service.getStatusMessage());

    const auto local = processor.getLocalCapture();
    juce::String localText = dmb::utf8 ("本机已捕获 ") + juce::String ((int) local.notes.size()) + dmb::utf8 (" 个音符");

    const auto keptCount = processor.getKeptCaptureCount();

    if (keptCount > 0)
        localText << dmb::utf8 ("  ·  已保留 ") << juce::String (keptCount) << dmb::utf8 (" 段");

    const auto trackName = processor.getHostTrackName();

    if (trackName.isNotEmpty())
        localText << dmb::utf8 ("  ·  本轨：") << trackName;

    if (processor.getHostIsPlaying())
        localText << dmb::utf8 ("  ·  宿主播放中 ") << juce::String (processor.getHostBpm(), 1) << " BPM";
    else
        localText << dmb::utf8 ("  ·  宿主停止（播放宿主或弹奏键盘即可捕获）");

    localStatusLabel.setText (localText, juce::dontSendNotification);
    localStatusLabel.setColour (juce::Label::textColourId, local.notes.empty() ? kTextDim : kAccent);
}

void DeepSeekMidiBridgeEditor::updateButtons()
{
    auto& service = processor.getGenerationService();
    const bool running = service.isRunning();

    if (running)
        generateButton.setButtonText (dmb::utf8 ("正在生成..."));
    else
        generateButton.setButtonText (dmb::utf8 ("生成 MIDI"));

    generateButton.setEnabled (! running);
    cancelButton.setEnabled (running);

    // the automatic length switch greys out the manual bar count
    const auto automaticLength = matchLoopButton.getToggleState();

    if (barsSlider.isEnabled() == automaticLength)
        barsSlider.setEnabled (! automaticLength);

    if (listenButton.getToggleState() != processor.isFreeRunPreview())
    {
        listenButton.setToggleState (processor.isFreeRunPreview(), juce::dontSendNotification);
        listenButton.setButtonText (processor.isFreeRunPreview() ? dmb::utf8 ("停止试听") : dmb::utf8 ("试听"));
    }
}

void DeepSeekMidiBridgeEditor::timerCallback()
{
    updateApiStateLabel();
    refreshLogAndStatus();
    refreshSources (false);
    refreshResult (false);
    updateButtons();
    updateProjectHeader();
}

//==============================================================================
void DeepSeekMidiBridgeEditor::paint (juce::Graphics&)
{
    // everything visible is drawn by the content component
}

bool DeepSeekMidiBridgeEditor::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey && showSettings)
    {
        toggleSettingsPanel (false);
        return true;
    }

    return false;
}

void DeepSeekMidiBridgeEditor::resized()
{
    // The content is designed at a fixed base size and scaled as one unit: that is
    // what keeps the text crisp at non-100% UI scales, because the host never has
    // to bitmap-stretch our window.
    content.setSize (kBaseWidth, kBaseHeight);
    content.setTransform (juce::AffineTransform::scale ((float) uiScale));
    layoutContent();
}

void DeepSeekMidiBridgeEditor::paintContent (juce::Graphics& g)
{
    auto bounds = juce::Rectangle<int> (0, 0, kBaseWidth, kBaseHeight);

    g.fillAll (kBackground);

    if (fileDragActive)
    {
        auto full = bounds.toFloat().reduced (2.0f);
        g.setColour (kAccent.withAlpha (0.9f));
        g.drawRoundedRectangle (full, 8.0f, 3.0f);

        g.setColour (kAccent);
        g.setFont (juce::Font (juce::FontOptions (16.0f, juce::Font::bold)));
        g.drawText (dmb::utf8 ("松开：把这个 .mid 当作参考素材"),
                    bounds.removeFromTop (60), juce::Justification::centred, false);
    }

    auto area = juce::Rectangle<int> (0, 0, kBaseWidth, kBaseHeight).reduced (8);

    // top bar (one compact row)
    g.setColour (kPanel);
    g.fillRoundedRectangle (area.removeFromTop (40).toFloat(), 6.0f);

    // left / right panels
    auto left = area.removeFromLeft (400);
    g.setColour (kPanel.withAlpha (0.55f));
    g.fillRoundedRectangle (left.toFloat(), 6.0f);

    area.removeFromLeft (8);

    g.setColour (kPanel.withAlpha (0.55f));
    g.fillRoundedRectangle (area.toFloat(), 6.0f);
}

//==============================================================================
void DeepSeekMidiBridgeEditor::layoutContent()
{
    auto area = juce::Rectangle<int> (0, 0, kBaseWidth, kBaseHeight).reduced (8);

    // ---- top bar: just the settings button and one status line --------------
    auto top = area.removeFromTop (40).reduced (8, 7);
    settingsButton.setBounds (top.removeFromLeft (96).reduced (0, 1));
    top.removeFromLeft (12);
    apiStateLabel.setBounds (top);

    area.removeFromTop (8);

    // (the API controls, advanced options and the UI scale live in the settings panel)

    // ---- left column --------------------------------------------------------
    auto left = area.removeFromLeft (400).reduced (10, 8);

    sourcesLabel.setBounds (left.removeFromTop (22));
    left.removeFromTop (4);
    sourcesList.setBounds (left.removeFromTop (116));
    left.removeFromTop (6);

    auto nameRow = left.removeFromTop (24);
    applyNameButton.setBounds (nameRow.removeFromLeft (96).reduced (0, 1));
    nameRow.removeFromLeft (6);
    instanceNameEditor.setBounds (nameRow.reduced (0, 1));
    left.removeFromTop (6);

    auto importRow = left.removeFromTop (24);
    importButton.setBounds (importRow.removeFromLeft (132).reduced (0, 1));
    importRow.removeFromLeft (6);
    clearRefButton.setBounds (importRow.removeFromLeft (100).reduced (0, 1));
    importRow.removeFromLeft (6);
    clearCaptureButton.setBounds (importRow.removeFromLeft (100).reduced (0, 1));
    left.removeFromTop (6);

    auto sourceActionRow = left.removeFromTop (24);
    renameSourceButton.setBounds (sourceActionRow.removeFromLeft (120).reduced (0, 1));
    sourceActionRow.removeFromLeft (6);
    deleteSourceButton.setBounds (sourceActionRow.removeFromLeft (120).reduced (0, 1));
    left.removeFromTop (8);

    localStatusLabel.setBounds (left.removeFromBottom (20));
    left.removeFromBottom (4);
    usageHint.setBounds (left.removeFromBottom (52));
    left.removeFromBottom (6);
    sourceRoll.setBounds (left);

    area.removeFromLeft (8);

    // ---- right column -------------------------------------------------------
    auto right = area.reduced (10, 8);

    auto promptHeader = right.removeFromTop (22);
    promptLabel.setBounds (promptHeader.removeFromLeft (150));
    quickFillBox.setBounds (promptHeader.removeFromLeft (200).reduced (0, 1));
    promptEditor.setBounds (right.removeFromTop (78));
    right.removeFromTop (6);

    auto extraRow = right.removeFromTop (38);
    const auto third = extraRow.getWidth() / 3;

    auto styleArea = extraRow.removeFromLeft (third).reduced (0, 1);
    styleLabel.setBounds (styleArea.removeFromTop (14));
    styleEditor.setBounds (styleArea);

    extraRow.removeFromLeft (6);
    auto instrumentArea = extraRow.removeFromLeft (third).reduced (0, 1);
    instrumentLabel.setBounds (instrumentArea.removeFromTop (14));
    instrumentEditor.setBounds (instrumentArea);

    extraRow.removeFromLeft (6);
    auto keyArea = extraRow.reduced (0, 1);
    keyLabel.setBounds (keyArea.removeFromTop (14));
    keyEditor.setBounds (keyArea);

    right.removeFromTop (8);

    // ---- options: one row with the everyday controls ------------------------
    auto row1 = right.removeFromTop (40);

    auto placeSlider = [] (juce::Rectangle<int> rowArea, juce::Label& label, juce::Slider& slider, int width)
    {
        auto cell = rowArea.removeFromLeft (width);
        label.setBounds (cell.removeFromTop (14));
        slider.setBounds (cell);
        return rowArea;
    };

    row1 = placeSlider (row1, barsLabel, barsSlider, 150);
    row1.removeFromLeft (8);
    row1 = placeSlider (row1, tempLabel, tempSlider, 170);
    row1.removeFromLeft (8);
    row1 = placeSlider (row1, transposeLabel, transposeSlider, 150);
    row1.removeFromLeft (8);

    auto channelArea = row1.removeFromLeft (80);
    channelLabel.setBounds (channelArea.removeFromTop (14));
    channelBox.setBounds (channelArea.reduced (0, 1));

    right.removeFromTop (8);

    auto buttonRow = right.removeFromTop (32);
    generateButton.setBounds (buttonRow.removeFromLeft (170).reduced (0, 1));
    buttonRow.removeFromLeft (8);
    cancelButton.setBounds (buttonRow.removeFromLeft (90).reduced (0, 1));
    buttonRow.removeFromLeft (8);
    previewPromptButton.setBounds (buttonRow.removeFromLeft (120).reduced (0, 1));

    right.removeFromTop (6);
    statusLabel.setBounds (right.removeFromTop (20));
    right.removeFromTop (4);
    logEditor.setBounds (right.removeFromTop (104));
    right.removeFromTop (8);

    resultLabel.setBounds (right.removeFromTop (22));
    right.removeFromTop (4);

    auto chipRow = right.removeFromTop (30);
    resultChip.setBounds (chipRow.removeFromLeft (juce::jmax (200, chipRow.getWidth() - 430)).reduced (0, 1));
    chipRow.removeFromLeft (6);
    listenButton.setBounds (chipRow.removeFromLeft (76).reduced (0, 1));
    chipRow.removeFromLeft (4);
    saveAsButton.setBounds (chipRow.removeFromLeft (86).reduced (0, 1));
    chipRow.removeFromLeft (4);
    revealResultButton.setBounds (chipRow.removeFromLeft (116).reduced (0, 1));
    chipRow.removeFromLeft (4);
    clearResultButton.setBounds (chipRow.removeFromLeft (86).reduced (0, 1));

    if (! trackChips.isEmpty())
    {
        right.removeFromTop (4);
        auto trackRow = right.removeFromTop (26);
        const auto chipWidth = juce::jmax (150, trackRow.getWidth() / juce::jmax (1, trackChips.size()));

        for (auto* chip : trackChips)
            chip->setBounds (trackRow.removeFromLeft (chipWidth).reduced (2, 1));
    }

    right.removeFromTop (6);
    resultRoll.setBounds (right);
}
