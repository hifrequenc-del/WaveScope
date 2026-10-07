#include "PluginEditor.h"

namespace
{
    const juce::Colour bgColour     { 0xff090b10 };
    const juce::Colour panelColour  { 0xff0f131b };
    const juce::Colour accentColour { 0xff38bdf8 };
    const juce::Colour textColour   { 0xffdce6f2 };
    const juce::Colour mutedColour  { 0xff738094 };

    struct LaneMap { int a; int b; };
    constexpr LaneMap laneMaps[] = {
        { 2, -1 }, // Mid
        { 0, -1 }, // Left
        { 1, -1 }, // Right
        { 3, -1 }, // Side
        { 0,  1 }, // L / R
        { 2,  3 }, // M / S
    };

    const char* channelShortName (int c)
    {
        switch (c)
        {
            case 0: return "L";
            case 1: return "R";
            case 2: return "M";
            default: return "S";
        }
    }

    float safeDb (float linear)
    {
        return 20.0f * std::log10 (juce::jmax (linear, 1.0e-7f));
    }
}

HexoscopeEditor::HexoscopeEditor (HexoscopeProcessor& p)
    : AudioProcessorEditor (&p), proc (p), history ((size_t) capacity)
{
    auto setupCombo = [this] (juce::ComboBox& box, const juce::String& paramID)
    {
        if (auto* param = dynamic_cast<juce::AudioParameterChoice*> (proc.apvts.getParameter (paramID)))
            box.addItemList (param->choices, 1);
        box.setTextWhenNothingSelected ("—");
        addAndMakeVisible (box);
    };

    setupCombo (displayModeBox, "displayMode");
    setupCombo (channelsBox, "channels");
    setupCombo (colorBox, "color");

    displayModeAtt = std::make_unique<CA> (proc.apvts, "displayMode", displayModeBox);
    channelsAtt    = std::make_unique<CA> (proc.apvts, "channels", channelsBox);
    colorAtt       = std::make_unique<CA> (proc.apvts, "color", colorBox);

    addAndMakeVisible (wrapButton);
    wrapAtt = std::make_unique<BA> (proc.apvts, "loop", wrapButton);

    addAndMakeVisible (freezeButton);
    freezeAtt = std::make_unique<BA> (proc.apvts, "freeze", freezeButton);

    addAndMakeVisible (triggerButton);
    triggerAtt = std::make_unique<BA> (proc.apvts, "trigger", triggerButton);

    addAndMakeVisible (clearButton);
    clearButton.onClick = [this] { clearDisplay(); };

    auto setupSlider = [this] (juce::Slider& slider, const juce::String& suffix)
    {
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 18);
        slider.setTextValueSuffix (suffix);
        addAndMakeVisible (slider);
    };

    setupSlider (windowSlider, " s");
    setupSlider (thresholdSlider, " dB");
    setupSlider (preSlider, " %");

    thresholdSlider.textFromValueFunction = [] (double value) { return juce::String (value, 1) + " dB"; };
    preSlider.textFromValueFunction = [] (double value) { return juce::String (juce::roundToInt (value * 100.0)) + "%"; };

    windowAtt = std::make_unique<SA> (proc.apvts, "speed", windowSlider);
    thresholdAtt = std::make_unique<SA> (proc.apvts, "triggerThreshold", thresholdSlider);
    preAtt = std::make_unique<SA> (proc.apvts, "preTrigger", preSlider);

    const auto setupLabel = [this] (juce::Label& label)
    {
        label.setColour (juce::Label::textColourId, mutedColour);
        label.setFont (juce::FontOptions (11.0f));
        addAndMakeVisible (label);
    };

    setupLabel (windowLabel);
    setupLabel (thresholdLabel);
    setupLabel (preLabel);

    setResizable (true, true);
    setResizeLimits (520, 190, 3000, 1200);
    setSize (1000, 360);

    setControlsHidden ((bool) proc.apvts.state.getProperty ("controlsHidden", false));
    lastClearGeneration = proc.getClearGeneration();
    startTimerHz (60);
}

