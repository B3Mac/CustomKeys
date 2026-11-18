#include "PluginEditor.h"

namespace {
    namespace Colours
    {
        const juce::Colour background { 0xff17181c };
        const juce::Colour panel      { 0xff23252b };
        const juce::Colour text       { 0xffe8e6e1 };
        const juce::Colour quietText  { 0xff9a9890 };
        const juce::Colour accent     { 0xffe0a64a }; // CustomKeys amber
        const juce::Colour warning    { 0xffe0604a };
    }

    constexpr int headerHeight = 64;
    constexpr int instrumentBarHeight = 44;
    constexpr int lowestKey = 21;  // A0
    constexpr int highestKey = 108; // C8
    constexpr int whiteKeyCount = 52;
}

CustomKeysEditor::CustomKeysEditor (CustomKeysProcessor& p)
    : AudioProcessorEditor (p),
      processor (p),
      keyboard (p.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    for (auto* label : { &latencyLabel, &loadLabel, &voicesLabel })
    {
        label->setColour (juce::Label::textColourId, Colours::text);
        label->setFont (juce::FontOptions (14.0f));
        label->setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (*label);
    }

    instrumentLabel.setColour (juce::Label::textColourId, Colours::text);
    instrumentLabel.setFont (juce::FontOptions (15.0f));
    instrumentLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (instrumentLabel);

    for (auto* button : { &resetPeaksButton, &panicButton, &loadButton })
    {
        button->setColour (juce::TextButton::buttonColourId, Colours::panel);
        button->setColour (juce::TextButton::textColourOffId, Colours::text);
        addAndMakeVisible (*button);
    }

    resetPeaksButton.onClick = [this] { processor.renderStats().resetPeaks(); };
    panicButton.onClick = [this] { processor.panic(); keyboard.repaint(); };
    loadButton.onClick = [this] { chooseInstrument(); };

    keyboard.setAvailableRange (lowestKey, highestKey);
    keyboard.setOctaveForMiddleC (4); // label middle C as C4, matching most keyboards and DAWs
    keyboard.setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, Colours::accent.withAlpha (0.7f));
    keyboard.setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, Colours::accent.withAlpha (0.25f));
    keyboard.setWantsKeyboardFocus (false);
    addAndMakeVisible (keyboard);

    setResizable (true, true);
    setResizeLimits (720, 264, 2400, 640);
    setSize (1100, 344);

    startTimerHz (15);
}

CustomKeysEditor::~CustomKeysEditor()
{
    stopTimer();
}

void CustomKeysEditor::paint (juce::Graphics& g)
{
    g.fillAll (Colours::background);

    auto header = getLocalBounds().removeFromTop (headerHeight);
    g.setColour (Colours::panel);
    g.fillRect (header);

    g.setColour (Colours::accent);
    g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText ("CustomKeys", header.reduced (16, 0).removeFromLeft (170), juce::Justification::centredLeft);

    g.setColour (Colours::panel.darker (0.3f));
    g.fillRect (getLocalBounds().withTrimmedTop (headerHeight).removeFromTop (instrumentBarHeight));
}

void CustomKeysEditor::resized()
{
    auto bounds = getLocalBounds();
    auto header = bounds.removeFromTop (headerHeight).reduced (16, 14);

    header.removeFromLeft (170); // title
    panicButton.setBounds (header.removeFromRight (80));
    header.removeFromRight (8);
    resetPeaksButton.setBounds (header.removeFromRight (110));
    header.removeFromRight (16);

    const int third = header.getWidth() / 3;
    latencyLabel.setBounds (header.removeFromLeft (third));
    loadLabel.setBounds (header.removeFromLeft (third));
    voicesLabel.setBounds (header);

    auto bar = bounds.removeFromTop (instrumentBarHeight).reduced (16, 7);
    loadButton.setBounds (bar.removeFromLeft (130));
    bar.removeFromLeft (14);
    instrumentLabel.setBounds (bar);

    keyboard.setBounds (bounds);
    keyboard.setKeyWidth (static_cast<float> (bounds.getWidth()) / whiteKeyCount);
}

void CustomKeysEditor::chooseInstrument()
{
    auto start = processor.instrumentFile().getParentDirectory();
    if (! start.isDirectory())
        start = juce::File::getSpecialLocation (juce::File::userHomeDirectory);

    chooser = std::make_unique<juce::FileChooser> ("Choose a Nord piano", start, "*.npno");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();
                              if (file != juce::File())
                                  processor.loadInstrument (file);
                          });
}

bool CustomKeysEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && CustomKeysProcessor::canLoad (juce::File (files[0]));
}

void CustomKeysEditor::filesDropped (const juce::StringArray& files, int, int)
{
    if (! files.isEmpty())
        processor.loadInstrument (juce::File (files[0]));
}

void CustomKeysEditor::timerCallback()
{
    const auto instrumentText = processor.isLoading()
        ? processor.instrumentStatus() + "  " + juce::String (juce::roundToInt (processor.loadProgress() * 100.0f)) + "%"
        : processor.instrumentStatus();
    instrumentLabel.setText (instrumentText, juce::dontSendNotification);

    const double sampleRate = processor.currentSampleRate();
    const int block = processor.lastBlockSize();

    if (sampleRate > 0.0 && block > 0)
        latencyLabel.setText (juce::String (block) + " samples = "
                                  + juce::String (1000.0 * block / sampleRate, 1) + " ms @ "
                                  + juce::String (sampleRate / 1000.0, 1) + " kHz",
                              juce::dontSendNotification);
    else
        latencyLabel.setText ("Audio not running", juce::dontSendNotification);

    const auto stats = processor.renderStats().snapshot();
    loadLabel.setText ("DSP " + juce::String (juce::roundToInt (stats.lastLoad * 100.0f)) + "% (peak "
                           + juce::String (juce::roundToInt (stats.peakLoad * 100.0f)) + "%)"
                           + (stats.overloads > 0 ? ", " + juce::String (stats.overloads) + " overloads" : juce::String()),
                       juce::dontSendNotification);
    loadLabel.setColour (juce::Label::textColourId, stats.overloads > 0 ? Colours::warning : Colours::text);

    voicesLabel.setText ("Voices " + juce::String (stats.activeVoices) + " (peak " + juce::String (stats.peakVoices) + ")"
                             + (stats.steals > 0 ? ", " + juce::String (stats.steals) + " stolen" : juce::String()),
                         juce::dontSendNotification);
}
