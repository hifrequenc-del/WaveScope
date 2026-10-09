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

    const int numCh = buffer.getNumChannels();
    const int n     = buffer.getNumSamples();
    if (numCh == 0 || n == 0) return;

    // Audio passes through untouched; we only read it.
    const float* left  = buffer.getReadPointer (0);
    const float* right = numCh > 1 ? buffer.getReadPointer (1) : left;

    for (int i = 0; i < n; ++i)
    {
        const float L = left[i], R = right[i];
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
