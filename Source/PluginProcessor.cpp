#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================Hallo
AudioPluginAudioProcessor::AudioPluginAudioProcessor()
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ), forwardFFT(fftOrder), window(fftSize, 
                           juce::dsp::WindowingFunction<float>::hann)
{

}

AudioPluginAudioProcessor::~AudioPluginAudioProcessor()
{
    juce::zeromem(fifo, sizeof(fifo));
    juce::zeromem(fftData, sizeof(fftData));
    juce::zeromem(scopeData, sizeof(scopeData));
}

//==============================================================================
const juce::String AudioPluginAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool AudioPluginAudioProcessor::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool AudioPluginAudioProcessor::producesMidi() const
{
   #if JucePlugin_ProducesMidiOutput
    return true;
   #else
    return false;
   #endif
}

bool AudioPluginAudioProcessor::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double AudioPluginAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int AudioPluginAudioProcessor::getNumPrograms()
{
    return 1;   // NB: some hosts don't cope very well if you tell them there are 0 programs,
                // so this should be at least 1, even if you're not really implementing programs.
}

int AudioPluginAudioProcessor::getCurrentProgram()
{
    return 0;
}

void AudioPluginAudioProcessor::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String AudioPluginAudioProcessor::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void AudioPluginAudioProcessor::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
void AudioPluginAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Use this method as the place to do any pre-playback
    // initialisation that you need..
    juce::ignoreUnused (sampleRate, samplesPerBlock);
}

void AudioPluginAudioProcessor::releaseResources()
{
    // When playback stops, you can use this as an opportunity to free up any
    // spare memory, etc.
}

bool AudioPluginAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    // This is the place where you check if the layout is supported.
    // In this template code we only support mono or stereo.
    // Some plugin hosts, such as certain GarageBand versions, will only
    // load plugins that support stereo bus layouts.
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    // This checks if the input layout matches the output layout
   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}

void AudioPluginAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // Clear any unused channels
    juce::ScopedNoDenormals noDenormals;
    for (auto i = 1; i < buffer.getNumChannels(); ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // Process audio samples for spectrum analysis (use first channel only)
    if (getTotalNumInputChannels() > 0)
    {
        auto* channelData = buffer.getReadPointer(0); // Get pointer to first channel
        auto numSamples = buffer.getNumSamples();

        // Push each sample into our FIFO buffer for FFT processing
        for (auto i = 0; i < numSamples; ++i)
            pushNextSampleIntoFifo(channelData[i]);
    }
}

// Collects audio samples into FIFO buffer
void AudioPluginAudioProcessor::pushNextSampleIntoFifo(float sample) noexcept
{
    // When FIFO is full, prepare data for FFT processing
    if (fifoIndex == fftSize)
    {
        if (!nextFFTBlockReady) // Only process if previous FFT is done
        {
            juce::zeromem(fftData, sizeof(fftData));        // Clear FFT buffer
            memcpy(fftData, fifo, sizeof(fifo));           // Copy audio data to FFT buffer
            nextFFTBlockReady = true;                        // Signal that FFT data is ready
        }
        fifoIndex = 0; // Reset FIFO index
    }

    // Store the current sample and move to next position
    fifo[fifoIndex++] = sample;
}



void AudioPluginAudioProcessor::updateSpectrumArray(double sampleRate)
{
    // Window anwenden
    window.multiplyWithWindowingTable(fftData, fftSize);
    forwardFFT.performFrequencyOnlyForwardTransform(fftData);

    auto mindB = -100.0f;
    auto maxdB = 0.0f;

    // Leeres Array füllen
    spectrumArray.clear();
    spectrumArray.reserve(scopeSize);

    for (int i = 0; i < scopeSize; ++i)
    {
        // Direkt linear von FFT-Index
        int fftDataIndex = juce::jlimit(0, fftSize / 2, i * (fftSize / 2) / scopeSize);

        auto level = juce::jmap(
            juce::jlimit(
                mindB,
                maxdB,
                juce::Decibels::gainToDecibels(fftData[fftDataIndex]) - juce::Decibels::gainToDecibels((float)fftSize)
            ),
            mindB,
            maxdB,
            0.0f,
            1.0f
        );

        // Frequenz zuordnen
        float frequency = (float)fftDataIndex * (sampleRate / (float)fftSize);
        spectrumArray.push_back({ frequency, level });
    }
}



//==============================================================================
bool AudioPluginAudioProcessor::hasEditor() const
{
    return true; // (change this to false if you choose to not supply an editor)
}

juce::AudioProcessorEditor* AudioPluginAudioProcessor::createEditor()
{
    return new AudioPluginAudioProcessorEditor (*this);
}

//==============================================================================
void AudioPluginAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // You should use this method to store your parameters in the memory block.
    // You could do that either as raw data, or use the XML or ValueTree classes
    // as intermediaries to make it easy to save and load complex data.
    juce::ignoreUnused (destData);
}

void AudioPluginAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // You should use this method to restore your parameters from this memory block,
    // whose contents will have been created by the getStateInformation() call.
    juce::ignoreUnused (data, sizeInBytes);
}

//==============================================================================
// This creates new instances of the plugin..
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AudioPluginAudioProcessor();
}