void HexoscopeEditor::setControlsHidden (bool hide)
{
    controlsHidden = hide;
    proc.apvts.state.setProperty ("controlsHidden", hide, nullptr);

    for (auto* c : std::initializer_list<juce::Component*> {
             &displayModeBox, &channelsBox, &colorBox, &wrapButton, &freezeButton,
             &triggerButton, &clearButton, &windowLabel, &windowSlider,
             &thresholdLabel, &thresholdSlider, &preLabel, &preSlider })
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
    if (controlsHidden)
        return;

    auto bar = getLocalBounds().removeFromBottom (controlsHeight).reduced (7, 5);
    auto row1 = bar.removeFromTop (29);
    auto row2 = bar.reduced (0, 2);

    auto put = [] (juce::Rectangle<int>& r, juce::Component& c, int width, int gap = 5)
    {
        c.setBounds (r.removeFromLeft (width));
        r.removeFromLeft (gap);
    };

    put (row1, displayModeBox, 110);
    put (row1, channelsBox, 92);
    put (row1, colorBox, 106);
    put (row1, wrapButton, 61);
    put (row1, freezeButton, 70);
    put (row1, triggerButton, 69);
    put (row1, clearButton, 56);

    put (row2, windowLabel, 46, 3);
    put (row2, windowSlider, 220, 10);
    put (row2, thresholdLabel, 45, 3);
    put (row2, thresholdSlider, 180, 10);
    put (row2, preLabel, 26, 3);
    put (row2, preSlider, 130, 0);
}

void HexoscopeEditor::clearDisplay()
{
    total = 0;
    heldPeakDb[0] = heldPeakDb[1] = -100.0f;
    clearPending = true;
    proc.requestClear();
    repaint();
}

void HexoscopeEditor::timerCallback()
{
    const juce::int64 generation = proc.getClearGeneration();
    if (generation != lastClearGeneration)
    {
        total = 0;
        lastClearGeneration = generation;
        clearPending = false;
        heldPeakDb[0] = heldPeakDb[1] = -100.0f;
    }

    // While the audio thread is applying a clear request, don't touch the FIFO.
    // This keeps AbstractFifo strictly single-producer/single-consumer even when
    // the user clicks Clear while the host is rendering.
    const bool frozen = proc.apvts.getRawParameterValue ("freeze")->load() > 0.5f;
    if (! frozen && ! clearPending)
    {
        Column tmp[512];
        int n;
        while ((n = proc.pullColumns (tmp, 512)) > 0)
        {
            for (int i = 0; i < n; ++i)
                history[(size_t) (total++ & mask)] = tmp[i];
        }
    }
    else
    {
        // Keep the audio->GUI queue flowing even while the display is frozen.
        Column tmp[512];
        while (proc.pullColumns (tmp, 512) > 0) {}
    }

    proc.copySpectrum (spectrumSnapshot.data(), (int) spectrumSnapshot.size());
    goniometerCount = proc.copyGoniometer (goniometerSnapshot.data(), (int) goniometerSnapshot.size());

    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
    const float dt = (float) juce::jlimit (0.0, 0.1, now - lastTimerTime);
    lastTimerTime = now;

    for (int i = 0; i < 2; ++i)
    {
        const float peak = proc.getPeakDb (i);
        heldPeakDb[i] = juce::jmax (peak, heldPeakDb[i] - 18.0f * dt);
    }

    repaint();
}

bool HexoscopeEditor::aggregate (juce::int64 start, int count, Agg& out) const
{
    if (start < 0 || count <= 0 || start + count > total || total - start > capacity)
        return false;

    for (auto& channel : out.channel)
    {
        channel.min = 1.0e9f;
        channel.max = -1.0e9f;
        channel.rms = 0.0f;
        channel.band[0] = channel.band[1] = channel.band[2] = 0.0f;
    }

    for (int i = 0; i < count; ++i)
    {
        const auto& col = history[(size_t) ((start + i) & mask)];
        for (int c = 0; c < 4; ++c)
        {
            auto& dst = out.channel[c];
            const auto& src = col.channel[c];
            dst.min = juce::jmin (dst.min, src.min);
            dst.max = juce::jmax (dst.max, src.max);
            dst.rms += src.rms * src.rms;
            for (int b = 0; b < 3; ++b)
                dst.band[b] += src.band[b] * src.band[b];
        }
    }

    const float inv = 1.0f / (float) count;
    for (auto& channel : out.channel)
    {
        channel.rms = std::sqrt (channel.rms * inv);
        for (float& b : channel.band) b = std::sqrt (b * inv);
    }

    return true;
}

