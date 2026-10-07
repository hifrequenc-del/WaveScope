#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr float epsilon = 1.0e-12f;
}

HexoscopeProcessor::HexoscopeProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "STATE", createLayout()),
      fifoData ((size_t) fifoSize)
{
    resetAccumulator();

    for (auto& v : latestPeakDb) v.store (-100.0f);
    for (auto& v : latestRmsDb) v.store (-100.0f);
    for (auto& v : latestDcOffset) v.store (0.0f);
    for (auto& v : latestAsymmetry) v.store (0.0f);

    // Hann window for the FFT. Precomputed so the audio thread does no trig.
    for (int i = 0; i < fftSize; ++i)
        fftWindow[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (fftSize - 1));
}

juce::AudioProcessorValueTreeState::ParameterLayout HexoscopeProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;

    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "displayMode", 1 }, "Display",
           StringArray { "Waveform", "Scope", "Spectrum", "Goniometer" }, 0));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "channels", 1 }, "Channels",
           StringArray { "Mid", "Left", "Right", "Side", "L / R", "M / S" }, 0));
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "color", 1 }, "Color",
           StringArray { "Static", "Band Balance", "Energy Map", "HEX" }, 1));

    // IDs "loop" and "speed" are retained for project/automation compatibility.
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "loop", 1 }, "Wrap", false));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "speed", 1 }, "Window",
           NormalisableRange<float> (1.0f, 30.0f, 0.1f, 0.5f), 6.0f));

    l.add (std::make_unique<AudioParameterBool> (ParameterID { "freeze", 1 }, "Freeze", false));
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "trigger", 1 }, "Trigger", false));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "triggerThreshold", 1 }, "Trigger Threshold",
           NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -18.0f));
    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "preTrigger", 1 }, "Pre Trigger",
           NormalisableRange<float> (0.0f, 0.5f, 0.01f), 0.20f));

    return l;
}

void HexoscopeProcessor::prepareToPlay (double sr, int)
{
    sampleRateHz = sr;
    a1 = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 200.0f  / (float) sr);
    a2 = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * 2500.0f / (float) sr);

    fifo.reset();
    resetAccumulator();
    resetBlockMetrics();
    for (int c = 0; c < 4; ++c)
    {
        lp1[c] = 0.0f;
        lp2[c] = 0.0f;
    }

    std::fill (fftRing.begin(), fftRing.end(), 0.0f);
    std::fill (previousSpectrum.begin(), previousSpectrum.end(), 0.0f);
    fftSamplesSinceUpdate = 0;
    fftRingWritePos = 0;
    processedSamples = 0;
    previousTriggerValue = 0.0f;
    lastTriggerSample.store (-1, std::memory_order_release);

    for (auto& b : spectrumBuffers)
        std::fill (b.begin(), b.end(), 0.0f);

    publishedSpectrum.store (0, std::memory_order_release);
    spectralCentroidHz.store (0.0f);
    spectralRolloffHz.store (0.0f);
    spectralFlux.store (0.0f);

    publishedGoniometer.store (0, std::memory_order_release);
    goniometerWriteBuffer = 1;
    goniometerWritePos = 0;
    goniometerDecimationCounter = 0;
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
    for (int c = 0; c < 4; ++c)
    {
        mn[c] = 1.0e9f;
        mx[c] = -1.0e9f;
        sumSquares[c] = 0.0f;
        for (float& e : bandEnergy[c]) e = 0.0f;
    }
}

void HexoscopeProcessor::resetBlockMetrics()
{
    std::fill (blockSum, blockSum + 4, 0.0f);
    std::fill (blockSumSquares, blockSumSquares + 4, 0.0f);
    std::fill (blockPositivePeak, blockPositivePeak + 4, 0.0f);
    std::fill (blockNegativePeak, blockNegativePeak + 4, 0.0f);
    blockCrossProduct = 0.0f;
}

void HexoscopeProcessor::flushColumn()
{
    if (count <= 0)
        return;

    Column col;

    for (int c = 0; c < 4; ++c)
    {
        col.channel[c].min = mn[c];
        col.channel[c].max = mx[c];
        col.channel[c].rms = std::sqrt (sumSquares[c] / (float) count);

        for (int b = 0; b < 3; ++b)
            col.channel[c].band[b] = std::sqrt (bandEnergy[c][b] / (float) count);
    }

    int s1, n1, s2, n2;
    fifo.prepareToWrite (1, s1, n1, s2, n2);
    if (n1 > 0)
        fifoData[(size_t) s1] = col; // Full FIFO: drop this visual column rather than block the audio thread.
    fifo.finishedWrite (n1 + n2);

    resetAccumulator();
}

