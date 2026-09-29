#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace dmb::params
{

inline constexpr const char* sendToBus      = "sendToBus";
inline constexpr const char* liveOutput     = "liveOutput";
inline constexpr const char* syncToHost     = "syncToHost";
inline constexpr const char* loopPlayback   = "loopPlayback";
inline constexpr const char* windowBars     = "windowBars";
inline constexpr const char* generationBars = "generationBars";
inline constexpr const char* beatsPerBar    = "beatsPerBar";
inline constexpr const char* temperature    = "temperature";
inline constexpr const char* maxNotes       = "maxNotes";
inline constexpr const char* referenceBars  = "referenceBars";
inline constexpr const char* matchLoop      = "matchLoop";
inline constexpr const char* autoGenerate   = "autoGenerate";
inline constexpr const char* channel        = "channel";
inline constexpr const char* transpose      = "transpose";

inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { sendToBus, 1 }, dmb::utf8 ("发送到共享总线 (让其他轨道的实例看到本轨 MIDI)"), true));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { liveOutput, 1 }, dmb::utf8 ("把生成的 MIDI 直接输出给下游乐器"), false));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { syncToHost, 1 }, dmb::utf8 ("跟随宿主播放位置"), true));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { loopPlayback, 1 }, dmb::utf8 ("循环播放生成的片段"), true));

    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { windowBars, 1 }, dmb::utf8 ("捕获最近多少小节"), 1, 64, 8));

    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { generationBars, 1 }, dmb::utf8 ("生成小节数"), 1, 128, 8));

    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { beatsPerBar, 1 }, dmb::utf8 ("每小节拍数"), 1, 16, 4));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { temperature, 1 }, dmb::utf8 ("创意程度 (temperature)"),
        juce::NormalisableRange<float> (0.0f, 2.0f, 0.01f), 0.7f));

    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { maxNotes, 1 }, dmb::utf8 ("最多音符数"), 32, 8000, 2000));

    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { referenceBars, 1 }, dmb::utf8 ("每条参考轨最多发给模型的小节数"), 1, 256, 16));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { matchLoop, 1 }, dmb::utf8 ("生成长度跟随宿主循环长度"), false));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { autoGenerate, 1 }, dmb::utf8 ("停止播放后自动生成"), false));

    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { channel, 1 }, dmb::utf8 ("生成的 MIDI 通道"), 1, 16, 1));

    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { transpose, 1 }, dmb::utf8 ("移调 (半音)"), -24, 24, 0));

    return layout;
}

} // namespace dmb::params
