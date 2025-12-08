#include "PluginProcessor.h"
#include "PluginEditor.h"


//==============================================================================
AudioPluginAudioProcessorEditor::AudioPluginAudioProcessorEditor (AudioPluginAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    // Timer für FFT erzeugen
    startTimerHz(30);  // Update display at 30 FPS

    // Fenster Einstellungen, x-Breite, y-Höhe
    setSize(1000, 690);
    setResizable(false, false);
    
    // Dropdown-Menue
    genreBox.setTextWhenNothingSelected("Genre auswahlen...");
    genreBox.addItem("Pop", 1);
    genreBox.addItem("HipHop", 2);
    genreBox.addItem("Rock", 3);
    genreBox.addItem("EDM", 4);
    genreBox.addItem("Klassik", 5);
    genreBox.addItem("Test", 6);

    // Dropdown: JSON laden und auf Änderung reagieren
    genreBox.onChange = [this]
        {
            const int id = genreBox.getSelectedId();

            switch (id)
            {
            case 1: // Pop
                loadReferenceCurve("pop_medref.json");
                break;

            case 2: // HipHop
                loadReferenceCurve("HipHop.json");
                break;

            case 3: // Rock
                loadReferenceCurve("Rock.json");
                break;

            case 4: // EDM
                loadReferenceCurve("EDM.json");
                break;

            case 5: // Klassik
                loadReferenceCurve("Klassik.json");
                break;

            case 6: // Test
                loadReferenceCurve("test.json");
                break;

            default:
                // Wenn nichts ausgewählt ist oder "leer"
                referenceBands.clear(); // Referenzkurve löschen
                break;
            }

            // Nach jeder Änderung neu zeichnen
            repaint();
        };

    // Button
    genreErkennenButton.setButtonText("Messung starten / stoppen");
    genreErkennenButton.setColour(juce::TextButton::buttonColourId, juce::Colours::grey);

    resetButton.setButtonText("Reset");
    resetButton.setColour(juce::TextButton::buttonColourId, juce::Colours::red);
    resetButton.onClick = [this]
        {
            processorRef.resetAllBandsToDefault();
        };

    // EQ Curve Toggle Button
    eqCurveToggleButton.setButtonText("EQ Kurve");
    eqCurveToggleButton.setColour(juce::TextButton::buttonColourId, juce::Colours::darkblue);
    eqCurveToggleButton.setClickingTogglesState(true);
    eqCurveToggleButton.onClick = [this]
        {
            showEQCurve = eqCurveToggleButton.getToggleState();
            repaint();
        };

    addAndMakeVisible(eqCurveToggleButton);

    // Silder konfigurieren
    for (int i = 0; i < 31; i++)
    {
        eqSlider[i].setSliderStyle(juce::Slider::LinearVertical);
        eqSlider[i].setRange(-12.0, 12.0, 0.1);
        eqSlider[i].setValue(0.0);
        eqSlider[i].setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        eqSlider[i].setColour(juce::Slider::thumbColourId, juce::Colours::white);
        eqSlider[i].setColour(juce::Slider::trackColourId, juce::Colours::lightgrey);
        
        eqAttachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processorRef.apvts, "band" + juce::String(i), eqSlider[i]);

        addAndMakeVisible(eqSlider[i]);
    }

    // Q Knobs konfigurieren
    for (int i = 0; i < 31; ++i)
    {
        eqKnob[i].setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        eqKnob[i].setRange(0.3, 10.0, 0.01); // selbe Range wie Parameter
        eqKnob[i].setValue(4.32); // Default Q
        eqKnob[i].setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);

        // Optik kannst du später verfeinern
        eqKnob[i].setColour(juce::Slider::thumbColourId, juce::Colours::white);
        eqKnob[i].setColour(juce::Slider::rotarySliderFillColourId, juce::Colours::darkgrey);
        eqKnob[i].setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colours::black);

        // Attachment: dieser Knob steuert qBand<i>
        eqQAttachments[i] = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processorRef.apvts, "bandQ" + juce::String(i), eqKnob[i]);

        addAndMakeVisible(eqKnob[i]);
    }

    // Sichtbar machen
    addAndMakeVisible(genreBox);
    addAndMakeVisible(genreErkennenButton);
    addAndMakeVisible(resetButton);

    // Input Gain Slider konfigurieren
    inputGainSlider.setSliderStyle(juce::Slider::LinearHorizontal); // <-- GEÄNDERT
    inputGainSlider.setRange(-24.0, 24.0, 0.1);
    inputGainSlider.setValue(0.0);
    inputGainSlider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20); // <-- GEÄNDERT
    inputGainSlider.setColour(juce::Slider::thumbColourId, juce::Colours::white);
    inputGainSlider.setColour(juce::Slider::trackColourId, juce::Colours::green);
    inputGainSlider.setTextValueSuffix(" dB");

    // Attachment erstellen
    inputGainAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
        processorRef.apvts, "inputGain", inputGainSlider);

    // Label konfigurieren
    inputGainLabel.setText("Input Gain", juce::dontSendNotification);
    inputGainLabel.setJustificationType(juce::Justification::centredLeft); // <-- GEÄNDERT
    inputGainLabel.attachToComponent(&inputGainSlider, true); // <-- true = links vom Slider

    addAndMakeVisible(inputGainSlider);
    addAndMakeVisible(inputGainLabel);
}

