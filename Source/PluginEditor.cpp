/**
 * @file PluginEditor.cpp
 * @brief Implementierung der grafischen Benutzeroberfläche für das Audio-Plugin.
 *
 * Diese Datei enthält die Implementierung des Plugin-Editors, einschließlich:
 * - Setup und Initialisierung aller UI-Komponenten
 * - Zeichnen des Spektrums und der EQ-Kurven
 * - Auto-EQ Berechnung und Visualisierung
 * - Layout-Management für alle Komponenten
 */

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <limits>
#include <complex>
#include <cstdint>
#include <cstring>
#include <cmath>

 //==============================================================================
 //                           Max-Correction
 //==============================================================================

namespace
{
    // Defaults
    constexpr float kAutoEqAmount = 1.0f; // 50%
    constexpr float kAutoEqMaxCorr = 12.0f;  // ±10 dB

    // Fixe Y-Skalierung für Referenz-Ansicht (Spektrumansicht)
    constexpr float kRefViewMinDb = -100.0f;  // unten
    constexpr float kRefViewMaxDb = -35.0f;  // oben

    // Rand-Fade: Bass & Air entschärfen
    static float edgeWeight(float f)
    {
        // Fade-in 20..40 Hz
        if (f < 40.0f)
            return juce::jlimit(0.0f, 1.0f, juce::jmap(f, 20.0f, 40.0f, 0.0f, 1.0f));

        // Fade-out 16k..20k
        if (f > 16000.0f)
            return juce::jlimit(0.0f, 1.0f, juce::jmap(f, 16000.0f, 20000.0f, 1.0f, 0.0f));

        return 1.0f;
    }

    // Breitband-Smoothing (Moving Average)
    static std::vector<float> smoothMovingAverage(const std::vector<float>& in, int windowSize, int passes)
    {
        if ((int)in.size() < 3 || windowSize < 3)
            return in;

        std::vector<float> cur = in;
        std::vector<float> out(in.size());
        const int half = windowSize / 2;

        for (int pass = 0; pass < passes; ++pass)
        {
            for (int i = 0; i < (int)cur.size(); ++i)
            {
                double sum = 0.0;
                int count = 0;

                for (int j = -half; j <= half; ++j)
                {
                    const int idx = i + j;
                    if (idx >= 0 && idx < (int)cur.size())
                    {
                        sum += cur[(size_t)idx];
                        ++count;
                    }
                }

                out[(size_t)i] = (count > 0) ? (float)(sum / (double)count) : cur[(size_t)i];
            }
            cur.swap(out);
        }

        return cur;
    }
    static void postProcessReferenceBands(std::vector<AudioPluginAudioProcessor::ReferenceBand>& bands)
    {
        if (bands.size() < 3) return;

        std::vector<float> p10, med, p90;
        p10.reserve(bands.size());
        med.reserve(bands.size());
        p90.reserve(bands.size());

        for (auto& b : bands)
        {
            p10.push_back(b.p10);
            med.push_back(b.median);
            p90.push_back(b.p90);
        }

        p10 = smoothMovingAverage(p10, 5, 2);
        med = smoothMovingAverage(med, 5, 2);
        p90 = smoothMovingAverage(p90, 5, 2);

        constexpr float kSpreadShrink = 0.55f;
        constexpr float kMaxBandWidthDb = 6.0f;
        constexpr float kMinBandWidthDb = 1.0f;

        for (size_t i = 0; i < bands.size(); ++i)
        {
            const float m = med[i];

            float lo = m - kSpreadShrink * (m - p10[i]);
            float hi = m + kSpreadShrink * (p90[i] - m);

            float w = hi - lo;
            if (w > kMaxBandWidthDb)
            {
                lo = m - 0.5f * kMaxBandWidthDb;
                hi = m + 0.5f * kMaxBandWidthDb;
            }
            else if (w < kMinBandWidthDb)
            {
                lo = m - 0.5f * kMinBandWidthDb;
                hi = m + 0.5f * kMinBandWidthDb;
            }

            if (lo > m) lo = m;
            if (hi < m) hi = m;

            bands[i].p10 = lo;
            bands[i].median = m;
            bands[i].p90 = hi;
        }
    }
}

 //==============================================================================
 //                           Log-Interpolation
 //==============================================================================

namespace
{
    static float sampleLogInterpolatedSpectrum(
        const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& pts,
        float fHz,
        float fallbackDb)
    {
        if (pts.empty())
            return fallbackDb;

        if (fHz <= pts.front().frequency) return pts.front().level;
        if (fHz >= pts.back().frequency)  return pts.back().level;

        const float lf = std::log10(fHz);

        for (size_t i = 1; i < pts.size(); ++i)
        {
            const float f1 = pts[i].frequency;
            if (f1 >= fHz)
            {
                const float f0 = pts[i - 1].frequency;

                const float l0 = std::log10(f0);
                const float l1 = std::log10(f1);

                const float t = (lf - l0) / (l1 - l0);

                return pts[i - 1].level + t * (pts[i].level - pts[i - 1].level);
            }
        }

        return pts.back().level;
    }

    // Interpoliert Median der Referenzbänder
    static float sampleLogInterpolatedReferenceMedian(
        const std::vector<AudioPluginAudioProcessor::ReferenceBand>& ref,
        float fHz,
        float fallbackDb)
    {
        if (ref.empty())
            return fallbackDb;

        if (fHz <= ref.front().freq) return ref.front().median;
        if (fHz >= ref.back().freq)  return ref.back().median;

        const float lf = std::log10(fHz);

        for (size_t i = 1; i < ref.size(); ++i)
        {
            const float f1 = ref[i].freq;
            if (f1 >= fHz)
            {
                const float f0 = ref[i - 1].freq;

                const float l0 = std::log10(f0);
                const float l1 = std::log10(f1);

                const float t = (lf - l0) / (l1 - l0);

                return ref[i - 1].median + t * (ref[i].median - ref[i - 1].median);
            }
        }

        return ref.back().median;
    }
}

 //==============================================================================
 //                               Smoothing
 //==============================================================================
 
namespace
{
    static std::vector<float> smoothResiduals3(const std::vector<float>& r)
    {
        if (r.size() < 3)
            return r;

        std::vector<float> out = r;

        // Ränder belassen
        for (size_t i = 1; i + 1 < r.size(); ++i)
            out[i] = 0.25f * r[i - 1] + 0.5f * r[i] + 0.25f * r[i + 1];

        return out;
    }
}

 //==============================================================================
 //                              KONSTRUKTOR
 //==============================================================================

 /**
  * @brief Konstruktor des Plugin-Editors.
  *
  * Initialisiert alle UI-Komponenten und startet den Timer für die
  * Spektrum-Aktualisierung mit 30 Hz.
  *
  * @param p Referenz auf den Audio-Processor
  */
AudioPluginAudioProcessorEditor::AudioPluginAudioProcessorEditor(AudioPluginAudioProcessor& p)
    : AudioProcessorEditor(&p), processorRef(p)
{
    // Initialzustand: EQ-Kurvenansicht deaktiviert
    showEQCurve = false;

    // Timer für FFT-Display-Updates (30 Frames pro Sekunde)
    startTimerHz(30);

    // Alle UI-Komponenten initialisieren
    initializeWindow();
    setupGenreDropdown();
    setupMeasurementButton();
    setupResetButton();
    setupEQCurveToggle();
    setupEQSliders();
    setupQKnobs();
    setupInputGainSlider();
    setupLoadReferenceButton();
}

/**
 * @brief Destruktor des Plugin-Editors.
 *
 * Räumt alle Ressourcen auf. Die meisten Ressourcen werden
 * automatisch durch JUCE verwaltet.
 */
AudioPluginAudioProcessorEditor::~AudioPluginAudioProcessorEditor()
{
}

//==============================================================================
//                                OFFSET
//==============================================================================

float AudioPluginAudioProcessorEditor::computeReferenceViewOffsetDb(
    const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum) const
{
    if (spectrum.empty() || processorRef.referenceBands.empty())
        return 0.0f;

    std::vector<float> diffs;
    diffs.reserve(31);

    // Mittlerer Bereich für stabile Werte
    const float fMin = 50.0f;
    const float fMax = 10000.0f;

    for (float f : eqFrequencies)
    {
        if (f < fMin || f > fMax) continue;

        const float ref = findReferenceLevel(f);
        const float meas = sampleLogInterpolatedSpectrum(spectrum, f, DisplayScale::minDb);

        diffs.push_back(ref - meas);
    }

    if (diffs.empty())
        return 0.0f;

    // Median
    std::nth_element(diffs.begin(), diffs.begin() + diffs.size() / 2, diffs.end());
    float median = diffs[diffs.size() / 2];

    // Clamping
    return juce::jlimit(-36.0f, 36.0f, median);
}


//==============================================================================
//                           SETUP-FUNKTIONEN
//==============================================================================

void AudioPluginAudioProcessorEditor::updateMeasurementButtonEnabledState()
{
    const bool hasGenre = (genreBox.getSelectedId() != 0);
    const bool hasReference = !processorRef.referenceBands.empty();

    const bool enable = hasGenre || hasReference;

    genreErkennenButton.setEnabled(enable);

    // Optional: Wenn disabled und gerade gemessen wird -> Messung beenden
    if (!enable && processorRef.isMeasuring())
    {
        processorRef.stopMeasurement();
        genreErkennenButton.setButtonText("Messung starten");
        genreErkennenButton.setColour(juce::TextButton::buttonColourId, juce::Colours::grey);
    }
}

/**
 * @brief Initialisiert die Fenstereinstellungen.
 *
 * Setzt die Fenstergröße auf 1000x690 Pixel und deaktiviert
 * die Größenänderung durch den Benutzer.
 */
