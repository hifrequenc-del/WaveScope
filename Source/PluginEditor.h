#pragma once
#include "PluginProcessor.h"

// Flat, minimal look for the controls (rounded translucent buttons, thin slider)
class HexLookAndFeel : public juce::LookAndFeel_V4
{
public:
    HexLookAndFeel()
    {
        const juce::Colour text (0xffe9ecf3);
        setColour (juce::ComboBox::textColourId, text);
        setColour (juce::ComboBox::backgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1a1d25));
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff3b82f6));
        setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
        setColour (juce::TextButton::textColourOffId, juce::Colour (0xffcfd5e2));
        setColour (juce::TextButton::textColourOnId, juce::Colours::white);
        setColour (juce::Slider::textBoxTextColourId, text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int,
                       juce::ComboBox& box) override
    {
        const juce::Rectangle<float> r (0.0f, 0.0f, (float) width, (float) height);
        g.setColour (juce::Colours::white.withAlpha (box.isMouseOver() ? 0.10f : 0.06f));
        g.fillRoundedRectangle (r, 9.0f);

        juce::Path p;
        const float cx = (float) width - 14.0f, cy = (float) height * 0.5f;
        p.startNewSubPath (cx - 3.5f, cy - 1.5f);
        p.lineTo (cx, cy + 2.0f);
        p.lineTo (cx + 3.5f, cy - 1.5f);
        g.setColour (juce::Colour (0xff8a93a6));
        g.strokePath (p, juce::PathStrokeType (1.5f));
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool highlighted, bool) override
    {
        const bool on = b.getToggleState();
        g.setColour (juce::Colours::white.withAlpha (on ? 0.16f : (highlighted ? 0.10f : 0.06f)));
        g.fillRoundedRectangle (b.getLocalBounds().toFloat(), 9.0f);
        g.setColour (on ? juce::Colours::white : juce::Colour (0xff9aa3b5));
        g.setFont (13.0f);
        g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred, false);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                               bool highlighted, bool down) override
    {
        g.setColour (juce::Colours::white.withAlpha (down ? 0.18f : (highlighted ? 0.12f : 0.07f)));
        g.fillRoundedRectangle (b.getLocalBounds().toFloat(), 9.0f);
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                           float, float, juce::Slider::SliderStyle, juce::Slider&) override
    {
        const float cy = (float) y + (float) height * 0.5f;
        const juce::Rectangle<float> track ((float) x, cy - 2.0f, (float) width, 4.0f);
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRoundedRectangle (track, 2.0f);
        g.setColour (juce::Colour (0xff4f8dff));
        g.fillRoundedRectangle (track.withRight (sliderPos), 2.0f);
        g.setColour (juce::Colours::white);
        g.fillEllipse (sliderPos - 7.0f, cy - 7.0f, 14.0f, 14.0f);
    }
};

class HexoscopeEditor : public juce::AudioProcessorEditor,
                        private juce::Timer
{
public:
    explicit HexoscopeEditor (HexoscopeProcessor&);
    ~HexoscopeEditor() override { stopTimer(); setLookAndFeel (nullptr); }

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    HexLookAndFeel lnf;   // declared first so it is destroyed last

    struct Agg
    {
        float mn[4], mx[4];
        float low, mid, high;
    };

    void timerCallback() override;
    void setControlsHidden (bool hide);
    bool metersOn() const;
    void drawMeters (juce::Graphics&, juce::Rectangle<int>);
    bool aggregate (juce::int64 start, int count, Agg& out) const;
    juce::Colour colourFor (const Agg&, int mode, float level) const;

    HexoscopeProcessor& proc;

    static constexpr int capacity = 1 << 17;
    static constexpr int mask = capacity - 1;
    std::vector<Column> history;
    juce::int64 total = 0;

    static constexpr int barHeight = 46;
    static constexpr int meterPanelHeight = 116;
    bool controlsHidden = false;

    juce::ComboBox channelsBox, colorBox;
    juce::ToggleButton loopButton { "Loop" };
    juce::ToggleButton metersButton { "Meters" };
    juce::TextButton resetButton { "Reset" };
    juce::Label windowLabel { {}, "Window" };
    juce::Slider windowSlider;

    using CA = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;
    using BA = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<CA> channelsAtt, colorAtt;
    std::unique_ptr<SA> windowAtt;
    std::unique_ptr<BA> loopAtt, metersAtt;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HexoscopeEditor)
};
