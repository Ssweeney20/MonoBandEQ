#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <memory>

ResponseCurveComponent::ResponseCurveComponent(AudioPluginAudioProcessor& p) : juce::AudioProcessorEditor(p), processorRef(p), leftChannelFifo(&processorRef.leftChannelFifo){
    const auto& params = processorRef.getParameters();
    for (auto param: params) {
        param->addListener(this);
    }

    leftChannelFFTDataGenerator.changeOrder(FFTOrder::order2048);
    monoBuffer.setSize(1, leftChannelFFTDataGenerator.getFFTSize());

    startTimer(60);
}

ResponseCurveComponent::~ResponseCurveComponent() {
    const auto& params = processorRef.getParameters();
    for (auto param: params) {
        param->removeListener(this);
    }
}


void ResponseCurveComponent::parameterValueChanged(int parameterIndex, float newValue) {
    parametersChanged.set(true);
}

void ResponseCurveComponent::timerCallback() {

    juce::AudioBuffer<float> tempIncomingBuffer;

    while (leftChannelFifo->getNumCompleteBuffersAvailable() > 0) {
        if (leftChannelFifo->getAudioBuffer(tempIncomingBuffer) ) {
            auto size = tempIncomingBuffer.getNumSamples();
            juce::FloatVectorOperations::copy(monoBuffer.getWritePointer(0, 0), monoBuffer.getReadPointer(0, size), monoBuffer.getNumSamples() - size );

            juce::FloatVectorOperations::copy(monoBuffer.getWritePointer(0, (monoBuffer.getNumSamples() - size)), tempIncomingBuffer.getReadPointer(0, 0 ), size);

            leftChannelFFTDataGenerator.produceFFTDataForRendering(monoBuffer, -48.f);
        }
    }

    auto fftBounds = getAnalysisArea().toFloat();
    auto fftSize = leftChannelFFTDataGenerator.getFFTSize();
    auto binWidth = processorRef.getSampleRate() / (double) fftSize;

    while (leftChannelFFTDataGenerator.getNumAvailableFFTDataBlocks() > 0){
        std::vector<float> fftData;
        if (leftChannelFFTDataGenerator.getFFTData(fftData)) {
            pathProducer.generatePath(fftData, fftBounds, fftSize, binWidth, -48.f);
        }
    }

    while (pathProducer.getNumPathsAvailable()) {
        pathProducer.getPath(leftChannelFFTPath);
    }



    if (parametersChanged.compareAndSetBool(false,true)) {
        auto chainSettings = getChainSettings(processorRef.apvts);
        auto peakCoefficients = makePeakFilter(chainSettings, processorRef.getSampleRate());
        updateCoefficients(monoChain.get<ChainPositions::Peak>().coefficients, peakCoefficients);

        //repaint();
    }

    repaint();
}
void ResponseCurveComponent::paint (juce::Graphics& g)
{
    using namespace juce;
    // (Our component is opaque, so we must completely fill the background with a solid colour)
    g.fillAll (Colours::black);
    g.drawImage(background, getLocalBounds().toFloat());

    //auto responseArea = getLocalBounds();
    //auto responseArea = getRenderArea();
    auto responseArea = getAnalysisArea();

    auto width = responseArea.getWidth();

    auto&peak = monoChain.get<ChainPositions::Peak>();
    auto sampleRate = processorRef.getSampleRate();

    std::vector<double> magnitudes;
    magnitudes.resize(width);

    for (int i = 0; i < width; i++) {

        double mag = 1.f;
        auto freq = mapToLog10(double(i) / double(width), 20.0, 20000.0);

        if (! monoChain.isBypassed<ChainPositions::Peak>()) {
            mag *= peak.coefficients->getMagnitudeForFrequency(freq,sampleRate);
        }
        magnitudes[i] = Decibels::gainToDecibels(mag);
    }

    Path responseCurve;

    const double outputMin = responseArea.getBottom();
    const double outputMax = responseArea.getY();
    auto map = [outputMin, outputMax](double input) {
        return jmap(input, -24.0, 24.0, outputMin, outputMax);
    };
    responseCurve.startNewSubPath(responseArea.getX(), map(magnitudes.front()));

    for (size_t i = 1; i < magnitudes.size(); ++i) {
        responseCurve.lineTo(responseArea.getX() + i, map(magnitudes[i]));
    }

    leftChannelFFTPath.applyTransform(AffineTransform().translation(responseArea.getX(),responseArea.getY()));

    g.setColour(Colours::lightgoldenrodyellow);
    g.strokePath(leftChannelFFTPath, PathStrokeType(1));

    g.setColour(Colours::orange);
    g.drawRoundedRectangle(getRenderArea().toFloat(), 4.f, 1.f);
    g.setColour(Colours::white);
    g.strokePath(responseCurve, PathStrokeType (2.f));
}