void AudioPluginAudioProcessorEditor::initializeWindow()
{
    setSize(1000, 690);
    setResizable(false, false);
}

void AudioPluginAudioProcessorEditor::setupLoadReferenceButton()
{
    loadReferenceButton.setButtonText("Referenz laden");
    loadReferenceButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkgrey);

    loadReferenceButton.onClick = [this]
        {
            if (referenceAnalysisRunning)
                return;

            // FileChooser muss als Member existieren (sonst wird Callback nie ausgeführt)
            referenceFileChooser = std::make_unique<juce::FileChooser>(
                "Referenztrack wählen",
                juce::File{},
                "*.wav;*.aiff;*.aif;*.mp3"
            );

            auto flags = juce::FileBrowserComponent::openMode
                | juce::FileBrowserComponent::canSelectFiles;

            referenceFileChooser->launchAsync(flags, [this](const juce::FileChooser& chooser)
                {
                    const juce::File file = chooser.getResult();
                    referenceFileChooser.reset(); // aufräumen

                    if (!file.existsAsFile())
                        return;

                    // UI: Busy State
                    referenceAnalysisRunning = true;
                    loadReferenceButton.setEnabled(false);
                    loadReferenceButton.setButtonText("Analysiere...");

                    // SafePointer schützt vor Crashes wenn Editor geschlossen wird
                    juce::Component::SafePointer<AudioPluginAudioProcessorEditor> safeThis(this);

                    struct Job : public juce::ThreadPoolJob
                    {
                        Job(juce::Component::SafePointer<AudioPluginAudioProcessorEditor> s,
                            AudioPluginAudioProcessor& p,
                            juce::File f)
                            : juce::ThreadPoolJob("ReferenceAnalysisJob"), safeEditor(s), processor(p), file(std::move(f)) {}

                        JobStatus runJob() override
                        {
                            // Analyse (CPU-heavy) -> hier rein
                            auto bands = analyseFileToReferenceBands(file);

                            juce::MessageManager::callAsync([safe = safeEditor, bands = std::move(bands)]() mutable
                                {
                                    if (safe == nullptr)
                                        return;

                                    // Ergebnis in Processor schreiben + UI freigeben
                                    safe->processorRef.referenceBands = std::move(bands);
                                    safe->processorRef.hasTargetCorrections = false; // optional: Zielkurve zurücksetzen

                                    safe->referenceAnalysisRunning = false;
                                    safe->loadReferenceButton.setEnabled(true);
                                    safe->loadReferenceButton.setButtonText("Referenz laden");

                                    safe->updateMeasurementButtonEnabledState();

                                    safe->repaint();
                                });

                            return jobHasFinished;
                        }

                        // ---- Kern: Datei -> ReferenceBands ----
                        static std::vector<AudioPluginAudioProcessor::ReferenceBand>
                            analyseFileToReferenceBands(const juce::File& f)
                        {
                            std::vector<AudioPluginAudioProcessor::ReferenceBand> out;

                            juce::AudioFormatManager fm;
                            fm.registerBasicFormats();

                            std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(f));
                            if (!reader)
                                return out;

                            const double sr = reader->sampleRate > 0.0 ? reader->sampleRate : 48000.0;
                            const int64 totalSamples = reader->lengthInSamples;
                            const int numCh = (int)reader->numChannels;

                            // FFT-Settings (offline)
                            constexpr int fftOrder = 12;               // 4096
                            constexpr int fftSize = 1 << fftOrder;
                            constexpr int hopSize = fftSize / 2;      // 50% overlap

                            juce::dsp::FFT fft(fftOrder);
                            juce::dsp::WindowingFunction<float> win(fftSize, juce::dsp::WindowingFunction<float>::hann);

                            std::vector<float> mono((size_t)fftSize, 0.0f);
                            std::vector<float> fftData((size_t)2 * fftSize, 0.0f);

                            // Wir sammeln pro EQ-Band viele dB-Werte -> später P10/Median/P90
                            constexpr float bandFreqs[31] =
                            {
                                20, 25, 31.5f, 40, 50, 63, 80, 100, 125, 160,
                                200, 250, 315, 400, 500, 630, 800, 1000, 1250, 1600,
                                2000, 2500, 3150, 4000, 5000, 6300, 8000, 10000, 12500, 16000, 20000
                            };

                            std::array<std::vector<float>, 31> bandDbValues;
                            for (auto& v : bandDbValues) v.reserve(4096);

                            auto percentile = [](std::vector<float>& v, float p)
                                {
                                    if (v.empty()) return DisplayScale::minDb;
                                    std::sort(v.begin(), v.end());
                                    const float pos = p * (float)(v.size() - 1);
                                    const int i0 = (int)std::floor(pos);
                                    const int i1 = (int)std::ceil(pos);
                                    if (i0 == i1) return v[(size_t)i0];
                                    const float t = pos - (float)i0;
                                    return v[(size_t)i0] + t * (v[(size_t)i1] - v[(size_t)i0]);
                                };

                            auto hzToBin = [&](float hz)
                                {
                                    const int bin = (int)std::round(hz * (float)fftSize / (float)sr);
                                    return juce::jlimit(0, fftSize / 2, bin);
                                };

                            // Bandbreite 1/3 Okt: Grenzen = f * 2^(±1/6)
                            const float bandEdge = std::pow(2.0f, 1.0f / 6.0f);

                            juce::AudioBuffer<float> temp(numCh, (int)juce::jmin<int64>(totalSamples, fftSize));

                            int64 readPos = 0;
                            std::vector<float> overlap((size_t)fftSize, 0.0f);
                            bool haveOverlap = false;

                            while (readPos < totalSamples)
                            {
                                const int toRead = (int)juce::jmin<int64>((int64)hopSize, totalSamples - readPos);
                                temp.setSize(numCh, toRead, false, false, true);
                                reader->read(&temp, 0, toRead, readPos, true, true);

                                // frame bauen (overlap-add)
                                if (!haveOverlap)
                                {
                                    std::fill(overlap.begin(), overlap.end(), 0.0f);
                                    haveOverlap = true;
                                }

                                // shift left um hopSize
                                std::memmove(overlap.data(), overlap.data() + hopSize, sizeof(float) * (fftSize - hopSize));

                                // hinten neue Samples rein (mono)
                                for (int i = 0; i < hopSize; ++i)
                                {
                                    float s = 0.0f;
                                    if (i < toRead)
                                    {
                                        for (int ch = 0; ch < numCh; ++ch)
                                            s += temp.getSample(ch, i);
                                        s /= (float)juce::jmax(1, numCh);
                                    }
                                    overlap[(size_t)(fftSize - hopSize + i)] = s;
                                }

                                // window + FFT input
                                std::copy(overlap.begin(), overlap.end(), mono.begin());
                                win.multiplyWithWindowingTable(mono.data(), fftSize);

                                std::fill(fftData.begin(), fftData.end(), 0.0f);
                                std::copy(mono.begin(), mono.end(), fftData.begin());

                                fft.performFrequencyOnlyForwardTransform(fftData.data());

                                // pro Band: mags im Bandbereich mitteln -> dB speichern
                                for (int b = 0; b < 31; ++b)
                                {
                                    const float f0 = bandFreqs[b];
                                    const float fLo = juce::jmax(20.0f, f0 / bandEdge);
                                    const float fHi = juce::jmin(20000.0f, f0 * bandEdge);

                                    const int binLo = hzToBin(fLo);
                                    const int binHi = hzToBin(fHi);

                                    float sum = 0.0f;
                                    int cnt = 0;

                                    for (int k = binLo; k <= binHi; ++k)
                                    {
                                        sum += fftData[(size_t)k];
                                        ++cnt;
                                    }

                                    const float magRaw = (cnt > 0) ? (sum / (float)cnt) : 0.0f;

                                    // Normalisierung: JUCE FFT Magnitude ist größenabhängig.
                                    // Sehr brauchbarer Start: auf fftSize skalieren (single-sided grob: *2/fftSize)
                                    const float mag = magRaw * (2.0f / (float)fftSize);

                                    const float db = juce::Decibels::gainToDecibels(mag, DisplayScale::minDb);
                                    bandDbValues[b].push_back(juce::jlimit(DisplayScale::minDb, 0.0f, db));
                                }

                                readPos += toRead;
                            }

                            out.reserve(31);
                            for (int b = 0; b < 31; ++b)
                            {
                                auto v = std::move(bandDbValues[b]);

                                AudioPluginAudioProcessor::ReferenceBand band;
                                band.freq = bandFreqs[b];
                                band.p10 = percentile(v, 0.20f);
                                band.median = percentile(v, 0.50f);
                                band.p90 = percentile(v, 0.80f);
                                out.push_back(band);
                            }
                            {
                                const int windowSize = 5;
                                const int passes = 2;

                                std::vector<float> med;
                                med.reserve(out.size());
                                for (const auto& b : out)
                                    med.push_back(b.median);

                                auto smooth = [&](std::vector<float> v)
                                    {
                                        if ((int)v.size() < 3 || windowSize < 3) return v;

                                        std::vector<float> cur = v;
                                        std::vector<float> tmp(v.size());
                                        const int half = windowSize / 2;

                                        for (int pass = 0; pass < passes; ++pass)
                                        {
                                            for (int i = 0; i < (int)cur.size(); ++i)
                                            {
                                                double sum = 0.0;
                                                int cnt = 0;
                                                for (int j = -half; j <= half; ++j)
                                                {
                                                    const int idx = i + j;
                                                    if (idx >= 0 && idx < (int)cur.size())
                                                    {
                                                        sum += cur[(size_t)idx];
                                                        ++cnt;
                                                    }
                                                }
                                                tmp[(size_t)i] = (cnt > 0) ? (float)(sum / (double)cnt) : cur[(size_t)i];
                                            }
                                            cur.swap(tmp);
                                        }
                                        return cur;
                                    };

                                auto medSmoothed = smooth(med);

                                for (size_t i = 0; i < out.size(); ++i)
                                    out[i].median = medSmoothed[i];
                            }

                            {
                                constexpr float targetMidMedianDb = -60.0f;

                                std::vector<float> mids;
                                mids.reserve(out.size());

                                for (const auto& b : out)
                                    if (b.freq >= 50.0f && b.freq <= 10000.0f)
                                        mids.push_back(b.median);

                                if (!mids.empty())
                                {
                                    std::sort(mids.begin(), mids.end());
                                    const float midMedian = mids[mids.size() / 2];

                                    const float shift = targetMidMedianDb - midMedian;

                                    for (auto& b : out)
                                    {
                                        b.p10 += shift;
                                        b.median += shift;
                                        b.p90 += shift;
                                    }
                                }
                            }
                            postProcessReferenceBands(out);
                            return out;
                        }

                        juce::Component::SafePointer<AudioPluginAudioProcessorEditor> safeEditor;
                        AudioPluginAudioProcessor& processor;
                        juce::File file;
                    };

                    referenceAnalysisPool.addJob(new Job(safeThis, processorRef, file), true);
                });
        };

    addAndMakeVisible(loadReferenceButton);
}


