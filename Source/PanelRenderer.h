// The panel's look: a background rendered once (body, trays, bezel, silkscreen, shadows, the
// knobs' wells) at the window's pixel density, and the parts that move drawn over it each frame
// (the screen, the knobs' rotors, buttons and keys with their LEDs).
#pragma once

#include <juce_graphics/juce_graphics.h>
#include "PanelLayout.h"

class PanelSkin
{
public:
    /** Render for `pixelsPerUnit` device pixels per design unit (if it changed). */
    void prepare (float pixelsPerUnit);

    /** Everything, in design units (the caller's transform scales them to the window). */
    struct Frame
    {
        const juce::Image* lcd = nullptr; // 240 x 240
        bool lcdOn = false;
        const float* leds = nullptr;      // 41 duties
        const bool* down = nullptr;       // 41 keys held
        float knobAngle[8] {};            // radians, 0 = pointer up
    };
    void paint (juce::Graphics&, const Frame&) const;

private:
    juce::Image renderBackground() const;
    juce::Image renderRotor (const layout::Knob&) const;
    juce::Image renderLight (const layout::Knob&) const;
    juce::Image renderLcdGrid() const;

    void paintButton (juce::Graphics&, const layout::Button&, bool down, float led) const;
    void paintKey (juce::Graphics&, const layout::Key&, bool down, float led) const;

    /** A photographed top face split into the room's light, which stays put, and the face's own
        markings (pointer, machining), which turn: turned faces are relit each time they move. */
    struct Face
    {
        int n = 0;                  // n x n device pixels
        std::vector<float> detail;  // rgb / light
        std::vector<float> light;   // the light falling on each pixel
        std::vector<uint8_t> alpha; // the disc's edge
        float radius = 0;           // in pixels, where the markings may be sampled
        mutable juce::Image turned;
        mutable float angle = 1.0e9f;
    };
    void splitFace (Face&, const juce::Image&) const;
    const juce::Image& turnFace (const Face&, float angle) const;

    float ppu = 0.0f;
    Face faces[8];
    juce::Image background, lcdGrid;
    juce::Image rotor[8], light[8];
    juce::Image photo; // a photographed top view (Resources/fm1_top.*), when there is one
    std::vector<layout::Button> buttons = layout::buttons();
    std::vector<layout::Key> keys = layout::keys();
};