// Referenzkurve laden
void AudioPluginAudioProcessorEditor::loadReferenceCurve(const juce::String& filename)
{
    // Speicher für Kurve leeren
    referenceBands.clear();

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

    // Analysiert Text aus fileContent und erstellt dynamisches var
    juce::var jsonData = juce::JSON::parse(fileContent);

    // Bänder aus JSON erstellen
    auto bands = jsonData["bands"];

    // Bänder erstellen
    for (auto& b : *bands.getArray())
    {
        // Neues Datenobjekt
        ReferenceBand rb;

        // Bänder durchlaufen
        rb.freq = (float)b["freq"];
        rb.p10 = (float)b["p10"];
        rb.median = (float)b["median"];
        rb.p90 = (float)b["p90"];

        // Band an Vektor anhängen
        referenceBands.push_back(rb);
    }
}

AudioPluginAudioProcessorEditor::~AudioPluginAudioProcessorEditor()
{

}

//==============================================================================
void AudioPluginAudioProcessorEditor::paint(juce::Graphics& g)
{
    // Topbar Farbe
    g.setColour(juce::Colour::fromString("ff2c2f33"));
    g.fillRect(topBarArea);

    // Layout in der Topbar, Abstand Links/Rechts, Oben/Unten
    auto top = topBarArea.reduced(12, 8);

    // Layout Area für alles unter der Topbar
    auto rest = getLocalBounds().withY(topBarArea.getBottom());

    // Spektogramm Hintergrundfarbe
    g.setColour(juce::Colour::fromString("ff111111"));
    g.fillRect(rest);

    // Frequenzspektrum Bereich färben
    g.setColour(juce::Colours::orange);
    g.fillRect(spectrogramArea);

    // Display Bereich färben
    g.setColour(juce::Colours::green);
    g.fillRect(spectrumDisplayArea);

    // Innerer Display Bereich
    g.setColour(juce::Colours::hotpink);
    g.fillRect(spectrumInnerArea);

    // Vertikale Frequenzlinien im Spektrogramm zeichnen
    g.setColour(juce::Colours::white.withAlpha(0.5f));

    // Define these constants at function scope
    const float minFreq = 20.0f;
    const float maxFreq = 20000.0f;
    const float displayMinDb = DisplayScale::minDb;
    const float displayMaxDb = DisplayScale::maxDb;

    // SPEKTRUM NUR ZEICHNEN WENN EQ KURVE AUS IST
    if (!showEQCurve)
    {
        drawFrame(g);
    }

    // EQ KURVE DARÜBER ZEICHNEN WENN AKTIV
    if (showEQCurve)
    {
        drawEQCurve(g);
    }

    // Schriftgröße für Achsenbeschriftung
    g.setFont(15.0f);

    // Y-Position für Achsenbeschriftung
    float textY = (float)spectrumDisplayArea.getBottom() + 3.0f;

    // <Vertikales Horizontales Raster zeichnen
    for (auto f : frequencies)
    {
        // Frequenzen in 0-1 Bereich umrechnen
        float normX = juce::mapFromLog10(f, minFreq, maxFreq);

        // Normierten Bereich (0-1) auf pinken Bereich skalieren
        float x = spectrumInnerArea.getX() + normX * spectrumInnerArea.getWidth();

        // Vertikale Linie innerhalb des pinken Bereichs zeichnen
        g.drawVerticalLine(
            static_cast<int>(x),
            (float)spectrumInnerArea.getY(), // obere Grenze
            (float)spectrumInnerArea.getBottom() // untere Grenze
        );

        // Achsenbeschriftung einfügen
        juce::String text;
        if (f >= 1000.0f)
            text = juce::String(f / 1000.0f) + "k";
        else
            text = juce::String((int)f);

        // Achsenbeschriftung einfügen
        g.drawFittedText(
            text,
            (int)(x - 15), // x-Position: Nach links verschieben
            (int)textY, // y-Position
            30, // Textbox-Breite
            15, // Textbox-Höhe
            juce::Justification::centred, // zentrieren
            1 // max. Anzahl an Zeilen
        );
    }

    float textX = (float)spectrumInnerArea.getX() - 40.0f;
    g.setColour(juce::Colours::lightgrey.withAlpha(0.5f));

    // Referenzbänder zeichnen
    if (!showEQCurve && !referenceBands.empty())
    {
        // Remove redefinitions - use variables defined above

        // Pfade für die Linienpunkte
        juce::Path pathP10;
        juce::Path pathP90;
        juce::Path pathMedian;

        // Überprüfen, ob aktueller Punkt der erste ist
        bool firstPoint = true;

        // Schleife für alle Bänder
        for (const auto& band : referenceBands)
        {
            // x-Position normieren
            float normX = juce::mapFromLog10(band.freq, minFreq, maxFreq);

            // x-Position auf Fenster skalieren
            float x = spectrumInnerArea.getX() + normX * spectrumInnerArea.getWidth();

            // y-Positionen invertieren (oben = laut)
            float yP10 = juce::jmap(band.p10, displayMinDb, displayMaxDb,
                (float)spectrumInnerArea.getBottom(),
                (float)spectrumInnerArea.getY());

            float yMedian = juce::jmap(band.median, displayMinDb, displayMaxDb,
                (float)spectrumInnerArea.getBottom(),
                (float)spectrumInnerArea.getY());

            float yP90 = juce::jmap(band.p90, displayMinDb, displayMaxDb,
                (float)spectrumInnerArea.getBottom(),
                (float)spectrumInnerArea.getY());

            // Neue Linie beim ersten Punkt der JSON Datei
            if (firstPoint)
            {
                pathP10.startNewSubPath(x, yP10);
                pathP90.startNewSubPath(x, yP90);
                pathMedian.startNewSubPath(x, yMedian);
                firstPoint = false;
            }
            // Sonst Linie verbinden
            else
            {
                pathP10.lineTo(x, yP10);
                pathP90.lineTo(x, yP90);
                pathMedian.lineTo(x, yMedian);
            }
        }

        // Zeichnen der Referenzlinien
        g.setColour(juce::Colours::blue.withAlpha(0.6f)); // untere (p10)
        g.strokePath(pathP10, juce::PathStrokeType(1.5f));

        g.setColour(juce::Colours::blue.withAlpha(0.6f)); // obere (p90)
        g.strokePath(pathP90, juce::PathStrokeType(1.5f));

        g.setColour(juce::Colours::grey); // Median
        g.strokePath(pathMedian, juce::PathStrokeType(2.0f));
    }

    // EQ Sliderbereich (blau)
    g.setColour(juce::Colours::blue);
    g.fillRect(eqArea);

    // Q-Bereich (hellgrün)
    g.setColour(juce::Colours::lightgreen);
    g.fillRect(eqKnobArea);

    // EQ Beschriftungsbereich (rot)
    g.setColour(juce::Colours::red);
    g.fillRect(eqLabelArea);


    // Slider Beschriftung
    g.setColour(juce::Colours::white.withAlpha(0.5f));
    g.setFont(14.0f);

    for (int i = 0; i < eqFrequencies.size(); i++)
    {
        // Positionen normieren
        float normX = juce::mapFromLog10(eqFrequencies[i], 16.0f, 25500.f);

        // Auf den EQ Bereich skalieren
        int x = eqArea.getX() + static_cast<int>(normX * eqArea.getWidth());

        // Textdarstellung anpassen
        juce::String label;
        if (eqFrequencies[i] >= 1000.0f)
        {
            float valueInK = eqFrequencies[i] / 1000.0f;

            if (valueInK  >= 10.0f)
                label = juce::String((int)valueInK) + "k";
            else
                label = juce::String(valueInK, 1) + "k";
        }
        else
        {
            label = juce::String((int)eqFrequencies[i]);
        }

        // Text einfügen
        g.drawFittedText(
            label,
            x - 20, // x-Position: Nach links verschieben
            eqLabelArea.getY() + 5, // y-Position: Oberer Rand vom roten Bereich + kleiner Abstand
            40, // Textbox-Breite
            20, // Textbox-Höhe
            juce::Justification::centred, // zentrieren
            1 // max. Anzahl an Zeilen
        );
    }
}