/**
 * @brief Konfiguriert das Genre-Dropdown-Menü.
 *
 * Erstellt das Dropdown mit allen verfügbaren Genres und registriert
 * den onChange-Callback zum Laden der entsprechenden Referenzkurve.
 * Stellt außerdem das zuletzt ausgewählte Genre wieder her.
 */
void AudioPluginAudioProcessorEditor::setupGenreDropdown()
{
    // Platzhaltertext wenn nichts ausgewählt
    genreBox.setTextWhenNothingSelected("Genre auswahlen...");

    // Alle verfügbaren Genres hinzufügen
    genreBox.addItem("Pop", 1);
    genreBox.addItem("HipHop", 2);
    genreBox.addItem("Rock", 3);
    genreBox.addItem("EDM", 4);
    genreBox.addItem("Klassik", 5);
    genreBox.addItem("Test", 6);

    // Callback bei Genre-Auswahl: Lädt die entsprechende JSON-Referenzkurve
    genreBox.onChange = [this]
        {
            const int id = genreBox.getSelectedId();
            processorRef.selectedGenreId = id;

            // Genre-spezifische Referenzkurve laden
            switch (id)
            {
            case 1:
                processorRef.loadReferenceCurve("pop_neu.json");
                postProcessReferenceBands(processorRef.referenceBands);
                break;
            case 2:
                processorRef.loadReferenceCurve("HipHop.json");
                postProcessReferenceBands(processorRef.referenceBands);
                break;
            case 3:
                processorRef.loadReferenceCurve("Rock.json");
                postProcessReferenceBands(processorRef.referenceBands);
                break;
            case 4:
                processorRef.loadReferenceCurve("EDM.json");
                postProcessReferenceBands(processorRef.referenceBands);
                break;
            case 5:
                processorRef.loadReferenceCurve("Klassik.json");
                postProcessReferenceBands(processorRef.referenceBands);
                break;
            case 6:
                processorRef.loadReferenceCurve("test.json");
                postProcessReferenceBands(processorRef.referenceBands);
                break;
            default:
                processorRef.referenceBands.clear();
                break;
            }

            repaint();
            updateMeasurementButtonEnabledState();
        };

    // Gespeichertes Genre aus vorheriger Session wiederherstellen
    if (processorRef.selectedGenreId > 0)
    {
        genreBox.setSelectedId(processorRef.selectedGenreId, juce::dontSendNotification);
    }
    updateMeasurementButtonEnabledState();
    addAndMakeVisible(genreBox);
}

/**
 * @brief Konfiguriert den Mess-Button.
 *
 * Erstellt den Button zum Starten/Stoppen der Spektrum-Messung.
 * Der Button wechselt zwischen "Messung starten" (grau) und
 * "Messung stoppen" (grün). Bei Messstopp wird automatisch
 * die Auto-EQ Berechnung durchgeführt.
 */
void AudioPluginAudioProcessorEditor::setupMeasurementButton()
{
    genreErkennenButton.setButtonText("Messung starten");
    genreErkennenButton.setColour(juce::TextButton::buttonColourId, juce::Colours::grey);

    genreErkennenButton.onClick = [this]
        {        
            // Messung starten oder stoppen
            if (processorRef.isMeasuring())
            {
                // Messung beenden
                processorRef.stopMeasurement();
                genreErkennenButton.setButtonText("Messung starten");
                genreErkennenButton.setEnabled(true);
                genreErkennenButton.setColour(juce::TextButton::buttonColourId, juce::Colours::grey);

                // Auto-EQ berechnen wenn Referenzkurve vorhanden
                if (!processorRef.referenceBands.empty())
                {
                    applyAutoEQ();
                }
                else
                {
                    DBG("Keine Referenzkurve ausgewählt!");
                }
            }
            else
            {
                // Messung starten
                processorRef.startMeasurement();
                genreErkennenButton.setButtonText("Messung stoppen");
                genreErkennenButton.setColour(juce::TextButton::buttonColourId, juce::Colours::green);
            }
        };

    addAndMakeVisible(genreErkennenButton);
}

/**
 * @brief Konfiguriert den Reset-Button.
 *
 * Erstellt einen roten Button, der alle EQ-Bänder auf ihre
 * Standardwerte zurücksetzt.
 */
void AudioPluginAudioProcessorEditor::setupResetButton()
{
    resetButton.setButtonText("Reset");
    resetButton.setColour(juce::TextButton::buttonColourId, juce::Colours::red);
    resetButton.onClick = [this]
        {
            processorRef.resetAllBandsToDefault();
        };

    addAndMakeVisible(resetButton);
}

/**
 * @brief Konfiguriert den EQ-Kurven-Toggle-Button.
 *
 * Erstellt einen Toggle-Button zum Umschalten zwischen
 * Spektrum-Ansicht und EQ-Kurven-Ansicht.
 */
void AudioPluginAudioProcessorEditor::setupEQCurveToggle()
{
    eqCurveToggleButton.setButtonText("EQ Ansicht");
    eqCurveToggleButton.setClickingTogglesState(true);
    eqCurveToggleButton.setToggleState(false, juce::dontSendNotification);

    eqCurveToggleButton.onClick = [this]
        {
            showEQCurve = eqCurveToggleButton.getToggleState();

            // Button-Text entsprechend der Ansicht aktualisieren
            if (showEQCurve)
                eqCurveToggleButton.setButtonText("Referenz Ansicht");
            else
                eqCurveToggleButton.setButtonText("EQ Ansicht");

            repaint();
        };

    addAndMakeVisible(eqCurveToggleButton);
}

/**
 * @brief Konfiguriert alle 31 EQ-Slider.
 *
 * Erstellt vertikale Slider für jedes der 31 EQ-Bänder.
 * Jeder Slider hat einen Bereich von -12 bis +12 dB und
 * ist mit dem entsprechenden Parameter im AudioProcessorValueTreeState
 * verbunden.
 */
void AudioPluginAudioProcessorEditor::setupEQSliders()
{
    for (int i = 0; i < 31; i++)
    {
        // Slider-Stil und Wertebereich konfigurieren
        eqSlider[i].setSliderStyle(juce::Slider::LinearVertical);
        eqSlider[i].setRange(-12.0, 12.0, 0.1);
        eqSlider[i].setValue(0.0);
        eqSlider[i].setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);

        // Farben setzen
        eqSlider[i].setColour(juce::Slider::thumbColourId, juce::Colours::white);
        eqSlider[i].setColour(juce::Slider::trackColourId, juce::Colours::lightgrey);

        // Mit Parameter-State verbinden für Automation und Preset-Speicherung
        eqAttachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processorRef.apvts, "band" + juce::String(i), eqSlider[i]);

        addAndMakeVisible(eqSlider[i]);
    }
}

/**
 * @brief Konfiguriert alle 31 Q-Faktor-Knobs.
 *
 * Erstellt Drehregler für den Q-Faktor (Bandbreite) jedes EQ-Bands.
 * Der Q-Bereich reicht von 0.3 (breit) bis 10.0 (schmal).
 */
void AudioPluginAudioProcessorEditor::setupQKnobs()
{
    for (int i = 0; i < 31; ++i)
    {
        // Knob-Stil und Wertebereich konfigurieren
        eqKnob[i].setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        eqKnob[i].setRange(0.3, 10.0, 0.01);
        eqKnob[i].setValue(4.32);  // Standard Q-Wert für 1/3-Oktav-EQ
        eqKnob[i].setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);

        // Farben setzen
        eqKnob[i].setColour(juce::Slider::thumbColourId, juce::Colours::white);
        eqKnob[i].setColour(juce::Slider::rotarySliderFillColourId, juce::Colours::darkgrey);
        eqKnob[i].setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colours::black);

        // Mit Parameter-State verbinden
        eqQAttachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processorRef.apvts, "bandQ" + juce::String(i), eqKnob[i]);

        addAndMakeVisible(eqKnob[i]);
    }
}

/**
 * @brief Konfiguriert den Input-Gain-Slider.
 *
 * Erstellt einen horizontalen Slider für die Eingangs-Verstärkung
 * mit einem Bereich von -24 bis +24 dB. Wird auch für die automatische
 * Lautheitsanpassung verwendet.
 */
