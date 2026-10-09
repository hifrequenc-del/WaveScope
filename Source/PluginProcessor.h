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

    // Loudness / peak readings (written by the audio thread, read by the UI)
    float getMomentaryLufs()  const { return momentaryLufs.load(); }
    float getShortLufs()      const { return shortLufs.load(); }
    float getIntegratedLufs() const { return integratedLufs.load(); }
    float getTruePeakDb()     const { return truePeakDb.load(); }
    float getSamplePeakDb()   const { return samplePeakDb.load(); }
    void  requestLoudnessReset() { resetRequested = true; }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void resetAccumulator();
    void flushColumn();

    static constexpr int fifoSize = 16384;
    juce::AbstractFifo fifo { fifoSize };
    std::vector<Column> fifoData;

    std::atomic<double> sampleRateHz { 44100.0 };

    // ---- loudness (ITU-R BS.1770 / EBU R128) and peak metering ----
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void clear() noexcept { z1 = z2 = 0.0; }
    };

    static constexpr int numBins = 750;      // -70 .. +5 LUFS in 0.1 LU steps

    // peak meter that holds a new maximum for 3 s, then follows the signal down again
    struct PeakHold
    {
        float value = 0.f;
        int left = 0;
        float update (float blockPeak, int numSamples, int holdLen) noexcept
        {
            left -= numSamples;
            if (blockPeak >= value) { value = blockPeak; left = holdLen; }
            else if (left <= 0)     { value = blockPeak; }
            return value;
        }
    };

    void setupLoudness (double sr);
    void resetLoudness();
    void analyseLoudness (float l, float r, bool stereo);
    void finishSubBlock();
    void trackPeaks (int ch, float x);

    Biquad shelf[2], highpass[2];
    double subSumSq = 0.0;
    int subCount = 0, subBlockLen = 4800, ringPos = 0, ringFilled = 0;
    double ring[30] {};
    double histEnergy[numBins] {};
    int histCount[numBins] {};
    float tpHist[2][12] {};
    float tpCoef[3][12] {};
    float truePeakLin = 0.f, samplePeakLin = 0.f;
    PeakHold trueHold, sampleHold;
    int peakHoldLen = 144000;

    std::atomic<bool> resetRequested { false };
    std::atomic<float> momentaryLufs { -100.f };
    std::atomic<float> shortLufs { -100.f };
    std::atomic<float> integratedLufs { -100.f };
    std::atomic<float> truePeakDb { -100.f };
    std::atomic<float> samplePeakDb { -100.f };

    // audio-thread state
    int   count = 0;
    float mn[4], mx[4];
    float energy[3] {};
    float lp1 = 0.f, lp2 = 0.f, a1 = 0.f, a2 = 0.f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HexoscopeProcessor)
};