juce::Colour HexoscopeEditor::colourFor (const Agg& a, int channel, int mode, float level) const
{
    const auto& stats = a.channel[juce::jlimit (0, 3, channel)];
    if (mode == 0)
        return accentColour;

    const float low = stats.band[0];
    const float mid = stats.band[1];
    const float high = stats.band[2];
    const float sum = low + mid + high + 1.0e-9f;

    if (mode == 1) // Band Balance
    {
        const float m = juce::jmax (low, juce::jmax (mid, high)) + 1.0e-9f;
        const auto curve = [m] (float v) { return 0.08f + 0.92f * std::pow (v / m, 1.35f); };
        return juce::Colour::fromFloatRGBA (curve (low), curve (mid), curve (high), 1.0f);
    }

    if (mode == 2) // Energy Map: warmer for lows, cooler for highs
    {
        const float highBias = (high + 0.5f * mid) / sum;
        const float hue = juce::jlimit (0.0f, 1.0f, 0.02f + 0.58f * highBias);
        const float brightness = juce::jlimit (0.32f, 1.0f, 0.32f + 0.68f * level);
        return juce::Colour::fromHSV (hue, 0.82f, brightness, 1.0f);
    }

    // HEX palette: deliberately consistent blue/cyan identity, modulated by
    // actual band balance and instantaneous waveform level.
    const float highBias = (high + 0.55f * mid) / sum;
    const float t = juce::jlimit (0.0f, 1.0f, 0.5f * std::sqrt (highBias) + 0.5f * level);
    static const juce::Colour stops[] {
        juce::Colour (0xff17346f), juce::Colour (0xff245ed7),
        juce::Colour (0xff35aaf0), juce::Colour (0xffb9f6ff)
    };
    const float pos = t * 3.0f;
    const int index = juce::jmin (2, (int) pos);
    return stops[index].interpolatedWith (stops[index + 1], pos - (float) index);
}

std::array<HexoscopeEditor::DisplayLane, 2> HexoscopeEditor::getDisplayLanes (int channelMode, int& numLanes) const
{
    const auto map = laneMaps[juce::jlimit (0, 5, channelMode)];
    numLanes = map.b >= 0 ? 2 : 1;
    return { DisplayLane { map.a }, DisplayLane { map.b < 0 ? map.a : map.b } };
}

