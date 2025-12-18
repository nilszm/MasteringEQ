#pragma once

#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <atomic>

//==============================================================================
class AudioPluginAudioProcessorEditor : public juce::AudioProcessorEditor,
    private juce::Timer
{
public:
    explicit AudioPluginAudioProcessorEditor(AudioPluginAudioProcessor&);
    ~AudioPluginAudioProcessorEditor() override;

    //==============================================================================
    void paint(juce::Graphics&) override;
    void resized() override;

 
private:
    // Setup-Funktionen (aus Konstruktor ausgelagert)
    void initializeWindow();
    void setupGenreDropdown();
    void setupMeasurementButton();
    void setupLoadReferenceButton();
    void setupResetButton();
    void setupEQCurveToggle();
    void setupEQSliders();
    void setupQKnobs();
    void setupInputGainSlider();
    void updateMeasurementButtonEnabledState();

    // ============================================================================
// Diese Funktionsdeklarationen in PluginEditor.h einfügen (private Bereich):
// ============================================================================
    // Draw-Funktionen (aus paint() ausgelagert)
    void drawTopBar(juce::Graphics& g);
    void drawBackground(juce::Graphics& g);
    void drawSpectrumArea(juce::Graphics& g);
    void drawReferenceBands(juce::Graphics& g, float minFreq, float maxFreq,
        float displayMinDb, float displayMaxDb);
    void drawFrequencyGrid(juce::Graphics& g);
    void drawEQAreas(juce::Graphics& g);
    void drawEQLabels(juce::Graphics& g);
    void drawEQDbGridLines(juce::Graphics& g);
    void drawEQDbGridLabels(juce::Graphics& g);
    void drawEQFaderDbScale(juce::Graphics& g);
    void drawEQFaderDbGuideLines(juce::Graphics& g);

    // ============================================================================
// Diese Funktionsdeklarationen in PluginEditor.h einfügen (private Bereich):
// ============================================================================
    // ============================================================================
// Diese Funktionsdeklarationen in PluginEditor.h einfügen (private Bereich):
// ============================================================================

private:
    // resized Hilfsfunktionen
    void layoutTopBar(juce::Rectangle<int>& area);
    void layoutSpectrumAreas(juce::Rectangle<int>& area);
    void layoutEQAreas(juce::Rectangle<int>& area);
    void layoutEQSliders();
    void layoutQKnobs();
    void calculateSpectrumInnerArea();
private:
    // applyAutoEQ Hilfsfunktionen
    bool validateAutoEQData(
        const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum);

    void logAutoEQStart(
        const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum);

    std::vector<float> calculateResiduals(
        const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum);

    float calculateMeanOffset(const std::vector<float>& residuals);

    void applyCorrections(const std::vector<float>& residuals, float meanOffset);

    // drawTargetEQCurve Hilfsfunktionen
    juce::Path buildTargetPath();

    void drawDashedTargetCurve(juce::Graphics& g, const juce::Path& targetPath);

    void drawTargetPoints(juce::Graphics& g);

// ============================================================================
// Diese Funktionsdeklarationen in PluginEditor.h einfügen (private Bereich):
// ============================================================================

private:
    // Offset
    float referenceViewOffsetDb = 0.0f;
    float referenceViewOffsetDbSmoothed = 0.0f;

    float computeReferenceViewOffsetDb(
        const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum) const;

    // drawEQCurve Hilfsfunktionen
    std::vector<float> generateLogFrequencies(int numPoints, float minFreq, float maxFreq);

    std::vector<float> calculateTotalMagnitude(
        const std::vector<float>& frequencies, int numPoints);

    juce::Path buildEQPath(
        const std::vector<float>& frequencies,
        const std::vector<float>& magnitudeDB,
        int numPoints, float minFreq, float maxFreq);

    void drawEQPathWithFill(juce::Graphics& g, const juce::Path& eqPath);

    // drawFrame Hilfsfunktionen
    void initializeSmoothedLevels(
        const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum);

    std::vector<juce::Point<float>> calculateSpectrumPoints(
        const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum);

    void applySpatialSmoothingToPoints(std::vector<juce::Point<float>>& points);

    void extrapolateTo20Hz(
        std::vector<juce::Point<float>>& points,
        const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum);

    void drawSpectrumPath(
        juce::Graphics& g,
        const std::vector<juce::Point<float>>& points);

    // Timer callback for GUI updates
    void timerCallback() override;

    void startAutoEqAsync(); // Auto-EQ im Background starten

    // Auto-EQ Funktion
    void applyAutoEQ();
    void drawTargetEQCurve(juce::Graphics& g);
    std::vector<float> calculateResidualsAligned(
        const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum,
        float offsetDb);


    // Hilfsfunktionen
    float findReferenceLevel(float frequency) const;
    float findMeasuredLevel(float frequency, const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum) const;

    // Input Gain Slider und Label
    juce::Slider inputGainSlider;
    juce::Label inputGainLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> inputGainAttachment;

    // Für temporal smoothing
    std::vector<float> previousLevels;

    // Helper function to draw the spectrum
    void drawFrame(juce::Graphics& g);

    // Background grid drawing
    void drawBackgroundGrid(juce::Graphics& g);

    AudioPluginAudioProcessor& processorRef;

    // Topbar einfügen
    juce::Rectangle<int> topBarArea;

    // Dropdown für Genres
    juce::ComboBox genreBox;

    // Button für Genre erkennen
    juce::TextButton genreErkennenButton;

    // Button für Referenz Laden
    juce::TextButton loadReferenceButton;

    // Async FileChooser am Leben halten
    std::unique_ptr<juce::FileChooser> referenceFileChooser;

    // Background-Job Pool (1 Thread reicht)
    juce::ThreadPool referenceAnalysisPool{ 1 };

    // UI-Status
    bool referenceAnalysisRunning = false;

    juce::ThreadPool autoEqPool{ 1 };              // 1 Thread reicht
    std::atomic<bool> autoEqRunning{ false };      // verhindert Doppelstarts

    // Button für Reset
    juce::TextButton resetButton;

    // Spektrogramm + Beschriftung Bereich einfügen
    juce::Rectangle<int> spectrogramArea;

    // Spektrogramm Display Bereich
    juce::Rectangle<int> spectrumDisplayArea;

    // Spektrogramm innerer Bereich
    juce::Rectangle<int> spectrumInnerArea;

    // EQ Fader mit Array erzeugen
    const std::array<float, 31> eqFrequencies =
    {
        20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160,
        200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
        2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000
    };

    // Array für vertikale Linien im Spektrogramm
    juce::Array<float> frequencies =
    {
        20, 50, 100,
        200, 500, 1000,
        2000, 5000, 10000, 20000
    };

    juce::Array<float> levels =
    {
        -20.0f, 0.0f, 20.0f, 40.0f, 60.0f, 80.0f
    };

    // Hintergrundbild: Spektogramm
    juce::Image background;

    // EQ Bereich einfügen
    juce::Rectangle<int> eqArea;

    // Silder erstellen
    juce::Slider eqSlider[31];
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> eqAttachments[31];

    // Q-Bereich
    juce::Rectangle<int> eqKnobArea;
    juce::Slider eqKnob[31];
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> eqQAttachments[31];

    // EQ Beschriftungsbereich
    juce::Rectangle<int> eqLabelArea;

    bool showEQCurve = false;
    juce::TextButton eqCurveToggleButton;
    float eqDisplayOffsetDb = 0.0f;

    // in class AudioPluginAudioProcessorEditor
    float averagedSpectrumDb = DisplayScale::minDb;

    // Bei den Funktionen hinzufügen:
    std::complex<float> peakingEQComplex(float freq, float f0, float Q, float gainDb, float sampleRate);
    void drawEQCurve(juce::Graphics& g);

    // Für Spektrum Smoothing (Ableton Standard)
    std::vector<float> smoothedLevels;
    static constexpr float smoothingFactor = 0.95f;

    // Räumliches Smoothing für glatteres Spektrum
    std::vector<float> applySpatialSmoothing(const std::vector<float>& levels, int windowSize = 3);

    // Layout Konstanten
    static constexpr int topBarHeight = 40;
    static constexpr int spectrogramOuterHeight = 430;
    static constexpr int spectrogramMargin = 10;
    static constexpr int eqLabelHeight = 22;
    static constexpr int eqSpacerHeight = 20;
    static constexpr int eqHeight = 180 + eqSpacerHeight;
    static constexpr int spectrumHeight = 390;
    static constexpr int spectrumBottomMargin = 20;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessorEditor)
};