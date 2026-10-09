#include "PluginProcessor.h"
#include "PluginEditor.h"

HexoscopeProcessor::HexoscopeProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout()),
      fifoData ((size_t) fifoSize)
{
    resetAccumulator();
}

juce::AudioProcessorValueTreeState::ParameterLayout HexoscopeProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;

    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "channels", 1 }, "Channels",
           StringArray { "Mid", "Left", "Right", "Side", "L / R", "M / S" }, 0));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "color", 1 }, "Color",
           StringArray { "Static", "Multi-Band", "Color Map", "HEX" }, 1));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "loop", 1 }, "Loop", false));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "meters", 1 }, "Meters", true));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "speed", 1 }, "Window",
           NormalisableRange<float> (1.0f, 30.0f, 0.1f, 0.5f), 6.0f));
    return l;
}

void HexoscopeProcessor::prepareToPlay (double sr, int)
{
    sampleRateHz = sr;
    a1 = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 200.0f  / (float) sr);
    a2 = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 2500.0f / (float) sr);
    lp1 = lp2 = 0.f;
    fifo.reset();
    resetAccumulator();
    setupLoudness (sr);
    resetLoudness();
}

bool HexoscopeProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto& in  = l.getMainInputChannelSet();
    const auto& out = l.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void HexoscopeProcessor::resetAccumulator()
{
    count = 0;
    for (int c = 0; c < 4; ++c) { mn[c] = 1.0e9f; mx[c] = -1.0e9f; }
    for (float& e : energy) e = 0.f;
}

void HexoscopeProcessor::flushColumn()
{
    Column col;
    for (int c = 0; c < 4; ++c) { col.mn[c] = mn[c]; col.mx[c] = mx[c]; }
    for (int b = 0; b < 3; ++b) col.band[b] = energy[b] / (float) count;

    int s1, n1, s2, n2;
    fifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0) fifoData[(size_t) s1] = col;   // if the FIFO is full we just drop it
    fifo.finishedWrite (n1 + n2);

    resetAccumulator();
}

void HexoscopeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    if (resetRequested.exchange (false))
        resetLoudness();

    const int numCh = buffer.getNumChannels();
    const int n     = buffer.getNumSamples();
    if (numCh == 0 || n == 0) return;

    // Audio passes through untouched; we only read it.
    const float* left  = buffer.getReadPointer (0);
    const float* right = numCh > 1 ? buffer.getReadPointer (1) : left;

    for (int i = 0; i < n; ++i)
    {
        const float L = left[i], R = right[i];

        analyseLoudness (L, R, numCh > 1);
        trackPeaks (0, L);
        if (numCh > 1) trackPeaks (1, R);

        const float M = 0.5f * (L + R), S = 0.5f * (L - R);
        const float v[4] { L, R, M, S };

        for (int c = 0; c < 4; ++c)
        {
            mn[c] = juce::jmin (mn[c], v[c]);
            mx[c] = juce::jmax (mx[c], v[c]);
        }

        lp1 += a1 * (M - lp1);
        lp2 += a2 * (M - lp2);
        const float low = lp1, mid = lp2 - lp1, high = M - lp2;
        energy[0] += low * low;
        energy[1] += mid * mid;
        energy[2] += high * high;

        if (++count >= samplesPerColumn)
            flushColumn();
    }

    const float tp = trueHold.update (truePeakLin, n, peakHoldLen);
    const float sp = sampleHold.update (samplePeakLin, n, peakHoldLen);
    truePeakLin = 0.f;
    samplePeakLin = 0.f;

    truePeakDb   = juce::Decibels::gainToDecibels (tp);
    samplePeakDb = juce::Decibels::gainToDecibels (sp);
}

// ---------------------------------------------------------------------------
// Loudness metering (K-weighting + gating as in ITU-R BS.1770-4 / EBU R128)
// ---------------------------------------------------------------------------
namespace
{
    inline float toLufs (double meanSquare)
    {
        return meanSquare > 1.0e-12 ? (float) (-0.691 + 10.0 * std::log10 (meanSquare)) : -100.0f;
    }
}

