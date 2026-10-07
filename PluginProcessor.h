#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <utility>
#include <vector>

// Analysis column produced on the audio thread. The waveform extrema are kept
// together with per-channel RMS/band energy so the GUI can display meaningful
// colour information for whichever channel is being viewed.
struct Column
{
    struct ChannelStats
    {
        float min = 0.0f;
        float max = 0.0f;
        float rms = 0.0f;
        float band[3] {}; // low / mid / high RMS
    };

    ChannelStats channel[4]; // 0=L, 1=R, 2=M, 3=S
};

struct GoniometerPoint
{
    float mid = 0.0f;
    float side = 0.0f;
};

class HexoscopeProcessor : public juce::AudioProcessor
{
public:
    static constexpr int samplesPerColumn = 128;
    static constexpr int fftOrder = 10;
    static constexpr int fftSize = 1 << fftOrder;
    static constexpr int fftBins = fftSize / 2;
    static constexpr int spectrumBufferCount = 2;
    static constexpr int goniometerBlockSize = 1024;

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

    // Called from the UI thread.
    int pullColumns (Column* dest, int maxNum);
    void requestClear();
    juce::int64 getClearGeneration() const { return clearGeneration.load (std::memory_order_acquire); }

    double getSampleRateHz() const { return sampleRateHz.load (std::memory_order_relaxed); }

    // Published display/measurement data. The audio thread is the only writer.
    int copySpectrum (float* dest, int maxNum) const;
    std::pair<float, float> getSpectrumRangeHz() const;
    float getSpectralCentroidHz() const { return spectralCentroidHz.load (std::memory_order_relaxed); }
    float getSpectralRolloffHz() const { return spectralRolloffHz.load (std::memory_order_relaxed); }
    float getSpectralFlux() const { return spectralFlux.load (std::memory_order_relaxed); }
    juce::int64 getLastTriggerSample() const { return lastTriggerSample.load (std::memory_order_acquire); }

    float getPeakDb (int channel) const;
    float getRmsDb (int channel) const;
    float getDcOffset (int channel) const;
    float getAsymmetry (int channel) const;
    float getCorrelation() const { return correlation.load (std::memory_order_relaxed); }

    int copyGoniometer (GoniometerPoint* dest, int maxNum) const;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void resetAccumulator();
    void resetBlockMetrics();
    void flushColumn();
    void publishBlockMetrics (int numSamples);
    void publishSpectrumIfReady();
    int getAnalysisChannel() const;
    static float toDb (float value);

    static constexpr int fifoSize = 16384;
    juce::AbstractFifo fifo { fifoSize };
    std::vector<Column> fifoData;

    std::atomic<double> sampleRateHz { 44100.0 };

    // Audio-thread fine-column state.
    int count = 0;
    float mn[4] {}, mx[4] {};
    float sumSquares[4] {};
    float bandEnergy[4][3] {};
    float lp1[4] {}, lp2[4] {};
    float a1 = 0.0f, a2 = 0.0f;

    // Per-block measurements.
    float blockSum[4] {};
    float blockSumSquares[4] {};
    float blockPositivePeak[4] {};
    float blockNegativePeak[4] {};
    float blockCrossProduct = 0.0f;

    std::array<std::atomic<float>, 4> latestPeakDb;
    std::array<std::atomic<float>, 4> latestRmsDb;
    std::array<std::atomic<float>, 4> latestDcOffset;
    std::array<std::atomic<float>, 4> latestAsymmetry;
    std::atomic<float> correlation { 1.0f };

    // FFT analyser. A short window is published into one of two buffers while
    // the GUI reads the other, avoiding shared mutable spectrum storage.
    juce::dsp::FFT fft { fftOrder };
    std::array<float, fftSize> fftWindow {};
    std::array<float, fftSize * 2> fftData {};
    int fftSamplesSinceUpdate = 0;
    std::array<float, fftSize> fftRing {};
    int fftRingWritePos = 0;
    std::array<float, fftBins> previousSpectrum {};
    std::array<std::array<float, fftBins>, spectrumBufferCount> spectrumBuffers {};
    std::atomic<int> publishedSpectrum { 0 };
    mutable std::atomic_flag spectrumGuiReadLock = ATOMIC_FLAG_INIT;
    std::atomic<float> spectralCentroidHz { 0.0f };
    std::atomic<float> spectralRolloffHz { 0.0f };
    std::atomic<float> spectralFlux { 0.0f };

    // Trigger / transport-like sample position.
    juce::int64 processedSamples = 0;
    std::atomic<juce::int64> lastTriggerSample { -1 };
    float previousTriggerValue = 0.0f;

    // GUI-driven clear request; the audio thread performs the FIFO reset.
    std::atomic<bool> clearRequested { false };
    std::atomic<juce::int64> clearGeneration { 0 };

    // Goniometer data is published in complete blocks so the GUI never reads
    // the buffer the audio thread is currently writing.
    std::array<std::array<GoniometerPoint, goniometerBlockSize>, 2> goniometerBuffers {};
    std::atomic<int> publishedGoniometer { 0 };
    mutable std::atomic_flag goniometerGuiReadLock = ATOMIC_FLAG_INIT;
    int goniometerWriteBuffer = 1;
    int goniometerWritePos = 0;
    int goniometerDecimationCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HexoscopeProcessor)
};

