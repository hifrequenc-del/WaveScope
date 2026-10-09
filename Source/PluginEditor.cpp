#include "PluginEditor.h"

namespace
{
    const juce::Colour bgColour     { 0xff0b0c10 };
    const juce::Colour accentColour { 0xff4f8dff };
    const juce::Colour panelColour  { 0xff13151b };
    const juce::Colour panelEdge    { 0x12ffffff };
    const juce::Colour labelColour  { 0xff8a93a6 };
    const juce::Colour valueColour  { 0xffe9ecf3 };

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

    windowLabel.setColour (juce::Label::textColourId, labelColour);
    addAndMakeVisible (windowLabel);

    setLookAndFeel (&lnf);

    setResizable (true, true);
    setResizeLimits (640, 260, 3000, 1200);
    setSize (960, 420);

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

    auto bar = getLocalBounds().removeFromBottom (barHeight).reduced (12, 9);
    channelsBox.setBounds (bar.removeFromLeft (92));   bar.removeFromLeft (8);
    colorBox.setBounds    (bar.removeFromLeft (112));  bar.removeFromLeft (8);
    loopButton.setBounds  (bar.removeFromLeft (60));   bar.removeFromLeft (8);
    metersButton.setBounds (bar.removeFromLeft (76));  bar.removeFromLeft (8);
    resetButton.setBounds (bar.removeFromLeft (60));   bar.removeFromLeft (14);

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
    auto meterPanel = full.removeFromBottom (showMeters ? meterPanelHeight : 0).reduced (12, 4);

    // with controls and meters hidden the waveform fills the whole window (no card)
    const bool bare = controlsHidden && ! showMeters;
    auto cardArea = bare ? full : full.reduced (12, 8);
    auto area = cardArea.reduced (bare ? 8 : 10, bare ? 6 : 10);
    if (area.isEmpty()) return;

    if (! bare)
    {
        g.setColour (panelColour);
        g.fillRoundedRectangle (cardArea.toFloat(), 16.0f);
        g.setColour (panelEdge);
        g.drawRoundedRectangle (cardArea.toFloat().reduced (0.5f), 16.0f, 1.0f);
    }

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
    if (r.getWidth() < 240 || r.getHeight() < 70)
        return;

    auto loudCard = r.removeFromLeft ((int) ((float) r.getWidth() * 0.55f));
    r.removeFromLeft (12);
    auto headCard = r;

    for (auto b : { loudCard, headCard })
    {
        g.setColour (panelColour);
        g.fillRoundedRectangle (b.toFloat(), 16.0f);
        g.setColour (panelEdge);
        g.drawRoundedRectangle (b.toFloat().reduced (0.5f), 16.0f, 1.0f);
    }

    auto text = [&g] (const juce::String& s, juce::Rectangle<int> b, float size,
                      juce::Colour c, juce::Justification j)
    {
        g.setColour (c);
        g.setFont (size);
        g.drawText (s, b, j, false);
    };

    auto fmt = [] (float v) { return v <= -99.0f ? juce::String ("--") : juce::String (v, 1); };

    // One card: title (+ optional status), a big value, two small values and a thin level bar
    auto drawCard = [&] (juce::Rectangle<int> card, const juce::String& title,
                         const juce::String& status, juce::Colour statusColour,
                         float bigValue, const juce::String& bigCaption,
                         float v1, const juce::String& cap1,
                         float v2, const juce::String& cap2,
                         float fraction, juce::Colour barEnd)
    {
        auto inner = card.reduced (16, 12);

        auto head = inner.removeFromTop (16);
        text (title, head, 12.0f, labelColour, juce::Justification::centredLeft);
        if (status.isNotEmpty())
            text (status, head, 12.0f, statusColour, juce::Justification::centredRight);

        auto bar = inner.removeFromBottom (4);
        inner.removeFromBottom (10);

        const auto barF = bar.toFloat();
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.fillRoundedRectangle (barF, 2.0f);
        if (fraction > 0.0f)
        {
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3b6ef0), barF.getX(), 0.0f,
                                                      barEnd, barF.getRight(), 0.0f, false));
            g.fillRoundedRectangle (barF.withWidth (barF.getWidth() * juce::jlimit (0.0f, 1.0f, fraction)), 2.0f);
        }

        auto bigArea = inner.removeFromLeft (inner.getWidth() * 45 / 100);
        text (fmt (bigValue), bigArea.removeFromTop (34), 32.0f, juce::Colour (0xfff4f6fb), juce::Justification::centredLeft);
        text (bigCaption, bigArea.removeFromTop (14), 12.0f, labelColour, juce::Justification::centredLeft);

        auto colA = inner.removeFromLeft (inner.getWidth() / 2);
        text (fmt (v1), colA.removeFromTop (34), 17.0f, valueColour, juce::Justification::centredLeft);
        text (cap1, colA.removeFromTop (14), 12.0f, labelColour, juce::Justification::centredLeft);
        text (fmt (v2), inner.removeFromTop (34), 17.0f, valueColour, juce::Justification::centredLeft);
        text (cap2, inner.removeFromTop (14), 12.0f, labelColour, juce::Justification::centredLeft);
    };

    const juce::Colour cyan (0xff7ee0ff), amber (0xfff5b84a), red (0xfff06565);

    // ---- loudness ----
    const float integrated = proc.getIntegratedLufs();
    drawCard (loudCard, "Loudness (LUFS)", {}, valueColour,
              integrated, "Integrated",
              proc.getShortLufs(), "Short-term",
              proc.getMomentaryLufs(), "Momentary",
              integrated > -99.0f ? (integrated + 40.0f) / 40.0f : 0.0f, cyan);

    // ---- headroom ----
    const float tp = proc.getTruePeakDb();
    const float sp = proc.getSamplePeakDb();

    juce::String status;
    juce::Colour statusColour = cyan;
    if (tp > 0.0f)       { status = "Over 0 dBTP"; statusColour = red; }
    else if (tp > -1.0f) { status = "Close to 0";  statusColour = amber; }

    drawCard (headCard, "Headroom (dB)", status, statusColour,
              tp > -99.0f ? -tp : -100.0f, "Below 0 dBFS",
              tp, "True peak",
              sp, "Sample peak",
              tp > -99.0f ? (tp + 24.0f) / 24.0f : 0.0f, statusColour);
}
