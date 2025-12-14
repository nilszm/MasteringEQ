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
    forwardFFT(fftOrder),
    window(fftSize, juce::dsp::WindowingFunction<float>::hann),
    preEQForwardFFT(fftOrder),
    preEQWindow(fftSize, juce::dsp::WindowingFunction<float>::hann)
{
    // Pre-EQ Puffer initialisieren
    juce::zeromem(preEQFifo, sizeof(preEQFifo));
    juce::zeromem(preEQFftData, sizeof(preEQFftData));

    // Target Corrections initialisieren
    targetCorrections.fill(0.0f);
}

//==============================================================================
// Destruktor
// Nullt alle Puffer beim Aufräumen, um Speicherreste zu vermeiden
AudioPluginAudioProcessor::~AudioPluginAudioProcessor()
{
    juce::zeromem(fifo, sizeof(fifo));
    juce::zeromem(fftData, sizeof(fftData));
    juce::zeromem(scopeData, sizeof(scopeData));
    juce::zeromem(preEQFifo, sizeof(preEQFifo));
    juce::zeromem(preEQFftData, sizeof(preEQFftData));
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

    // Input Gain Parameter hinzufügen
    layout.add(std::make_unique<juce::AudioParameterFloat>(
        "inputGain",
        "Input Gain",
        juce::NormalisableRange<float>(-24.0f, 24.0f, 0.1f),
        0.0f  // Default-Wert
    ));

    // 31 Q - Parameter(Filtergüte)
    for (int i = 0; i < numBands; ++i)
    {
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            "bandQ" + juce::String(i),
            "Band Q " + juce::String(i),
            juce::NormalisableRange<float>(0.3f, 10.0f, 0.01f),
            4.32f // Default
        ));
    }

    // Die 31 EQ Bänder
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
        auto* gainParam = apvts.getRawParameterValue("band" + juce::String(i));
        auto* qParam = apvts.getRawParameterValue("bandQ" + juce::String(i));
        if (gainParam == nullptr || qParam == nullptr)
            continue;

        float gainDB = gainParam->load(); // Gain aus Parameter (dB)
        float gainLinear = juce::Decibels::decibelsToGain(gainDB); // Linearer Gain
        float Q = qParam->load(); // Bandbreite für jedes Band

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
        // Gain zurücksetzen
        {
            juce::String paramID = "band" + juce::String(i);
            if (auto* p = dynamic_cast<juce::RangedAudioParameter*>(apvts.getParameter(paramID)))
                p->setValueNotifyingHost(p->getDefaultValue());
        }

        // NEU: Q zurücksetzen
        {
            juce::String qID = "bandQ" + juce::String(i);
            if (auto* pQ = dynamic_cast<juce::RangedAudioParameter*>(apvts.getParameter(qID)))
                pQ->setValueNotifyingHost(pQ->getDefaultValue());
        }
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
    // Input Gain anwenden
    float inputGainDb = apvts.getRawParameterValue("inputGain")->load();
    float inputGainLinear = juce::Decibels::decibelsToGain(inputGainDb);

    juce::ignoreUnused(midiMessages);
    updateFilters(); // Sicherstellen, dass Filter auf aktuelle Parameter reagieren
    juce::ScopedNoDenormals noDenormals;

    auto totalNumInputChannels = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();

    // Nur überschüssige Output-Kanäle clearen (z.B. bei Surround)
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear(i, 0, buffer.getNumSamples());

    //==========================================================================
    // PRE-EQ FFT: Samples VOR den Filtern erfassen (für Messung)
    //==========================================================================
    {
        auto numSamples = buffer.getNumSamples();
        auto numChannels = getTotalNumInputChannels();

        if (numChannels >= 2)
        {
            // Stereo: Links und Rechts mitteln für echte Mono-Summe
            auto* leftData = buffer.getReadPointer(0);
            auto* rightData = buffer.getReadPointer(1);

            for (auto i = 0; i < numSamples; ++i)
            {
                float monoSample = ((leftData[i] + rightData[i]) * 0.5f) * inputGainLinear;
                pushNextSampleIntoPreEQFifo(monoSample);

            }
        }
        else if (numChannels == 1)
        {
            // Mono-Input: direkt verwenden
            auto* channelData = buffer.getReadPointer(0);
            for (auto i = 0; i < numSamples; ++i)
                pushNextSampleIntoPreEQFifo(channelData[i] * inputGainLinear);

        }
    }

    for (int channel = 0; channel < totalNumInputChannels; ++channel)
    {
        auto* channelData = buffer.getWritePointer(channel);
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            channelData[sample] *= inputGainLinear;
        }
    }

    //==========================================================================
    // EQ-Filter anwenden
    //==========================================================================
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