void HexoscopeEditor::paintWaveform (juce::Graphics& g, juce::Rectangle<float> area, int colorMode,
                                     bool scopeMode, bool wrap, bool triggerEnabled, float seconds)
{
    if (area.isEmpty())
        return;

    const int channelMode = (int) proc.apvts.getRawParameterValue ("channels")->load();
    int numLanes = 1;
    const auto lanes = getDisplayLanes (channelMode, numLanes);
    const double sr = proc.getSampleRateHz();
    const int slots = juce::jmax (1, (int) std::floor (area.getWidth()));
    const int columnsNeeded = juce::jmax (1, (int) std::ceil (seconds * sr / (double) HexoscopeProcessor::samplesPerColumn));
    const int fpp = juce::jmax (1, (int) std::ceil ((double) columnsNeeded / (double) slots));
    const float laneH = area.getHeight() / (float) numLanes;
    const juce::int64 lastGroup = total > 0 ? (total - 1) / fpp : -1;

    // Subtle dBFS grid around the waveform centre.
    const float guideDb[] { -6.0f, -12.0f, -24.0f };
    for (int lane = 0; lane < numLanes; ++lane)
    {
        const float centre = area.getY() + laneH * ((float) lane + 0.5f);
        const float half = laneH * 0.48f;

        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawHorizontalLine ((int) std::round (centre), area.getX(), area.getRight());

        for (const float db : guideDb)
        {
            const float a = std::pow (10.0f, db / 20.0f);
            g.setColour (juce::Colours::white.withAlpha (0.035f));
            g.drawHorizontalLine ((int) std::round (centre - a * half), area.getX(), area.getRight());
            g.drawHorizontalLine ((int) std::round (centre + a * half), area.getX(), area.getRight());
        }
    }

    if (total <= 0)
        return;

    const int triggerSlot = juce::jlimit (0, slots - 1,
                                          juce::roundToInt ((float) slots * proc.apvts.getRawParameterValue ("preTrigger")->load()));
    const juce::int64 triggerSample = proc.getLastTriggerSample();
    const int triggerSamplesPerGroup = fpp * HexoscopeProcessor::samplesPerColumn;

    for (int x = 0; x < slots; ++x)
    {
        juce::int64 group = 0;

        if (scopeMode)
        {
            const juce::int64 triggerGroup = (triggerEnabled && triggerSample >= 0 && triggerSamplesPerGroup > 0)
                                               ? triggerSample / triggerSamplesPerGroup
                                               : lastGroup;
            group = triggerGroup - triggerSlot + x;
        }
        else if (wrap)
        {
            const int headSlot = (int) ((lastGroup % slots + slots) % slots);
            const int age = (headSlot - x + slots) % slots;
            group = lastGroup - age;
        }
        else
        {
            group = lastGroup - (slots - 1 - x);
        }

        Agg a;
        if (! aggregate (group * fpp, fpp, a))
            continue;

        const float px = area.getX() + (float) x;

        for (int lane = 0; lane < numLanes; ++lane)
        {
            const int ch = lanes[(size_t) lane].channel;
            const auto& stats = a.channel[ch];
            const float centre = area.getY() + laneH * ((float) lane + 0.5f);
            const float half = laneH * 0.46f;
            const float hiV = juce::jlimit (-1.0f, 1.0f, stats.max);
            const float loV = juce::jlimit (-1.0f, 1.0f, stats.min);
            const float level = juce::jmax (std::abs (hiV), std::abs (loV));
            float top = centre - hiV * half;
            float bottom = centre - loV * half;

            if (bottom < top)
                std::swap (bottom, top);

            if (bottom - top < 1.0f)
            {
                top = centre - 0.5f;
                bottom = centre + 0.5f;
            }

            g.setColour (colourFor (a, ch, colorMode, level));
            g.fillRect (px, top, 1.0f, bottom - top);
        }
    }

    if (scopeMode)
    {
        g.setColour (juce::Colours::white.withAlpha (0.45f));
        g.fillRect (area.getX() + (float) triggerSlot, area.getY(), 1.0f, area.getHeight());
    }
    else if (wrap)
    {
        const int headSlot = (int) ((lastGroup % slots + slots) % slots);
        g.setColour (juce::Colours::white.withAlpha (0.38f));
        g.fillRect (area.getX() + (float) headSlot, area.getY(), 1.0f, area.getHeight());
    }
}

