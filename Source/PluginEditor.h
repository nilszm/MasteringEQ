#pragma once

#include "PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <complex>  // <-- HINZUFÜGEN für std::complex

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

    // Referenzkurve laden
    void loadReferenceCurve(const juce::String& filename);

private:
    // This reference is provided as a quick way for your editor to
    // access the processor object that created it.
    // Timer callback for GUI updates
    void timerCallback() override;

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

    // Button für Reset
    juce::TextButton resetButton;

    // Spektrogramm + Beschriftung Bereich einfügen
    juce::Rectangle<int> spectrogramArea;

    // Spektrogramm Display Bereich
    juce::Rectangle<int> spectrumDisplayArea;

    // Spektrogramm innerer Bereich
    juce::Rectangle<int> spectrumInnerArea;

    // EQ Fader mit Array erzeugen
    const std::array<float, 31> eqFrequencies = // <-- = hinzugefügt
    {
        20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160,
        200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
        2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000
    };

    // Array für vertikale Linien im Spektrogramm
    juce::Array<float> frequencies =  // <-- = hinzugefügt
    {
        20, 50, 100,
        200, 500, 1000,
        2000, 5000, 10000, 20000
    };

    juce::Array<float> levels =  // <-- = hinzugefügt
    {
        -20.0f, 0.0f, 20.0f, 40.0f, 60.0f, 80.0f
    };

    // Struct für Referenzkurven
    struct ReferenceBand
    {
        float freq; // Frequenz in Hz
        float p10;  // Unteres 10%-Perzentil
        float median; // Median (Zentralwert)
        float p90; // Oberes 90%-Perzentil
    };

    // Container für Frequenzbänder der Referenzkurven
    std::vector<ReferenceBand> referenceBands;

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
    float averagedSpectrumDb = DisplayScale::minDb;  // or e.g. -60.0f

    // Bei den Funktionen hinzufügen:
    std::complex<float> peakingEQComplex(float freq, float f0, float Q, float gainDb, float sampleRate);
    void drawEQCurve(juce::Graphics& g);

    // Für Spektrum Smoothing (Ableton Standard)
    std::vector<float> smoothedLevels;
    static constexpr float smoothingFactor = 0.8f; // Ableton Standard Avg

    // Räumliches Smoothing für glatteres Spektrum
    std::vector<float> applySpatialSmoothing(const std::vector<float>& levels, int windowSize = 3);

    // Layout Konstanten
    static constexpr int topBarHeight = 40; // Höhe der Topbar für Buttons und Dropdown
    static constexpr int spectrogramOuterHeight = 430;
    static constexpr int spectrogramMargin = 10;
    // static constexpr int eqHeight = 180;
    static constexpr int eqLabelHeight = 30;
    static constexpr int eqSpacerHeight = 40;
    static constexpr int eqHeight = 180 + eqSpacerHeight;
    //
    static constexpr int spectrumHeight = 390;
    static constexpr int spectrumBottomMargin = 20;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPluginAudioProcessorEditor)
};