//==========================================================================
// POST-EQ FFT: Samples NACH den Filtern erfassen (für Anzeige)
// Mono-Summe aus beiden Kanälen (L+R gemittelt)
//==========================================================================
    {
        auto numSamples = buffer.getNumSamples();
        auto numChannels = getTotalNumInputChannels();

        if (numChannels >= 2)
        {
            // Stereo: Links und Rechts mitteln für echte Mono-Summe
            auto* leftData = buffer.getReadPointer(0);
            auto* rightData = buffer.getReadPointer(1);

            for (auto i = 0; i < numSamples; ++i)
            {
                float monoSample = (leftData[i] + rightData[i]) * 0.5f;
                pushNextSampleIntoFifo(monoSample);
            }
        }
        else if (numChannels == 1)
        {
            // Mono-Input: direkt verwenden
            auto* channelData = buffer.getReadPointer(0);
            for (auto i = 0; i < numSamples; ++i)
                pushNextSampleIntoFifo(channelData[i]);
        }
    }
}

//==============================================================================
// Samples in Post-EQ FIFO speichern
// FIFO wird gefüllt, bis FFT durchgeführt werden kann
void AudioPluginAudioProcessor::pushNextSampleIntoFifo(float sample) noexcept
{
    if (fifoIndex == fftSize)
    {
        if (!nextFFTBlockReady.load())
        {
            juce::zeromem(fftData, sizeof(fftData));
            memcpy(fftData, fifo, sizeof(fifo));
            nextFFTBlockReady.store(true);
        }
        fifoIndex = 0;
    }

    fifo[fifoIndex++] = sample;
}

//==============================================================================
// Samples in Pre-EQ FIFO speichern (für Messung)
void AudioPluginAudioProcessor::pushNextSampleIntoPreEQFifo(float sample) noexcept
{
    if (preEQFifoIndex == fftSize)
    {
        if (!nextPreEQFFTBlockReady)
        {
            juce::zeromem(preEQFftData, sizeof(preEQFftData));
            memcpy(preEQFftData, preEQFifo, sizeof(preEQFifo));
            nextPreEQFFTBlockReady = true; // Signalisiert, dass Pre-EQ FFT-Daten bereit sind
        }
        preEQFifoIndex = 0;
    }

    preEQFifo[preEQFifoIndex++] = sample;
}

//==============================================================================
// Post-EQ Spectrum Array aktualisieren (für Anzeige)
// Berechnet FFT, wendet Windowing an und erstellt normalisiertes Spektrum
void AudioPluginAudioProcessor::updateSpectrumArray(double sampleRate)
{
    // Fensterung
    window.multiplyWithWindowingTable(fftData, fftSize);
    forwardFFT.performFrequencyOnlyForwardTransform(fftData);

    spectrumArray.clear();
    spectrumArray.reserve(scopeSize);

    // Terzband-Mittenfrequenzen (nach IEC 61260)
    // Von 25 Hz bis 20 kHz
    std::vector<float> thirdOctaveCenterFreqs = {
        20.0f, 25.0f, 31.5f, 40.0f, 50.0f, 63.0f, 80.0f, 100.0f, 125.0f, 160.0f, 200.0f,
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

        const int maxBin = (fftSize / 2) - 1;  // gültig: 0..(fftSize/2 - 1)
        lowerBin = juce::jlimit(1, maxBin, lowerBin);
        upperBin = juce::jlimit(1, maxBin, upperBin);

        if (upperBin < lowerBin)
            continue;

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

        // In dB umrechnen
        const float floorDb = -160.0f;
        float dbFs = juce::Decibels::gainToDecibels(bandMagnitude, floorDb);

        // Speichern
        spectrumArray.push_back({ centerFreq, dbFs });
    }
}

