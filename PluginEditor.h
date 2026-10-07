#pragma once
#include "PluginProcessor.h"

class HexoscopeEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit HexoscopeEditor (HexoscopeProcessor&);
    ~HexoscopeEditor() override { stopTimer(); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    struct Agg
    {
        Column::ChannelStats channel[4];
    };

    struct DisplayLane
    {
        int channel = 2;
    };

    void timerCallback() override;
    void setControlsHidden (bool hide);
    bool aggregate (juce::int64 start, int count, Agg& out) const;
    juce::Colour colourFor (const Agg&, int channel, int mode, float level) const;
    void paintWaveform (juce::Graphics&, juce::Rectangle<float>, int mode, bool scopeMode, bool wrap, bool triggerEnabled, float seconds);
    void paintSpectrum (juce::Graphics&, juce::Rectangle<float>);
    void paintGoniometer (juce::Graphics&, juce::Rectangle<float>);
    void paintMeters (juce::Graphics&, juce::Rectangle<float>);
    void clearDisplay();
    std::array<DisplayLane, 2> getDisplayLanes (int channelMode, int& numLanes) const;

    HexoscopeProcessor& proc;

    static constexpr int capacity = 1 << 16;
    static constexpr int mask = capacity - 1;
    std::vector<Column> history;
    juce::int64 total = 0;
    juce::int64 lastClearGeneration = 0;
    bool clearPending = false;

    static constexpr int controlsHeight = 70;
    bool controlsHidden = false;

    juce::ComboBox displayModeBox, channelsBox, colorBox;
    juce::ToggleButton wrapButton { "Wrap" };
    juce::ToggleButton freezeButton { "Freeze" };
    juce::ToggleButton triggerButton { "Trigger" };
    juce::TextButton clearButton { "Clear" };

    juce::Label windowLabel { {}, "Window" };
    juce::Slider windowSlider;
    juce::Label thresholdLabel { {}, "Thresh" };
    juce::Slider thresholdSlider;
    juce::Label preLabel { {}, "Pre" };
    juce::Slider preSlider;

    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<CA> displayModeAtt, channelsAtt, colorAtt;
    std::unique_ptr<SA> windowAtt, thresholdAtt, preAtt;
    std::unique_ptr<BA> wrapAtt, freezeAtt, triggerAtt;

    std::array<float, HexoscopeProcessor::fftBins> spectrumSnapshot {};
    std::array<GoniometerPoint, HexoscopeProcessor::goniometerBlockSize> goniometerSnapshot {};
    int goniometerCount = 0;

    float heldPeakDb[2] { -100.0f, -100.0f };
    double lastTimerTime = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HexoscopeEditor)
};