void ResponseCurveComponent::resized() {
    using namespace juce;
    background = Image(Image::PixelFormat::RGB, getWidth(), getHeight(), true);
    Graphics g(background);

    Array<float> freqs{
        20, 50, 100,
        200, 500, 1000,
        2000, 5000, 10000,
        20000
    };

    auto renderArea = getAnalysisArea();
    auto left = renderArea.getX();
    auto right = renderArea.getRight();
    auto top = renderArea.getY();
    auto bottom = renderArea.getBottom();
    auto width = renderArea.getWidth();

    std::vector<float> xs;
    for( auto f : freqs )
    {
        auto normX = mapFromLog10(f, 20.f, 20000.f);
        xs.push_back(left + width * normX);
    }

    g.setColour(Colours::dimgrey);
    for (auto x : xs) {
        //auto normx = mapFromLog10(freq, 20.f, 20000.f);
        g.drawVerticalLine(x, top, bottom);
        //g.drawVerticalLine(getWidth() * normx, 0.f, getHeight());
    }

    Array<float> gain{
        -24, -12, 0 , 12 , 24
    };

    for (auto gDb : gain) {
        auto normy = jmap(gDb, -24.f, 24.f, float(bottom), float(top));
        //g.drawHorizontalLine(normy, 0, getWidth());
        g.setColour(Colours::darkgrey);
        g.drawHorizontalLine(normy,left, right);
    }
    //g.drawRect(getAnalysisArea());

    g.setColour(Colours::white);
    const int fontHeight = 10;
    g.setFont(fontHeight);

    for (int i = 0; i < freqs.size(); ++i) {
        auto f = freqs[i];
        auto x  = xs[i];

        bool addK = false;
        String str;
        if( f > 999.f )
        {
            addK = true;
            f /= 1000.f;
        }

        str << f;
        if (addK)
            str << "k";
        str << "Hz";

        auto textWidth = g.getCurrentFont().getStringWidth(str);

        Rectangle<int> r;
        r.setSize(textWidth, fontHeight);
        r.setCentre(x, 0);
        r.setY(1);

        g.drawFittedText(str, r, juce::Justification::centred, 1);
    }
    for (auto gDb : gain) {
        auto normy = jmap(gDb, -24.f, 24.f, float(bottom), float(top));
        String str;
        if (gDb > 0) {
            str << "+";
        }
        str << gDb;

        auto textWidth = g.getCurrentFont().getStringWidth(str);
        Rectangle<int> r;
        r.setSize(textWidth, fontHeight);
        r.setX(getWidth() - textWidth);
        r.setCentre(r.getCentreX(), normy);
        g.setColour(Colours::white);
        g.drawFittedText(str, r, juce::Justification::centred, 1);

        str.clear();
        str << (gDb - 24.f);
        r.setX(1);
        textWidth = g.getCurrentFont().getStringWidth(str);
        r.setSize(textWidth, fontHeight);
        g.setColour(Colours::white);
        g.drawFittedText(str, r, juce::Justification::centred, 1);

    }

}

juce::Rectangle<int> ResponseCurveComponent::getRenderArea() {
    auto bounds = getLocalBounds();
    //bounds.reduce(10,8);
    bounds.removeFromTop(12);
    bounds.removeFromBottom(2);
    bounds.removeFromLeft(20);
    bounds.removeFromRight(20);
    return bounds;
}

juce::Rectangle<int> ResponseCurveComponent::getAnalysisArea() {
    auto bounds = getRenderArea();
    bounds.removeFromTop(4);
    bounds.removeFromBottom(4);
    return bounds;
}


//==============================================================================
AudioPluginAudioProcessorEditor::AudioPluginAudioProcessorEditor (AudioPluginAudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p),
    responseCurveComponent(processorRef),
    peakFreqSliderAttachment(processorRef.apvts, "Peak Freq", peakFreqSlider),
    peakGainSliderAttachment(processorRef.apvts, "Peak Gain", peakGainSlider),
    peakQualitySliderAttachment(processorRef.apvts, "Peak Quality", peakQualitySlider)
{
    juce::ignoreUnused (processorRef);
    // Make sure that before the constructor has finished, you've set the
    // editor's size to whatever you need it to be.

    for (auto *comp : getComps()) {
        addAndMakeVisible(comp);
    }

    setSize (600, 400);
}

AudioPluginAudioProcessorEditor::~AudioPluginAudioProcessorEditor()
{

}

//==============================================================================
void AudioPluginAudioProcessorEditor::paint (juce::Graphics& g)
{
    using namespace juce;
    // (Our component is opaque, so we must completely fill the background with a solid colour)
    g.fillAll (Colours::black);

}

void AudioPluginAudioProcessorEditor::resized()
{
    // This is generally where you'll want to lay out the positions of any
    // subcomponents in your editor..
    auto bounds = getLocalBounds();
    auto responseArea = bounds.removeFromTop(bounds.getHeight() * 0.33);

    gainSliderLabel.setText("Gain", juce::dontSendNotification);
    gainSliderLabel.setJustificationType(juce::Justification::centred);
    gainSliderLabel.setBounds(250, 260, 100, 30);
    gainSliderLabel.setFont(juce::Font(16.5f, juce::Font::bold));
    addAndMakeVisible(gainSliderLabel);

    freqSliderLabel.setText("Frequency", juce::dontSendNotification);
    freqSliderLabel.setJustificationType(juce::Justification::centred);
    freqSliderLabel.setBounds(55, 260, 100, 30);
    freqSliderLabel.setFont(juce::Font(16.5f, juce::Font::bold));
    addAndMakeVisible(freqSliderLabel);

    qualitySliderLabel.setText("Quality", juce::dontSendNotification);
    qualitySliderLabel.setJustificationType(juce::Justification::centred);
    qualitySliderLabel.setBounds(455, 260, 100, 30);
    qualitySliderLabel.setFont(juce::Font(16.5f, juce::Font::bold));
    addAndMakeVisible(qualitySliderLabel);

    responseCurveComponent.setBounds(responseArea);

    peakFreqSlider.setBounds(30, 200, 150, 150);
    peakGainSlider.setBounds(225, 200, 150, 150);
    peakQualitySlider.setBounds(430, 200, 150, 150);
}

std::vector<juce::Component*> AudioPluginAudioProcessorEditor::getComps() {
    return
    {
        &peakFreqSlider,
        &peakGainSlider,
        &peakQualitySlider,
        &responseCurveComponent
    };
}
