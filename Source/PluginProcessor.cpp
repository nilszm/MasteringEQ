#include "PluginProcessor.h"
#include "PluginEditor.h"

//==============================================================================
// Konstruktor
// Initialisiert AudioProcessor, Parameter-Layout, FFT und Fensterfunktion
AudioPluginAudioProcessor::AudioPluginAudioProcessor()
    : AudioProcessor(BusesProperties()
#if ! JucePlugin_IsMidiEffect
#if ! JucePlugin_IsSynth
        .withInput("Input", juce::AudioChannelSet::stereo(), true)
#endif
        .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
    ),
    apvts(*this, nullptr, "Parameters", createParameterLayout()),
    forwardFFT(fftOrder), window(fftSize,
        juce::dsp::WindowingFunction<float>::hann)
{
}

//==============================================================================
// Destruktor
// Nullt alle Puffer beim Aufräumen, um Speicherreste zu vermeiden
AudioPluginAudioProcessor::~AudioPluginAudioProcessor()
{
    juce::zeromem(fifo, sizeof(fifo));
    juce::zeromem(fftData, sizeof(fftData));
    juce::zeromem(scopeData, sizeof(scopeData));
}

//==============================================================================
// Basisinformationen über das Plugin
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

// Gibt die Länge des Nachklangs zurück (hier 0, kein Hall)
double AudioPluginAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

//==============================================================================
// Programminformationen
int AudioPluginAudioProcessor::getNumPrograms()
{
    return 1; // Wir implementieren keine Programme, aber Host benötigt ≥1
}

int AudioPluginAudioProcessor::getCurrentProgram()
{
    return 0;
}

void AudioPluginAudioProcessor::setCurrentProgram(int index)
{
    juce::ignoreUnused(index);
}

const juce::String AudioPluginAudioProcessor::getProgramName(int index)
{
    juce::ignoreUnused(index);
    return {};
}

void AudioPluginAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName);
}

//==============================================================================
// Parameter-Layout erstellen
// Erstellt ein ParameterArray mit 31 Bändern für EQ (Gain von -12dB bis +12dB)
juce::AudioProcessorValueTreeState::ParameterLayout
AudioPluginAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (int i = 0; i < numBands; ++i)
    {
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "band" + juce::String(i),
            "Band " + juce::String(i),
            juce::NormalisableRange<float>(-12.0f, 12.0f, 0.1f),
            0.0f
        ));
    }

    return layout;
}

//==============================================================================
// Vorbereitung für Playback
// Setzt DSP-Spezifikationen und initialisiert alle Filter
void AudioPluginAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = samplesPerBlock;
    spec.numChannels = 1; // Mono pro Filter, getrennt für L/R

    // Alle 31 Filter vorbereiten
    for (int i = 0; i < numBands; ++i)
    {
        leftFilters[i].prepare(spec);
        rightFilters[i].prepare(spec);
    }

    updateFilters();
}

//==============================================================================
// Filter aktualisieren
// Berechnet die aktuellen Biquad-Koeffizienten für jeden Band-EQ
void AudioPluginAudioProcessor::updateFilters()
{
    auto sampleRate = getSampleRate();
    if (sampleRate <= 0)
        return;

    for (int i = 0; i < numBands; ++i)
    {
        auto* param = apvts.getRawParameterValue("band" + juce::String(i));
        if (param == nullptr)
            continue;

        float gainDB = param->load(); // Gain aus Parameter (dB)
        float gainLinear = juce::Decibels::decibelsToGain(gainDB); // Linearer Gain
        float Q = 4.32f; // Feste Bandbreite für alle Bänder

        // Biquad-Koeffizienten für Peak-Filter berechnen
        auto coeffs = juce::dsp::IIR::Coefficients<float>::makePeakFilter(
            sampleRate,
            filterFrequencies[i],
            Q,
            gainLinear
        );

        // Filter aktualisieren
        *leftFilters[i].coefficients = *coeffs;
        *rightFilters[i].coefficients = *coeffs;
    }
}

//==============================================================================
// Ressourcen freigeben
// Wird beim Stoppen der Wiedergabe aufgerufen
void AudioPluginAudioProcessor::releaseResources()
{
}