void AudioPluginAudioProcessorEditor::resized()
{
    // Hauptbereich "Area"
    auto area = getLocalBounds();

    // Topbar abtrennen
    topBarArea = area.removeFromTop(topBarHeight);

    // Dropdown Position (x-Position, y-Position, x-Breite, y-Höhe)
    const int barDropW = 220;
    const int barDropH = 30;
    genreBox.setBounds(710, 5, 220, 30);

    // Input Gain Slider - UNTER dem Dropdown
    inputGainSlider.setBounds(770, 45, 220, 30); 

    // Button Position (x-Position, y-Position, x-Breite, y-Höhe)
    genreErkennenButton.setBounds(10, 5, 200, 30);
    resetButton.setBounds(940, 5, 50, 30);

    eqCurveToggleButton.setBounds(220, 5, 120, 30);

    // Restbereich unter der Topbar
    auto rest = area;

    // Äußerer Bereich vom Spektrogramm
    auto spectroOuter = rest.removeFromTop(spectrogramOuterHeight);

    // Innerer Bereich vom Spektrogramm
    spectrogramArea = spectroOuter.reduced(spectrogramMargin);

    // Spektrogramm Display Bereich
    spectrumDisplayArea = spectrogramArea.removeFromTop(spectrumHeight);

    // EQ Bereich
    auto eqFullArea = rest.removeFromTop(eqHeight);

    // EQ Beschriftung
    eqLabelArea = eqFullArea.removeFromBottom(eqLabelHeight);

    // Q Filter
    eqKnobArea = eqFullArea.removeFromBottom(eqSpacerHeight);

    // Silder Bereich
    eqArea = eqFullArea;

    // Silder setzen
    for (int i = 0; i < 31; ++i)
    {
        // Frequenz in log-Skala umrechnen
        float normX = juce::mapFromLog10(eqFrequencies[i], 16.0f, 25500.0f);

        // Auf den Bereich vom EQ skalieren
        int x = eqArea.getX() + static_cast<int>(normX * eqArea.getWidth());

        int sliderWidth = 16;

        const int verticalMargin = 8;

        int sliderHeight = eqArea.getHeight() - 2 * verticalMargin;
        int sliderY = eqArea.getY() + verticalMargin;

        eqSlider[i].setBounds(
            x - sliderWidth / 2,
            eqArea.getY() + 10,
            sliderWidth,
            sliderHeight
        );
    }

    // Q Knobs setzen
    for (int i = 0; i < 31; ++i)
    {
        int centerX = eqSlider[i].getX() + eqSlider[i].getWidth() / 2;

        float bandWidth = (float)eqArea.getWidth() / 31.0f;

        // z.B. 1.4x Bandbreite, aber nicht höher als der grüne Bereich
        int knobDiameter = (int)std::floor(bandWidth * 1.3f);

        int x = centerX - knobDiameter / 2;
        int y = eqKnobArea.getCentreY() - knobDiameter / 2;

        eqKnob[i].setBounds(x, y, knobDiameter, knobDiameter);
    }

    // Innerer Spektrumsbereich auf Sliderbreite setzen
    const int firstIndex = 0;
    const int lastIndex = 30;

    int leftX = eqSlider[firstIndex].getX() + eqSlider[firstIndex].getWidth() / 2;
    int rightX = eqSlider[lastIndex].getX() + eqSlider[lastIndex].getWidth() / 2;
    int innerWidth = rightX - leftX;

    spectrumInnerArea = juce::Rectangle<int>(
        leftX,
        spectrumDisplayArea.getY(),
        innerWidth,
        spectrumDisplayArea.getHeight()
    );   
}

