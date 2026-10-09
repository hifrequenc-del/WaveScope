#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <vector>

// One "fine" column of analysis data, produced every samplesPerColumn samples.
// Channel order: 0 = Left, 1 = Right, 2 = Mid, 3 = Side
struct Column
{
    float mn[4] {};
    float mx[4] {};
    float band[3] {};   // mean-square energy of Mid signal: low, mid, high
};

class HexoscopeProcessor : public juce::AudioProcessor
{
public:
    static constexpr int samplesPerColumn = 128;

    HexoscopeProcessor();
    ~HexoscopeProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "HEXOSCOPE"; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // Called from the UI thread
    int pullColumns (Column* dest, int maxNum);
    double getSampleRateHz() const { return sampleRateHz.load(); }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void resetAccumulator();
    void flushColumn();

    static constexpr int fifoSize = 16384;
    juce::AbstractFifo fifo { fifoSize };
    std::vector<Column> fifoData;

    std::atomic<double> sampleRateHz { 44100.0 };

    // audio-thread state
    int   count = 0;
    float mn[4], mx[4];
    float energy[3] {};
    float lp1 = 0.f, lp2 = 0.f, a1 = 0.f, a2 = 0.f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HexoscopeProcessor)
};
