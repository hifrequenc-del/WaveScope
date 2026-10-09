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
        float mn[4], mx[4];
        float low, mid, high;
    };

    void timerCallback() override;
    void setControlsHidden (bool hide);
    bool aggregate (juce::int64 start, int count, Agg& out) const;
    juce::Colour colourFor (const Agg&, int mode, float level) const;

    HexoscopeProcessor& proc;

    static constexpr int capacity = 1 << 17;
    static constexpr int mask = capacity - 1;
    std::vector<Column> history;
    juce::int64 total = 0;

    static constexpr int barHeight = 34;
    bool controlsHidden = false;

    juce::ComboBox channelsBox, colorBox;
    juce::ToggleButton loopButton { "Loop" };
    juce::Label windowLabel { {}, "Window" };
    juce::Slider windowSlider;

    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<CA> channelsAtt, colorAtt;
    std::unique_ptr<SA> windowAtt;
    std::unique_ptr<BA> loopAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HexoscopeEditor)
};