//==============================================================================
// Pre-EQ Spectrum Array aktualisieren (für Messung)
void AudioPluginAudioProcessor::updatePreEQSpectrumArray(double sampleRate)
{
    // Fensterung
    preEQWindow.multiplyWithWindowingTable(preEQFftData, fftSize);
    preEQForwardFFT.performFrequencyOnlyForwardTransform(preEQFftData);

    preEQSpectrumArray.clear();
    preEQSpectrumArray.reserve(scopeSize);

    // Terzband-Mittenfrequenzen (nach IEC 61260)
    std::vector<float> thirdOctaveCenterFreqs = {
        20.0f, 25.0f, 31.5f, 40.0f, 50.0f, 63.0f, 80.0f, 100.0f, 125.0f, 160.0f, 200.0f,
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

        const int maxBin = (fftSize / 2) - 1;  // gültig: 0..(fftSize/2 - 1)
        lowerBin = juce::jlimit(1, maxBin, lowerBin);
        upperBin = juce::jlimit(1, maxBin, upperBin);

        if (upperBin < lowerBin)
            continue;

        // Energie über alle Bins im Band summieren
        float bandEnergy = 0.0f;
        int binCount = 0;

        for (int bin = lowerBin; bin <= upperBin; ++bin)
        {
            float mag = preEQFftData[bin] / fftNorm;
            bandEnergy += mag * mag;
            binCount++;
        }

        // Mittlere Energie und zurück zu Amplitude
        if (binCount > 0)
            bandEnergy /= binCount;

        float bandMagnitude = std::sqrt(bandEnergy);

        // In dB umrechnen
        const float floorDb = -160.0f;
        float dbFs = juce::Decibels::gainToDecibels(bandMagnitude, floorDb);

        // Speichern
        preEQSpectrumArray.push_back({ centerFreq, dbFs });
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

//==============================================================================
// Messung starten
void AudioPluginAudioProcessor::startMeasurement()
{
    measurementBuffer.clear();
    measuring.store(true);
    DBG("Messung gestartet");
}

//==============================================================================
// Messung stoppen
void AudioPluginAudioProcessor::stopMeasurement()
{
    measuring.store(false);
    DBG("Messung gestoppt - " + juce::String(measurementBuffer.size()) + " Snapshots gesammelt");
}

//==============================================================================
// Snapshot hinzufügen (wird vom Timer aufgerufen)
// WICHTIG: Verwendet jetzt preEQSpectrumArray statt spectrumArray!
void AudioPluginAudioProcessor::addMeasurementSnapshot()
{
    if (measuring.load() && !preEQSpectrumArray.empty())
    {
        measurementBuffer.push_back(preEQSpectrumArray);
    }
}

//==============================================================================
// Messung löschen
void AudioPluginAudioProcessor::clearMeasurement()
{
    measurementBuffer.clear();
    measuring = false;
}

//==============================================================================
// Gemitteltes Spektrum berechnen
std::vector<AudioPluginAudioProcessor::SpectrumPoint>
AudioPluginAudioProcessor::getAveragedSpectrum() const
{
    std::vector<SpectrumPoint> averaged;

    if (measurementBuffer.empty())
        return averaged;

    const size_t numBands = measurementBuffer[0].size();
    const size_t numSnapshots = measurementBuffer.size();

    averaged.resize(numBands);

    constexpr float floorDb = -160.0f;
    const float floorPower = std::pow(10.0f, floorDb / 10.0f);

    for (size_t band = 0; band < numBands; ++band)
    {
        double powerSum = 0.0;
        int validCount = 0;

        for (const auto& snapshot : measurementBuffer)
        {
            if (band < snapshot.size())
            {
                const float db = snapshot[band].level;
                const double p = std::pow(10.0, (double)db / 10.0);

                powerSum += p;
                ++validCount;
            }
        }

        // Frequenz aus erstem Snapshot übernehmen (ist konstant)
        averaged[band].frequency = measurementBuffer[0][band].frequency;

        // Mittelwert Power-Domain berechnen
        if (validCount > 0)
        {
            const double meanPower = powerSum / (double)validCount;

            if (meanPower <= floorPower)
                averaged[band].level = floorDb;
            else
                averaged[band].level = (float)(10.0 * std::log10(meanPower));
        }
        else
        {
            averaged[band].level = floorDb;
        }
    }

    return averaged;
}

//==============================================================================
// Referenzkurve laden
void AudioPluginAudioProcessor::loadReferenceCurve(const juce::String& filename)
{
    // Speicher für Kurve leeren
    referenceBands.clear();

    if (filename.isEmpty())
        return;

    // File laden
    juce::File refFileBase = juce::File::getSpecialLocation(
        juce::File::currentApplicationFile);

    // Solange im Pfad nach oben gehen, bis "build"
    for (int i = 0; i < 8; ++i)
    {
        if (refFileBase.getFileName().equalsIgnoreCase("build"))
            break;

        refFileBase = refFileBase.getParentDirectory();
    }
    
    // Von der "build" Ebene aus laden
    juce::File refFile = refFileBase
        .getChildFile("ReferenceCurves")
        .getChildFile(filename);

    // Inhalt von JSON in String laden
    juce::String fileContent = refFile.loadFileAsString();

    if (fileContent.isEmpty())
    {
        DBG("Konnte Referenzkurve nicht laden: " + filename);
        return;
    }

    // Analysiert Text aus fileContent und erstellt dynamisches var
    juce::var jsonData = juce::JSON::parse(fileContent);

    // Bänder aus JSON erstellen
    auto bands = jsonData["bands"];

    if (bands.isArray())
    {
        for (auto& b : *bands.getArray())
        {
            ReferenceBand rb;
            rb.freq = (float)b["freq"];
            rb.p10 = (float)b["p10"];
            rb.median = (float)b["median"];
            rb.p90 = (float)b["p90"];

            referenceBands.push_back(rb);
        }
    }

    DBG("Referenzkurve geladen: " + filename + " (" + juce::String(referenceBands.size()) + " Bänder)");
}