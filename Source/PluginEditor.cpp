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
#include <array>
#include <vector>

 // Farben
namespace Theme
{
    static const juce::Colour curveMeasured = juce::Colour(0xff33E0FF); // cyan
    static const juce::Colour curveEQ = juce::Colour(0xffFF4B9A); // hot pink
    static const juce::Colour curveTarget = juce::Colour(0xffFF375F); // neon red/pink

    static const juce::Colour refBandFill = juce::Colour(0xff0FBAC4).withAlpha(0.18f); // teal band fill
    static const juce::Colour refBandEdge = juce::Colour(0xff0FBAC4).withAlpha(0.55f); // p10/p90 outlines
    static const juce::Colour refMedian = juce::Colour(0xffD9F7FF).withAlpha(0.90f); // median line

    static const juce::Colour bgDeep = juce::Colour(0xff101010); // #101010
    static const juce::Colour bgPanel = juce::Colour(0xff161616); // leicht heller
    static const juce::Colour bgPanel2 = juce::Colour(0xff181818); // minimal heller (Knob area)
    static const juce::Colour separator = juce::Colour(0xff262626); // Linien/Kanten

    // ---- Unified Controls ----
    static const juce::Colour controlBg = juce::Colour(0xff1b1d21);                 // Buttons/Combo
    static const juce::Colour controlBgOn = juce::Colour(0xff252a31);                 // Toggle "on"
    static const juce::Colour controlText = juce::Colours::white.withAlpha(0.88f);

    static const juce::Colour disabledBg = juce::Colour(0xff2a2d31);
    static const juce::Colour disabledText = juce::Colours::white.withAlpha(0.35f);

    // Statusfarben Messung
    static const juce::Colour readyGreen = juce::Colour(0xff2ecc71);
    static const juce::Colour recordRed = juce::Colour(0xffe74c3c);
}

namespace
{
    static void applyUnifiedButtonStyle(juce::TextButton& b,
        juce::Colour base,
        bool isToggleButton = false)
    {
        b.setColour(juce::TextButton::buttonColourId, base);

        // Bei Toggle-Buttons wird buttonOnColourId sichtbar, bei normalen Buttons nur wenn "on".
        const auto onCol = isToggleButton ? Theme::controlBgOn : base.brighter(0.10f);
        b.setColour(juce::TextButton::buttonOnColourId, onCol);

        b.setColour(juce::TextButton::textColourOffId, Theme::controlText);
        b.setColour(juce::TextButton::textColourOnId, Theme::controlText);
    }

    static void applyUnifiedComboStyle(juce::ComboBox& cb)
    {
        cb.setColour(juce::ComboBox::backgroundColourId, Theme::controlBg);
        cb.setColour(juce::ComboBox::textColourId, Theme::controlText);
        cb.setColour(juce::ComboBox::outlineColourId, juce::Colours::white.withAlpha(0.12f));
        cb.setColour(juce::ComboBox::arrowColourId, Theme::controlText.withAlpha(0.75f));
    }