void HexoscopeEditor::paintSpectrum (juce::Graphics& g, juce::Rectangle<float> area)
{
    if (area.isEmpty()) return;

    const float left = area.getX() + 42.0f;
    const float right = area.getRight() - 8.0f;
    const float top = area.getY() + 5.0f;
    const float bottom = area.getBottom() - 20.0f;
    const float width = right - left;
    const float height = bottom - top;
    const float nyquist = (float) proc.getSpectrumRangeHz().second;

    const float dbMarks[] { 0.0f, -12.0f, -24.0f, -36.0f, -48.0f, -60.0f, -72.0f, -84.0f };
    for (const float db : dbMarks)
    {
        const float y = top + (0.0f - db) / 90.0f * height;
        g.setColour (juce::Colours::white.withAlpha (db == 0.0f ? 0.10f : 0.035f));
        g.drawHorizontalLine ((int) y, (int) left, (int) right);
        g.setColour (mutedColour);
        g.setFont (juce::FontOptions (9.0f));
        g.drawText (juce::String (db, 0) + " dB", 2, (int) y - 7, 38, 14, juce::Justification::right);
    }

    const float freqs[] { 20.0f, 100.0f, 1000.0f, 10000.0f };
    for (const float freq : freqs)
    {
        if (freq >= nyquist) continue;
        const float x = left + std::log (freq / 20.0f) / std::log (nyquist / 20.0f) * width;
        g.setColour (juce::Colours::white.withAlpha (0.035f));
        g.drawVerticalLine ((int) x, top, bottom);
        g.setColour (mutedColour);
        g.setFont (juce::FontOptions (9.0f));
        const juce::String text = freq >= 1000.0f ? juce::String (freq / 1000.0f, 0) + "k" : juce::String (freq, 0);
        g.drawText (text, (int) x - 18, (int) bottom + 4, 36, 14, juce::Justification::centred);
    }

    juce::Path spectrumPath;
    bool started = false;
    for (int bin = 1; bin < HexoscopeProcessor::fftBins; ++bin)
    {
        const float freq = (float) bin / (float) (HexoscopeProcessor::fftSize) * (float) (nyquist * 2.0f);
        if (freq < 20.0f) continue;

        const float x = left + std::log (freq / 20.0f) / std::log (nyquist / 20.0f) * width;
        const float mag = spectrumSnapshot[(size_t) bin];
        const float db = juce::jlimit (-90.0f, 3.0f, safeDb (mag));
        const float y = top + (3.0f - db) / 93.0f * height;

        if (! started) { spectrumPath.startNewSubPath (x, y); started = true; }
        else spectrumPath.lineTo (x, y);
    }

    g.setColour (accentColour.withAlpha (0.95f));
    g.strokePath (spectrumPath, juce::PathStrokeType (1.5f));

    const float centroid = proc.getSpectralCentroidHz();
    const float rolloff = proc.getSpectralRolloffHz();
    for (const auto metric : { std::pair<float, juce::Colour> { centroid, juce::Colours::white },
                               std::pair<float, juce::Colour> { rolloff, juce::Colours::orange } })
    {
        if (metric.first <= 20.0f || metric.first >= nyquist) continue;
        const float x = left + std::log (metric.first / 20.0f) / std::log (nyquist / 20.0f) * width;
        g.setColour (metric.second.withAlpha (0.65f));
        g.drawVerticalLine ((int) x, top, bottom);
    }

    g.setColour (mutedColour);
    g.setFont (juce::FontOptions (9.0f));
    g.drawText ("Centroid " + juce::String (centroid, 0) + " Hz   Rolloff " + juce::String (rolloff, 0)
                + " Hz   Flux " + juce::String (proc.getSpectralFlux(), 2),
                (int) left, (int) bottom + 4, 370, 14, juce::Justification::left);
}

void HexoscopeEditor::paintGoniometer (juce::Graphics& g, juce::Rectangle<float> area)
{
    const float side = juce::jmin (area.getWidth(), area.getHeight()) * 0.78f;
    const juce::Point<float> centre (area.getCentreX(), area.getCentreY() + 6.0f);
    const float radius = side * 0.5f;

    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.drawEllipse (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f, 1.0f);
    g.drawEllipse (centre.x - radius * 0.5f, centre.y - radius * 0.5f, radius, radius, 1.0f);
    g.drawLine (centre.x - radius, centre.y, centre.x + radius, centre.y, 1.0f);
    g.drawLine (centre.x, centre.y - radius, centre.x, centre.y + radius, 1.0f);

    for (int i = 1; i < goniometerCount; ++i)
    {
        const auto& a = goniometerSnapshot[(size_t) (i - 1)];
        const auto& b = goniometerSnapshot[(size_t) i];
        const float fade = (float) i / (float) goniometerCount;
        const auto p1 = centre + juce::Point<float> { juce::jlimit (-1.0f, 1.0f, a.mid) * radius,
                                                      -juce::jlimit (-1.0f, 1.0f, a.side) * radius };
        const auto p2 = centre + juce::Point<float> { juce::jlimit (-1.0f, 1.0f, b.mid) * radius,
                                                      -juce::jlimit (-1.0f, 1.0f, b.side) * radius };
        g.setColour (accentColour.withAlpha (0.04f + 0.7f * fade));
        g.drawLine (p1.x, p1.y, p2.x, p2.y, 1.1f);
    }

    g.setColour (textColour);
    g.setFont (juce::FontOptions (10.0f));
    g.drawText ("MID", (int) (centre.x + radius + 5), (int) centre.y - 8, 36, 16, juce::Justification::left);
    g.drawText ("SIDE", (int) centre.x - 24, (int) (centre.y - radius - 22), 48, 16, juce::Justification::centred);

    g.setColour (mutedColour);
    g.drawText ("+1 correlation", (int) area.getX() + 8, (int) area.getY() + 7, 110, 16, juce::Justification::left);
    g.drawText ("Correlation " + juce::String (proc.getCorrelation(), 2),
                (int) area.getRight() - 150, (int) area.getY() + 7, 142, 16, juce::Justification::right);
}

