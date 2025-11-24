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
// Berechnet die aktuellen Biquad-Parameter für jeden Band-EQ
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

void AudioPluginAudioProcessor::resetAllBandsToDefault()
{
    for (int i = 0; i < numBands; ++i)
    {
        juce::String paramID = "band" + juce::String(i);
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*>(apvts.getParameter(paramID)))
            p->setValueNotifyingHost(p->getDefaultValue());
    }

    updateFilters();
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

    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // Nur überschüssige Output-Kanäle clearen (z.B. bei Surround)
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    // Alle Input-Kanäle filtern
    for (int channel = 0; channel < totalNumInputChannels; ++channel)
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

    // Terzband-Mittenfrequenzen (nach IEC 61260)
    // Von 25 Hz bis 20 kHz
    std::vector<float> thirdOctaveCenterFreqs = {
        25.0f, 31.5f, 40.0f, 50.0f, 63.0f, 80.0f, 100.0f, 125.0f, 160.0f, 200.0f,
        250.0f, 315.0f, 400.0f, 500.0f, 630.0f, 800.0f, 1000.0f, 1250.0f, 1600.0f, 2000.0f,
        2500.0f, 3150.0f, 4000.0f, 5000.0f, 6300.0f, 8000.0f, 10000.0f, 12500.0f, 16000.0f, 20000.0f
    };

    // Amplituden normieren
    const float fftNorm = (float)fftSize;
    const float binWidth = sampleRate / (float)fftSize;

    for (float centerFreq : thirdOctaveCenterFreqs)
    {
        // Bandgrenzen berechnen (Faktor 2^(1/6) ≈ 1.122 für Terz)
        const float bandwidthFactor = std::pow(2.0f, 1.0f / 6.0f);
        float lowerFreq = centerFreq / bandwidthFactor;
        float upperFreq = centerFreq * bandwidthFactor;

        // Nur Bänder innerhalb der Nyquist-Frequenz
        if (lowerFreq >= sampleRate / 2.0f)
            break;

        upperFreq = std::min(upperFreq, (float)(sampleRate / 2.0f));

        // FFT-Bins für dieses Band finden
        int lowerBin = (int)std::floor(lowerFreq / binWidth);
        int upperBin = (int)std::ceil(upperFreq / binWidth);

        lowerBin = juce::jlimit(0, fftSize / 2, lowerBin);
        upperBin = juce::jlimit(0, fftSize / 2, upperBin);

        // Energie über alle Bins im Band summieren
        float bandEnergy = 0.0f;
        int binCount = 0;

        for (int bin = lowerBin; bin <= upperBin; ++bin)
        {
            float mag = fftData[bin] / fftNorm;
            bandEnergy += mag * mag; // Energie = Amplitude²
            binCount++;
        }

        // Mittlere Energie und zurück zu Amplitude
        if (binCount > 0)
            bandEnergy /= binCount;

        float bandMagnitude = std::sqrt(bandEnergy);

        // Schutz vor log(0)
        if (bandMagnitude <= 0.0f)
            bandMagnitude = 1.0e-9f;

        // In dB umrechnen
        float dbFs = juce::Decibels::gainToDecibels(bandMagnitude);
        float displayDb = dbFs + DisplayScale::offsetDb;

        // Auf Ausgangsbereich begrenzen
        displayDb = juce::jlimit(DisplayScale::minDb, DisplayScale::maxDb, displayDb);

        // Speichern
        spectrumArray.push_back({ centerFreq, displayDb });
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