#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "PluginProcessor.h"
#include "PanelRenderer.h"

/** The front panel, played with the mouse: keys and buttons held while pressed, a drag across
    the keys a glissando, knobs dragged up / down or scrolled, MASTER the host's "Master". */
class PanelView final : public juce::Component
{
public:
    explicit PanelView (Fm1Processor&);
    ~PanelView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMove (const juce::MouseEvent&) override;

    /** Fetch the machine's screen and LEDs (from the editor's timer). */
    void refresh();
    Fm1Status status {};

private:
    juce::Point<float> toPanel (juce::Point<float>) const;
    void key (int id, bool down);
    void turn (int knob, int clicks);
    void setMaster (float v);

    Fm1Processor& proc;
    PanelSkin skin;
    juce::Image lcd { juce::Image::ARGB, 240, 240, true };
    std::array<uint32_t, 240 * 240> pixels {};
    std::array<float, 41> leds {};
    bool down[64] {};
    double pressedAt[64] {};
    static constexpr double minHoldMs = 60.0;
    int encClicks[8] {};
    struct Touch
    {
        int grab = -1, held = -1;
        float lastY = 0, acc = 0;
    };
    std::array<Touch, 10> touches;
    Touch& touch (const juce::MouseEvent&);
    void press (int id, bool on);
    float wheelAcc = 0;
};

class Fm1Editor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit Fm1Editor (Fm1Processor&);
    ~Fm1Editor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;

private:
    void timerCallback() override;
    void autosave();
    void loadBundledFirmware();
    juce::AudioProcessorValueTreeState& params() { return proc.params; }
    juce::String lastSnapshot;
    double saveDueAt = 0, lastSaveAt = 0;
    juce::Component::SafePointer<juce::DocumentWindow> styledWindow;
    void loadFile();
    void exportFile();
    void showMessage (const juce::String&, bool sticky);

    Fm1Processor& proc;
    PanelView panel;
    juce::TextButton loadButton { "Load firmware / save..." }, exportButton { "Export save..." }, mpeButton { "MPE" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> mpeAttach;
    juce::Label nameLabel, channelLabel { {}, "MIDI ch" }, clockLabel { {}, "CPU" }, statusLabel;
    juce::ComboBox channelBox, clockBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> channelAttach, clockAttach;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::String message;
    double messageUntil = 0;
    bool busy = false;

   #if JUCE_IOS
    static constexpr int barHeight = 44;
   #else
    static constexpr int barHeight = 38;
   #endif
    juce::BorderSize<int> safe;
    int separators[2] {};
    juce::TooltipWindow tooltips { this, 600 };
    juce::ComponentBoundsConstrainer constrainer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Fm1Editor)
};
