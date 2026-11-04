// The FM-1's front panel in design units: the pixels of the photographed top view
// (Resources/fm1_top.png, 1478 x 898), measured off it. MASTER / SELECT / PRESETS / ALGORITHM
// top left with OCT- / OCT+ under them, the screen, KNOB1-4 over two rows of six buttons, and
// the 27 pill keys (F3..G5) in two rows at the bottom. Key-matrix ids as in fm1-live's panel.rs.
#pragma once

#include <juce_graphics/juce_graphics.h>
#include <array>
#include <vector>

namespace layout
{
constexpr float W = 1478.0f, H = 898.0f;

/** The black bezel, and the LCD (240 x 240 pixels) filling its glass up to the rounded corners. */
const juce::Rectangle<float> bezel { 390.0f, 83.0f, 342.0f, 340.0f };
const juce::Rectangle<float> lcd { 398.0f, 90.0f, 326.0f, 326.0f };
constexpr float lcdCorner = 24.0f;

// recessed trays (the rendered body, when there is no photo)
const juce::Rectangle<float> octTray { 100.0f, 386.0f, 229.0f, 73.0f };
const juce::Rectangle<float> buttonTray { 785.0f, 256.0f, 580.0f, 204.0f };
const juce::Rectangle<float> keyTray { 70.0f, 508.0f, 1340.0f, 316.0f };

struct Knob
{
    const char* label;
    int enc;            // matrix encoder, -1 the MASTER pot
    float cx, cy, r;    // the whole knob, as it stands on the panel
    float tx, ty, tr;   // its flat top face in the photo: the camera sees it off the base's centre
};

/** Encoders, and the pot last. */
const std::array<Knob, 8> knobs { {
    { "SELECT", 0, 292.0f, 156.0f, 33.0f, 283.0f, 153.0f, 22.5f },
    { "PRESETS", 6, 140.0f, 306.0f, 33.0f, 129.0f, 305.0f, 21.5f },
    { "ALGORITHM", 1, 292.0f, 307.0f, 33.0f, 282.0f, 306.0f, 21.5f },
    { "KNOB1", 2, 830.0f, 156.0f, 33.0f, 830.0f, 153.0f, 21.5f },
    { "KNOB2", 3, 992.0f, 156.0f, 33.0f, 993.0f, 153.0f, 21.5f },
    { "KNOB3", 4, 1154.0f, 156.0f, 33.0f, 1155.0f, 153.0f, 21.5f },
    { "KNOB4", 5, 1316.0f, 156.0f, 33.0f, 1319.0f, 151.0f, 21.5f },
    { "MASTER", -1, 140.0f, 156.0f, 33.0f, 129.0f, 153.0f, 22.5f },
} };
constexpr int potIndex = 7;
/** Where the photographed MASTER points (radians from straight up). */
constexpr float potPhotoAngle = -2.09f;

struct Button
{
    const char* label;
    int id; // key-matrix id
    juce::Rectangle<float> r;
};

inline std::vector<Button> buttons()
{
    std::vector<Button> v { { "OCT-", 0, { 115, 396, 87, 54 } }, { "OCT+", 1, { 228, 396, 87, 54 } } };
    const char* rows[2][6] = { { "FX", "SEL", "ENV", "LFO", "EDIT", "GLO" },
                               { "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC" } };
    for (int r = 0; r < 2; ++r)
        for (int i = 0; i < 6; ++i)
            v.push_back ({ rows[r][i], 2 + r * 6 + i, { 811.0f + (float) i * 90.6f, 273.0f + (float) r * 90.0f, 78.0f, 78.0f } });
    return v;
}

struct Key
{
    int id; // key-matrix id (14..40)
    juce::Rectangle<float> r;
    juce::Rectangle<float> stripe; // its light bar
    bool black;
};

/** F3..G5: 16 white keys along the bottom row, the 11 black ones in the row above. */
inline std::vector<Key> keys()
{
    std::vector<Key> v;
    const int whites[] = { 0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26 };
    for (int i = 0; i < 16; ++i)
    {
        const float x = 92.0f + (float) i * 81.47f;
        v.push_back ({ 14 + whites[i], { x, 666.0f, 75.0f, 137.0f }, { x + 33.5f, 699.0f, 8.0f, 74.0f }, false });
    }
    const int blacks[] = { 1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25 };
    const float centres[] = { 169.0f, 251.0f, 332.5f, 495.5f, 576.0f, 738.5f, 820.5f, 901.5f, 1063.5f, 1145.5f, 1307.5f };
    for (int i = 0; i < 11; ++i)
    {
        const float cx = centres[i];
        const bool tall = i == 10; // the last one has a long bar
        v.push_back ({ 14 + blacks[i], { cx - 37.5f, 530.0f, 75.0f, 132.0f },
                       { cx - 4.0f, tall ? 560.0f : 563.0f, 8.0f, tall ? 73.0f : 39.0f }, true });
    }
    return v;
}

/** What a point is: -1 nothing, 0..7 a knob (7 the pot), 100 + id a button or key. */
inline int hit (juce::Point<float> p)
{
    for (int i = 0; i < (int) knobs.size(); ++i)
        if (p.getDistanceFrom ({ knobs[(size_t) i].cx, knobs[(size_t) i].cy }) <= knobs[(size_t) i].r + 8.0f)
            return i;
    for (auto& b : buttons())
        if (b.r.expanded (3.0f).contains (p))
            return 100 + b.id;
    for (auto& k : keys())
        if (k.r.expanded (3.0f).contains (p))
            return 100 + k.id;
    return -1;
}

inline bool isNoteKey (int id) { return id >= 14 && id <= 40; }

// colours of the black FM-1, for the rendered body
const juce::Colour room { 0xff18191b }, bodyTop { 0xff6a6b6e }, bodyBot { 0xff5e5f62 },
    tray { 0xff2e2f31 }, silk { 0xffd6d4cf }, btnTop { 0xff232426 }, btnBot { 0xff1a1b1c },
    btnInk { 0xffe8e6e1 }, keyTop { 0xff222325 }, keyBot { 0xff19191b }, keyLine { 0xffeeeeee },
    amber { 0xfff2a33a }, green { 0xff62d36e };
} // namespace layout