int HexoscopeProcessor::getAnalysisChannel() const
{
    const auto value = apvts.getRawParameterValue ("channels");
    const int mode = value != nullptr ? (int) value->load() : 0;

    switch (mode)
    {
        case 1: return 0; // Left
        case 2: return 1; // Right
        case 3: return 3; // Side
        case 4: return 0; // L/R -> Left for scalar spectrum / trigger
        default: return 2; // Mid / M/S
    }
}

float HexoscopeProcessor::toDb (float value)
{
    return 20.0f * std::log10 (juce::jmax (value, 1.0e-7f));
}

void HexoscopeProcessor::publishBlockMetrics (int numSamples)
{
    const float n = (float) juce::jmax (1, numSamples);

    for (int c = 0; c < 4; ++c)
    {
        const float mean = blockSum[c] / n;
        const float meanSquare = blockSumSquares[c] / n;
        const float absolutePeak = juce::jmax (std::abs (blockPositivePeak[c]),
                                                std::abs (blockNegativePeak[c]));
        latestPeakDb[(size_t) c].store (toDb (absolutePeak), std::memory_order_release);
        latestRmsDb[(size_t) c].store (toDb (std::sqrt (juce::jmax (0.0f, meanSquare))), std::memory_order_release);
        latestDcOffset[(size_t) c].store (mean, std::memory_order_release);

        const float asym = (std::abs (blockPositivePeak[c]) - std::abs (blockNegativePeak[c]))
                         / juce::jmax (juce::jmax (std::abs (blockPositivePeak[c]), std::abs (blockNegativePeak[c])), 1.0e-7f);
        latestAsymmetry[(size_t) c].store (asym, std::memory_order_release);
    }

    const float numerator = (n * blockCrossProduct) - (blockSum[0] * blockSum[1]);
    const float leftVar = juce::jmax (n * blockSumSquares[0] - blockSum[0] * blockSum[0], 0.0f);
    const float rightVar = juce::jmax (n * blockSumSquares[1] - blockSum[1] * blockSum[1], 0.0f);
    const float denom = std::sqrt (leftVar * rightVar);
    const float corr = denom > epsilon ? numerator / denom : 1.0f;
    correlation.store (juce::jlimit (-1.0f, 1.0f, corr), std::memory_order_release);
}

void HexoscopeProcessor::publishSpectrumIfReady()
{
    if (fftSamplesSinceUpdate < fftSize)
        return;

    fftSamplesSinceUpdate = 0;

    for (int i = 0; i < fftSize; ++i)
    {
        const int idx = (fftRingWritePos + i) % fftSize;
        fftData[(size_t) i] = fftRing[(size_t) idx] * fftWindow[(size_t) i];
    }

    std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
    fft.performFrequencyOnlyForwardTransform (fftData.data(), true);

    if (spectrumGuiReadLock.test_and_set (std::memory_order_acquire))
        return; // GUI is snapshotting the published buffer; skip this visual update rather than blocking audio.

    const int current = publishedSpectrum.load (std::memory_order_relaxed);
    const int writeIndex = 1 - current;
    auto& out = spectrumBuffers[(size_t) writeIndex];

    double weightedFrequency = 0.0;
    double magnitudeSum = 0.0;
    double rolloffSum = 0.0;
    double fluxSum = 0.0;
    const float nyquist = (float) sampleRateHz.load (std::memory_order_relaxed) * 0.5f;

    for (int bin = 0; bin < fftBins; ++bin)
    {
        const float magnitude = (2.0f / (float) fftSize) * fftData[(size_t) bin];
        out[(size_t) bin] = magnitude;

        const double freq = (double) bin * (double) sampleRateHz.load (std::memory_order_relaxed) / (double) fftSize;
        weightedFrequency += freq * magnitude;
        magnitudeSum += magnitude;

        const float diff = magnitude - previousSpectrum[(size_t) bin];
        if (diff > 0.0f)
            fluxSum += diff;
    }

    const double threshold = magnitudeSum * 0.85;
    for (int bin = 0; bin < fftBins; ++bin)
    {
        const float mag = out[(size_t) bin];
        rolloffSum += mag;
        if (rolloffSum >= threshold)
        {
            spectralRolloffHz.store ((float) bin * (float) sampleRateHz.load (std::memory_order_relaxed) / (float) fftSize,
                                     std::memory_order_release);
            break;
        }
    }

    const float centroid = magnitudeSum > epsilon ? (float) (weightedFrequency / magnitudeSum) : 0.0f;
    spectralCentroidHz.store (centroid, std::memory_order_release);
    spectralFlux.store ((float) fluxSum, std::memory_order_release);

    previousSpectrum = out;
    publishedSpectrum.store (writeIndex, std::memory_order_release);
    spectrumGuiReadLock.clear (std::memory_order_release);
}

void HexoscopeProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    if (clearRequested.exchange (false, std::memory_order_acq_rel))
    {
        fifo.reset();
        resetAccumulator();
        resetBlockMetrics();
        for (int c = 0; c < 4; ++c)
        {
            lp1[c] = 0.0f;
            lp2[c] = 0.0f;
        }
        std::fill (fftRing.begin(), fftRing.end(), 0.0f);
        std::fill (previousSpectrum.begin(), previousSpectrum.end(), 0.0f);
        for (auto& b : spectrumBuffers)
            std::fill (b.begin(), b.end(), 0.0f);
        fftSamplesSinceUpdate = 0;
        fftRingWritePos = 0;
        publishedSpectrum.store (0, std::memory_order_release);
        spectralCentroidHz.store (0.0f, std::memory_order_release);
        spectralRolloffHz.store (0.0f, std::memory_order_release);
        spectralFlux.store (0.0f, std::memory_order_release);
        previousTriggerValue = 0.0f;
        lastTriggerSample.store (-1, std::memory_order_release);

        for (auto& b : goniometerBuffers)
            std::fill (b.begin(), b.end(), GoniometerPoint {});
        publishedGoniometer.store (0, std::memory_order_release);
        goniometerWriteBuffer = 1;
        goniometerWritePos = 0;
        goniometerDecimationCounter = 0;

        clearGeneration.fetch_add (1, std::memory_order_acq_rel);
    }

    const int numCh = buffer.getNumChannels();
    const int n = buffer.getNumSamples();
    if (numCh == 0 || n == 0)
        return;

    const float* left = buffer.getReadPointer (0);
    const float* right = numCh > 1 ? buffer.getReadPointer (1) : left;
    const int analysisChannel = getAnalysisChannel();
    const bool triggerEnabled = apvts.getRawParameterValue ("trigger")->load() > 0.5f;
    const float triggerThresholdDb = apvts.getRawParameterValue ("triggerThreshold")->load();
    const float triggerThreshold = std::pow (10.0f, triggerThresholdDb / 20.0f);

    for (int i = 0; i < n; ++i)
    {
        const float L = left[i];
        const float R = right[i];
        const float M = 0.5f * (L + R);
        const float S = 0.5f * (L - R);
        const float v[4] { L, R, M, S };

        for (int c = 0; c < 4; ++c)
        {
            const float value = v[c];
            mn[c] = juce::jmin (mn[c], value);
            mx[c] = juce::jmax (mx[c], value);
            sumSquares[c] += value * value;

            blockSum[c] += value;
            blockSumSquares[c] += value * value;
            blockPositivePeak[c] = juce::jmax (blockPositivePeak[c], value);
            blockNegativePeak[c] = juce::jmin (blockNegativePeak[c], value);

            lp1[c] += a1 * (value - lp1[c]);
            lp2[c] += a2 * (value - lp2[c]);
            const float low = lp1[c];
            const float mid = lp2[c] - lp1[c];
            const float high = value - lp2[c];
            bandEnergy[c][0] += low * low;
            bandEnergy[c][1] += mid * mid;
            bandEnergy[c][2] += high * high;
        }

        blockCrossProduct += L * R;

        const float fftValue = v[analysisChannel];
        fftRing[(size_t) fftRingWritePos] = fftValue;
        fftRingWritePos = (fftRingWritePos + 1) % fftSize;
        ++fftSamplesSinceUpdate;

        const float previous = previousTriggerValue;
        previousTriggerValue = fftValue;
        if (triggerEnabled && previous < triggerThreshold && fftValue >= triggerThreshold)
            lastTriggerSample.store (processedSamples + i, std::memory_order_release);

        // Decimated M/S points for the goniometer.
        if (++goniometerDecimationCounter >= 2)
        {
            goniometerDecimationCounter = 0;
            auto& pointBuffer = goniometerBuffers[(size_t) goniometerWriteBuffer];
            pointBuffer[(size_t) goniometerWritePos] = { M, S };
            ++goniometerWritePos;

            if (goniometerWritePos >= goniometerBlockSize)
            {
                if (! goniometerGuiReadLock.test_and_set (std::memory_order_acquire))
                {
                    const int completed = goniometerWriteBuffer;
                    goniometerWriteBuffer = 1 - goniometerWriteBuffer;
                    publishedGoniometer.store (completed, std::memory_order_release);
                    goniometerGuiReadLock.clear (std::memory_order_release);
                }

                // If the GUI was busy, simply start another block in the same
                // private write buffer. Dropping a visual block is preferable
                // to ever waiting on the audio thread.
                goniometerWritePos = 0;
            }
        }

        if (++count >= samplesPerColumn)
            flushColumn();

        publishSpectrumIfReady();
    }

    processedSamples += n;
    publishBlockMetrics (n);
    resetBlockMetrics();
}

