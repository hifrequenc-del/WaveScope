#include "PluginEditor.h"

namespace
{
    const juce::Colour bgColour     { 0xff090b10 };
    const juce::Colour accentColour { 0xfff5a25d };
    const juce::Colour panelColour  { 0xff0b1120 };
    const juce::Colour panelEdge    { 0xff1a2744 };
    const juce::Colour labelColour  { 0xff8aa0c4 };
    const juce::Colour valueColour  { 0xffe6f1ff };

    struct LaneMap { int a; int b; };           // channel index for lane 1 / lane 2 (-1 = none)
    constexpr LaneMap laneMaps[] = {
        { 2, -1 },  // Mid
        { 0, -1 },  // Left
        { 1, -1 },  // Right
        { 3, -1 },  // Side
        { 0,  1 },  // L / R
        { 2,  3 },  // M / S
    };
}

HexoscopeEditor::HexoscopeEditor (HexoscopeProcessor& p)
    : AudioProcessorEditor (&p), proc (p), history ((size_t) capacity)
{
    auto setupCombo = [this] (juce::ComboBox& box, const juce::String& paramID)
    {
        const auto* param = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (paramID));
        box.addItemList (param->choices, 1);
        addAndMakeVisible (box);
    };
    setupCombo (channelsBox, "channels");
    setupCombo (colorBox,    "color");

    channelsAtt = std::make_unique<CA> (proc.apvts, "channels", channelsBox);
    colorAtt    = std::make_unique<CA> (proc.apvts, "color",    colorBox);

    addAndMakeVisible (loopButton);
    loopAtt = std::make_unique<BA> (proc.apvts, "loop", loopButton);

    addAndMakeVisible (metersButton);
    metersAtt = std::make_unique<BA> (proc.apvts, "meters", metersButton);
    metersButton.onClick = [this] { repaint(); };

    resetButton.onClick = [this] { proc.requestLoudnessReset(); repaint(); };
    addAndMakeVisible (resetButton);

    windowSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    windowSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
    windowSlider.setTextValueSuffix (" s");
    addAndMakeVisible (windowSlider);
    windowAtt = std::make_unique<SA> (proc.apvts, "speed", windowSlider);

    windowLabel.setColour (juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible (windowLabel);

    setResizable (true, true);
    setResizeLimits (560, 220, 3000, 1200);
    setSize (900, 360);

    // Double-click the display to hide / show all controls (saved with the project)
    setControlsHidden ((bool) proc.apvts.state.getProperty ("controlsHidden", false));

    startTimerHz (60);
}

void HexoscopeEditor::setControlsHidden (bool hide)
{
    controlsHidden = hide;
    proc.apvts.state.setProperty ("controlsHidden", hide, nullptr);

    for (auto* c : std::initializer_list<juce::Component*> { &channelsBox, &colorBox, &loopButton,
                                                              &metersButton, &resetButton,
                                                              &windowLabel, &windowSlider })
        c->setVisible (! hide);

    resized();
    repaint();
}

void HexoscopeEditor::mouseDoubleClick (const juce::MouseEvent&)
{
    setControlsHidden (! controlsHidden);
}

void HexoscopeEditor::resized()
{
    if (controlsHidden) return;

    auto bar = getLocalBounds().removeFromBottom (barHeight).reduced (6, 5);
    channelsBox.setBounds (bar.removeFromLeft (84));   bar.removeFromLeft (6);
    colorBox.setBounds    (bar.removeFromLeft (104));  bar.removeFromLeft (6);
    loopButton.setBounds  (bar.removeFromLeft (64));   bar.removeFromLeft (6);
    metersButton.setBounds (bar.removeFromLeft (76));  bar.removeFromLeft (6);
    resetButton.setBounds (bar.removeFromLeft (56));   bar.removeFromLeft (10);

    windowLabel.setBounds (bar.removeFromLeft (56));
    windowSlider.setBounds (bar);
}

void HexoscopeEditor::timerCallback()
{
    Column tmp[512];
    bool got = false;
    int n;
    while ((n = proc.pullColumns (tmp, 512)) > 0)
    {
        for (int i = 0; i < n; ++i)
            history[(size_t) (total++ & mask)] = tmp[i];
        got = true;
    }
    if (got) repaint();
}

