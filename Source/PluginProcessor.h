#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

namespace DisplayScale
{
    constexpr float minDb = -140.0f; // Untere Anzeigegrenze
    constexpr float maxDb = -20.0f; // Obere Anzeigegrenze
}

//==============================================================================
// Hauptklasse des Plugins
// Enthält EQ-Filter (31-Band), FFT-basiertes Spektrum und Parameterverwaltung
class AudioPluginAudioProcessor final : public juce::AudioProcessor
{
public:
    //==============================================================================
    // Konstruktor / Destruktor
    AudioPluginAudioProcessor();
    ~AudioPluginAudioProcessor() override;

    //==============================================================================
    // Vorbereitung und Cleanup für Playback
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    //==============================================================================
    // Prüft, ob ein bestimmtes Bus-Layout unterstützt wird
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;

    //==============================================================================
    // Audio-Verarbeitung
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    //==============================================================================
    // Editor
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    // Basisinformationen über das Plugin
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    // Programminformationen (Preset-Handling)
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    //==============================================================================
    // State Management (Speichern / Laden von Parametern)
    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    //==============================================================================
    // Zugriff auf Spektrum-Daten (Post-EQ für Anzeige)
    void getNextScopeData(float* destBuffer, int numPoints);
    void pushNextSampleIntoFifo(float sample) noexcept;       // Sample in FIFO speichern (Post-EQ)
    void updateSpectrumArray(double sampleRate);              // FFT durchführen und Spektrum berechnen
    bool getNextFFTBlockReady() const { return nextFFTBlockReady; }
    const float* getScopeData() const { return scopeData; }
    int getScopeSize() const { return scopeSize; }
    void setNextFFTBlockReady(bool ready) { nextFFTBlockReady = ready; }

    // Pre-EQ Spektrum für Messung
    void pushNextSampleIntoPreEQFifo(float sample) noexcept;  // Sample in Pre-EQ FIFO speichern
    void updatePreEQSpectrumArray(double sampleRate);         // Pre-EQ FFT berechnen
    bool getNextPreEQFFTBlockReady() const { return nextPreEQFFTBlockReady; }
    void setNextPreEQFFTBlockReady(bool ready) { nextPreEQFFTBlockReady = ready; }

    // Spektrum-Punktstruktur
    struct SpectrumPoint
    {
        float frequency; // Frequenz in Hz
        float level;     // Level (dB)
    };

    std::vector<SpectrumPoint> spectrumArray;      // Post-EQ Spektrum für Anzeige
    std::vector<SpectrumPoint> preEQSpectrumArray; // Pre-EQ Spektrum für Messung

    //==============================================================================
    // Referenzkurven-Struktur
    struct ReferenceBand
    {
        float freq;   // Frequenz in Hz
        float p10;    // Unteres 10%-Perzentil
        float median; // Median (Zentralwert)
        float p90;    // Oberes 90%-Perzentil
    };

    //==============================================================================
    // Persistente Daten für Referenz- und Differenzkurve
    std::vector<ReferenceBand> referenceBands;           // Referenzkurve
    std::array<float, 31> targetCorrections;             // Berechnete Korrekturen
    bool hasTargetCorrections = false;                   // Flag ob Differenzkurve vorhanden
    int selectedGenreId = 0;                             // Ausgewähltes Genre im Dropdown

    // Referenzkurve laden
    void loadReferenceCurve(const juce::String& filename);

    //==============================================================================
    // Parameterverwaltung
    juce::AudioProcessorValueTreeState apvts;
    void updateFilters(); // Aktualisiert alle Bandfilter mit aktuellen Parametern

    void resetAllBandsToDefault();

    //==============================================================================
    // Messung / Spektrum-Aufnahme
    void startMeasurement();
    void stopMeasurement();
    void addMeasurementSnapshot();
    bool isMeasuring() const { return measuring; }

    // Gespeicherte Spektrum-Daten abrufen
    const std::vector<std::vector<SpectrumPoint>>& getMeasurementBuffer() const { return measurementBuffer; }
    std::vector<SpectrumPoint> getAveragedSpectrum() const;
    void clearMeasurement();

private:
    //==============================================================================
    // Parameter-Layout erstellen (31-Band EQ)
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    static constexpr int numBands = 31;
    using Filter = juce::dsp::IIR::Filter<float>;

    std::array<Filter, numBands> leftFilters;   // Linker Kanal
    std::array<Filter, numBands> rightFilters;  // Rechter Kanal

    //==============================================================================
    // Messungs-Speicher
    std::vector<std::vector<SpectrumPoint>> measurementBuffer;  // Alle Snapshots während Messung
    bool measuring = false;                                      // Flag ob Messung aktiv ist

    // Festgelegte Filterfrequenzen für 31 Bänder
    const std::array<float, numBands> filterFrequencies = {
        20.0f, 25.0f, 31.5f, 40.0f, 50.0f, 63.0f, 80.0f, 100.0f,
        125.0f, 160.0f, 200.0f, 250.0f, 315.0f, 400.0f, 500.0f,
        630.0f, 800.0f, 1000.0f, 1250.0f, 1600.0f, 2000.0f, 2500.0f,
        3150.0f, 4000.0f, 5000.0f, 6300.0f, 8000.0f, 10000.0f,
        12500.0f, 16000.0f, 20000.0f
    };

    //==============================================================================
    // FFT / Spectrum Analyzer (Post-EQ für Anzeige)
    enum {
        fftOrder = 12,
        fftSize = 1 << fftOrder,
        scopeSize = 512
    };

    juce::dsp::FFT forwardFFT;                        // FFT-Berechnung
    juce::dsp::WindowingFunction<float> window;       // Fensterfunktion (Hann)
    float fifo[fftSize];                              // FIFO für FFT-Eingang (Post-EQ)
    float fftData[2 * fftSize];                       // FFT-Datenpuffer
    int fifoIndex = 0;                                // Index für FIFO
    bool nextFFTBlockReady = false;                   // Flag für neue FFT-Daten
    float scopeData[scopeSize];                       // Normiertes Spektrum für Anzeige

    //==============================================================================
    // FFT / Spectrum Analyzer (Pre-EQ für Messung)
    juce::dsp::FFT preEQForwardFFT;                   // FFT-Berechnung für Pre-EQ
    juce::dsp::WindowingFunction<float> preEQWindow;  // Fensterfunktion für Pre-EQ
    float preEQFifo[fftSize];                         // FIFO für FFT-Eingang (Pre-EQ)
    float preEQFftData[2 * fftSize];                  // FFT-Datenpuffer für Pre-EQ
    int preEQFifoIndex = 0;                           // Index für Pre-EQ FIFO
    bool nextPreEQFFTBlockReady = false;              // Flag für neue Pre-EQ FFT-Daten

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessor)
};