int HexoscopeProcessor::pullColumns (Column* dest, int maxNum)
{
    if (dest == nullptr || maxNum <= 0)
        return 0;

    int s1, n1, s2, n2;
    fifo.prepareToRead (maxNum, s1, n1, s2, n2);
    for (int i = 0; i < n1; ++i) dest[i] = fifoData[(size_t) (s1 + i)];
    for (int i = 0; i < n2; ++i) dest[n1 + i] = fifoData[(size_t) (s2 + i)];
    fifo.finishedRead (n1 + n2);
    return n1 + n2;
}

void HexoscopeProcessor::requestClear()
{
    clearRequested.store (true, std::memory_order_release);
}

int HexoscopeProcessor::copySpectrum (float* dest, int maxNum) const
{
    if (dest == nullptr || maxNum <= 0)
        return 0;

    if (spectrumGuiReadLock.test_and_set (std::memory_order_acquire))
        return 0;

    const int index = publishedSpectrum.load (std::memory_order_acquire);
    const int num = juce::jmin (maxNum, fftBins);
    std::copy_n (spectrumBuffers[(size_t) index].begin(), num, dest);
    spectrumGuiReadLock.clear (std::memory_order_release);
    return num;
}

std::pair<float, float> HexoscopeProcessor::getSpectrumRangeHz() const
{
    return { 20.0f, (float) sampleRateHz.load (std::memory_order_relaxed) * 0.5f };
}

float HexoscopeProcessor::getPeakDb (int channel) const
{
    return juce::jlimit (-100.0f, 6.0f, latestPeakDb[(size_t) juce::jlimit (0, 3, channel)].load (std::memory_order_acquire));
}

float HexoscopeProcessor::getRmsDb (int channel) const
{
    return juce::jlimit (-100.0f, 6.0f, latestRmsDb[(size_t) juce::jlimit (0, 3, channel)].load (std::memory_order_acquire));
}

float HexoscopeProcessor::getDcOffset (int channel) const
{
    return latestDcOffset[(size_t) juce::jlimit (0, 3, channel)].load (std::memory_order_acquire);
}

float HexoscopeProcessor::getAsymmetry (int channel) const
{
    return latestAsymmetry[(size_t) juce::jlimit (0, 3, channel)].load (std::memory_order_acquire);
}

int HexoscopeProcessor::copyGoniometer (GoniometerPoint* dest, int maxNum) const
{
    if (dest == nullptr || maxNum <= 0)
        return 0;

    if (goniometerGuiReadLock.test_and_set (std::memory_order_acquire))
        return 0;

    const int published = publishedGoniometer.load (std::memory_order_acquire);
    const auto& source = goniometerBuffers[(size_t) published];
    const int num = juce::jmin (maxNum, goniometerBlockSize);
    std::copy_n (source.begin(), num, dest);
    goniometerGuiReadLock.clear (std::memory_order_release);
    return num;
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