void AudioPluginAudioProcessorEditor::setupInputGainSlider()
{
    // Slider-Stil und Wertebereich konfigurieren
    inputGainSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    inputGainSlider.setRange(-24.0, 24.0, 0.1);
    inputGainSlider.setValue(0.0);
    inputGainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);

    // Transparente Textbox für bessere Optik
    inputGainSlider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    inputGainSlider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    inputGainSlider.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    inputGainSlider.setColour(juce::Slider::trackColourId, juce::Colours::green);
    inputGainSlider.setTextValueSuffix(" dB");

    // Mit Parameter-State verbinden
    inputGainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processorRef.apvts, "inputGain", inputGainSlider);

    // Label konfigurieren und links vom Slider positionieren
    inputGainLabel.setText("Input Gain", juce::dontSendNotification);
    inputGainLabel.setJustificationType(juce::Justification::centredLeft);
    inputGainLabel.attachToComponent(&inputGainSlider, true);

    addAndMakeVisible(inputGainSlider);
    addAndMakeVisible(inputGainLabel);
}

//==============================================================================
//                            PAINT-FUNKTION
//==============================================================================

/**
 * @brief Hauptzeichenfunktion des Editors.
 *
 * Wird automatisch von JUCE aufgerufen wenn das Fenster neu
 * gezeichnet werden muss. Delegiert an spezialisierte Draw-Funktionen.
 *
 * @param g Der Graphics-Kontext zum Zeichnen
 */
void AudioPluginAudioProcessorEditor::paint(juce::Graphics& g)
{
    drawTopBar(g);
    drawBackground(g);
    drawSpectrumArea(g);
    drawFrequencyGrid(g);
    drawEQAreas(g);
    drawEQLabels(g);
}

//==============================================================================
//                           DRAW-FUNKTIONEN
//==============================================================================

/**
 * @brief Zeichnet die obere Menüleiste.
 *
 * Füllt den Topbar-Bereich mit der definierten Hintergrundfarbe.
 *
 * @param g Der Graphics-Kontext zum Zeichnen
 */
void AudioPluginAudioProcessorEditor::drawTopBar(juce::Graphics& g)
{
    g.setColour(juce::Colour::fromString("ff2c2f33"));
    g.fillRect(topBarArea);
}

/**
 * @brief Zeichnet den Haupthintergrund.
 *
 * Füllt den Bereich unterhalb der Topbar mit dunkler Farbe.
 *
 * @param g Der Graphics-Kontext zum Zeichnen
 */
void AudioPluginAudioProcessorEditor::drawBackground(juce::Graphics& g)
{
    auto rest = getLocalBounds().withY(topBarArea.getBottom());
    g.setColour(juce::Colour::fromString("ff111111"));
    g.fillRect(rest);
}

/**
 * @brief Zeichnet den Spektrum-Anzeigebereich.
 *
 * Zeichnet die Debug-Bereiche (orange, grün, pink) und ruft je nach
 * Ansichtsmodus entweder drawFrame() für das Spektrum oder
 * drawEQCurve() für die EQ-Kurve auf. Zeichnet auch die Referenzbänder
 * wenn vorhanden.
 *
 * @param g Der Graphics-Kontext zum Zeichnen
 */
void AudioPluginAudioProcessorEditor::drawSpectrumArea(juce::Graphics& g)
{
    // Konstanten für Frequenz- und dB-Bereich
    const float minFreq = 20.0f;
    const float maxFreq = 20000.0f;
    const float displayMinDb = kRefViewMinDb;
    const float displayMaxDb = kRefViewMaxDb;

    // Debug-Bereiche färben (TODO: Im Release entfernen)
    g.setColour(juce::Colours::orange);
    g.fillRect(spectrogramArea);

    g.setColour(juce::Colours::green);
    g.fillRect(spectrumDisplayArea);

    g.setColour(juce::Colours::hotpink);
    g.fillRect(spectrumInnerArea);

    // Spektrum/EQ-Kurve im inneren Bereich zeichnen mit Clipping
    {
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(spectrumInnerArea);

        // Je nach Ansichtsmodus zeichnen
        if (!showEQCurve)
        {
            drawFrame(g);
        }
        else
        {
            drawEQCurve(g);
        }

        // Referenzbänder nur in Spektrum-Ansicht zeichnen
        if (!showEQCurve && !processorRef.referenceBands.empty())
        {
            drawReferenceBands(g, minFreq, maxFreq, displayMinDb, displayMaxDb);
        }
    }
}

/**
 * @brief Zeichnet die Referenzbänder (P10, Median, P90).
 *
 * Zeichnet drei Linien für die statistische Verteilung der
 * Referenzkurve: 10. Perzentil (blau), Median (grau), 90. Perzentil (blau).
 *
 * @param g Der Graphics-Kontext zum Zeichnen
 * @param minFreq Minimale Frequenz in Hz (20)
 * @param maxFreq Maximale Frequenz in Hz (20000)
 * @param displayMinDb Minimaler dB-Wert für Anzeige
 * @param displayMaxDb Maximaler dB-Wert für Anzeige
 */
void AudioPluginAudioProcessorEditor::drawReferenceBands(juce::Graphics& g,
    float minFreq, float maxFreq, float displayMinDb, float displayMaxDb)
{
    // Pfade für die drei Referenzlinien
    juce::Path pathP10;
    juce::Path pathP90;
    juce::Path pathMedian;
    bool firstPoint = true;

    auto clampDb = [&](float db)
        {
            return juce::jlimit(displayMinDb, displayMaxDb, db);
        };

    // Durch alle Referenzbänder iterieren und Pfade aufbauen
    for (const auto& band : processorRef.referenceBands)
    {
        // Frequenzbänder außerhalb der Darstellung überspringen
        if (band.freq < minFreq || band.freq > maxFreq)
            continue;

        // X-Position: Frequenz logarithmisch auf Pixelposition abbilden
        float normX = juce::mapFromLog10(band.freq, minFreq, maxFreq);
        float x = spectrumInnerArea.getX() + normX * spectrumInnerArea.getWidth();

        // Y-Positionen: dB-Werte auf Pixelpositionen abbilden (invertiert: oben = laut)
        float yP10 = juce::jmap(clampDb(band.p10), displayMinDb, displayMaxDb,
            (float)spectrumInnerArea.getBottom(),
            (float)spectrumInnerArea.getY());

        float yMedian = juce::jmap(clampDb(band.median), displayMinDb, displayMaxDb,
            (float)spectrumInnerArea.getBottom(),
            (float)spectrumInnerArea.getY());

        float yP90 = juce::jmap(clampDb(band.p90), displayMinDb, displayMaxDb,
            (float)spectrumInnerArea.getBottom(),
            (float)spectrumInnerArea.getY());

        // Pfade aufbauen
        if (firstPoint)
        {
            pathP10.startNewSubPath(x, yP10);
            pathP90.startNewSubPath(x, yP90);
            pathMedian.startNewSubPath(x, yMedian);
            firstPoint = false;
        }
        else
        {
            pathP10.lineTo(x, yP10);
            pathP90.lineTo(x, yP90);
            pathMedian.lineTo(x, yMedian);
        }
    }

    // Referenzlinien zeichnen
    g.setColour(juce::Colours::blue.withAlpha(0.6f));
    g.strokePath(pathP10, juce::PathStrokeType(1.5f));

    g.setColour(juce::Colours::blue.withAlpha(0.6f));
    g.strokePath(pathP90, juce::PathStrokeType(1.5f));

    g.setColour(juce::Colours::grey);
    g.strokePath(pathMedian, juce::PathStrokeType(2.0f));
}

/**
 * @brief Zeichnet das Frequenzraster mit Beschriftung.
 *
 * Zeichnet vertikale Linien bei bestimmten Frequenzen und
 * fügt die Frequenzbeschriftung unterhalb des Spektrums hinzu.
 *
 * @param g Der Graphics-Kontext zum Zeichnen
 */
void AudioPluginAudioProcessorEditor::drawFrequencyGrid(juce::Graphics& g)
{
    const float minFreq = 20.0f;
    const float maxFreq = 20000.0f;

    g.setFont(15.0f);
    g.setColour(juce::Colours::white.withAlpha(0.5f));

    // Y-Position für Beschriftung (unterhalb des Spektrums)
    float textY = (float)spectrumDisplayArea.getBottom() + 3.0f;

    // Durch alle definierten Rasterfrequenzen iterieren
    for (auto f : frequencies)
    {
        // X-Position logarithmisch berechnen
        float normX = juce::mapFromLog10(f, minFreq, maxFreq);
        float x = spectrumInnerArea.getX() + normX * spectrumInnerArea.getWidth();

        // Vertikale Rasterlinie zeichnen
        g.drawVerticalLine(
            static_cast<int>(x),
            (float)spectrumInnerArea.getY(),
            (float)spectrumInnerArea.getBottom()
        );

        // Frequenzbeschriftung formatieren (z.B. "1k" statt "1000")
        juce::String text;
        if (f >= 1000.0f)
            text = juce::String(f / 1000.0f) + "k";
        else
            text = juce::String((int)f);

        // Beschriftung zentriert unter der Linie
        g.drawFittedText(
            text,
            (int)(x - 15),
            (int)textY,
            30,
            15,
            juce::Justification::centred,
            1
        );
    }
}

/**
 * @brief Zeichnet die EQ-Bereiche (Slider, Knobs, Labels).
 *
 * Füllt die drei EQ-Bereiche mit Debug-Farben.
 * TODO: Im Release durch finale Farben ersetzen.
 *
 * @param g Der Graphics-Kontext zum Zeichnen
 */