// Timer callback Displayupdate
void AudioPluginAudioProcessorEditor::timerCallback()
{
    // Überprüft ob FFT-Daten verfügbar sind
    if (processorRef.getNextFFTBlockReady())
    {
        // Verarbeitet die FFT Daten zur darstellung
        processorRef.updateSpectrumArray(processorRef.getSampleRate());
        processorRef.setNextFFTBlockReady(false); // Reset flag
        repaint(); // Triggert Redraw
    }
}

void AudioPluginAudioProcessorEditor::drawFrame(juce::Graphics& g)
{
    auto& spectrum = processorRef.spectrumArray;
    if (spectrum.empty())
        return;

    auto area = spectrumInnerArea.toFloat();

    const float displayMinDb = DisplayScale::minDb;
    const float displayMaxDb = DisplayScale::maxDb;

    const float minFreq = 20.0f;
    const float maxFreq = 20000.0f;
    const float logMin = std::log10(minFreq);
    const float logMax = std::log10(maxFreq);

    // Direkt die Punkte ohne Smoothing sammeln
    std::vector<juce::Point<float>> validPoints;

    for (size_t i = 0; i < spectrum.size(); ++i)
    {
        auto& point = spectrum[i];
        if (point.frequency < minFreq || point.frequency > maxFreq)
            continue;

        // Direkt den rohen Level verwenden - kein Smoothing
        float level = point.level;

        float logFreq = std::log10(point.frequency);
        float x = area.getX() + juce::jmap(logFreq, logMin, logMax, 0.0f, 1.0f) * area.getWidth();
        float db = juce::jlimit(displayMinDb, displayMaxDb, level);
        float y = juce::jmap(db, displayMinDb, displayMaxDb, area.getBottom(), area.getY());

        validPoints.push_back({ x, y });
    }

    // Extrapolation zu 20 Hz (optional beibehalten)
    if (!validPoints.empty() && spectrum[0].frequency > minFreq)
    {
        if (validPoints.size() >= 2)
        {
            float freq1 = spectrum[0].frequency;
            float freq2 = spectrum[1].frequency;
            float level1 = spectrum[0].level;
            float level2 = spectrum[1].level;

            float slope = (level2 - level1) / (freq2 - freq1);
            float extrapolatedLevel = level1 + slope * (minFreq - freq1);
            extrapolatedLevel = juce::jlimit(displayMinDb, displayMaxDb, extrapolatedLevel);

            float x = area.getX();
            float y = juce::jmap(extrapolatedLevel, displayMinDb, displayMaxDb,
                area.getBottom(), area.getY());

            validPoints.insert(validPoints.begin(), { x, y });
        }
    }

    if (validPoints.size() < 2)
        return;

    // Einfacher Path ohne Interpolation - direkte Linien zwischen Punkten
    juce::Path spectrumPath;
    spectrumPath.startNewSubPath(validPoints[0]);

    for (size_t i = 1; i < validPoints.size(); ++i)
    {
        spectrumPath.lineTo(validPoints[i]);
    }

    // Kurve zeichnen
    if (showEQCurve)
    {
        g.setColour(juce::Colours::cyan.withAlpha(0.6f));
        g.strokePath(spectrumPath, juce::PathStrokeType(1.5f));
    }
    else
    {
        g.setColour(juce::Colours::cyan);
        g.strokePath(spectrumPath, juce::PathStrokeType(2.0f));
    }
}