bool HexoscopeEditor::aggregate (juce::int64 start, int count, Agg& out) const
{
    if (start < 0 || start + count > total || total - start > capacity)
        return false;

    for (int c = 0; c < 4; ++c) { out.mn[c] = 1e9f; out.mx[c] = -1e9f; }
    float lo = 0, mi = 0, hi = 0;

    for (int i = 0; i < count; ++i)
    {
        const auto& col = history[(size_t) ((start + i) & mask)];
        for (int c = 0; c < 4; ++c)
        {
            out.mn[c] = juce::jmin (out.mn[c], col.mn[c]);
            out.mx[c] = juce::jmax (out.mx[c], col.mx[c]);
        }
        lo += col.band[0]; mi += col.band[1]; hi += col.band[2];
    }
    out.low  = std::sqrt (lo / (float) count);
    out.mid  = std::sqrt (mi / (float) count);
    out.high = std::sqrt (hi / (float) count);
    return true;
}

juce::Colour HexoscopeEditor::colourFor (const Agg& a, int mode, float level) const
{
    if (mode == 0)
        return accentColour;

    const float sum = a.low + a.mid + a.high + 1.0e-9f;

    if (mode == 1)   // Multi-band: low = red, mid = green, high = blue
    {
        const float m = juce::jmax (a.low, a.mid, a.high) + 1.0e-9f;
        auto f = [m] (float v) { return 0.12f + 0.88f * std::pow (v / m, 1.6f); };
        return juce::Colour::fromFloatRGBA (f (a.low), f (a.mid), f (a.high), 1.0f);
    }

    if (mode == 3)   // HEX: modern blues, shifting toward cyan with brightness and high-frequency content
    {
        const float centroid = (a.mid * 0.5f + a.high) / sum;
        const float t = juce::jlimit (0.0f, 1.0f, 0.55f * std::sqrt (centroid) + 0.45f * level);
        static const juce::Colour stops[] { juce::Colour (0xff1e3a8a), juce::Colour (0xff2563eb),
                                            juce::Colour (0xff38bdf8), juce::Colour (0xffa5f3fc) };
        const float pos = t * 3.0f;
        const int i = juce::jmin (2, (int) pos);
        return stops[i].interpolatedWith (stops[i + 1], pos - (float) i);
    }

    // Color map: spectral centroid -> hue, level -> brightness
    const float centroid = (a.mid * 0.5f + a.high) / sum;
    const float hue = 0.03f + 0.58f * centroid;
    return juce::Colour::fromHSV (hue, 0.75f, juce::jlimit (0.45f, 1.0f, 0.45f + level), 1.0f);
}

void HexoscopeEditor::paint (juce::Graphics& g)
{
    g.fillAll (bgColour);

    const bool showMeters = metersOn();

    auto full = getLocalBounds().withTrimmedBottom (controlsHidden ? 0 : barHeight);
    auto meterPanel = full.removeFromBottom (showMeters ? meterPanelHeight : 0).reduced (8, 4);
    auto area = full.reduced (8, 6);
    if (area.isEmpty()) return;

    auto getInt   = [this] (const char* id) { return (int) proc.apvts.getRawParameterValue (id)->load(); };
    auto getFloat = [this] (const char* id) { return proc.apvts.getRawParameterValue (id)->load(); };

    const int   chMode    = juce::jlimit (0, 5, getInt ("channels"));
    const int   colorMode = getInt ("color");
    const bool  loop      = getInt ("loop") != 0;
    const float seconds   = getFloat ("speed");

    const int pxStep = 1;
    const int slots  = juce::jmax (1, area.getWidth() / pxStep);
    const double sr  = proc.getSampleRateHz();
    const int fpp    = juce::jmax (1, (int) std::round (seconds * sr / HexoscopeProcessor::samplesPerColumn / slots));

    const auto lanes = laneMaps[chMode];
    const int numLanes = lanes.b >= 0 ? 2 : 1;
    const float laneH = (float) area.getHeight() / (float) numLanes;

    const juce::int64 head = total / fpp;                     // number of completed pixel groups
    const int headSlot = (int) (head % slots);

    for (int x = 0; x < slots; ++x)
    {
        juce::int64 group;
        if (loop)
            group = (x < headSlot) ? head - headSlot + x : head - headSlot + x - slots;
        else
            group = head - (slots - x);

        Agg a;
        if (! aggregate (group * fpp, fpp, a))
            continue;

        const float px = (float) area.getX() + (float) (x * pxStep);

        for (int k = 0; k < numLanes; ++k)
        {
            const int ch = k == 0 ? lanes.a : lanes.b;
            const float centre = (float) area.getY() + laneH * ((float) k + 0.5f);
            const float half   = laneH * 0.5f * 0.96f;

            const float hiV = juce::jlimit (-1.f, 1.f, a.mx[ch]);
            const float loV = juce::jlimit (-1.f, 1.f, a.mn[ch]);
            const float level = juce::jmax (std::abs (hiV), std::abs (loV));
            if (level < 0.001f) continue;   // silence: draw nothing

            float top = centre - hiV * half;
            float bot = centre - loV * half;
            if (bot - top < 1.0f) { top = centre - 0.5f; bot = centre + 0.5f; }

            const auto colour = colourFor (a, colorMode, level);

            g.setColour (colour);
            g.fillRect (px, top, 1.0f, bot - top);
        }
    }

    if (showMeters)
        drawMeters (g, meterPanel);
}