void AudioPluginAudioProcessorEditor::drawEQAreas(juce::Graphics& g)
{
    // EQ Sliderbereich (blau)
    g.setColour(juce::Colours::blue);
    g.fillRect(eqArea);

    // Q-Knob-Bereich (hellgrün)
    g.setColour(juce::Colours::lightgreen);
    g.fillRect(eqKnobArea);

    // EQ Beschriftungsbereich (rot)
    g.setColour(juce::Colours::red);
    g.fillRect(eqLabelArea);
}

/**
 * @brief Zeichnet die Frequenzbeschriftung unter den EQ-Slidern.
 *
 * Zeigt für jeden der 31 Slider die zugehörige Frequenz an.
 * Frequenzen >= 1kHz werden als "Xk" dargestellt.
 *
 * @param g Der Graphics-Kontext zum Zeichnen
 */
void AudioPluginAudioProcessorEditor::drawEQLabels(juce::Graphics& g)
{
    g.setColour(juce::Colours::white.withAlpha(0.5f));
    g.setFont(14.0f);

    for (int i = 0; i < eqFrequencies.size(); i++)
    {
        // X-Position logarithmisch berechnen
        float normX = juce::mapFromLog10(eqFrequencies[i], 16.0f, 25500.f);
        int x = eqArea.getX() + static_cast<int>(normX * eqArea.getWidth());

        // Frequenzlabel formatieren
        juce::String label;
        if (eqFrequencies[i] >= 1000.0f)
        {
            float valueInK = eqFrequencies[i] / 1000.0f;

            if (valueInK >= 10.0f)
                label = juce::String((int)valueInK) + "k";
            else
                label = juce::String(valueInK, 1) + "k";
        }
        else
        {
            label = juce::String((int)eqFrequencies[i]);
        }

        // Label zentriert zeichnen
        g.drawFittedText(
            label,
            x - 20,
            eqLabelArea.getY() + 5,
            40,
            20,
            juce::Justification::centred,
            1
        );
    }
}

//==============================================================================
//                           TIMER-CALLBACK
//==============================================================================

/**
 * @brief Timer-Callback für regelmäßige Display-Updates.
 *
 * Wird 30 mal pro Sekunde aufgerufen um das Spektrum zu aktualisieren.
 * Prüft ob neue FFT-Daten verfügbar sind und aktualisiert die Anzeige.
 * Speichert auch Messungs-Snapshots wenn eine Messung aktiv ist.
 */
void AudioPluginAudioProcessorEditor::timerCallback()
{
    bool needsRepaint = false;

    // Post-EQ FFT für Anzeige aktualisieren
    if (processorRef.getNextFFTBlockReady())
    {
        processorRef.updateSpectrumArray(processorRef.getSampleRate());
        processorRef.setNextFFTBlockReady(false);

        // Offset live berechnen
        if (!processorRef.referenceBands.empty())
        {
            const float targetOffset = computeReferenceViewOffsetDb(processorRef.spectrumArray);

            // Glätten
            const float a = 0.90f; // 0.90 = sehr ruhig
            referenceViewOffsetDbSmoothed = a * referenceViewOffsetDbSmoothed + (1.0f - a) * targetOffset;
            referenceViewOffsetDb = referenceViewOffsetDbSmoothed;
        }
        else
        {
            referenceViewOffsetDb = referenceViewOffsetDbSmoothed = 0.0f;
        }

        needsRepaint = true;
    }

    // Pre-EQ FFT für Messung aktualisieren
    if (processorRef.getNextPreEQFFTBlockReady())
    {
        processorRef.updatePreEQSpectrumArray(processorRef.getSampleRate());
        processorRef.setNextPreEQFFTBlockReady(false);

        // Snapshot für Durchschnittsberechnung speichern wenn Messung aktiv
        if (processorRef.isMeasuring())
        {
            processorRef.addMeasurementSnapshot();
        }
    }

    // Nur neu zeichnen wenn sich etwas geändert hat
    if (needsRepaint)
    {
        repaint();
    }
}

//==============================================================================
//                          RESIZED-FUNKTION
//==============================================================================

/**
 * @brief Layout-Funktion für alle Komponenten.
 *
 * Wird aufgerufen wenn sich die Fenstergröße ändert (oder beim Start).
 * Berechnet und setzt die Positionen aller UI-Elemente.
 */
void AudioPluginAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    layoutTopBar(area);
    layoutSpectrumAreas(area);
    layoutEQAreas(area);
    layoutEQSliders();
    layoutQKnobs();
    calculateSpectrumInnerArea();
}

//==============================================================================
//                      RESIZED HILFSFUNKTIONEN
//==============================================================================

/**
 * @brief Positioniert alle Komponenten in der Topbar.
 *
 * @param area Referenz auf den verfügbaren Bereich (wird modifiziert)
 */
void AudioPluginAudioProcessorEditor::layoutTopBar(juce::Rectangle<int>& area)
{
    // Topbar-Bereich vom Hauptbereich abtrennen
    topBarArea = area.removeFromTop(topBarHeight);

    // Alle Controls mit festen Positionen
    genreErkennenButton.setBounds(10, 5, 200, 30);
    loadReferenceButton.setBounds(560, 5, 140, 30);
    eqCurveToggleButton.setBounds(220, 5, 150, 30);
    genreBox.setBounds(710, 5, 220, 30);
    resetButton.setBounds(940, 5, 50, 30);
    inputGainSlider.setBounds(770, 45, 220, 30);
}

/**
 * @brief Berechnet die Spektrum-Anzeigebereiche.
 *
 * @param area Referenz auf den verfügbaren Bereich (wird modifiziert)
 */
void AudioPluginAudioProcessorEditor::layoutSpectrumAreas(juce::Rectangle<int>& area)
{
    // Äußerer Bereich für Spektrogramm
    auto spectroOuter = area.removeFromTop(spectrogramOuterHeight);

    // Innerer Bereich mit Rand
    spectrogramArea = spectroOuter.reduced(spectrogramMargin);

    // Display-Bereich für das Spektrum
    spectrumDisplayArea = spectrogramArea.removeFromTop(spectrumHeight);
}

/**
 * @brief Berechnet die EQ-Bereiche (Slider, Knobs, Labels).
 *
 * @param area Referenz auf den verfügbaren Bereich (wird modifiziert)
 */
void AudioPluginAudioProcessorEditor::layoutEQAreas(juce::Rectangle<int>& area)
{
    // Gesamter EQ-Bereich
    auto eqFullArea = area.removeFromTop(eqHeight);

    // Von unten nach oben aufteilen
    eqLabelArea = eqFullArea.removeFromBottom(eqLabelHeight);
    eqKnobArea = eqFullArea.removeFromBottom(eqSpacerHeight);
    eqArea = eqFullArea;
}

/**
 * @brief Positioniert alle 31 EQ-Slider.
 *
 * Die Slider werden logarithmisch entsprechend ihrer Frequenz positioniert.
 */
void AudioPluginAudioProcessorEditor::layoutEQSliders()
{
    for (int i = 0; i < 31; ++i)
    {
        // Logarithmische X-Position berechnen
        float normX = juce::mapFromLog10(eqFrequencies[i], 16.0f, 25500.0f);
        int x = eqArea.getX() + static_cast<int>(normX * eqArea.getWidth());

        // Slider-Dimensionen
        int sliderWidth = 16;
        const int verticalMargin = 8;
        int sliderHeight = eqArea.getHeight() - 2 * verticalMargin;

        // Slider zentriert positionieren
        eqSlider[i].setBounds(
            x - sliderWidth / 2,
            eqArea.getY() + 10,
            sliderWidth,
            sliderHeight
        );
    }
}

/**
 * @brief Positioniert alle 31 Q-Faktor-Knobs.
 *
 * Die Knobs werden direkt unter den zugehörigen Slidern positioniert.
 */
void AudioPluginAudioProcessorEditor::layoutQKnobs()
{
    for (int i = 0; i < 31; ++i)
    {
        // X-Position von Slider übernehmen (zentriert)
        int centerX = eqSlider[i].getX() + eqSlider[i].getWidth() / 2;

        // Knob-Durchmesser basierend auf verfügbarem Platz
        float bandWidth = (float)eqArea.getWidth() / 31.0f;
        int knobDiameter = (int)std::floor(bandWidth * 1.3f);

        // Knob zentriert positionieren
        int x = centerX - knobDiameter / 2;
        int y = eqKnobArea.getCentreY() - knobDiameter / 2;

        eqKnob[i].setBounds(x, y, knobDiameter, knobDiameter);
    }
}

/**
 * @brief Berechnet den inneren Spektrum-Bereich.
 *
 * Der innere Bereich wird so berechnet, dass er genau mit den
 * äußeren Slidern (20 Hz und 20 kHz) ausgerichtet ist.
 */
void AudioPluginAudioProcessorEditor::calculateSpectrumInnerArea()
{
    const int firstIndex = 0;   // 20 Hz Slider
    const int lastIndex = 30;   // 20 kHz Slider

    // X-Grenzen von den äußeren Slidern ableiten
    int leftX = eqSlider[firstIndex].getX() + eqSlider[firstIndex].getWidth() / 2;
    int rightX = eqSlider[lastIndex].getX() + eqSlider[lastIndex].getWidth() / 2;
    int innerWidth = rightX - leftX;

    // Inneren Bereich definieren
    spectrumInnerArea = juce::Rectangle<int>(
        leftX,
        spectrumDisplayArea.getY(),
        innerWidth,
        spectrumDisplayArea.getHeight()
    );
}