//==============================================================================
// Bus-Layout prüfen
// Unterstützt nur Mono oder Stereo Input/Output
bool AudioPluginAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
#if JucePlugin_IsMidiEffect
    juce::ignoreUnused(layouts);
    return true;
#else
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

#if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
#endif

    return true;
#endif
}

//==============================================================================
// Audio-Block verarbeiten
void AudioPluginAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    updateFilters(); // Sicherstellen, dass Filter auf aktuelle Parameter reagieren
    juce::ScopedNoDenormals noDenormals;

    // Überschüssige Kanäle clearen
    for (auto i = 1; i < buffer.getNumChannels(); ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // Alle Kanäle filtern
    for (int channel{ 0 }; channel < totalNumInputChannels; ++channel)
    {
        auto* channelData = buffer.getWritePointer(channel);
        juce::dsp::AudioBlock<float> block(&channelData, 1, buffer.getNumSamples());

        for (int i = 0; i < numBands; ++i)
        {
            juce::dsp::ProcessContextReplacing<float> context(block);

            if (channel == 0)
                leftFilters[i].process(context);
            else if (channel == 1)
                rightFilters[i].process(context);
        }
    }

    // FFT vorbereiten (nur erster Kanal)
    if (getTotalNumInputChannels() > 0)
    {
        auto* channelData = buffer.getReadPointer(0);
        auto numSamples = buffer.getNumSamples();

        for (auto i = 0; i < numSamples; ++i)
            pushNextSampleIntoFifo(channelData[i]);
    }
}

//==============================================================================
// Samples in FIFO speichern
// FIFO wird gefüllt, bis FFT durchgeführt werden kann
void AudioPluginAudioProcessor::pushNextSampleIntoFifo(float sample) noexcept
{
    if (fifoIndex == fftSize)
    {
        if (!nextFFTBlockReady)
        {
            juce::zeromem(fftData, sizeof(fftData));
            memcpy(fftData, fifo, sizeof(fifo));
            nextFFTBlockReady = true; // Signalisiert, dass FFT-Daten bereit sind
        }
        fifoIndex = 0;
    }

    fifo[fifoIndex++] = sample;
}

//==============================================================================
// Spectrum Array aktualisieren
// Berechnet FFT, wendet Windowing an und erstellt normalisiertes Spektrum
void AudioPluginAudioProcessor::updateSpectrumArray(double sampleRate)
{
    // Fensterung
    window.multiplyWithWindowingTable(fftData, fftSize);
    forwardFFT.performFrequencyOnlyForwardTransform(fftData);

    // Anzeigebereich für FFT und JSON Referenz
    const float displayMinDb = -20.0f;
    const float displayMaxDb = 80.0f;

    spectrumArray.clear();
    spectrumArray.reserve(scopeSize);

    // Amplituden normieren
    const float fftNorm = (float)fftSize;

    for (int i = 0; i < scopeSize; ++i)
    {
        int fftDataIndex = juce::jlimit(0, fftSize / 2,
                                        i * (fftSize / 2) / scopeSize);

        // Werte aus der FFT
        float mag = fftData[fftDataIndex];

        // Normieren
        mag /= fftNorm;

        if (mag <= 0.0f)
            mag = 1.0e-9f; // Schutz vor log(0)

        // 1) dbFS nutzen
        float dbFs = juce::Decibels::gainToDecibels(mag);

        // In dB umrechnen
        float displayDb = dbFs + DisplayScale::offsetDb;

        // Auf Ausgangsbereich begrenzen
        displayDb = juce::jlimit(DisplayScale::minDb,
            DisplayScale::maxDb,
            displayDb);

        // Frequenz dieses Bins
        float frequency = (float)fftDataIndex * (sampleRate / (float)fftSize);

        // Speichern
        spectrumArray.push_back({ frequency, displayDb });
    }
}

//==============================================================================
// Editor
bool AudioPluginAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* AudioPluginAudioProcessor::createEditor()
{
    return new AudioPluginAudioProcessorEditor(*this);
}

//==============================================================================
// State Management
void AudioPluginAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::ignoreUnused(destData);
}

void AudioPluginAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    juce::ignoreUnused(data, sizeInBytes);
}

//==============================================================================
// Plugin Factory
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AudioPluginAudioProcessor();
}