bool HexoscopeEditor::metersOn() const
{
    return proc.apvts.getRawParameterValue ("meters")->load() > 0.5f;
}

void HexoscopeEditor::drawMeters (juce::Graphics& g, juce::Rectangle<int> r)
{
    if (r.getWidth() < 240 || r.getHeight() < 60)
        return;

    auto loudCard = r.removeFromLeft ((int) ((float) r.getWidth() * 0.58f));
    r.removeFromLeft (8);
    auto headCard = r;

    for (auto b : { loudCard, headCard })
    {
        g.setColour (panelColour);
        g.fillRoundedRectangle (b.toFloat(), 8.0f);
        g.setColour (panelEdge);
        g.drawRoundedRectangle (b.toFloat().reduced (0.5f), 8.0f, 1.0f);
    }

    auto text = [&g] (const juce::String& s, juce::Rectangle<int> b, float size,
                      juce::Colour c, juce::Justification j)
    {
        g.setColour (c);
        g.setFont (size);
        g.drawText (s, b, j, false);
    };

    auto fmt = [] (float v) { return v <= -99.0f ? juce::String ("--") : juce::String (v, 1); };

    auto readout = [&] (juce::Rectangle<int> b, float v, const juce::String& caption, float size)
    {
        text (fmt (v), b.removeFromTop ((int) size + 8), size, valueColour, juce::Justification::centredLeft);
        text (caption, b.removeFromTop (14), 11.0f, labelColour, juce::Justification::centredLeft);
    };

    // ---- loudness ----
    {
        auto inner = loudCard.reduced (12, 8);
        text ("LOUDNESS (LUFS)", inner.removeFromTop (16), 11.0f, labelColour, juce::Justification::centredLeft);

        auto c1 = inner.removeFromLeft (inner.getWidth() * 2 / 5);
        auto c2 = inner.removeFromLeft (inner.getWidth() / 2);
        readout (c1,    proc.getIntegratedLufs(), "Integrated", 28.0f);
        readout (c2,    proc.getShortLufs(),      "Short-term", 20.0f);
        readout (inner, proc.getMomentaryLufs(),  "Momentary",  20.0f);
    }

    // ---- headroom ----
    {
        const float tp = proc.getTruePeakDb();
        const float sp = proc.getSamplePeakDb();

        auto inner = headCard.reduced (12, 8);
        auto top = inner.removeFromTop (16);
        text ("HEADROOM (dB)", top, 11.0f, labelColour, juce::Justification::centredLeft);

        if (tp > -1.0f)
        {
            const bool over = tp > 0.0f;
            const auto col = over ? juce::Colour (0xffef4444) : juce::Colour (0xfff59e0b);
            auto badge = top.removeFromRight (juce::jmin (112, top.getWidth()));
            g.setColour (col.withAlpha (0.2f));
            g.fillRoundedRectangle (badge.toFloat(), 5.0f);
            text (over ? "Over 0 dBTP" : "Close to 0", badge, 11.0f, col, juce::Justification::centred);
        }

        auto d1 = inner.removeFromLeft (inner.getWidth() * 2 / 5);
        auto d2 = inner.removeFromLeft (inner.getWidth() / 2);
        readout (d1,    tp > -99.0f ? -tp : -100.0f, "Headroom",    28.0f);
        readout (d2,    tp,                          "True peak",   20.0f);
        readout (inner, sp,                          "Sample peak", 20.0f);
    }
}