//==============================================================================
//                    DRAW FRAME - SPEKTRUM ZEICHNEN
//==============================================================================

/**
 * @brief Zeichnet das aktuelle Frequenzspektrum.
 *
 * Hauptfunktion zum Zeichnen des Echtzeit-Spektrums.
 * Wendet verschiedene Smoothing-Algorithmen an und extrapoliert zu 20 Hz.
 *
 * @param g Der Graphics-Kontext zum Zeichnen
 */
void AudioPluginAudioProcessorEditor::drawFrame(juce::Graphics& g)
{
    auto& spectrum = processorRef.spectrumArray;
    if (spectrum.empty())
        return;

    // 1. Smoothing-Buffer initialisieren
    initializeSmoothedLevels(spectrum);

    // 2. Spektrumpunkte mit Exponential Smoothing berechnen
    auto validPoints = calculateSpectrumPoints(spectrum);

    // 3. Räumliches Smoothing für glattere Kurve anwenden
    applySpatialSmoothingToPoints(validPoints);

    // 5. Mindestens 2 Punkte für eine Linie benötigt
    if (validPoints.size() < 2)
        return;

    // 6. Spektrumkurve zeichnen
    drawSpectrumPath(g, validPoints);
}

//==============================================================================
//                   DRAW FRAME HILFSFUNKTIONEN
//==============================================================================

/**
 * @brief Initialisiert den Smoothing-Buffer für die Spektrumwerte.
 *
 * Passt die Größe des Buffers an und initialisiert neue Werte
 * mit den aktuellen Spektrumwerten.
 *
 * @param spectrum Referenz auf das aktuelle Spektrum-Array
 */
void AudioPluginAudioProcessorEditor::initializeSmoothedLevels(
    const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum)
{
    // Buffer nur bei Größenänderung neu initialisieren
    if (smoothedLevels.size() != spectrum.size())
    {
        smoothedLevels.resize(spectrum.size());
        for (size_t i = 0; i < spectrum.size(); ++i)
        {
            smoothedLevels[i] = spectrum[i].level;
        }
    }
}

/**
 * @brief Berechnet die Pixel-Koordinaten für alle Spektrumpunkte.
 *
 * Wendet dabei Exponential Smoothing (zeitliches Glätten) an und
 * konvertiert Frequenz/dB-Werte zu Bildschirmkoordinaten.
 *
 * @param spectrum Referenz auf das aktuelle Spektrum-Array
 * @return Vector mit Punktkoordinaten für den Pfad
 */
std::vector<juce::Point<float>> AudioPluginAudioProcessorEditor::calculateSpectrumPoints(
    const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum)
{
    auto area = spectrumInnerArea.toFloat();

    // Display-Grenzen
    // Display-Grenzen je nach Ansicht
    const float displayMinDb = showEQCurve ? -12.0f : kRefViewMinDb;
    const float displayMaxDb = showEQCurve ? 12.0f : kRefViewMaxDb;
    const float minFreq = 20.0f;
    const float maxFreq = 20000.0f;
    const float logMin = std::log10(minFreq);
    const float logMax = std::log10(maxFreq);

    std::vector<juce::Point<float>> validPoints;

    for (size_t i = 0; i < spectrum.size(); ++i)
    {
        auto& point = spectrum[i];

        // Nur Frequenzen im sichtbaren Bereich verarbeiten
        if (point.frequency < minFreq || point.frequency > maxFreq)
            continue;

        // Exponential Smoothing: Kombiniert alten und neuen Wert
        // smoothingFactor nahe 1.0 = langsame Änderung (mehr Glätten)
        smoothedLevels[i] = smoothedLevels[i] * smoothingFactor +
            point.level * (1.0f - smoothingFactor);

        float level = smoothedLevels[i];
        if (!processorRef.referenceBands.empty())
            level += referenceViewOffsetDb;

        // Frequenz logarithmisch auf X-Position abbilden
        float logFreq = std::log10(point.frequency);
        float x = area.getX() + juce::jmap(logFreq, logMin, logMax, 0.0f, 1.0f) * area.getWidth();

        // dB-Wert auf Y-Position abbilden (invertiert: oben = laut)
        float db = juce::jlimit(displayMinDb, displayMaxDb, level);
        float y = juce::jmap(db, displayMinDb, displayMaxDb, area.getBottom(), area.getY());

        validPoints.push_back({ x, y });
    }

    return validPoints;
}

/**
 * @brief Wendet räumliches Smoothing auf die Y-Werte an.
 *
 * Glättet die Kurve durch Mittelwertbildung benachbarter Punkte.
 *
 * @param points Referenz auf die Punktliste (wird modifiziert)
 */
void AudioPluginAudioProcessorEditor::applySpatialSmoothingToPoints(
    std::vector<juce::Point<float>>& points)
{
    // Y-Werte extrahieren
    std::vector<float> yValues;
    yValues.reserve(points.size());

    for (const auto& point : points)
        yValues.push_back(point.getY());

    // Räumliches Smoothing mit Fenstergröße 5 anwenden
    auto smoothedY = applySpatialSmoothing(yValues, 5);

    // Geglättete Y-Werte zurückschreiben
    for (size_t i = 0; i < points.size() && i < smoothedY.size(); ++i)
        points[i].setY(smoothedY[i]);
}

/**
 * @brief Zeichnet den Spektrum-Pfad.
 *
 * Erstellt einen Pfad aus den Punkten und zeichnet ihn.
 * Verwendet unterschiedliche Transparenz je nach Ansichtsmodus.
 *
 * @param g Der Graphics-Kontext zum Zeichnen
 * @param points Die zu zeichnenden Punkte
 */
void AudioPluginAudioProcessorEditor::drawSpectrumPath(
    juce::Graphics& g,
    const std::vector<juce::Point<float>>& points)
{
    // Pfad aus Punkten aufbauen
    juce::Path spectrumPath;
    spectrumPath.startNewSubPath(points[0]);

    for (size_t i = 1; i < points.size(); ++i)
    {
        spectrumPath.lineTo(points[i]);
    }

    // Farbe und Stärke je nach Ansichtsmodus
    if (showEQCurve)
    {
        // Transparenter im EQ-Modus (Hintergrund)
        g.setColour(juce::Colours::cyan.withAlpha(0.6f));
        g.strokePath(spectrumPath, juce::PathStrokeType(1.5f));
    }
    else
    {
        // Voll sichtbar im Spektrum-Modus
        g.setColour(juce::Colours::cyan);
        g.strokePath(spectrumPath, juce::PathStrokeType(2.0f));
    }
}

/**
 * @brief Wendet räumliches Smoothing (Moving Average) auf ein Array an.
 *
 * Berechnet für jeden Wert den Mittelwert der Nachbarwerte
 * innerhalb des angegebenen Fensters.
 *
 * @param levels Die zu glättenden Werte
 * @param windowSize Größe des Glättungsfensters
 * @return Vector mit geglätteten Werten
 */
std::vector<float> AudioPluginAudioProcessorEditor::applySpatialSmoothing(
    const std::vector<float>& levels, int windowSize)
{
    if (levels.empty() || windowSize < 1)
        return levels;

    std::vector<float> smoothed(levels.size());
    int halfWindow = windowSize / 2;

    for (size_t i = 0; i < levels.size(); ++i)
    {
        float sum = 0.0f;
        int count = 0;

        // Mittelwert über Nachbarn berechnen
        for (int j = -halfWindow; j <= halfWindow; ++j)
        {
            int idx = static_cast<int>(i) + j;

            // Nur gültige Indizes berücksichtigen
            if (idx >= 0 && idx < static_cast<int>(levels.size()))
            {
                sum += levels[idx];
                count++;
            }
        }

        smoothed[i] = (count > 0) ? (sum / count) : levels[i];
    }

    return smoothed;
}

//==============================================================================
//                  DRAW EQ CURVE - EQ-KURVE ZEICHNEN
//==============================================================================

/**
 * @brief Zeichnet die kombinierte EQ-Frequenzgang-Kurve.
 *
 * Berechnet und zeichnet den Gesamtfrequenzgang aller aktiven
 * EQ-Bänder sowie die Ziel-Korrekturkurve.
 *
 * @param g Der Graphics-Kontext zum Zeichnen
 */
void AudioPluginAudioProcessorEditor::drawEQCurve(juce::Graphics& g)
{
    const int numPoints = 2000;  // Hohe Auflösung für glatte Kurve
    const float minFreq = 20.0f;
    const float maxFreq = 20000.0f;

    // 1. Logarithmisches Frequenzarray generieren
    auto frequencies = generateLogFrequencies(numPoints, minFreq, maxFreq);

    // 2. Gesamtmagnitude über alle Bänder berechnen
    auto totalMagnitudeDB = calculateTotalMagnitude(frequencies, numPoints);

    // 3. Pfad aus den Daten erstellen
    auto eqPath = buildEQPath(frequencies, totalMagnitudeDB, numPoints, minFreq, maxFreq);

    // 4. Kurve mit Füllung zeichnen
    drawEQPathWithFill(g, eqPath);

    // 5. Ziel-Korrekturkurve überlagern
    drawTargetEQCurve(g);
}

//==============================================================================
//                  DRAW EQ CURVE HILFSFUNKTIONEN
//==============================================================================

/**
 * @brief Generiert ein Array mit logarithmisch verteilten Frequenzen.
 *
 * @param numPoints Anzahl der zu generierenden Frequenzpunkte
 * @param minFreq Minimale Frequenz in Hz
 * @param maxFreq Maximale Frequenz in Hz
 * @return Vector mit logarithmisch verteilten Frequenzen
 */
