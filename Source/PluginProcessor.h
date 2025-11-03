#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

//==============================================================================
class AudioPluginAudioProcessor final : public juce::AudioProcessor
{
public:
    //==============================================================================
    AudioPluginAudioProcessor();
    ~AudioPluginAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;
    void getNextScopeData(float* destBuffer, int numPoints);

    // Spectrum Analyzer Specific Public Methods
    void pushNextSampleIntoFifo(float sample) noexcept;
    void updateSpectrumArray(double sampleRate);
    bool getNextFFTBlockReady() const { return nextFFTBlockReady; }
    const float* getScopeData() const { return scopeData; }
    int getScopeSize() const { return scopeSize; }
    void setNextFFTBlockReady(bool ready) { nextFFTBlockReady = ready; }

    struct SpectrumPoint
    {
        float frequency; // Hz
        float level;     // dB oder normalisiert 0..1
    };

    std::vector<SpectrumPoint> spectrumArray; // Membervariable

private:
    //==============================================================================

    enum {
        fftOrder = 11,
        fftSize = 1 << fftOrder,
        scopeSize = 512
    };

    juce::dsp::FFT forwardFFT;
    juce::dsp::WindowingFunction<float> window;
    float fifo[fftSize];
    float fftData[2 * fftSize];
    int fifoIndex = 0;
    bool nextFFTBlockReady = false;
    float scopeData[scopeSize];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioPluginAudioProcessor)
};