void HexoscopeProcessor::setupLoudness (double sr)
{
    const double pi = juce::MathConstants<double>::pi;

    {   // stage 1: high shelf (head-related)
        const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        const double K  = std::tan (pi * f0 / sr);
        const double Vh = std::pow (10.0, G / 20.0);
        const double Vb = std::pow (Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;
        for (auto& f : shelf)
        {
            f.b0 = (Vh + Vb * K / Q + K * K) / a0;
            f.b1 = 2.0 * (K * K - Vh) / a0;
            f.b2 = (Vh - Vb * K / Q + K * K) / a0;
            f.a1 = 2.0 * (K * K - 1.0) / a0;
            f.a2 = (1.0 - K / Q + K * K) / a0;
        }
    }

    {   // stage 2: high-pass (RLB weighting)
        const double f0 = 38.13547087602444, Q = 0.5003270373238773;
        const double K  = std::tan (pi * f0 / sr);
        const double a0 = 1.0 + K / Q + K * K;
        for (auto& f : highpass)
        {
            f.b0 = 1.0; f.b1 = -2.0; f.b2 = 1.0;
            f.a1 = 2.0 * (K * K - 1.0) / a0;
            f.a2 = (1.0 - K / Q + K * K) / a0;
        }
    }

    subBlockLen = juce::jmax (1, (int) std::round (sr * 0.1));   // 100 ms hop
    peakHoldLen = juce::jmax (1, (int) std::round (sr * 3.0));    // peak hold time: 3 s

    // 4x oversampling interpolator for true-peak estimation (windowed sinc)
    for (int p = 0; p < 3; ++p)
    {
        const double frac = (double) (p + 1) / 4.0;
        for (int k = 0; k < 12; ++k)
        {
            const double d = 5.0 + frac - (double) k;
            const double s = std::abs (d) < 1.0e-9 ? 1.0 : std::sin (pi * d) / (pi * d);
            const double w = std::abs (d) < 6.0 ? 0.5 * (1.0 + std::cos (pi * d / 6.0)) : 0.0;
            tpCoef[p][k] = (float) (s * w);
        }
    }
}

void HexoscopeProcessor::resetLoudness()
{
    for (auto& f : shelf)    f.clear();
    for (auto& f : highpass) f.clear();

    subSumSq = 0.0; subCount = 0; ringPos = 0; ringFilled = 0;
    for (auto& v : ring) v = 0.0;
    for (auto& v : histEnergy) v = 0.0;
    for (auto& v : histCount) v = 0;
    for (auto& h : tpHist) for (auto& v : h) v = 0.f;
    truePeakLin = 0.f;
    samplePeakLin = 0.f;
    trueHold = PeakHold();
    sampleHold = PeakHold();

    momentaryLufs  = -100.0f;
    shortLufs      = -100.0f;
    integratedLufs = -100.0f;
    truePeakDb     = -100.0f;
    samplePeakDb   = -100.0f;
}

void HexoscopeProcessor::analyseLoudness (float l, float r, bool stereo)
{
    const double kl = highpass[0].process (shelf[0].process ((double) l));
    double e = kl * kl;

    if (stereo)
    {
        const double kr = highpass[1].process (shelf[1].process ((double) r));
        e += kr * kr;
    }

    subSumSq += e;
    if (++subCount >= subBlockLen)
        finishSubBlock();
}

void HexoscopeProcessor::finishSubBlock()
{
    const double ms = subSumSq / (double) subCount;
    subSumSq = 0.0;
    subCount = 0;

    ring[ringPos] = ms;
    ringPos = (ringPos + 1) % 30;
    ringFilled = juce::jmin (30, ringFilled + 1);

    auto meanOfLast = [this] (int n)
    {
        double s = 0.0;
        for (int i = 1; i <= n; ++i)
            s += ring[(ringPos - i + 30) % 30];
        return s / (double) n;
    };

    if (ringFilled < 4)
        return;

    // momentary = last 400 ms, short-term = last 3 s
    const double m400 = meanOfLast (4);
    const float lk = toLufs (m400);
    momentaryLufs = lk;
    shortLufs = toLufs (meanOfLast (ringFilled));

    // integrated: 400 ms blocks every 100 ms, absolute gate -70 LUFS, relative gate -10 LU
    if (lk > -70.0f)
    {
        const int bin = juce::jlimit (0, numBins - 1, (int) ((lk + 70.0f) * 10.0f));
        histEnergy[bin] += m400;
        ++histCount[bin];
    }

    double sumAll = 0.0;
    int cntAll = 0;
    for (int b = 0; b < numBins; ++b) { sumAll += histEnergy[b]; cntAll += histCount[b]; }

    if (cntAll > 0)
    {
        const float rel = toLufs (sumAll / (double) cntAll) - 10.0f;
        const int relBin = juce::jlimit (0, numBins - 1, (int) std::floor ((rel + 70.0f) * 10.0f));

        double s = 0.0;
        int c = 0;
        for (int b = relBin; b < numBins; ++b) { s += histEnergy[b]; c += histCount[b]; }

        if (c > 0)
            integratedLufs = toLufs (s / (double) c);
    }
}

void HexoscopeProcessor::trackPeaks (int ch, float x)
{
    float* h = tpHist[ch];
    for (int k = 0; k < 11; ++k) h[k] = h[k + 1];
    h[11] = x;

    const float ax = std::abs (x);
    samplePeakLin = juce::jmax (samplePeakLin, ax);
    truePeakLin   = juce::jmax (truePeakLin, ax);

    for (int p = 0; p < 3; ++p)
    {
        float acc = 0.f;
        for (int k = 0; k < 12; ++k) acc += tpCoef[p][k] * h[k];
        truePeakLin = juce::jmax (truePeakLin, std::abs (acc));
    }
}

int HexoscopeProcessor::pullColumns (Column* dest, int maxNum)
{
    int s1, n1, s2, n2;
    fifo.prepareToRead (maxNum, s1, n1, s2, n2);
    for (int i = 0; i < n1; ++i) dest[i] = fifoData[(size_t) (s1 + i)];
    for (int i = 0; i < n2; ++i) dest[n1 + i] = fifoData[(size_t) (s2 + i)];
    fifo.finishedRead (n1 + n2);
    return n1 + n2;
}

juce::AudioProcessorEditor* HexoscopeProcessor::createEditor()
{
    return new HexoscopeEditor (*this);
}

void HexoscopeProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, dest);
}

void HexoscopeProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new HexoscopeProcessor();
}