std::vector<float> AudioPluginAudioProcessorEditor::generateLogFrequencies(
    int numPoints, float minFreq, float maxFreq)
{
    std::vector<float> frequencies;
    frequencies.reserve(numPoints);

    float logMin = std::log10(minFreq);
    float logMax = std::log10(maxFreq);

    // Gleichmäßig im logarithmischen Raum verteilen
    for (int i = 0; i < numPoints; ++i)
    {
        float logFreq = logMin + (logMax - logMin) * i / (numPoints - 1);
        frequencies.push_back(std::pow(10.0f, logFreq));
    }

    return frequencies;
}

/**
 * @brief Berechnet die Gesamtmagnitude aller EQ-Bänder.
 *
 * Summiert die Frequenzgänge aller aktiven EQ-Filter.
 * Filter mit Gain < 0.01 dB werden übersprungen.
 *
 * @param frequencies Array der Frequenzpunkte
 * @param numPoints Anzahl der Frequenzpunkte
 * @return Vector mit Magnitude in dB für jeden Frequenzpunkt
 */
std::vector<float> AudioPluginAudioProcessorEditor::calculateTotalMagnitude(
    const std::vector<float>& frequencies, int numPoints)
{
    std::vector<float> totalMagnitudeDB(numPoints, 0.0f);

    // Samplerate für korrekte Filterberechnung
    float sampleRate = processorRef.getSampleRate();
    if (sampleRate <= 0.0f)
        sampleRate = 48000.0f;  // Fallback

    // Durch alle 31 EQ-Bänder iterieren
    for (int bandIdx = 0; bandIdx < 31; ++bandIdx)
    {
        float f0 = eqFrequencies[bandIdx];
        float gainDb = eqSlider[bandIdx].getValue();
        float Q = eqKnob[bandIdx].getValue();

        // Nur aktive Bänder berechnen (Gain > 0.01 dB)
        if (std::abs(gainDb) > 0.01f)
        {
            // Magnitude für jede Frequenz berechnen
            for (int i = 0; i < numPoints; ++i)
            {
                auto H = peakingEQComplex(frequencies[i], f0, Q, gainDb, sampleRate);
                float magDb = 20.0f * std::log10(std::abs(H));
                totalMagnitudeDB[i] += magDb;  // Additiv in dB (= multiplikativ linear)
            }
        }
    }

    return totalMagnitudeDB;
}

/**
 * @brief Erstellt einen Pfad aus Frequenz- und Magnitude-Daten.
 *
 * @param frequencies Array der Frequenzpunkte
 * @param magnitudeDB Array der Magnitude-Werte in dB
 * @param numPoints Anzahl der Punkte
 * @param minFreq Minimale Frequenz für Skalierung
 * @param maxFreq Maximale Frequenz für Skalierung
 * @return juce::Path Objekt für das Zeichnen
 */
juce::Path AudioPluginAudioProcessorEditor::buildEQPath(
    const std::vector<float>& frequencies,
    const std::vector<float>& magnitudeDB,
    int numPoints, float minFreq, float maxFreq)
{
    juce::Path eqPath;
    bool firstPoint = true;
    auto area = spectrumInnerArea.toFloat();

    for (int i = 0; i < numPoints; ++i)
    {
        float freq = frequencies[i];
        float db = magnitudeDB[i];

        // Frequenz auf X-Position abbilden
        float normX = juce::mapFromLog10(freq, minFreq, maxFreq);
        float x = area.getX() + normX * area.getWidth();

        // dB auf Y-Position abbilden (begrenzt auf ±12 dB)
        float clampedDb = juce::jlimit(-12.0f, 12.0f, db);
        float y = juce::jmap(clampedDb, -12.0f, 12.0f,
            area.getBottom(), area.getY());

        // Pfad aufbauen
        if (firstPoint)
        {
            eqPath.startNewSubPath(x, y);
            firstPoint = false;
        }
        else
        {
            eqPath.lineTo(x, y);
        }
    }

    return eqPath;
}

/**
 * @brief Zeichnet die EQ-Kurve mit 0dB-Linie und Füllung.
 *
 * @param g Der Graphics-Kontext zum Zeichnen
 * @param eqPath Der zu zeichnende Pfad
 */
void AudioPluginAudioProcessorEditor::drawEQPathWithFill(juce::Graphics& g, const juce::Path& eqPath)
{
    auto area = spectrumInnerArea.toFloat();

    // 0 dB Referenzlinie zeichnen
    float y0dB = juce::jmap(0.0f, -12.0f, 12.0f, area.getBottom(), area.getY());
    g.setColour(juce::Colours::white.withAlpha(0.3f));
    g.drawHorizontalLine((int)y0dB, area.getX(), area.getRight());

    // EQ Kurve zeichnen (gelb, 3px)
    g.setColour(juce::Colours::yellow.withAlpha(0.9f));
    g.strokePath(eqPath, juce::PathStrokeType(3.0f));

    // Gefüllten Bereich zwischen Kurve und 0dB-Linie zeichnen
    juce::Path filledPath = eqPath;
    filledPath.lineTo(area.getRight(), y0dB);
    filledPath.lineTo(area.getX(), y0dB);
    filledPath.closeSubPath();

    g.setColour(juce::Colours::yellow.withAlpha(0.15f));
    g.fillPath(filledPath);
}

/**
 * @brief Berechnet die komplexe Übertragungsfunktion eines Peaking-EQ-Filters.
 *
 * Implementiert einen parametrischen EQ-Filter basierend auf dem
 * "Audio EQ Cookbook" von Robert Bristow-Johnson.
 *
 * @param freq Frequenz für die Berechnung in Hz
 * @param f0 Mittenfrequenz des Filters in Hz
 * @param Q Q-Faktor (Güte/Bandbreite) des Filters
 * @param gainDb Verstärkung/Dämpfung in dB
 * @param sampleRate Abtastrate in Hz
 * @return Komplexe Übertragungsfunktion H(f)
 */
std::complex<float> AudioPluginAudioProcessorEditor::peakingEQComplex(
    float freq, float f0, float Q, float gainDb, float sampleRate)
{
    // Gain-Faktor (linear)
    const float A = std::pow(10.0f, gainDb / 40.0f);

    // Normierte Kreisfrequenzen
    const float w0 = juce::MathConstants<float>::twoPi * f0 / sampleRate;
    const float w = juce::MathConstants<float>::twoPi * freq / sampleRate;

    // Bandbreiten-Parameter
    const float alpha = std::sin(w0) / (2.0f * Q);

    // Biquad-Koeffizienten berechnen
    float b0 = 1.0f + alpha * A;
    float b1 = -2.0f * std::cos(w0);
    float b2 = 1.0f - alpha * A;
    float a0 = 1.0f + alpha / A;
    float a1 = -2.0f * std::cos(w0);
    float a2 = 1.0f - alpha / A;

    // Normieren auf a0
    b0 /= a0;
    b1 /= a0;
    b2 /= a0;
    a1 /= a0;
    a2 /= a0;

    // Komplexe Exponentialfunktionen für z^-1 und z^-2
    std::complex<float> expMinusJw(std::cos(-w), std::sin(-w));
    std::complex<float> expMinus2Jw(std::cos(-2.0f * w), std::sin(-2.0f * w));

    // Übertragungsfunktion H(z) = (b0 + b1*z^-1 + b2*z^-2) / (1 + a1*z^-1 + a2*z^-2)
    std::complex<float> num = b0 + b1 * expMinusJw + b2 * expMinus2Jw;
    std::complex<float> den = 1.0f + a1 * expMinusJw + a2 * expMinus2Jw;

    return num / den;
}

//==============================================================================
//                       AUTO-EQ FUNKTIONEN
//==============================================================================

/**
 * @brief Führt die automatische EQ-Berechnung durch.
 *
 * Vergleicht das gemessene Spektrum mit der Referenzkurve und berechnet
 * die notwendigen Korrekturen. Setzt auch den Input-Gain für
 * Lautheitsanpassung.
 */
void AudioPluginAudioProcessorEditor::applyAutoEQ()
{
    auto averagedSpectrum = processorRef.getAveragedSpectrum();

    if (!validateAutoEQData(averagedSpectrum))
        return;

    logAutoEQStart(averagedSpectrum);

    const float offsetDb = computeReferenceViewOffsetDb(averagedSpectrum);

    auto residuals = calculateResidualsAligned(averagedSpectrum, offsetDb);

    auto dbgMinMax = [](const char* name, const std::vector<float>& v)
        {
            if (v.empty()) return;
            auto mm = std::minmax_element(v.begin(), v.end());
            DBG(juce::String(name) + "  min=" + juce::String(*mm.first, 2) +
                "  max=" + juce::String(*mm.second, 2));
        };

    dbgMinMax("raw residuals", residuals);

    // meanOffset sauber raus (50 Hz – 10 kHz)
    const float meanOffset = calculateMeanOffset(residuals);

    // NICHT plattbügeln: nur wenig globalen Offset rausnehmen
    for (auto& r : residuals)
        r -= 0.2f * meanOffset;

    dbgMinMax("after mean removal", residuals);

    // breitbandiges Smoothing (Mastering-tauglich)
    residuals = smoothMovingAverage(residuals, 5, 1); // weniger platt als 7/11
    dbgMinMax("after smoothing", residuals);

    // Amount 30%
    for (auto& r : residuals)
        r *= kAutoEqAmount;

    dbgMinMax("after amount", residuals);

    // in targetCorrections speichern (Clamp macht applyCorrections)
    applyCorrections(residuals, 0.0f);


    processorRef.hasTargetCorrections = true;

    DBG("=== Auto-EQ Berechnung abgeschlossen (Kurve wird angezeigt) ===");
    repaint();
}


