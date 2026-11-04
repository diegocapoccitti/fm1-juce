#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Fm1Bridge.h"
#include "MpeFold.h"

/** The FM-1 as an instrument: the host's MIDI into the emulated MIDI IN jack, the machine's
    44.1 kHz output resampled to the host's rate (both in the Rust core), the firmware package
    and the flash sectors it wrote saved with the project. */
class Fm1Processor final : public juce::AudioProcessor
{
public:
    Fm1Processor();
    ~Fm1Processor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "FM-1"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool supportsMPE() const override { return true; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    const Fm1* machine() const noexcept { return fm1; }
    juce::AudioProcessorValueTreeState params;

    static constexpr int clocks[] = { 120, 160, 240, 320, 400 };

    /** The editor's size, kept with the project. */
    int editorWidth = 0;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout layout();

    Fm1* fm1 = nullptr;
    std::atomic<float>* master = nullptr;
    std::atomic<float>* channel = nullptr;
    std::atomic<float>* clock = nullptr;
    std::atomic<float>* mpe = nullptr;
    MpeFold fold;
    bool folding = false;
    juce::AudioBuffer<float> scratch;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Fm1Processor)
};
