#include "PluginProcessor.h"
#include "PluginEditor.h"

Fm1Processor::Fm1Processor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (*this, nullptr, "FM1", layout()),
      fm1 (fm1_new())
{
    master = params.getRawParameterValue ("master");
    channel = params.getRawParameterValue ("channel");
    clock = params.getRawParameterValue ("clock");
    mpe = params.getRawParameterValue ("mpe");
}

Fm1Processor::~Fm1Processor()
{
    fm1_free (fm1);
}

juce::AudioProcessorValueTreeState::ParameterLayout Fm1Processor::layout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout l;

    l.add (std::make_unique<AudioParameterFloat> (ParameterID { "master", 1 }, "Master",
                                                  NormalisableRange<float> (0.0f, 1.0f), 900.0f / 1023.0f,
                                                  AudioParameterFloatAttributes()
                                                      .withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v * 100.0f)) + " %"; })
                                                      .withValueFromStringFunction ([] (const String& s) { return s.getFloatValue() / 100.0f; })));

    l.add (std::make_unique<AudioParameterInt> (ParameterID { "channel", 1 }, "MIDI channel", 0, 16, 0,
                                                AudioParameterIntAttributes()
                                                    .withStringFromValueFunction ([] (int v, int) { return v == 0 ? String ("as sent") : String (v); })));

    // the emulated CPU clock: 240 MHz as on the chip; changing it reboots the firmware
    StringArray names;
    for (auto c : clocks)
        names.add (String (c) + " MHz");
    l.add (std::make_unique<AudioParameterChoice> (ParameterID { "clock", 1 }, "CPU clock", names, 2,
                                                   AudioParameterChoiceAttributes().withAutomatable (false)));

    // MPE in: with a MIDI channel set, folded into it (the last note's expression); "as sent",
    // passed as it comes, for firmwares that speak MPE themselves
    l.add (std::make_unique<AudioParameterBool> (ParameterID { "mpe", 1 }, "MPE", false,
                                                 AudioParameterBoolAttributes().withAutomatable (false)));
    return l;
}

bool Fm1Processor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void Fm1Processor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    setLatencySamples (fm1_prepare (fm1, sampleRate, samplesPerBlock));
    scratch.setSize (2, samplesPerBlock);
}

void Fm1Processor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    fm1_set_master (fm1, master->load());
    fm1_set_clock (fm1, clocks[juce::jlimit (0, 4, (int) clock->load())]);
    const int ch = (int) channel->load();
    const bool fold_ = mpe->load() > 0.5f && ch != 0;
    if (fold_ != folding)
    {
        folding = fold_;
        fold.reset();
    }

    for (const auto meta : midi)
    {
        const auto* b = meta.data;
        if (meta.numBytes < 1)
            continue;
        const uint8_t status = b[0];
        int len = 0;
        switch (status & 0xf0)
        {
            case 0x80: case 0x90: case 0xa0: case 0xb0: case 0xe0: len = 3; break;
            case 0xc0: case 0xd0: len = 2; break;
            default: break;
        }
        if (len == 0 || meta.numBytes < len)
            continue;
        const uint8_t d1 = len > 1 ? b[1] : 0, d2 = len > 2 ? b[2] : 0;
        if (folding)
        {
            fold.process (status, d1, d2, len, ch - 1, [this, at = meta.samplePosition] (uint8_t st, uint8_t a, uint8_t c, int l)
                          { fm1_midi (fm1, at, st, a, c, (uint8_t) l); });
            continue;
        }
        const uint8_t s = ch != 0 ? (uint8_t) ((status & 0xf0) | (ch - 1)) : status;
        fm1_midi (fm1, meta.samplePosition, s, d1, d2, (uint8_t) len);
    }

    const int n = buffer.getNumSamples();
    if (buffer.getNumChannels() >= 2)
    {
        fm1_process (fm1, buffer.getWritePointer (0), buffer.getWritePointer (1), n);
    }
    else if (buffer.getNumChannels() == 1)
    {
        if (scratch.getNumSamples() < n)
            scratch.setSize (2, n, false, false, true);
        fm1_process (fm1, buffer.getWritePointer (0), scratch.getWritePointer (1), n);
    }
}

juce::AudioProcessorEditor* Fm1Processor::createEditor()
{
    return new Fm1Editor (*this);
}

void Fm1Processor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = params.copyState();
    state.setProperty ("editorWidth", editorWidth, nullptr);

    auto buf = fm1_get_state (fm1);
    if (buf.data != nullptr && buf.len > 0)
        state.setProperty ("fm1save", juce::var (juce::MemoryBlock (buf.data, buf.len)), nullptr);
    else
        state.removeProperty ("fm1save", nullptr);
    fm1_buf_free (buf);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void Fm1Processor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (params.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    editorWidth = (int) state.getProperty ("editorWidth", 0);

    if (auto* mb = state.getProperty ("fm1save").getBinaryData())
        fm1_set_state (fm1, static_cast<const uint8_t*> (mb->getData()), mb->getSize());

    state.removeProperty ("fm1save", nullptr);
    state.removeProperty ("editorWidth", nullptr);
    params.replaceState (state);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new Fm1Processor();
}