//==============================================================================
//                    AUTO-EQ HILFSFUNKTIONEN
//==============================================================================

/**
 * @brief Validiert die Eingabedaten für Auto-EQ.
 *
 * Prüft ob sowohl Messdaten als auch Referenzkurve vorhanden sind.
 *
 * @param spectrum Das zu validierende Spektrum
 * @return true wenn alle Daten vorhanden, false sonst
 */
bool AudioPluginAudioProcessorEditor::validateAutoEQData(
    const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum)
{
    if (spectrum.empty())
    {
        DBG("Keine Messdaten vorhanden!");
        return false;
    }

    if (processorRef.referenceBands.empty())
    {
        DBG("Keine Referenzkurve geladen!");
        return false;
    }

    return true;
}

/**
 * @brief Gibt Debug-Informationen zum Auto-EQ-Start aus.
 *
 * @param spectrum Das Spektrum für Größeninformation
 */
void AudioPluginAudioProcessorEditor::logAutoEQStart(
    const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum)
{
    DBG("=== Auto-EQ Berechnung (Pre-EQ Messung) ===");
    DBG("Anzahl Referenzbänder: " + juce::String(processorRef.referenceBands.size()));
    DBG("Anzahl gemessene Bänder: " + juce::String(spectrum.size()));
}

std::vector<float> AudioPluginAudioProcessorEditor::calculateResidualsAligned(
    const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum,
    float offsetDb)
{
    std::vector<float> residuals;
    residuals.reserve(31);

    for (int i = 0; i < 31; ++i)
    {
        const float freq = eqFrequencies[i];

        const float refLevel = findReferenceLevel(freq);
        float measuredLevel = findMeasuredLevel(freq, spectrum);

        const float gateDb = DisplayScale::minDb + 10.0f;
        if (measuredLevel < gateDb)
            measuredLevel = gateDb;

        measuredLevel += offsetDb;

        const float residual = refLevel - measuredLevel;
        residuals.push_back(residual * edgeWeight(freq));
    }

    return residuals;
}


/**
 * @brief Berechnet den mittleren Offset aller Residuen.
 *
 * Der Mittelwert wird für die globale Lautheitsanpassung verwendet.
 *
 * @param residuals Die berechneten Residuen
 * @return Mittlerer Offset in dB
 */
float AudioPluginAudioProcessorEditor::calculateMeanOffset(const std::vector<float>& residuals)
{
    // Offset-Berechnung: nur sinnvolle Bänder nutzen
    const float fMin = 50.0f;
    const float fMax = 10000.0f;

    float sum = 0.0f;
    int count = 0;

    for (int i = 0; i < 31; ++i)
    {
        const float f = eqFrequencies[i];

        if (f < fMin || f > fMax)
            continue;

        sum += residuals[i];
        ++count;
    }

    float meanOffset = (count > 0) ? (sum / (float)count) : 0.0f;

    // Auf InputGain-Range clampen
    meanOffset = juce::jlimit(-24.0f, 24.0f, meanOffset);

    DBG("Mittlerer Offset (50Hz-10kHz): " + juce::String(meanOffset, 2) + " dB");
    return meanOffset;
}

/**
 * @brief Berechnet und speichert die EQ-Korrekturen.
 *
 * Die Korrekturen werden nach Abzug des mittleren Offsets berechnet
 * und auf ±12 dB begrenzt. Sie werden nur zur Visualisierung gespeichert,
 * die Slider werden nicht automatisch verändert.
 *
 * @param residuals Die berechneten Residuen
 * @param meanOffset Der mittlere Offset für Normalisierung
 */
void AudioPluginAudioProcessorEditor::applyCorrections(
    const std::vector<float>& residuals, float meanOffset)
{
    DBG("=== EQ-Band Korrekturen (nur Visualisierung) ===");

    for (int i = 0; i < 31; ++i)
    {
        float correction = residuals[i];

        correction = juce::jlimit(-kAutoEqMaxCorr, kAutoEqMaxCorr, correction);

        processorRef.targetCorrections[i] = correction;

        DBG("Band " + juce::String(i) + " (" + juce::String(eqFrequencies[i]) + " Hz): "
            + juce::String(correction, 2) + " dB");
    }

}

//==============================================================================
//                     FIND LEVEL FUNKTIONEN
//==============================================================================

/**
 * @brief Findet den Referenz-Level (Median) für eine gegebene Frequenz.
 *
 * Sucht den nächstgelegenen Frequenzpunkt in der Referenzkurve
 * und gibt dessen Median-Wert zurück.
 *
 * @param frequency Die gesuchte Frequenz in Hz
 * @return Der Median-Level in dB, oder 0 wenn keine Referenz vorhanden
 */


float AudioPluginAudioProcessorEditor::findReferenceLevel(float frequency) const
{
    return sampleLogInterpolatedReferenceMedian(processorRef.referenceBands,
        frequency,
        DisplayScale::minDb);
}



/**
 * @brief Findet den gemessenen Level für eine gegebene Frequenz.
 *
 * Sucht den nächstgelegenen Frequenzpunkt im Spektrum
 * und gibt dessen Level zurück.
 *
 * @param frequency Die gesuchte Frequenz in Hz
 * @param spectrum Das Spektrum zum Durchsuchen
 * @return Der Level in dB, oder 0 wenn kein Spektrum vorhanden
 */
float AudioPluginAudioProcessorEditor::findMeasuredLevel(
    float frequency,
    const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum) const
{
    return sampleLogInterpolatedSpectrum(spectrum, frequency, DisplayScale::minDb);
}

//==============================================================================
//                    DRAW TARGET EQ CURVE
//==============================================================================

/**
 * @brief Zeichnet die Ziel-Korrekturkurve.
 *
 * Zeigt die berechneten EQ-Korrekturen als gestrichelte Linie
 * mit Punktmarkierungen an den EQ-Frequenzen.
 *
 * @param g Der Graphics-Kontext zum Zeichnen
 */
void AudioPluginAudioProcessorEditor::drawTargetEQCurve(juce::Graphics& g)
{
    // Nur zeichnen wenn Korrekturen berechnet wurden
    if (!processorRef.hasTargetCorrections)
        return;

    // Pfad erstellen
    auto targetPath = buildTargetPath();

    // Gestrichelte Kurve und Punkte zeichnen
    drawDashedTargetCurve(g, targetPath);
    drawTargetPoints(g);
}

//==============================================================================
//               DRAW TARGET EQ CURVE HILFSFUNKTIONEN
//==============================================================================

/**
 * @brief Erstellt den Pfad für die Ziel-Korrekturkurve.
 *
 * @return juce::Path mit den Korrekturwerten
 */
juce::Path AudioPluginAudioProcessorEditor::buildTargetPath()
{
    auto area = spectrumInnerArea.toFloat();

    const float minFreq = 20.0f;
    const float maxFreq = 20000.0f;
    const float minDb = -12.0f;
    const float maxDb = 12.0f;

    juce::Path targetPath;
    bool firstPoint = true;

    // Pfad durch alle 31 Korrekturpunkte
    for (int i = 0; i < 31; ++i)
    {
        float freq = eqFrequencies[i];
        float correction = processorRef.targetCorrections[i];

        // Koordinaten berechnen
        float normX = juce::mapFromLog10(freq, minFreq, maxFreq);
        float x = area.getX() + normX * area.getWidth();
        float y = juce::jmap(correction, minDb, maxDb, area.getBottom(), area.getY());

        // Pfad aufbauen
        if (firstPoint)
        {
            targetPath.startNewSubPath(x, y);
            firstPoint = false;
        }
        else
        {
            targetPath.lineTo(x, y);
        }
    }

    return targetPath;
}

/**
 * @brief Zeichnet die gestrichelte Zielkurve.
 *
 * @param g Der Graphics-Kontext zum Zeichnen
 * @param targetPath Der zu zeichnende Pfad
 */
void AudioPluginAudioProcessorEditor::drawDashedTargetCurve(
    juce::Graphics& g, const juce::Path& targetPath)
{
    // Gestrichelten Pfad erstellen
    juce::Path dashedPath;
    float dashLengths[] = { 6.0f, 4.0f };  // 6px Linie, 4px Lücke
    juce::PathStrokeType strokeType(2.0f);
    strokeType.createDashedStroke(dashedPath, targetPath, dashLengths, 2);

    // In Lime-Grün zeichnen
    g.setColour(juce::Colours::lime.withAlpha(0.9f));
    g.fillPath(dashedPath);
}

/**
 * @brief Zeichnet Markierungspunkte an den EQ-Frequenzen.
 *
 * @param g Der Graphics-Kontext zum Zeichnen
 */
void AudioPluginAudioProcessorEditor::drawTargetPoints(juce::Graphics& g)
{
    auto area = spectrumInnerArea.toFloat();

    const float minFreq = 20.0f;
    const float maxFreq = 20000.0f;
    const float minDb = -12.0f;
    const float maxDb = 12.0f;

    g.setColour(juce::Colours::lime);

    // Punkt für jede EQ-Frequenz zeichnen
    for (int i = 0; i < 31; ++i)
    {
        float freq = eqFrequencies[i];
        float correction = processorRef.targetCorrections[i];

        // Koordinaten berechnen
        float normX = juce::mapFromLog10(freq, minFreq, maxFreq);
        float x = area.getX() + normX * area.getWidth();
        float y = juce::jmap(correction, minDb, maxDb, area.getBottom(), area.getY());

        // Kreis mit 6px Durchmesser zeichnen
        g.fillEllipse(x - 3.0f, y - 3.0f, 6.0f, 6.0f);
    }
}