#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

#include "PianoRollComponent.h"
#include "PluginProcessor.h"

/** The plugin window.

    Layout:
      ① other tracks' captured MIDI (list + piano roll)
      ② the user's request + musical options + the DeepSeek call
      ③ the result: a MIDI file that can be dragged straight into the host
*/
class DeepSeekMidiBridgeEditor : public juce::AudioProcessorEditor,
                                 private juce::Timer,
                                 public juce::FileDragAndDropTarget
{
public:
    explicit DeepSeekMidiBridgeEditor (DeepSeekMidiBridgeProcessor&);
    ~DeepSeekMidiBridgeEditor() override;

    /** The plugin window is a plain frame; everything the user sees lives in the
        "content" component so the whole UI can be scaled crisply (see UI scale in
        the settings panel) without touching the host or other plugins. */
    static constexpr int kBaseWidth = 1060;
    static constexpr int kBaseHeight = 720;

    void paint (juce::Graphics& g) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress& key) override;

    // dropping .mid files onto the window turns them into reference material
    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    //==============================================================================
    /** Holds the whole user interface; scaled as one unit. */
    class ContentHolder : public juce::Component
    {
    public:
        explicit ContentHolder (DeepSeekMidiBridgeEditor& e) : owner (e) {}
        void paint (juce::Graphics& g) override { owner.paintContent (g); }
        void resized() override { owner.layoutContent(); }

    private:
        DeepSeekMidiBridgeEditor& owner;
    };

    /** Dims everything behind the settings panel (and closes it when clicked). */
    class SettingsBackdrop : public juce::Component
    {
    public:
        explicit SettingsBackdrop (DeepSeekMidiBridgeEditor& e) : owner (e) {}
        void paint (juce::Graphics& g) override;
        void mouseDown (const juce::MouseEvent&) override;

    private:
        DeepSeekMidiBridgeEditor& owner;
    };

    /** The overlay panel with API settings, advanced options and the UI scale. */
    class SettingsPanel : public juce::Component
    {
    public:
        explicit SettingsPanel (DeepSeekMidiBridgeEditor& e) : owner (e) {}
        void paint (juce::Graphics& g) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent& e) override;

    private:
        DeepSeekMidiBridgeEditor& owner;
    };

    //==============================================================================
    /** Shows one generated .mid; clicking it reveals the file in Explorer so it can
        be dragged into the host from there (dragging straight out of a plugin window
        is refused by some hosts - Ableton Live ignores it). */
    class ResultChip : public juce::Component
    {
    public:
        explicit ResultChip (const juce::String& captionText);

        void setFile (const juce::File& newFile);
        void setCaption (const juce::String& text);
        juce::File getFile() const { return file; }

        void paint (juce::Graphics& g) override;
        void mouseDown (const juce::MouseEvent& e) override;

        std::function<void()> onClick;

    private:
        juce::File file;
        juce::String caption;
    };

    //==============================================================================
    struct SourceItem
    {
        dmb::SourceTrack source;
        bool included = true;
    };

    class SourceListModel : public juce::ListBoxModel
    {
    public:
        explicit SourceListModel (DeepSeekMidiBridgeEditor& ownerToUse) : owner (ownerToUse) {}

        int getNumRows() override;
        void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
        void listBoxItemClicked (int row, const juce::MouseEvent& e) override;

    private:
        DeepSeekMidiBridgeEditor& owner;
    };

    //==============================================================================
    void timerCallback() override;

    void refreshSources (bool force);
    void refreshResult (bool force, bool autoReveal = true);
    void refreshLogAndStatus();
    void updateButtons();

    void applySettingsFromFields();
    void pushSettingsToFields();

    // quick "API + model" switching
    void refreshProfiles (bool selectCurrent);
    void applyProfile (int profileIndex);
    void saveCurrentAsProfile();
    void deleteSelectedProfile();
    void runApiTest();
    void updateApiStateLabel();

    void chooseOutputDirectory();
    void importMidiFiles (const juce::StringArray& paths);
    void chooseMidiFileToImport();
    void saveResultAs();
    void showPromptPreview();
    void startGeneration();
    void togglePreview();

    // project (see core/ProjectStore.h)
    void refreshProjectFields();
    void applyProjectName();
    void selectProjectFromBox();
    void createNewProject();
    void updateProjectHeader();

    std::vector<dmb::SourceTrack> getSelectedSources() const;
    juce::String buildRequirementText() const;
    void setStatus (const juce::String& text);
    void applyQuickFill (int index);
    void revealResultInFolder();

    /** rename / delete one entry of the ① list */
    void beginRenameSelected();
    void commitRename();
    void cancelRename();
    void deleteSelectedSource();
    void showSourceMenu (int row);
    juce::String getSelectedSourceId() const;

    //==============================================================================
    DeepSeekMidiBridgeProcessor& processor;

    // the whole UI lives in here, so it can be scaled as one unit
    ContentHolder content { *this };
    SettingsPanel settingsPanel { *this };
    SettingsBackdrop settingsBackdrop { *this };
    double uiScale = 1.0;
    bool showSettings = false;

    void paintContent (juce::Graphics& g);
    void layoutContent();
    void toggleSettingsPanel (bool shouldShow);
    void setUiScale (double newScale);
    double getPreferredScaleFromSettings() const;
    double getDisplayScale() const;

    std::unique_ptr<SourceListModel> listModel;
    std::vector<SourceItem> sourceItems;
    juce::String sourceSignature;
    juce::String previewSourceId;
    juce::int64 lastClipVersion = -1;
    juce::int64 lastLogVersion = -1;
    bool fileDragActive = false;
    juce::String renamingSourceId;
    bool updatingFieldsProgrammatically = false;   // guards manual-vs-auto overrides

    // --- top bar (simple: settings button + one status line) -----------------
    juce::TextButton settingsButton { dmb::utf8 ("⚙ 设置") };
    juce::Label apiStateLabel;

    // --- moved into the settings panel ---------------------------------------
    juce::Label apiKeyLabel, modelLabel, endpointLabel, profileLabel, uiScaleLabel;
    juce::TextEditor apiKeyEditor, endpointEditor;
    juce::ComboBox profileBox, modelBox, uiScaleBox;
    juce::TextButton saveApiButton { dmb::utf8 ("保存设置") };
    juce::TextButton saveProfileButton { dmb::utf8 ("+ 存为方案") };
    juce::TextButton deleteProfileButton { dmb::utf8 ("删除方案") };
    juce::TextButton testApiButton { dmb::utf8 ("测试连接") };
    juce::TextButton outputDirButton { dmb::utf8 ("输出目录...") };
    juce::TextButton openOutputButton { dmb::utf8 ("打开输出文件夹") };
    juce::TextButton closeSettingsButton { dmb::utf8 ("关闭") };
    juce::TextButton dragLogButton { dmb::utf8 ("诊断日志") };
    juce::ToggleButton openFolderAfterGenerateButton { dmb::utf8 ("生成完成后自动打开文件夹") };
    juce::Label settingsTitle;

    // --- project (one folder per host project) -------------------------------
    juce::Label projectSectionTitle;
    juce::Label projectNameLabel, projectPickLabel;
    juce::TextEditor projectNameEditor;
    juce::ComboBox projectBox;
    juce::TextButton renameProjectButton { dmb::utf8 ("改名并移动文件夹") };
    juce::TextButton newProjectButton { dmb::utf8 ("新建工程") };
    juce::TextButton openProjectFolderButton { dmb::utf8 ("打开工程文件夹") };
    juce::TextButton mergeDetachedButton { dmb::utf8 ("并入未归类素材") };
    juce::Label projectPathLabel;
    std::vector<dmb::ProjectInfo> projectEntries;

    // --- left column ---------------------------------------------------------
    juce::Label sourcesLabel;
    juce::ListBox sourcesList;
    juce::TextEditor instanceNameEditor;
    juce::TextButton applyNameButton { dmb::utf8 ("命名本实例") };
    juce::TextButton importButton { dmb::utf8 ("导入 .mid 素材") };
    juce::TextButton clearRefButton { dmb::utf8 ("清空导入") };
    juce::TextButton clearCaptureButton { dmb::utf8 ("清空捕获") };
    juce::TextButton renameSourceButton { dmb::utf8 ("重命名所选") };
    juce::TextButton deleteSourceButton { dmb::utf8 ("删除所选") };
    juce::TextEditor renameEditor;                 // inline rename, appears over the list
    PianoRollView sourceRoll;
    juce::Label usageHint;
    juce::Label localStatusLabel;

    // --- right column --------------------------------------------------------
    juce::Label promptLabel;
    juce::ComboBox quickFillBox;
    juce::TextEditor promptEditor;
    juce::TextEditor styleEditor, instrumentEditor, keyEditor;
    juce::Label styleLabel, instrumentLabel, keyLabel;

    juce::Label barsLabel, tempLabel, channelLabel, transposeLabel, maxNotesLabel, referenceBarsLabel;
    juce::Slider barsSlider, tempSlider, transposeSlider, maxNotesSlider, referenceBarsSlider;
    juce::ComboBox channelBox;
    juce::ToggleButton sendToBusButton { dmb::utf8 ("共享给其它实例") };
    juce::ToggleButton liveOutputButton { dmb::utf8 ("直接输出 MIDI") };
    juce::ToggleButton syncButton { dmb::utf8 ("跟随宿主") };
    juce::ToggleButton loopButton { dmb::utf8 ("循环") };
    juce::ToggleButton matchLoopButton { dmb::utf8 ("长度=循环长度") };
    juce::ToggleButton autoGenerateButton { dmb::utf8 ("停止后自动生成") };

    juce::TextButton generateButton { dmb::utf8 ("生成 MIDI") };
    juce::TextButton cancelButton { dmb::utf8 ("取消") };
    juce::TextButton previewPromptButton { dmb::utf8 ("查看提示词") };

    juce::Label statusLabel;
    juce::TextEditor logEditor;

    juce::Label resultLabel;
    ResultChip resultChip { dmb::utf8 ("还没有生成结果") };
    juce::OwnedArray<ResultChip> trackChips;
    juce::TextButton listenButton { dmb::utf8 ("试听") };
    juce::TextButton saveAsButton { dmb::utf8 ("另存为...") };
    juce::TextButton revealResultButton { dmb::utf8 ("打开文件夹") };
    juce::TextButton clearResultButton { dmb::utf8 ("清空结果") };
    PianoRollView resultRoll;

    std::unique_ptr<juce::FileChooser> fileChooser;

    // parameter attachments (kept alive for the lifetime of the editor)
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    std::unique_ptr<SliderAttachment> barsAttachment, tempAttachment, transposeAttachment, maxNotesAttachment, referenceBarsAttachment;
    std::unique_ptr<ComboBoxAttachment> channelAttachment;
    std::unique_ptr<ButtonAttachment> sendToBusAttachment, liveOutputAttachment, syncAttachment, loopAttachment;
    std::unique_ptr<ButtonAttachment> matchLoopAttachment, autoGenerateAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DeepSeekMidiBridgeEditor)
};