void HexoscopeEditor::paintMeters (juce::Graphics& g, juce::Rectangle<float> area)
{
    const float x0 = area.getX();
    const float y0 = area.getY();

    const float peakL = proc.getPeakDb (0);
    const float peakR = proc.getPeakDb (1);
    const float rmsL = proc.getRmsDb (0);
    const float rmsR = proc.getRmsDb (1);

    auto drawMeter = [&] (float x, const char* name, float peak, float rms, float held)
    {
        constexpr float meterW = 135.0f;
        g.setColour (panelColour);
        g.fillRoundedRectangle (x, y0 + 4.0f, meterW, 24.0f, 4.0f);

        const float peakNorm = juce::jlimit (0.0f, 1.0f, (peak + 60.0f) / 66.0f);
        const float rmsNorm = juce::jlimit (0.0f, 1.0f, (rms + 60.0f) / 66.0f);
        g.setColour (accentColour.withAlpha (0.28f));
        g.fillRoundedRectangle (x + 3.0f, y0 + 11.0f, (meterW - 6.0f) * rmsNorm, 8.0f, 2.0f);
        g.setColour (accentColour);
        g.fillRoundedRectangle (x + 3.0f, y0 + 11.0f, (meterW - 6.0f) * peakNorm, 8.0f, 2.0f);
        const float heldNorm = juce::jlimit (0.0f, 1.0f, (held + 60.0f) / 66.0f);
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        g.fillRect (x + 3.0f + (meterW - 6.0f) * heldNorm, y0 + 8.0f, 1.0f, 14.0f);

        g.setColour (textColour);
        g.setFont (juce::FontOptions (10.0f));
        g.drawText (name, (int) x + 5, (int) y0 + 4, 14, 14, juce::Justification::left);
        g.setColour (mutedColour);
        g.drawText ("P " + juce::String (peak, 1) + "  R " + juce::String (rms, 1),
                    (int) x + 17, (int) y0 + 21, 110, 12, juce::Justification::left);
    };

    drawMeter (x0, "L", peakL, rmsL, heldPeakDb[0]);
    drawMeter (x0 + 145.0f, "R", peakR, rmsR, heldPeakDb[1]);

    const float dc = proc.getDcOffset (2);
    const float asym = proc.getAsymmetry (2) * 100.0f;
    const float corr = proc.getCorrelation();
    g.setColour (mutedColour);
    g.setFont (juce::FontOptions (10.0f));
    const juce::String polarity = corr < -0.55f ? "POLARITY /" : "POLARITY OK";
    g.drawText ("Corr " + juce::String (corr, 2) +
                "   DC " + juce::String (dc, 3) +
                "   Asym " + juce::String (asym, 1) + "%   " + polarity,
                (int) (x0 + 300.0f), (int) y0 + 7, 340, 20, juce::Justification::left);

    g.drawText ("HEXOSCOPE", (int) area.getRight() - 130, (int) y0 + 7, 124, 20, juce::Justification::right);
}

void HexoscopeEditor::paint (juce::Graphics& g)
{
    g.fillAll (bgColour);

    auto area = getLocalBounds().withTrimmedBottom (controlsHidden ? 0 : controlsHeight).reduced (8, 6).toFloat();
    if (area.isEmpty()) return;

    paintMeters (g, area.removeFromTop (34.0f));
    area = area.reduced (0.0f, 2.0f);

    const int mode = juce::jlimit (0, 3, (int) proc.apvts.getRawParameterValue ("displayMode")->load());
    const int colorMode = juce::jlimit (0, 3, (int) proc.apvts.getRawParameterValue ("color")->load());
    const bool wrap = proc.apvts.getRawParameterValue ("loop")->load() > 0.5f;
    const bool trigger = proc.apvts.getRawParameterValue ("trigger")->load() > 0.5f;
    const float seconds = proc.apvts.getRawParameterValue ("speed")->load();

    if (mode == 2)
        paintSpectrum (g, area);
    else if (mode == 3)
        paintGoniometer (g, area);
    else
        paintWaveform (g, area, colorMode, mode == 1, wrap, trigger, seconds);
}