void AudioPluginAudioProcessorEditor::drawEQCurve(juce::Graphics& g)
{
    const int numPoints = 2000;
    const float minFreq = 20.0f;
    const float maxFreq = 20000.0f;

    std::vector<float> frequencies;
    frequencies.reserve(numPoints);

    float logMin = std::log10(minFreq);
    float logMax = std::log10(maxFreq);

    for (int i = 0; i < numPoints; ++i)
    {
        float logFreq = logMin + (logMax - logMin) * i / (numPoints - 1);
        frequencies.push_back(std::pow(10.0f, logFreq));
    }

    std::vector<float> totalMagnitudeDB(numPoints, 0.0f);

    float sampleRate = processorRef.getSampleRate();
    if (sampleRate <= 0.0f)
        sampleRate = 48000.0f;

    for (int bandIdx = 0; bandIdx < 31; ++bandIdx)
    {
        float f0 = eqFrequencies[bandIdx];
        float gainDb = eqSlider[bandIdx].getValue();
        float Q = eqKnob[bandIdx].getValue();

        if (std::abs(gainDb) > 0.01f)
        {
            for (int i = 0; i < numPoints; ++i)
            {
                auto H = peakingEQComplex(frequencies[i], f0, Q, gainDb, sampleRate);
                float magDb = 20.0f * std::log10(std::abs(H));
                totalMagnitudeDB[i] += magDb;
            }
        }
    }

    juce::Path eqPath;
    bool firstPoint = true;

    auto area = spectrumInnerArea.toFloat();

    for (int i = 0; i < numPoints; ++i)
    {
        float freq = frequencies[i];
        float db = totalMagnitudeDB[i];

        float normX = juce::mapFromLog10(freq, minFreq, maxFreq);
        float x = area.getX() + normX * area.getWidth();

        float clampedDb = juce::jlimit(-12.0f, 12.0f, db);
        float y = juce::jmap(clampedDb, -12.0f, 12.0f,
            area.getBottom(), area.getY());

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

    g.setColour(juce::Colours::white.withAlpha(0.3f));
    float y0dB = juce::jmap(0.0f, -12.0f, 12.0f,
        area.getBottom(), area.getY());
    g.drawHorizontalLine((int)y0dB, area.getX(), area.getRight());

    g.setColour(juce::Colours::yellow.withAlpha(0.9f));
    g.strokePath(eqPath, juce::PathStrokeType(3.0f));

    juce::Path filledPath = eqPath;
    filledPath.lineTo(area.getRight(), y0dB);
    filledPath.lineTo(area.getX(), y0dB);
    filledPath.closeSubPath();

    g.setColour(juce::Colours::yellow.withAlpha(0.15f));
    g.fillPath(filledPath);
}

std::complex<float> AudioPluginAudioProcessorEditor::peakingEQComplex(
    float freq, float f0, float Q, float gainDb, float sampleRate)
{
    const float A = std::pow(10.0f, gainDb / 40.0f);
    const float w0 = juce::MathConstants<float>::twoPi * f0 / sampleRate;
    const float w = juce::MathConstants<float>::twoPi * freq / sampleRate;
    const float alpha = std::sin(w0) / (2.0f * Q);

    float b0 = 1.0f + alpha * A;
    float b1 = -2.0f * std::cos(w0);
    float b2 = 1.0f - alpha * A;
    float a0 = 1.0f + alpha / A;
    float a1 = -2.0f * std::cos(w0);
    float a2 = 1.0f - alpha / A;

    b0 /= a0;
    b1 /= a0;
    b2 /= a0;
    a1 /= a0;
    a2 /= a0;

    std::complex<float> expMinusJw(std::cos(-w), std::sin(-w));
    std::complex<float> expMinus2Jw(std::cos(-2.0f * w), std::sin(-2.0f * w));

    std::complex<float> num = b0 + b1 * expMinusJw + b2 * expMinus2Jw;
    std::complex<float> den = 1.0f + a1 * expMinusJw + a2 * expMinus2Jw;

    return num / den;
}