    static void setButtonDisabledStyle(juce::TextButton& b, bool disabled)
    {
        if (disabled)
        {
            b.setColour(juce::TextButton::buttonColourId, Theme::disabledBg);
            b.setColour(juce::TextButton::textColourOffId, Theme::disabledText);
        }
        else
        {
            // Text wieder normal
            b.setColour(juce::TextButton::textColourOffId, Theme::controlText);
        }
    }
}

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

    static inline bool isFinite(float x) noexcept
    {
        return std::isfinite(x);
    }

    static inline float finiteOr(float x, float fallback) noexcept
    {
        return std::isfinite(x) ? x : fallback;
    }

    static inline float finiteClamp(float x, float lo, float hi, float fallback = 0.0f) noexcept
    {
        if (!std::isfinite(x)) return fallback;
        return juce::jlimit(lo, hi, x);
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
    referenceAnalysisPool.removeAllJobs(true, 2000);
    autoEqPool.removeAllJobs(true, 2000);
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

    if (!enable)
    {
        // disabled look
        applyUnifiedButtonStyle(genreErkennenButton, Theme::disabledBg);
        setButtonDisabledStyle(genreErkennenButton, true);

        // Falls disabled und gerade gemessen wird -> sauber stoppen
        if (processorRef.isMeasuring())
        {
            processorRef.stopMeasurement();
            genreErkennenButton.setButtonText("Messung starten");
        }
        return;
    }

    // enabled look (abhängig davon ob gerade aufgenommen wird)
    setButtonDisabledStyle(genreErkennenButton, false);

    if (processorRef.isMeasuring())
    {
        genreErkennenButton.setButtonText("Messung stoppen");
        applyUnifiedButtonStyle(genreErkennenButton, Theme::recordRed);
    }
    else
    {
        genreErkennenButton.setButtonText("Messung starten");
        applyUnifiedButtonStyle(genreErkennenButton, Theme::readyGreen);
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
    setSize(1000, 680);
    setResizable(false, false);
}

void AudioPluginAudioProcessorEditor::setupLoadReferenceButton()
{
    loadReferenceButton.setButtonText("Referenz laden");
    applyUnifiedButtonStyle(loadReferenceButton, Theme::controlBg);

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
    applyUnifiedComboStyle(genreBox);
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
    applyUnifiedButtonStyle(genreErkennenButton, Theme::disabledBg);
    setButtonDisabledStyle(genreErkennenButton, true);

    genreErkennenButton.onClick = [this]
        {
            // 1) Wenn gerade gemessen wird -> STOP (ohne Reset!)
            if (processorRef.isMeasuring())
            {
                processorRef.stopMeasurement();

                genreErkennenButton.setButtonText("Messung starten");
                applyUnifiedButtonStyle(genreErkennenButton, Theme::readyGreen);

                // Auto-EQ berechnen wenn Referenzkurve vorhanden
                if (!processorRef.referenceBands.empty())
                    startAutoEqAsync();
                else
                    DBG("Keine Referenzkurve ausgewählt!");

                repaint();
                return;
            }

            // 2) START -> vorher alles zurücksetzen
            processorRef.resetMeasurement();          // Messpuffer, FIFOs, FFT flags, Target Kurve etc.
            processorRef.resetAllBandsToDefault();    // EQ + Q etc.

            showEQCurve = false;
            eqCurveToggleButton.setToggleState(false, juce::dontSendNotification);
            eqCurveToggleButton.setButtonText("EQ Ansicht");

            smoothedLevels.clear();
            referenceViewOffsetDb = 0.0f;
            referenceViewOffsetDbSmoothed = 0.0f;

            processorRef.startMeasurement();

            genreErkennenButton.setButtonText("Messung stoppen");
            applyUnifiedButtonStyle(genreErkennenButton, Theme::recordRed);

            repaint();
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
    resetButton.setColour(juce::TextButton::textColourOffId, Theme::controlText);
    resetButton.setColour(juce::TextButton::textColourOnId, Theme::controlText);

    resetButton.onClick = [this]
        {
            // 1) Falls gerade Messung läuft: sauber stoppen + Button-UI zurück
            if (processorRef.isMeasuring())
                processorRef.stopMeasurement();

            genreErkennenButton.setButtonText("Messung starten");
            genreErkennenButton.setColour(juce::TextButton::buttonColourId, juce::Colours::grey);
            updateMeasurementButtonEnabledState();

            // 2) Processor Runtime reset (Messpuffer, FIFOs, FFT flags, Target Kurve etc.)
            processorRef.resetMeasurement();

            // 3) Parameter zurück (EQ + Q + InputGain)
            processorRef.resetAllBandsToDefault();

            if (auto* p = dynamic_cast<juce::RangedAudioParameter*>(processorRef.apvts.getParameter("inputGain")))
                p->setValueNotifyingHost(p->getDefaultValue());

            // 4) UI-View zurück auf "Spektrum/Referenzansicht"
            showEQCurve = false;
            eqCurveToggleButton.setToggleState(false, juce::dontSendNotification);
            eqCurveToggleButton.setButtonText("EQ Ansicht");

            // 5) Glättungs-/Offset-Zustände zurück (sonst “hängt” Anzeige optisch)
            smoothedLevels.clear();
            referenceViewOffsetDb = 0.0f;
            referenceViewOffsetDbSmoothed = 0.0f;

            // 6) neu zeichnen
            repaint();
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
    applyUnifiedButtonStyle(eqCurveToggleButton, Theme::controlBg, true);


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

        // Popup wie Tooltip (nur beim Drag anzeigen)
        eqSlider[i].setPopupDisplayEnabled(false, true, this);
        eqSlider[i].setNumDecimalPlacesToDisplay(1);
        eqSlider[i].setTextValueSuffix(" dB");
        eqSlider[i].setPopupDisplayEnabled(true, true, this);

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
        eqKnob[i].setPopupDisplayEnabled(false, true, this);
        eqKnob[i].setNumDecimalPlacesToDisplay(2);

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
    drawEQFaderDbScale(g);
    drawEQFaderDbGuideLines(g);
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
    g.setColour(Theme::bgDeep);
    g.fillRect(spectrogramArea);

    g.setColour(Theme::bgDeep);
    g.fillRect(spectrumDisplayArea);

    g.setColour(Theme::bgDeep);
    g.fillRect(spectrumInnerArea);

    // Spektrum/EQ-Kurve im inneren Bereich zeichnen mit Clipping
    {
        juce::Graphics::ScopedSaveState save(g);
        g.reduceClipRegion(spectrumInnerArea);

        if (showEQCurve)
            drawEQDbGridLines(g);

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
    // --- Frame-Linien für Spektrum (oben/unten), damit es "geschlossen" wirkt ---
    {
        const int x1 = spectrumInnerArea.getX();
        const int x2 = spectrumInnerArea.getRight();

        const int yTop = spectrumInnerArea.getY();
        const int yBot = spectrumInnerArea.getBottom();

        g.setColour(juce::Colours::white.withAlpha(0.5f)); // fein, wie TBC

        // obere Linie
        g.drawLine((float)x1, (float)yTop, (float)x2, (float)yTop, 1.0f);

        // untere Linie
        g.drawLine((float)x1, (float)yBot, (float)x2, (float)yBot, 1.0f);
    }

    if (showEQCurve)
        drawEQDbGridLabels(g);
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
    // ==============================
    //   Build Points (P10 / Median / P90)
    // ==============================

    if (processorRef.referenceBands.size() < 2)
        return;

    auto clampDb = [&](float db)
        {
            return juce::jlimit(displayMinDb, displayMaxDb, db);
        };

    std::vector<juce::Point<float>> p10Pts;
    std::vector<juce::Point<float>> p90Pts;
    std::vector<juce::Point<float>> medPts;

    p10Pts.reserve(processorRef.referenceBands.size());
    p90Pts.reserve(processorRef.referenceBands.size());
    medPts.reserve(processorRef.referenceBands.size());

    for (const auto& band : processorRef.referenceBands)
    {
        // ------------------------------
        // Skip out-of-range bands
        // ------------------------------
        if (band.freq < minFreq || band.freq > maxFreq)
            continue;

        // ------------------------------
        // X: log-frequency mapping
        // ------------------------------
        const float normX = juce::mapFromLog10(band.freq, minFreq, maxFreq);
        const float x = (float)spectrumInnerArea.getX() + normX * (float)spectrumInnerArea.getWidth();

        // ------------------------------
        // Y: dB mapping (top = louder)
        // ------------------------------
        const float yP10 = juce::jmap(clampDb(band.p10), displayMinDb, displayMaxDb,
            (float)spectrumInnerArea.getBottom(),
            (float)spectrumInnerArea.getY());

        const float yMed = juce::jmap(clampDb(band.median), displayMinDb, displayMaxDb,
            (float)spectrumInnerArea.getBottom(),
            (float)spectrumInnerArea.getY());

        const float yP90 = juce::jmap(clampDb(band.p90), displayMinDb, displayMaxDb,
            (float)spectrumInnerArea.getBottom(),
            (float)spectrumInnerArea.getY());

        p10Pts.push_back({ x, yP10 });
        medPts.push_back({ x, yMed });
        p90Pts.push_back({ x, yP90 });
    }

    if (p10Pts.size() < 2 || p90Pts.size() < 2 || medPts.size() < 2)
        return;

    auto buildLinePath = [](const std::vector<juce::Point<float>>& pts)
        {
            juce::Path p;
            p.startNewSubPath(pts[0]);

            for (size_t i = 1; i < pts.size(); ++i)
                p.lineTo(pts[i]);

            return p;
        };

    // ==============================
    //   Build Filled Band (P10..P90)
    // ==============================

    juce::Path bandPath;

    bandPath.startNewSubPath(p90Pts[0]);
    for (size_t i = 1; i < p90Pts.size(); ++i)
        bandPath.lineTo(p90Pts[i]);

    for (size_t i = p10Pts.size(); i-- > 0; )
        bandPath.lineTo(p10Pts[i]);

    bandPath.closeSubPath();

    // ------------------------------
    // Fill band (TBC style)
    // ------------------------------
    g.setColour(Theme::refBandFill);
    g.fillPath(bandPath);

    // ==============================
    //   Optional: Outlines (P10 / P90)
    // ==============================

    const auto p10Path = buildLinePath(p10Pts);
    const auto p90Path = buildLinePath(p90Pts);
    const auto medPath = buildLinePath(medPts);

    g.setColour(Theme::refBandEdge);
    g.strokePath(p10Path, juce::PathStrokeType(1.25f));
    g.strokePath(p90Path, juce::PathStrokeType(1.25f));

    // ==============================
    //   Median Line (on top)
    // ==============================

    g.setColour(Theme::refMedian);
    g.strokePath(medPath, juce::PathStrokeType(2.0f));
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

void AudioPluginAudioProcessorEditor::drawEQDbGridLines(juce::Graphics& g)
{
    // Nur im EQ-View sinnvoll
    if (!showEQCurve)
        return;

    auto area = spectrumInnerArea.toFloat();

    const float minDb = -12.0f;
    const float maxDb = 12.0f;

    // Linien bei ±2,4,6,8,10 dB
    const int ticks[] = { -10, -8, -6, -4, -2, 2, 4, 6, 8, 10 };

    g.setColour(juce::Colours::white.withAlpha(0.12f)); // subtil

    for (int db : ticks)
    {
        const float y = juce::jmap((float)db, minDb, maxDb, area.getBottom(), area.getY());
        g.drawHorizontalLine((int)std::round(y), area.getX(), area.getRight());
    }

    // 0 dB Linie kannst du hier optional noch etwas betonen,
    // oder du lässt sie weiterhin aus drawEQPathWithFill() kommen.
    // (Wenn du sie hier zeichnest, dann bitte NICHT doppelt.)
}

void AudioPluginAudioProcessorEditor::drawEQDbGridLabels(juce::Graphics& g)
{
    if (!showEQCurve)
        return;

    auto inner = spectrumInnerArea.toFloat();
    auto display = spectrumDisplayArea.toFloat();

    const float minDb = -12.0f;
    const float maxDb = 12.0f;

    // Links/rechts: in die "sichtbaren Ränder" neben dem inneren Bereich
    const float leftBandX0 = display.getX();
    const float leftBandX1 = inner.getX();

    const float rightBandX0 = inner.getRight();
    const float rightBandX1 = display.getRight();

    // Falls die Ränder extrem schmal sind: trotzdem nicht crashen/zeichnen
    if ((leftBandX1 - leftBandX0) < 10.0f || (rightBandX1 - rightBandX0) < 10.0f)
        return;

    const float labelW = 36.0f;
    const float labelH = 16.0f;

    const float leftLabelX = (leftBandX0 + leftBandX1) * 0.5f - labelW * 0.5f;
    const float rightLabelX = (rightBandX0 + rightBandX1) * 0.5f - labelW * 0.5f;

    g.setColour(juce::Colours::white.withAlpha(0.5f));
    g.setFont(12.0f);

    auto drawLabel = [&](float x, float y, const juce::String& text)
        {
            juce::Rectangle<float> r(x, y - labelH * 0.5f, labelW, labelH);
            g.drawFittedText(text, r.toNearestInt(), juce::Justification::centred, 1);
        };

    // Beschriftungen (bei 0 soll "dB" stehen)
    const int ticks[] = { 10, 8, 6, 4, 2, 0, -2, -4, -6, -8, -10 };

    for (int db : ticks)
    {
        const float y = juce::jmap((float)db, minDb, maxDb, inner.getBottom(), inner.getY());
        const juce::String text = (db == 0) ? "dB" : juce::String(db);

        drawLabel(leftLabelX, y, text);
        drawLabel(rightLabelX, y, text);
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
    g.setColour(Theme::bgPanel);
    g.fillRect(eqArea);

    // Q-Knob-Bereich (hellgrün)
    g.setColour(Theme::bgPanel);
    g.fillRect(eqKnobArea);

    // EQ Beschriftungsbereich (rot)
    g.setColour(Theme::bgPanel);
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

void AudioPluginAudioProcessorEditor::drawEQFaderDbScale(juce::Graphics& g)
{
    // Wir brauchen Slider-Bounds (also nach resized() vorhanden)
    const int leftIdx = 0;
    const int rightIdx = 30;

    const auto& sL = eqSlider[leftIdx];
    const auto& sR = eqSlider[rightIdx];

    if (sL.getWidth() <= 0 || sR.getWidth() <= 0)
        return;

    // Werte, die du willst: +12, +6, "dB" bei 0, -6, -12
    struct Tick { float db; const char* text; };
    const Tick ticks[] =
    {
        {  12.0f, "12" },
        {   6.0f, "6"  },
        {   0.0f, "dB" },
        {  -6.0f, "-6" },
        { -12.0f, "-12"}
    };

    // Y-Position exakt aus Slider-Skala (lokal -> global)
    auto yForDb = [](const juce::Slider& s, float db)
        {
            const double localPos = s.getPositionOfValue(db); // Pixel im Slider (local)
            return (float)s.getY() + (float)localPos;
        };

    // Layout der Textboxen links/rechts
    const int labelW = 34;
    const int labelH = 16;
    const int pad = 6;

    int leftX = sL.getX() - pad - labelW;
    int rightX = sR.getRight() + pad;

    // falls links zu knapp ist, clampen
    leftX = juce::jmax(0, leftX);
    rightX = juce::jmin(getWidth() - labelW, rightX);

    g.setColour(juce::Colours::white.withAlpha(0.5f));
    g.setFont(12.0f);

    for (const auto& t : ticks)
    {
        const float yL = yForDb(sL, t.db);
        const float yR = yForDb(sR, t.db);

        g.drawFittedText(t.text,
            leftX, (int)std::round(yL - labelH * 0.5f),
            labelW, labelH,
            juce::Justification::centred, 1);

        g.drawFittedText(t.text,
            rightX, (int)std::round(yR - labelH * 0.5f),
            labelW, labelH,
            juce::Justification::centred, 1);
    }
}

void AudioPluginAudioProcessorEditor::drawEQFaderDbGuideLines(juce::Graphics& g)
{
    const int leftIdx = 0;
    const int rightIdx = 30;

    const auto& sL = eqSlider[leftIdx];
    const auto& sR = eqSlider[rightIdx];

    if (sL.getWidth() <= 0 || sR.getWidth() <= 0)
        return;

    struct Tick { float db; };
    const Tick ticks[] =
    {
        {  12.0f },
        {   6.0f },
        {   0.0f }, // "dB" / Null-Linie
        {  -6.0f },
        { -12.0f }
    };

    auto yForDb = [](const juce::Slider& s, float db)
        {
            const double localPos = s.getPositionOfValue(db);
            float y = (float)s.getY() + (float)localPos;

            // Safety: im Sliderbereich halten
            const float top = (float)s.getY() + 1.0f;
            const float bot = (float)s.getBottom() - 1.0f;
            return juce::jlimit(top, bot, y);
        };

    // Linien nur im Slider-Bereich (eqArea) zeichnen
    juce::Graphics::ScopedSaveState ss(g);
    g.reduceClipRegion(eqArea);

    g.setColour(juce::Colours::white.withAlpha(0.5f));
    g.setFont(12.0f);

    const float xMin = (float)eqArea.getX();
    const float xMax = (float)eqArea.getRight();

    // Wie viel Abstand um jeden Fader ausgespart werden soll
    const int gapPad = 3;

    // Für jede Linie: wir bauen Ausschlussbereiche (x-ranges) um jeden Slider
    for (const auto& t : ticks)
    {
        const float y = yForDb(sL, t.db);

        // Sammle alle "Lücken" (exclusion ranges) entlang X
        struct Range { float a, b; };
        std::vector<Range> gaps;
        gaps.reserve(31);

        for (int i = 0; i < 31; ++i)
        {
            auto r = eqSlider[i].getBounds().expanded(gapPad, 0);

            float a = (float)r.getX();
            float b = (float)r.getRight();

            // clamp in unseren Zeichenbereich
            a = juce::jlimit(xMin, xMax, a);
            b = juce::jlimit(xMin, xMax, b);

            if (b > a)
                gaps.push_back({ a, b });
        }

        // sort + merge (damit keine Doppel-Lücken entstehen)
        std::sort(gaps.begin(), gaps.end(), [](const Range& r1, const Range& r2) { return r1.a < r2.a; });

        std::vector<Range> merged;
        for (const auto& r : gaps)
        {
            if (merged.empty() || r.a > merged.back().b)
                merged.push_back(r);
            else
                merged.back().b = std::max(merged.back().b, r.b);
        }

        // Wenn wir mind. links+rechts einen Gap haben, begrenzen wir den Zeichenbereich:
// Start = Ende des ersten (linken) Gaps, Ende = Anfang des letzten (rechten) Gaps.
// Dadurch gibt es KEINE Linie zwischen Rand und erstem/letztem Fader.
        if (merged.size() < 2)
            continue;

        const float drawStart = merged.front().b; // nach linkem Fader
        const float drawEnd = merged.back().a;  // vor rechtem Fader

        if (drawEnd <= drawStart + 1.0f)
            continue;

        float curX = drawStart;

        for (const auto& m : merged)
        {
            // Alles links vom drawStart ignorieren
            if (m.b <= drawStart)
                continue;

            // Wenn der nächste Gap schon rechts von drawEnd liegt -> fertig
            if (m.a >= drawEnd)
                break;

            const float segEnd = juce::jlimit(drawStart, drawEnd, m.a);

            if (segEnd > curX + 1.0f)
                g.drawLine(curX, y, segEnd, y, 1.0f);

            curX = std::max(curX, m.b);
        }

        // Letztes Segment nur bis drawEnd (NICHT bis zum Rand)
        if (drawEnd > curX + 1.0f)
            g.drawLine(curX, y, drawEnd, y, 1.0f);

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
    genreErkennenButton.setBounds(10, 5, 140, 30);
    loadReferenceButton.setBounds(560, 5, 140, 30);
    eqCurveToggleButton.setBounds(160, 5, 140, 30);
    genreBox.setBounds(710, 5, 220, 30);
    resetButton.setBounds(940, 5, 50, 30);
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
        // Bass bekommt viel mehr zeitliche Glättung (ruhiger), Höhen dürfen schneller reagieren
        float a = smoothingFactor;
        if (point.frequency < 150.0f) a = 0.96f;
        if (point.frequency < 80.0f) a = 0.98f;
        if (point.frequency < 40.0f) a = 0.985f;

        smoothedLevels[i] = smoothedLevels[i] * a + point.level * (1.0f - a);


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
    auto smoothedY = applySpatialSmoothing(yValues, 3);

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
        g.setColour(Theme::curveMeasured.withAlpha(0.55f));
        g.strokePath(spectrumPath, juce::PathStrokeType(1.5f));
    }
    else
    {
        g.setColour(Theme::curveMeasured.withAlpha(0.95f));
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
    const int numPoints = 2000;
    const float minFreq = 20.0f;

    float sr = (float)processorRef.getSampleRate();
    if (!(sr > 0.0f)) sr = 48000.0f;

    const float maxUsable = 0.5f * sr * 0.999f;
    const float maxFreqDraw = juce::jmin(20000.0f, maxUsable);

    // WICHTIG: genau dieses Array später weiterverwenden
    auto frequencies = generateLogFrequencies(numPoints, minFreq, maxFreqDraw);

    auto totalMagnitudeDB = calculateTotalMagnitude(frequencies, numPoints);

    auto eqPath = buildEQPath(frequencies, totalMagnitudeDB, numPoints, minFreq, maxFreqDraw);

    drawEQPathWithFill(g, eqPath);
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
    g.setColour(Theme::curveEQ.withAlpha(0.92f));
    g.strokePath(eqPath, juce::PathStrokeType(3.0f));

    juce::Path filledPath = eqPath;
    filledPath.lineTo(area.getRight(), y0dB);
    filledPath.lineTo(area.getX(), y0dB);
    filledPath.closeSubPath();

    g.setColour(Theme::curveEQ.withAlpha(0.14f));
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
//                              Gains-Fit
//==============================================================================

namespace
{
    // Log-Interpolation: 31 Bandpunkte -> Zielkurve auf beliebigen Frequenzen
    static float interpLogCurveDb(const std::vector<float>& bandFreqs,
        const std::vector<float>& bandDb,
        float fHz)
    {
        jassert(bandFreqs.size() == bandDb.size());
        if (bandFreqs.empty()) return 0.0f;

        if (fHz <= bandFreqs.front()) return bandDb.front();
        if (fHz >= bandFreqs.back())  return bandDb.back();

        const float lf = std::log10(fHz);

        for (size_t i = 1; i < bandFreqs.size(); ++i)
        {
            if (bandFreqs[i] >= fHz)
            {
                const float f0 = bandFreqs[i - 1];
                const float f1 = bandFreqs[i];
                const float l0 = std::log10(f0);
                const float l1 = std::log10(f1);

                const float t = (lf - l0) / (l1 - l0);
                return bandDb[i - 1] + t * (bandDb[i] - bandDb[i - 1]);
            }
        }

        return bandDb.back();
    }

    // Berechnet EQ-Response in dB (Summe der log-Magnitudes) für gegebene Gains+Qs
    static void computeEQResponseDb(const std::vector<float>& freqs,
        const std::array<float, 31>& gainsDb,
        const std::array<float, 31>& Qs,
        float sampleRate,
        std::vector<float>& outDb,
        const std::vector<float>& eqFreqs)
    {
        outDb.assign(freqs.size(), 0.0f);

        float sr = sampleRate;
        if (!(sr > 0.0f))
            sr = 48000.0f;

        const float nyq = 0.5f * sr;
        const float maxUsableHz = nyq * 0.999f; // Sicherheitsabstand zu Nyquist

        for (size_t k = 0; k < freqs.size(); ++k)
        {
            const float fIn = freqs[k];
            const float f = juce::jlimit(20.0f, maxUsableHz, fIn);

            double sumDb = 0.0;

            for (int i = 0; i < 31; ++i)
            {
                const float gRaw = gainsDb[(size_t)i];
                if (std::abs(gRaw) <= 0.0001f)
                    continue;

                float f0 = eqFreqs[(size_t)i];
                f0 = juce::jlimit(20.0f, maxUsableHz, f0);

                float Q = Qs[(size_t)i];
                Q = juce::jmax(0.001f, Q);

                // Gain clamp + finite
                const float g = finiteClamp(gRaw, -12.0f, 12.0f, 0.0f);

                const float A = std::pow(10.0f, g / 40.0f);

                const float w0 = juce::MathConstants<float>::twoPi * f0 / sr;
                const float w = juce::MathConstants<float>::twoPi * f / sr;

                const float alpha = std::sin(w0) / (2.0f * Q);

                float b0 = 1.0f + alpha * A;
                float b1 = -2.0f * std::cos(w0);
                float b2 = 1.0f - alpha * A;

                float a0 = 1.0f + alpha / A;
                float a1 = -2.0f * std::cos(w0);
                float a2 = 1.0f - alpha / A;

                // Normieren auf a0
                b0 /= a0; b1 /= a0; b2 /= a0;
                a1 /= a0; a2 /= a0;

                // H(e^jw) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2), z^-1 = e^{-jw}
                const std::complex<float> z1(std::cos(-w), std::sin(-w));
                const std::complex<float> z2(std::cos(-2.0f * w), std::sin(-2.0f * w));

                const std::complex<float> num = b0 + b1 * z1 + b2 * z2;
                const std::complex<float> den = 1.0f + a1 * z1 + a2 * z2;

                const float denMag = std::abs(den);
                if (!std::isfinite(denMag) || denMag < 1.0e-12f)
                    continue; // Beitrag überspringen statt NaN zu erzeugen

                float mag = std::abs(num / den);
                if (!std::isfinite(mag)) mag = 1.0f;
                mag = std::max(1.0e-8f, mag);

                float magDb = 20.0f * std::log10(mag);
                if (!std::isfinite(magDb)) magDb = 0.0f;

                sumDb += (double)magDb;
            }

            outDb[k] = (float)sumDb;
        }
    }

    static juce::Path buildResponsePath(
        const juce::Rectangle<float>& area,
        const std::vector<float>& freqs,
        const std::vector<float>& respDb,
        float minFreq, float maxFreq,
        float minDb, float maxDb)
    {
        juce::Path p;
        bool first = true;

        for (size_t i = 0; i < freqs.size() && i < respDb.size(); ++i)
        {
            const float f = freqs[i];
            const float db = juce::jlimit(minDb, maxDb, respDb[i]);

            const float x = area.getX() + juce::mapFromLog10(f, minFreq, maxFreq) * area.getWidth();
            const float y = juce::jmap(db, minDb, maxDb, area.getBottom(), area.getY());

            if (first) { p.startNewSubPath(x, y); first = false; }
            else { p.lineTo(x, y); }
        }
        return p;
    }

    static float computeMakeupGainDbFromEQ(const std::vector<float>& freqs,
        const std::array<float, 31>& gainsDb,
        const std::array<float, 31>& Qs,
        float sampleRate,
        const std::vector<float>& eqFreqs)
    {
        std::vector<float> respDb;
        computeEQResponseDb(freqs, gainsDb, Qs, sampleRate, respDb, eqFreqs);

        // Wir mitteln im "relevanten" Bereich (Bass nicht überbewerten)
        const float fMin = 60.0f;
        const float fMax = 12000.0f;

        double sumW = 0.0;
        double sumPow = 0.0;

        for (size_t k = 0; k < freqs.size(); ++k)
        {
            const float f = freqs[k];
            if (f < fMin || f > fMax) continue;

            // Gewicht: Bass etwas weniger, Mitten normal
            double w = 1.0;
            if (f < 120.0f) w *= 0.6;      // Bass weniger stark zählen
            if (f > 8000.0f) w *= 0.8;     // Air weniger stark zählen

            const double db = (double)respDb[k];

            // respDb = 20log10(|H|) -> Power-Gain = |H|^2 = 10^(db/10)
            const double powGain = std::pow(10.0, db / 10.0);

            sumPow += w * powGain;
            sumW += w;
        }

        if (sumW <= 1.0e-12) return 0.0f;

        const double meanPow = sumPow / sumW;

        // Makeup so, dass mittlere Power wieder ~1 wird
        const double makeupDb = -10.0 * std::log10(std::max(1.0e-12, meanPow));
        return (float)makeupDb;
    }

    // Löser für symmetrisches, positiv definites LGS (Cholesky) – n ist klein (31)
    static bool solveSPD_Cholesky(std::vector<double>& A, std::vector<double>& b, int n)
    {
        // A ist n*n in row-major
        // Cholesky: A = L*L^T (wir speichern L in A)
        for (int i = 0; i < n; ++i)
        {
            for (int j = 0; j <= i; ++j)
            {
                double sum = A[(size_t)i * n + j];

                for (int k = 0; k < j; ++k)
                    sum -= A[(size_t)i * n + k] * A[(size_t)j * n + k];

                if (i == j)
                {
                    if (sum <= 1.0e-12)
                        return false;

                    A[(size_t)i * n + j] = std::sqrt(sum);
                }
                else
                {
                    A[(size_t)i * n + j] = sum / A[(size_t)j * n + j];
                }
            }

            // oberen Teil nicht nötig
            for (int j = i + 1; j < n; ++j)
                A[(size_t)i * n + j] = 0.0;
        }

        // Forward solve: L*y = b
        std::vector<double> y((size_t)n, 0.0);
        for (int i = 0; i < n; ++i)
        {
            double sum = b[(size_t)i];
            for (int k = 0; k < i; ++k)
                sum -= A[(size_t)i * n + k] * y[(size_t)k];

            y[(size_t)i] = sum / A[(size_t)i * n + i];
        }

        // Backward solve: L^T*x = y  (x wird in b zurückgeschrieben)
        for (int i = n - 1; i >= 0; --i)
        {
            double sum = y[(size_t)i];
            for (int k = i + 1; k < n; ++k)
                sum -= A[(size_t)k * n + i] * b[(size_t)k];

            b[(size_t)i] = sum / A[(size_t)i * n + i];
        }

        return true;
    }

    static double computeLossWithSmoothness(const std::vector<float>& freqs,
        const std::vector<float>& targetDb,
        const std::array<float, 31>& gainsDb,
        const std::array<float, 31>& Qs,
        float sampleRate,
        const std::vector<float>& eqFreqs,
        double lambdaSmooth,
        double lambdaQ,
        const std::array<float, 31>& Q0)
    {
        std::vector<float> respDb;
        computeEQResponseDb(freqs, gainsDb, Qs, sampleRate, respDb, eqFreqs);

        // 1) Fit error
        double fit = 0.0;
        for (size_t k = 0; k < freqs.size(); ++k)
        {
            const double e = (double)respDb[k] - (double)targetDb[k];
            fit += e * e;
        }

        // 2) Smoothness (2nd derivative penalty) -> reduziert "Kammfilter/Ripple"
        double smooth = 0.0;
        if (respDb.size() >= 3)
        {
            for (size_t k = 1; k + 1 < respDb.size(); ++k)
            {
                const double d2 = (double)respDb[k + 1] - 2.0 * (double)respDb[k] + (double)respDb[k - 1];
                smooth += d2 * d2;
            }
        }

        // 3) Q regularization (log-domain, damit Multiplikativänderungen sinnvoll sind)
        double qpen = 0.0;
        for (int i = 0; i < 31; ++i)
        {
            const double q = juce::jmax(0.3, (double)Qs[(size_t)i]);
            const double q0 = juce::jmax(0.3, (double)Q0[(size_t)i]);
            const double t = std::log(q / q0);
            qpen += t * t;
        }

        return fit + lambdaSmooth * smooth + lambdaQ * qpen;
    }

    static std::array<float, 31> fitQsStage2_Coordinate(const std::vector<float>& freqs,
        const std::vector<float>& targetDb,
        const std::array<float, 31>& gainsDbFixed,
        std::array<float, 31> Qs,
        float sampleRate,
        const std::vector<float>& eqFreqs)
    {
        // Defaults / Gewichte: praxisnah starten
        const double lambdaSmooth = 0.25;  // höher => glatter, weniger Ripple
        const double lambdaQ = 0.05;  // höher => Q bleibt näher am Default

        std::array<float, 31> Q0 = Qs;      // "aktueller" Ausgangspunkt als Default (oder 4.32 überall)

        // Kandidatenfaktoren (multiplikativ)
        const float factors[] = { 0.70f, 0.85f, 1.0f, 1.18f, 1.35f };

        double bestLoss = computeLossWithSmoothness(freqs, targetDb, gainsDbFixed, Qs, sampleRate, eqFreqs,
            lambdaSmooth, lambdaQ, Q0);

        constexpr int iters = 4; // 3..6 reicht oft

        for (int iter = 0; iter < iters; ++iter)
        {
            bool anyImproved = false;

            for (int i = 0; i < 31; ++i)
            {
                const float qCur = Qs[(size_t)i];

                float bestQ = qCur;
                double localBest = bestLoss;

                for (float fac : factors)
                {
                    float qTry = juce::jlimit(0.3f, 10.0f, qCur * fac);

                    auto QtryArr = Qs;
                    QtryArr[(size_t)i] = qTry;

                    const double L = computeLossWithSmoothness(freqs, targetDb, gainsDbFixed, QtryArr, sampleRate, eqFreqs,
                        lambdaSmooth, lambdaQ, Q0);

                    if (L < localBest)
                    {
                        localBest = L;
                        bestQ = qTry;
                    }
                }

                if (bestQ != qCur)
                {
                    Qs[(size_t)i] = bestQ;
                    bestLoss = localBest;
                    anyImproved = true;
                }
            }

            if (!anyImproved)
                break;
        }

        return Qs;
    }

    // Stufe 1 Fit: Gauss-Newton nur für Gains, Q fix
    static std::array<float, 31> fitGainsStage1(const std::vector<float>& freqs,
        const std::vector<float>& targetDb,
        const std::array<float, 31>& Qs,
        float sampleRate,
        const std::vector<float>& eqFreqs,
        const std::array<double, 31>* extraDiagPenalty = nullptr)

    {
        std::array<float, 31> gains{};
        gains.fill(0.0f);

        const int N = (int)freqs.size();
        const int M = 31;

        std::vector<float> curDb, plusDb, minusDb;
        std::vector<double> r((size_t)N, 0.0);

        constexpr float deltaDb = 0.25f;     // finite difference step
        constexpr int iters = 8;             // 6–10 ist meist genug
        constexpr double damping = 1e-2;     // stabilisiert (Tikhonov)

        for (int iter = 0; iter < iters; ++iter)
        {
            // current response
            computeEQResponseDb(freqs, gains, Qs, sampleRate, curDb, eqFreqs);

            // residual r = target - current
            for (int k = 0; k < N; ++k)
                r[(size_t)k] = (double)targetDb[(size_t)k] - (double)curDb[(size_t)k];

            // Normal equations: (J^T J + λI) * dg = J^T r
            std::vector<double> AtA((size_t)M * M, 0.0);
            std::vector<double> Atb((size_t)M, 0.0);

            // Für jede Spalte i: Jacobian per finite diff
            for (int i = 0; i < M; ++i)
            {
                auto gPlus = gains;
                auto gMinus = gains;
                gPlus[(size_t)i] = juce::jlimit(-12.0f, 12.0f, gPlus[(size_t)i] + deltaDb);
                gMinus[(size_t)i] = juce::jlimit(-12.0f, 12.0f, gMinus[(size_t)i] - deltaDb);

                computeEQResponseDb(freqs, gPlus, Qs, sampleRate, plusDb, eqFreqs);
                computeEQResponseDb(freqs, gMinus, Qs, sampleRate, minusDb, eqFreqs);

                // Ji[k] = d(curDb)/d(gain_i)
                // Wir bauen AtA und Atb inkrementell
                std::vector<double> Ji((size_t)N, 0.0);
                const double denom = 1.0 / (2.0 * (double)deltaDb);

                for (int k = 0; k < N; ++k)
                    Ji[(size_t)k] = ((double)plusDb[(size_t)k] - (double)minusDb[(size_t)k]) * denom;

                // Atb[i] += Σ Ji[k] * r[k]
                double sumAtb = 0.0;
                for (int k = 0; k < N; ++k)
                    sumAtb += Ji[(size_t)k] * r[(size_t)k];
                Atb[(size_t)i] = sumAtb;

                // AtA[i,j] += Σ Ji[k]*Jj[k]
                // -> wir brauchen Jj; für M=31 geht’s so:
                for (int j = 0; j <= i; ++j)
                {
                    // Jj per finite diff erneut zu rechnen wäre teuer.
                    // Trick: wir berechnen AtA "spaltenweise" nicht optimal.
                    // Für Stufe 1 reicht aber Performance; M ist klein.
                    // => Wir rechnen Jj ebenfalls kurz aus.
                    auto gPlus2 = gains;
                    auto gMinus2 = gains;
                    gPlus2[(size_t)j] = juce::jlimit(-12.0f, 12.0f, gPlus2[(size_t)j] + deltaDb);
                    gMinus2[(size_t)j] = juce::jlimit(-12.0f, 12.0f, gMinus2[(size_t)j] - deltaDb);

                    std::vector<float> plusDb2, minusDb2;
                    computeEQResponseDb(freqs, gPlus2, Qs, sampleRate, plusDb2, eqFreqs);
                    computeEQResponseDb(freqs, gMinus2, Qs, sampleRate, minusDb2, eqFreqs);

                    double sum = 0.0;
                    for (int k = 0; k < N; ++k)
                    {
                        const double Jj = ((double)plusDb2[(size_t)k] - (double)minusDb2[(size_t)k]) * denom;
                        sum += Ji[(size_t)k] * Jj;
                    }

                    AtA[(size_t)i * M + j] += sum;
                    AtA[(size_t)j * M + i] += sum; // symmetrisch
                }
            }

            // Damping auf Diagonale
            for (int i = 0; i < M; ++i)
                AtA[(size_t)i * M + i] += damping;

            if (extraDiagPenalty != nullptr)
            {
                for (int i = 0; i < M; ++i)
                    AtA[(size_t)i * M + i] += (*extraDiagPenalty)[(size_t)i];
            }

            // --- Gain Smoothness Regularization (verhindert Ripple/Kammfilter in der EQ-Kurve) ---
            // Minimiert Sum (g[i] - g[i-1])^2
            constexpr double lambdaGainSmooth = 0.35; // 0.15 .. 1.0 (höher = glatter)

            for (int i = 0; i < M; ++i)
            {
                double diag = 0.0;

                if (i > 0)
                {
                    diag += lambdaGainSmooth;
                    AtA[(size_t)i * M + (i - 1)] -= lambdaGainSmooth;
                    AtA[(size_t)(i - 1) * M + i] -= lambdaGainSmooth;
                }

                if (i < M - 1)
                {
                    diag += lambdaGainSmooth;
                    // (i,i+1) kommt implizit über den i>0-Teil beim nächsten Index rein
                }

                AtA[(size_t)i * M + i] += diag;
            }

            // Solve
            auto Awork = AtA;
            auto bwork = Atb;

            if (!solveSPD_Cholesky(Awork, bwork, M))
                break;

            // Update gains
            float maxStep = 0.0f;
            for (int i = 0; i < M; ++i)
            {
                const float step = (float)bwork[(size_t)i];
                maxStep = std::max(maxStep, std::abs(step));

                float stepF = finiteOr((float)bwork[(size_t)i], 0.0f);

                // Optional: Step begrenzen (stabilisiert den Solver extrem)
                stepF = juce::jlimit(-3.0f, 3.0f, stepF);

                const float newG = gains[(size_t)i] + stepF;
                gains[(size_t)i] = finiteClamp(newG, -12.0f, 12.0f, 0.0f);
            }

            // Abbruch wenn kaum Änderung
            if (maxStep < 0.02f)
                break;
        }

        return gains;
    }
}

static void applyGainsToApvts(AudioPluginAudioProcessor& proc,
    const std::array<float, 31>& gainsDb)
{
    for (int i = 0; i < 31; ++i)
    {
        const juce::String paramID = "band" + juce::String(i);

        if (auto* p = dynamic_cast<juce::RangedAudioParameter*>(proc.apvts.getParameter(paramID)))
        {
            const float clamped = juce::jlimit(-12.0f, 12.0f, gainsDb[(size_t)i]);

            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1(clamped)); // <-- wichtig (Host/Automation-safe)
            p->endChangeGesture();
        }
    }
}

static void applyQsToApvts(AudioPluginAudioProcessor& proc,
    const std::array<float, 31>& Qs)
{
    for (int i = 0; i < 31; ++i)
    {
        const juce::String paramID = "bandQ" + juce::String(i);

        if (auto* p = dynamic_cast<juce::RangedAudioParameter*>(proc.apvts.getParameter(paramID)))
        {
            const float clamped = juce::jlimit(0.3f, 10.0f, Qs[(size_t)i]);

            p->beginChangeGesture();
            p->setValueNotifyingHost(p->convertTo0to1(clamped));
            p->endChangeGesture();
        }
    }
}

static void applyInputGainToApvts(AudioPluginAudioProcessor& proc, float gainDb)
{
    if (auto* p = dynamic_cast<juce::RangedAudioParameter*>(proc.apvts.getParameter("inputGain")))
    {
        const float clamped = juce::jlimit(-24.0f, 24.0f, gainDb);
        p->beginChangeGesture();
        p->setValueNotifyingHost(p->convertTo0to1(clamped));
        p->endChangeGesture();
    }
}

void AudioPluginAudioProcessorEditor::startAutoEqAsync()
{
    // Schon am laufen? -> nichts tun
    if (autoEqRunning.exchange(true))
        return;

    // UI "busy" (optional, aber hilfreich)
    genreErkennenButton.setEnabled(false);
    genreErkennenButton.setButtonText("Berechne...");
    loadReferenceButton.setEnabled(false);
    resetButton.setEnabled(false);
    eqCurveToggleButton.setEnabled(false);

    // WICHTIG: Daten KOPIEREN (Job darf später NICHT auf UI zugreifen)
    const auto averagedSpectrumCopy = processorRef.getAveragedSpectrum();
    const auto referenceBandsCopy = processorRef.referenceBands;

    std::array<float, 31> qCopy{};
    for (int i = 0; i < 31; ++i)
        qCopy[(size_t)i] = (float)eqKnob[i].getValue(); // nur JETZT lesen (Message Thread)

    std::array<float, 31> eqFreqCopy{};
    for (int i = 0; i < 31; ++i)
        eqFreqCopy[(size_t)i] = (float)eqFrequencies[(size_t)i];

    float sr = (float)processorRef.getSampleRate();
    if (!(sr > 0.0f))
        sr = 48000.0f;

    float inputGainBeforeDb = 0.0f;
    if (auto* v = processorRef.apvts.getRawParameterValue("inputGain"))
        inputGainBeforeDb = v->load();

    // SafePointer: falls Editor geschlossen wird während der Berechnung
    juce::Component::SafePointer<AudioPluginAudioProcessorEditor> safeThis(this);

    struct Job : public juce::ThreadPoolJob
    {
        Job(juce::Component::SafePointer<AudioPluginAudioProcessorEditor> s,
            AudioPluginAudioProcessor& p,
            std::vector<AudioPluginAudioProcessor::SpectrumPoint> spec,
            std::vector<AudioPluginAudioProcessor::ReferenceBand> ref,
            std::array<float, 31> q,
            std::array<float, 31> eqF,
            float sampleRate,
            float inputGainBefore)
            : juce::ThreadPoolJob("AutoEqJob"),
            safeEditor(s), processor(p),
            spectrum(std::move(spec)), reference(std::move(ref)),
            qFixed(q), eqFreqs(eqF), sr(sampleRate), inputGainBeforeDb(inputGainBefore) {
        }

        static float computeOffsetFromCopies(
            const std::vector<AudioPluginAudioProcessor::SpectrumPoint>& spectrum,
            const std::vector<AudioPluginAudioProcessor::ReferenceBand>& reference,
            const std::array<float, 31>& eqFreqs)
        {
            if (spectrum.empty() || reference.empty())
                return 0.0f;

            std::vector<float> diffs;
            diffs.reserve(31);

            const float fMin = 50.0f;
            const float fMax = 10000.0f;

            for (int i = 0; i < 31; ++i)
            {
                const float f = eqFreqs[(size_t)i];
                if (f < fMin || f > fMax) continue;

                const float ref = sampleLogInterpolatedReferenceMedian(reference, f, DisplayScale::minDb);
                const float meas = sampleLogInterpolatedSpectrum(spectrum, f, DisplayScale::minDb);
                diffs.push_back(ref - meas);
            }

            if (diffs.empty())
                return 0.0f;

            std::nth_element(diffs.begin(), diffs.begin() + diffs.size() / 2, diffs.end());
            const float median = diffs[diffs.size() / 2];
            return juce::jlimit(-36.0f, 36.0f, median);
        }

        static std::vector<float> generateLogFrequenciesLocal(int numPoints, float minFreq, float maxFreq)
        {
            std::vector<float> out;
            out.reserve((size_t)numPoints);

            const float logMin = std::log10(minFreq);
            const float logMax = std::log10(maxFreq);

            for (int i = 0; i < numPoints; ++i)
            {
                const float t = (numPoints <= 1) ? 0.0f : (float)i / (float)(numPoints - 1);
                const float lf = logMin + (logMax - logMin) * t;
                out.push_back(std::pow(10.0f, lf));
            }
            return out;
        }

        JobStatus runJob() override
        {
            if (safeEditor == nullptr)
                return jobHasFinished;

            // --- HEAVY COMPUTE (kein GUI!) ---
            const float offsetDb = computeOffsetFromCopies(spectrum, reference, eqFreqs);

            // Residuals (31)
            std::vector<float> residuals;
            residuals.reserve(31);

            for (int i = 0; i < 31; ++i)
            {
                const float f = eqFreqs[(size_t)i];

                const float refLevel = sampleLogInterpolatedReferenceMedian(reference, f, DisplayScale::minDb);
                float measLevel = sampleLogInterpolatedSpectrum(spectrum, f, DisplayScale::minDb);

                const float gateDb = DisplayScale::minDb + 10.0f;
                if (measLevel < gateDb) measLevel = gateDb;

                measLevel += offsetDb;

                auto bassWeight = [](float f)
                    {
                        if (f < 40.0f) return 0.20f;
                        if (f < 80.0f) return 0.35f;
                        if (f < 120.0f) return 0.55f;
                        return 1.0f;
                    };

                auto bandMaxCorr = [](float f)
                    {
                        if (f < 60.0f)  return 4.0f;   // ganz unten nie “wild” korrigieren
                        if (f < 120.0f) return 6.0f;
                        return 12.0f;
                    };

                float r = (refLevel - measLevel) * edgeWeight(f) * bassWeight(f);
                r = juce::jlimit(-bandMaxCorr(f), bandMaxCorr(f), r);
                residuals.push_back(r);
            }

            // smoothing + amount
            residuals = smoothMovingAverage(residuals, 5, 1);
            for (auto& r : residuals)
                r *= 1.0f; // kAutoEqAmount

            std::array<float, 31> residualsArr{};
            for (int i = 0; i < 31; ++i)
                residualsArr[(size_t)i] = juce::jlimit(-12.0f, 12.0f, residuals[(size_t)i]);

            // ==============================
            // Hybrid Bass Mode (Peak+Dip breitbandig)
            // ==============================
            bool hybridBass = false;
            int idxMax = -1;
            int idxMin = -1;

            std::array<double, 31> extraPenalty{};
            extraPenalty.fill(0.0);

            auto isBass = [&](float f)
                {
                    return (f >= 40.0f && f <= 400.0f);
                };

            // 1) Bass-Schwankung messen (Peak-to-Peak)
            float bassMax = -1.0e9f;
            float bassMin = 1.0e9f;

            for (int i = 0; i < 31; ++i)
            {
                const float f = eqFreqs[(size_t)i];
                if (!isBass(f)) continue;

                bassMax = std::max(bassMax, residuals[(size_t)i]);
                bassMin = std::min(bassMin, residuals[(size_t)i]);
            }

            const float bassP2P = bassMax - bassMin;

            // Schwelle: ab hier "große" Schwankung -> Hybrid-Modus
            // (Tipp: 5..8 dB je nach Material)
            if (bassP2P > 6.0f)
            {
                hybridBass = true;

                // 2) stärkstes Maximum + Minimum finden
                float bestPos = -1.0e9f;
                float bestNeg = 1.0e9f;

                for (int i = 0; i < 31; ++i)
                {
                    const float f = eqFreqs[(size_t)i];
                    if (!isBass(f)) continue;

                    const float r = residuals[(size_t)i];

                    if (r > bestPos) { bestPos = r; idxMax = i; }
                    if (r < bestNeg) { bestNeg = r; idxMin = i; }
                }

                // 3) coarse bass target aus 2 breiten Gaussians bauen
                std::vector<float> coarse = residuals;

                // Bassbereich "leer machen"
                for (int i = 0; i < 31; ++i)
                    if (isBass(eqFreqs[(size_t)i]))
                        coarse[(size_t)i] = 0.0f;

                const float sigmaOct = 0.55f; // Breite in Oktaven (größer = breiter)

                auto g = [&](float f, float fc)
                    {
                        const float x = std::log2(f);
                        const float xc = std::log2(fc);
                        const float d = (x - xc) / sigmaOct;
                        return std::exp(-0.5f * d * d);
                    };

                if (idxMax >= 0)
                {
                    const float fc = eqFreqs[(size_t)idxMax];
                    const float A = residuals[(size_t)idxMax];
                    for (int i = 0; i < 31; ++i)
                    {
                        const float f = eqFreqs[(size_t)i];
                        if (!isBass(f)) continue;
                        coarse[(size_t)i] += A * g(f, fc);
                    }
                }

                if (idxMin >= 0)
                {
                    const float fc = eqFreqs[(size_t)idxMin];
                    const float A = residuals[(size_t)idxMin];
                    for (int i = 0; i < 31; ++i)
                    {
                        const float f = eqFreqs[(size_t)i];
                        if (!isBass(f)) continue;
                        coarse[(size_t)i] += A * g(f, fc);
                    }
                }

                // 4) Bass-Residuals ersetzen (Mix, damit’s nicht "zu hart" wird)
                const float mix = 0.85f; // 0.7..0.95
                for (int i = 0; i < 31; ++i)
                {
                    const float f = eqFreqs[(size_t)i];
                    if (!isBass(f)) continue;

                    residuals[(size_t)i] = (1.0f - mix) * residuals[(size_t)i] + mix * coarse[(size_t)i];
                }

                // 5) Solver zwingen: nur die 2 Extrem-Bänder im Bass sollen "frei" sein
                // Alle anderen Bass-Bänder bekommen Zusatz-Penalty -> bleiben näher an 0 dB Gain.
                for (int i = 0; i < 31; ++i)
                {
                    const float f = eqFreqs[(size_t)i];
                    if (!isBass(f)) continue;

                    if (i == idxMax || i == idxMin)
                        extraPenalty[(size_t)i] = 0.05;  // fast frei
                    else
                        extraPenalty[(size_t)i] = 2.5;   // stärker "zu 0 drücken" (1.0..6.0)
                }
            }

            // Fit (etwas kleiner -> weniger Freeze/CPU)
            const int fitPoints = 350; // 250..400 ist sehr praxisnah
            auto fitFreqs = generateLogFrequenciesLocal(fitPoints, 20.0f, 20000.0f);

            std::vector<float> bandFreqs;
            bandFreqs.reserve(31);
            for (int i = 0; i < 31; ++i)
                bandFreqs.push_back(eqFreqs[(size_t)i]);

            std::vector<float> targetDb;
            targetDb.reserve(fitFreqs.size());
            for (auto f : fitFreqs)
                targetDb.push_back(interpLogCurveDb(bandFreqs, residuals, f));

            // Stufe 1: Gains fitten (Q fix)
            const std::array<double, 31>* penaltyPtr = hybridBass ? &extraPenalty : nullptr;

            std::array<float, 31> gainsStage1 = fitGainsStage1(fitFreqs, targetDb, qFixed, sr, bandFreqs, penaltyPtr);

            // Stufe 2: Qs optimieren (gegen Ripple)
            std::array<float, 31> qStage2 = fitQsStage2_Coordinate(fitFreqs, targetDb, gainsStage1, qFixed, sr, bandFreqs);

            if (hybridBass)
            {
                // Nur die 2 aktiven Bassbänder wirklich breit (kleines Q)
                if (idxMax >= 0) qStage2[(size_t)idxMax] = juce::jlimit(0.6f, 1.4f, qStage2[(size_t)idxMax]);
                if (idxMin >= 0) qStage2[(size_t)idxMin] = juce::jlimit(0.6f, 1.4f, qStage2[(size_t)idxMin]);

                // Restliche Bassbänder notfalls auch nicht zu schmal werden lassen
                for (int i = 0; i < 31; ++i)
                    if (eqFreqs[(size_t)i] >= 40.0f && eqFreqs[(size_t)i] <= 400.0f)
                        qStage2[(size_t)i] = juce::jmin(qStage2[(size_t)i], 2.0f);
            }

            // Q hard limits (Basis)
            for (auto& q : qStage2)
                q = juce::jlimit(0.6f, 6.0f, q);

            // Danach: Gains nochmal fitten mit neuen Qs
            std::array<float, 31> finalGains = fitGainsStage1(fitFreqs, targetDb, qStage2, sr, bandFreqs, penaltyPtr);

            // Optional: Q abhängig von Gain begrenzen
            for (int i = 0; i < 31; ++i)
            {
                const float gAbs = std::abs(finalGains[(size_t)i]);
                float q = qStage2[(size_t)i];

                const float qMax = (gAbs > 8.0f) ? 1.4f
                    : (gAbs > 5.0f) ? 2.2f
                    : 4.0f;

                qStage2[(size_t)i] = juce::jlimit(0.6f, qMax, q);
            }

            // WICHTIG: Nach Q-Begrenzung Gains nochmal refitten, sonst passt’s nicht mehr!
            finalGains = fitGainsStage1(fitFreqs, targetDb, qStage2, sr, bandFreqs, penaltyPtr);

            // === Makeup Gain so berechnen, dass (Meas + offset + EQResponse) wieder zur Referenz passt ===
            // 1) EQ-Response in dB auf fitFreqs berechnen
            std::vector<float> respDb;
            computeEQResponseDb(fitFreqs, finalGains, qStage2, sr, respDb, bandFreqs);

            // 2) Median-Differenz im stabilen Bereich bilden
            std::vector<float> diffs;
            diffs.reserve(fitFreqs.size());

            for (size_t k = 0; k < fitFreqs.size(); ++k)
            {
                const float f = fitFreqs[k];
                if (f < 50.0f || f > 10000.0f) // ggf. 60..10k wenn du Bass noch mehr rausnehmen willst
                    continue;

                const float ref = sampleLogInterpolatedReferenceMedian(reference, f, DisplayScale::minDb);
                float meas = sampleLogInterpolatedSpectrum(spectrum, f, DisplayScale::minDb);

                // Gate wie bei Residuals (verhindert "Noise floor" Einfluss)
                const float gateDb = DisplayScale::minDb + 10.0f;
                if (meas < gateDb) meas = gateDb;

                const float predictedPost = meas + offsetDb + respDb[k];
                diffs.push_back(ref - predictedPost);
            }

            float makeupDeltaDb = 0.0f;
            if (!diffs.empty())
            {
                std::nth_element(diffs.begin(), diffs.begin() + diffs.size() / 2, diffs.end());
                makeupDeltaDb = diffs[diffs.size() / 2];
            }

            makeupDeltaDb = juce::jlimit(-12.0f, 12.0f, makeupDeltaDb);

            const float inputGainBefore = inputGainBeforeDb;


            juce::MessageManager::callAsync([safe = safeEditor,
                finalGains,
                finalQs = qStage2,
                residualsArr,
                makeupDeltaDb,
                inputGainBefore]() mutable
                {
                    if (safe == nullptr)
                        return;

                    // 1) Zielkurve (31 Punkte) speichern -> gestrichelt zeichnen (ohne Kammfilter!)
                    safe->processorRef.targetResidualsDb = residualsArr;
                    safe->processorRef.hasTargetResiduals = true;

                    // 2) Dein bisheriges: fitted Gains speichern (falls du das weiter nutzt)
                    for (int i = 0; i < 31; ++i)
                        safe->processorRef.targetCorrections[i] = juce::jlimit(-12.0f, 12.0f, finalGains[(size_t)i]);

                    safe->processorRef.hasTargetCorrections = true;

                    // Qs/Gains/InputGain anwenden ...
                    applyQsToApvts(safe->processorRef, finalQs);
                    applyGainsToApvts(safe->processorRef, finalGains);

                    const float newInputGain = juce::jlimit(-24.0f, 24.0f, inputGainBefore + makeupDeltaDb);

                    // UI wieder freigeben ...
                    safe->genreErkennenButton.setEnabled(true);
                    safe->genreErkennenButton.setButtonText("Messung starten");
                    safe->loadReferenceButton.setEnabled(true);
                    safe->resetButton.setEnabled(true);
                    safe->eqCurveToggleButton.setEnabled(true);

                    safe->repaint();
                    safe->autoEqRunning.store(false);
                });

            return jobHasFinished;
        }

        juce::Component::SafePointer<AudioPluginAudioProcessorEditor> safeEditor;
        AudioPluginAudioProcessor& processor;

        std::vector<AudioPluginAudioProcessor::SpectrumPoint> spectrum;
        std::vector<AudioPluginAudioProcessor::ReferenceBand> reference;

        std::array<float, 31> qFixed{};
        std::array<float, 31> eqFreqs{};
        float sr = 48000.0f;
        float inputGainBeforeDb = 0.0f;
    };

    autoEqPool.addJob(new Job(safeThis,
        processorRef,
        averagedSpectrumCopy,
        referenceBandsCopy,
        qCopy,
        eqFreqCopy,
        sr,
        inputGainBeforeDb),
        true);
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

    //======================
    //      Gains-Fit
    //======================

    // 1) Zielkurve (targetDb) auf dichtes Frequenzraster bringen
    const int fitPoints = 600; // 400..1000 ist praxisnah, 2000 ist möglich aber langsamer
    auto fitFreqs = generateLogFrequencies(fitPoints, 20.0f, 20000.0f);

    // residuals ist dein 31er Ziel (an den eqFrequencies)
    // -> daraus machen wir targetDb auf fitFreqs
    std::vector<float> bandFreqs;
    bandFreqs.reserve(31);
    for (int i = 0; i < 31; ++i)
        bandFreqs.push_back(eqFrequencies[i]);

    std::vector<float> targetDb;
    targetDb.reserve((size_t)fitFreqs.size());
    for (auto f : fitFreqs)
        targetDb.push_back(interpLogCurveDb(bandFreqs, residuals, f));

    // 2) Q fest lassen (aktueller Knob-Stand)
    std::array<float, 31> fixedQs;
    for (int i = 0; i < 31; ++i)
        fixedQs[(size_t)i] = (float)eqKnob[i].getValue();

    // 3) SampleRate
    float sr = (float)processorRef.getSampleRate();
    if (sr <= 0.0f) sr = 48000.0f;

    // 4) Fit berechnen: gives recommended slider gains
    std::array<float, 31> fittedGains = fitGainsStage1(fitFreqs, targetDb, fixedQs, sr, bandFreqs);

    // 5) Ergebnis in targetCorrections speichern (das sind jetzt "Slider-Gains", nicht nur Residual-Punkte)
    for (int i = 0; i < 31; ++i)
    {
        const float g = fittedGains[(size_t)i];
        processorRef.targetCorrections[i] = finiteClamp(g, -12.0f, 12.0f, 0.0f);
    }

    // Flag erst setzen, wenn die Daten sicher sind
    processorRef.hasTargetCorrections = true;


    DBG("=== Auto-EQ Stufe 1 (Gains-Fit) abgeschlossen ===");
    repaint();


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
    const auto targetPath = buildTargetPath();
    if (targetPath.isEmpty())
        return;

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
    juce::Path path;

    const bool useResiduals = processorRef.hasTargetResiduals;
    const bool useCorrections = processorRef.hasTargetCorrections.load(std::memory_order_acquire);

    if (!useResiduals && !useCorrections)
        return path;

    auto area = spectrumInnerArea.toFloat();

    const float minFreq = 20.0f;
    float sr = (float)processorRef.getSampleRate();
    if (!(sr > 0.0f)) sr = 48000.0f;
    const float maxFreq = juce::jmin(20000.0f, 0.5f * sr * 0.999f);

    const float minDb = -12.0f, maxDb = 12.0f;

    bool first = true;
    for (int i = 0; i < 31; ++i)
    {
        const float f = juce::jlimit(minFreq, maxFreq, eqFrequencies[i]);

        float db = 0.0f;
        if (useResiduals)
            db = finiteClamp(processorRef.targetResidualsDb[(size_t)i], minDb, maxDb, 0.0f);
        else
            db = finiteClamp(processorRef.targetCorrections[i], minDb, maxDb, 0.0f);

        const float x = area.getX() + juce::mapFromLog10(f, minFreq, maxFreq) * area.getWidth();
        const float y = juce::jmap(db, minDb, maxDb, area.getBottom(), area.getY());

        if (first) { path.startNewSubPath(x, y); first = false; }
        else { path.lineTo(x, y); }
    }

    return path;
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
    g.setColour(Theme::curveTarget.withAlpha(0.95f));
    g.fillPath(dashedPath);
}

/**
 * @brief Zeichnet Markierungspunkte an den EQ-Frequenzen.
 *
 * @param g Der Graphics-Kontext zum Zeichnen
 */
void AudioPluginAudioProcessorEditor::drawTargetPoints(juce::Graphics& g)
{
    const bool useResiduals = processorRef.hasTargetResiduals;
    const bool useCorrections = processorRef.hasTargetCorrections.load(std::memory_order_acquire);
    if (!useResiduals && !useCorrections)
        return;

    auto area = spectrumInnerArea.toFloat();

    const float minFreq = 20.0f;
    float sr = (float)processorRef.getSampleRate();
    if (!(sr > 0.0f)) sr = 48000.0f;
    const float maxFreq = juce::jmin(20000.0f, 0.5f * sr * 0.999f);

    const float minDb = -12.0f;
    const float maxDb = 12.0f;

    g.setColour(Theme::curveTarget.withAlpha(0.95f));

    for (int i = 0; i < 31; ++i)
    {
        const float f = juce::jlimit(minFreq, maxFreq, (float)eqFrequencies[i]);

        float db = 0.0f;
        if (useResiduals)
            db = processorRef.targetResidualsDb[(size_t)i];
        else
            db = processorRef.targetCorrections[(size_t)i];

        db = juce::jlimit(minDb, maxDb, db);

        const float x = area.getX() + juce::mapFromLog10(f, minFreq, maxFreq) * area.getWidth();
        const float y = juce::jmap(db, minDb, maxDb, area.getBottom(), area.getY());

        g.fillEllipse(x - 3.0f, y - 3.0f, 6.0f, 6.0f);
    }
}
