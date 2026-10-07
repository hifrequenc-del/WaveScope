#pragma once
#include "PluginProcessor.h"

class WaveScopeEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit WaveScopeEditor (WaveScopeProcessor&);
    ~WaveScopeEditor() override { stopTimer(); }

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Agg
    {
        float mn[4], mx[4];
        float low, mid, high;
    };

    void timerCallback() override;
    bool aggregate (juce::int64 start, int count, Agg& out) const;
    juce::Colour colourFor (const Agg&, int mode, float level) const;

    WaveScopeProcessor& proc;

    static constexpr int capacity = 1 << 17;
    static constexpr int mask = capacity - 1;
    std::vector<Column> history;
    juce::int64 total = 0;

    static constexpr int barHeight = 34;

    juce::ComboBox channelsBox, colorBox, styleBox;
    juce::ToggleButton loopButton { "Loop" };
    juce::Label windowLabel { {}, "Window" }, scaleLabel { {}, "Scale" };
    juce::Slider windowSlider, scaleSlider;

    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<CA> channelsAtt, colorAtt, styleAtt;
    std::unique_ptr<SA> windowAtt, scaleAtt;
    std::unique_ptr<BA> loopAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveScopeEditor)
};
