#include "PluginEditor.h"
#if FM1_BUNDLED_FIRMWARE
 #include "FirmwareData.h"
#endif
#if JucePlugin_Build_Standalone
 #include <juce_audio_devices/juce_audio_devices.h>
 #include <juce_audio_utils/juce_audio_utils.h>
 #include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#endif

using namespace juce;

// ------------------------------------------------------------------------------- panel --

PanelView::PanelView (Fm1Processor& p) : proc (p)
{
    setOpaque (true);
}

PanelView::~PanelView()
{
    // a window closed with a key still held (or its release still pending) lets it go
    for (int id = 0; id < 64; ++id)
        if (down[id] || pressedAt[id] > 0)
            fm1_key (proc.machine(), id, 0);
}

Point<float> PanelView::toPanel (Point<float> p) const
{
    const float k = (float) getWidth() / layout::W;
    return p / k;
}

void PanelView::resized()
{
    const float k = (float) getWidth() / layout::W;
    float display = 1.0f;
    if (auto* d = Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds()))
        display = (float) d->scale;
    skin.prepare (k * display);
}

void PanelView::refresh()
{
    fm1_view (proc.machine(), pixels.data(), leds.data(), &status);
    {
        Image::BitmapData d (lcd, Image::BitmapData::writeOnly);
        for (int y = 0; y < 240; ++y)
        {
            auto* row = reinterpret_cast<PixelARGB*> (d.getLinePointer (y));
            for (int x = 0; x < 240; ++x)
            {
                const auto v = pixels[(size_t) (y * 240 + x)];
                row[x].setARGB (255, (uint8) (v >> 16), (uint8) (v >> 8), (uint8) v);
            }
        }
    }
    repaint();
}

void PanelView::paint (Graphics& g)
{
    g.fillAll (layout::room);
    g.addTransform (AffineTransform::scale ((float) getWidth() / layout::W));

    PanelSkin::Frame f;
    f.lcd = &lcd;
    f.lcdOn = status.booted != 0;
    f.leds = leds.data();
    f.down = down;
    for (int i = 0; i < 7; ++i)
        f.knobAngle[i] = degreesToRadians ((float) encClicks[i] * 18.0f);
    const float pot = proc.params.getRawParameterValue ("master")->load();
    f.knobAngle[layout::potIndex] = degreesToRadians (-135.0f + 270.0f * pot);
    skin.paint (g, f);
}

void PanelView::key (int id, bool isDown)
{
    if (id < 0 || id >= 64 || down[id] == isDown)
        return;
    down[id] = isDown;
    repaint();
    const double now = Time::getMillisecondCounterHiRes();
    if (isDown)
    {
        pressedAt[id] = now;
        fm1_key (proc.machine(), id, 1);
        return;
    }
    // the firmware scans its key matrix every few ms: a click shorter than a scan would be lost,
    // so a release comes at least minHoldMs after its press
    const int wait = (int) std::ceil (minHoldMs - (now - pressedAt[id]));
    if (wait <= 0)
    {
        fm1_key (proc.machine(), id, 0);
        return;
    }
    Timer::callAfterDelay (wait, [safe = Component::SafePointer<PanelView> (this), id]
                           {
                               if (safe != nullptr && ! safe->down[id])
                                   fm1_key (safe->proc.machine(), id, 0);
                           });
}

void PanelView::turn (int knob, int clicks)
{
    encClicks[knob] += clicks;
    fm1_enc (proc.machine(), layout::knobs[(size_t) knob].enc, clicks);
    repaint();
}

void PanelView::setMaster (float v)
{
    if (auto* p = proc.params.getParameter ("master"))
        p->setValueNotifyingHost (jlimit (0.0f, 1.0f, v));
    repaint();
}

void PanelView::mouseMove (const MouseEvent& e)
{
    const int h = layout::hit (toPanel (e.position));
    setMouseCursor (h < 0 ? MouseCursor::NormalCursor
                          : h < 100 ? MouseCursor::UpDownResizeCursor : MouseCursor::PointingHandCursor);
}

PanelView::Touch& PanelView::touch (const MouseEvent& e)
{
    // each finger its own: chords on the keys, a knob turned while a key is held
    return touches[(size_t) jlimit (0, (int) touches.size() - 1, e.source.getIndex())];
}

void PanelView::press (int id, bool on)
{
    // a key is held while any finger holds it
    int fingers = 0;
    for (auto& t : touches)
        if (t.held == id)
            ++fingers;
    if (on ? fingers == 1 : fingers == 0)
        key (id, on);
}

void PanelView::mouseDown (const MouseEvent& e)
{
    auto& t = touch (e);
    const auto p = toPanel (e.position);
    t.grab = layout::hit (p);
    t.lastY = p.y;
    t.acc = 0;
    t.held = -1;
    if (t.grab >= 100)
    {
        t.held = t.grab - 100;
        press (t.held, true);
    }
    else if (t.grab == layout::potIndex)
    {
        if (auto* prm = proc.params.getParameter ("master"))
            prm->beginChangeGesture();
    }
}

void PanelView::mouseDrag (const MouseEvent& e)
{
    auto& t = touch (e);
    const auto p = toPanel (e.position);
    if (t.grab >= 100 && layout::isNoteKey (t.held))
    {
        // a glissando: the key under the finger plays
        const int h = layout::hit (p) - 100;
        if (layout::isNoteKey (h) && h != t.held)
        {
            const int was = t.held;
            t.held = -1;
            press (was, false);
            t.held = h;
            press (h, true);
        }
    }
    else if (t.grab >= 0 && t.grab < layout::potIndex)
    {
        // 12 units of drag a click
        t.acc += t.lastY - p.y;
        const int clicks = (int) (t.acc / 12.0f);
        t.acc -= (float) clicks * 12.0f;
        if (clicks != 0)
            turn (t.grab, clicks);
    }
    else if (t.grab == layout::potIndex)
    {
        const float v = proc.params.getRawParameterValue ("master")->load();
        if (auto* prm = proc.params.getParameter ("master"))
            prm->setValueNotifyingHost (jlimit (0.0f, 1.0f, v + (t.lastY - p.y) * 4.0f / 1023.0f));
        repaint();
    }
    t.lastY = p.y;
}

void PanelView::mouseUp (const MouseEvent& e)
{
    auto& t = touch (e);
    if (t.held >= 0)
    {
        const int was = t.held;
        t.held = -1;
        press (was, false);
    }
    if (t.grab == layout::potIndex)
        if (auto* prm = proc.params.getParameter ("master"))
            prm->endChangeGesture();
    t.grab = -1;
}

void PanelView::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    const int h = layout::hit (toPanel (e.position));
    const float dy = w.deltaY * (w.isReversed ? -1.0f : 1.0f);
    if (h == layout::potIndex)
    {
        const float v = proc.params.getRawParameterValue ("master")->load();
        if (auto* prm = proc.params.getParameter ("master"))
        {
            prm->beginChangeGesture();
            prm->setValueNotifyingHost (jlimit (0.0f, 1.0f, v + dy * 0.1f));
            prm->endChangeGesture();
        }
        repaint();
    }
    else if (h >= 0 && h < layout::potIndex)
    {
        // a notch (about 0.1 here) a click; a trackpad's scroll adds up
        wheelAcc += dy;
        while (wheelAcc >= 0.1f) { wheelAcc -= 0.1f; turn (h, 1); }
        while (wheelAcc <= -0.1f) { wheelAcc += 0.1f; turn (h, -1); }
    }
}

// ------------------------------------------------------------------------------ editor --

namespace
{
// the bar's look follows the FM-1's: matte grey plastic, the black caps of its buttons, silver
// silkscreen, amber for what is lit
const Colour barBody { 0xff4f4f51 }, barBodyLow { 0xff3d3d3f }, capTop { 0xff2c2c2e }, capBot { 0xff1f1f21 },
    silkInk { 0xffd9d6cf }, dimInk { 0xffa9a69f };

Font barFont (float size, bool bold = true)
{
    return Font (FontOptions ("Helvetica Neue", size, bold ? Font::bold : Font::plain)).withExtraKerningFactor (0.06f);
}

/** A cap of the panel's buttons: soft black, lit along the top, a shadow under it. */
void drawCap (Graphics& g, Rectangle<float> r, bool down, bool hover, float corner = 6.0f)
{
    g.setColour (Colours::black.withAlpha (down ? 0.25f : 0.45f));
    g.fillRoundedRectangle (r.translated (0, down ? 0.5f : 1.5f), corner);
    if (down)
        r = r.translated (0, 1.0f);
    g.setGradientFill (ColourGradient (capTop.brighter (hover ? 0.12f : 0.0f), 0, r.getY(),
                                       capBot.brighter (hover ? 0.08f : 0.0f), 0, r.getBottom(), false));
    g.fillRoundedRectangle (r, corner);
    g.setGradientFill (ColourGradient (Colours::white.withAlpha (down ? 0.05f : 0.13f), 0, r.getY(),
                                       Colours::transparentWhite, 0, r.getY() + r.getHeight() * 0.5f, false));
    g.drawRoundedRectangle (r.reduced (0.6f), corner - 0.6f, 1.0f);
    g.setColour (Colours::black.withAlpha (0.7f));
    g.drawRoundedRectangle (r, corner, 0.7f);
}

struct BarLook final : public LookAndFeel_V4
{
    BarLook()
        // the dialogs (audio / MIDI settings, alerts) take the panel's colours too
        : LookAndFeel_V4 ({ Colour (0xff2a2a2c),  // windowBackground
                            Colour (0xff1c1c1e),  // widgetBackground
                            Colour (0xff1c1c1e),  // menuBackground
                            Colour (0xff3a3a3c),  // outline
                            silkInk,              // defaultText
                            Colour (0xff4f4f51),  // defaultFill
                            Colour (0xff1a1206),  // highlightedText
                            layout::amber,        // highlightedFill
                            silkInk })            // menuText
    {
        setColour (ResizableWindow::backgroundColourId, Colour (0xff2a2a2c));
        setColour (DocumentWindow::textColourId, silkInk);
        setColour (ListBox::backgroundColourId, Colour (0xff121214));
        setColour (ListBox::outlineColourId, Colour (0xff3a3a3c));
        setColour (ListBox::textColourId, silkInk);
        setColour (ScrollBar::thumbColourId, Colour (0xff5a5a5c));
        setColour (ToggleButton::textColourId, silkInk);
        setColour (ToggleButton::tickColourId, layout::amber);
        setColour (ToggleButton::tickDisabledColourId, dimInk.withAlpha (0.5f));
        setColour (AlertWindow::backgroundColourId, Colour (0xff2a2a2c));
        setColour (AlertWindow::textColourId, silkInk);
        setColour (AlertWindow::outlineColourId, Colour (0xff3a3a3c));
        setColour (TextEditor::backgroundColourId, Colour (0xff121214));
        setColour (TextEditor::textColourId, silkInk);
        setColour (TextEditor::outlineColourId, Colour (0xff3a3a3c));
        setColour (TextEditor::focusedOutlineColourId, layout::amber.withAlpha (0.6f));
        setColour (Slider::thumbColourId, layout::amber);
        setColour (Slider::trackColourId, Colour (0xff5a5a5c));
        setColour (Slider::backgroundColourId, Colour (0xff121214));
        setColour (TextButton::textColourOffId, silkInk);
        setColour (TextButton::textColourOnId, layout::amber);
        setColour (ComboBox::textColourId, layout::amber);
        setColour (PopupMenu::backgroundColourId, Colour (0xff1c1c1e));
        setColour (PopupMenu::textColourId, silkInk);
        setColour (PopupMenu::highlightedBackgroundColourId, layout::amber);
        setColour (PopupMenu::highlightedTextColourId, Colour (0xff1a1206));
        setColour (Label::textColourId, dimInk);
        setColour (TooltipWindow::backgroundColourId, Colour (0xff1c1c1e));
        setColour (TooltipWindow::textColourId, silkInk);
        setColour (TooltipWindow::outlineColourId, Colour (0xff3a3a3c));
    }

    void drawButtonBackground (Graphics& g, juce::Button& b, const Colour&, bool hover, bool down) override
    {
        const auto r = b.getLocalBounds().toFloat().reduced (1.0f, 1.5f);
        drawCap (g, r, down, hover && b.isEnabled());
        if (b.getToggleState())
        {
            // lit like the panel's buttons: amber through the cap, a glow around it
            for (int i = 3; i >= 1; --i)
            {
                g.setColour (layout::amber.withAlpha (0.07f * (float) (4 - i)));
                g.fillRoundedRectangle (r.expanded ((float) i * 1.5f), 6.0f + (float) i * 1.5f);
            }
            g.setGradientFill (ColourGradient (layout::amber.withAlpha (0.85f), 0, r.getY(), layout::amber.darker (0.35f).withAlpha (0.85f), 0, r.getBottom(), false));
            g.fillRoundedRectangle (r.reduced (1.0f), 5.0f);
        }
    }
    Font getTextButtonFont (TextButton& b, int) override
    {
        // as large as fits (the Standalone's Options button is narrow), 12 at most
        const auto text = b.getButtonText().toUpperCase();
        float size = 12.0f;
        while (size > 8.0f && GlyphArrangement::getStringWidth (barFont (size), text) > (float) b.getWidth() - 12.0f)
            size -= 0.5f;
        return barFont (size);
    }
    void drawButtonText (Graphics& g, TextButton& b, bool hover, bool down) override
    {
        g.setFont (getTextButtonFont (b, b.getHeight()));
        g.setColour (b.getToggleState() ? Colour (0xff1a1206)
                                        : (hover ? silkInk.brighter (0.2f) : silkInk).withAlpha (b.isEnabled() ? 1.0f : 0.4f));
        g.drawText (b.getButtonText().toUpperCase(), b.getLocalBounds().translated (0, down ? 1 : 0).reduced (5, 0),
                    Justification::centred, false);
    }

    // a combo box is a little window of the panel's screen, sunk into the plastic
    void drawComboBox (Graphics& g, int w, int h, bool down, int, int, int, int, ComboBox& box) override
    {
        auto r = Rectangle<float> ((float) w, (float) h).reduced (1.0f, 1.5f);
        g.setColour (Colours::white.withAlpha (0.10f));
        g.fillRoundedRectangle (r.translated (0, 1.0f), 5.0f); // the lit lip under it
        g.setGradientFill (ColourGradient (Colour (0xff080809), 0, r.getY(), Colour (0xff121214), 0, r.getBottom(), false));
        g.fillRoundedRectangle (r, 5.0f);
        g.setGradientFill (ColourGradient (Colours::black.withAlpha (0.8f), 0, r.getY(),
                                           Colours::transparentBlack, 0, r.getY() + 5.0f, false));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.5f, 1.0f);
        if (box.hasKeyboardFocus (true) || down || box.isMouseOver (true))
        {
            g.setColour (layout::amber.withAlpha (down ? 0.5f : 0.25f));
            g.drawRoundedRectangle (r.reduced (0.5f), 4.5f, 1.0f);
        }
        // the arrow, lit like the screen's text
        const float ax = r.getRight() - 12.0f, ay = r.getCentreY();
        Path p;
        p.addTriangle (ax - 3.5f, ay - 1.8f, ax + 3.5f, ay - 1.8f, ax, ay + 2.6f);
        g.setColour (layout::amber.withAlpha (box.isEnabled() ? 0.85f : 0.3f));
        g.fillPath (p);
    }
    Font getComboBoxFont (ComboBox&) override { return barFont (12.5f); }
    void positionComboBoxText (ComboBox& box, Label& l) override
    {
        l.setBounds (6, 1, box.getWidth() - 22, box.getHeight() - 2);
        l.setMinimumHorizontalScale (0.6f);
        l.setFont (getComboBoxFont (box));
        l.setJustificationType (Justification::centredLeft);
    }

    void drawPopupMenuBackground (Graphics& g, int w, int h) override
    {
        g.fillAll (findColour (PopupMenu::backgroundColourId));
        g.setColour (Colour (0xff3a3a3c));
        g.drawRect (0, 0, w, h, 1);
    }
    Font getPopupMenuFont() override { return barFont (13.0f, false); }
    void drawPopupMenuItem (Graphics& g, const Rectangle<int>& area, bool separator, bool active, bool highlighted,
                            bool ticked, bool, const String& text, const String&, const Drawable*, const Colour*) override
    {
        if (separator)
        {
            g.setColour (Colour (0xff3a3a3c));
            g.fillRect (area.reduced (6, 0).withHeight (1).withY (area.getCentreY()));
            return;
        }
        auto r = area.reduced (3, 1).toFloat();
        if (highlighted && active)
        {
            g.setColour (layout::amber);
            g.fillRoundedRectangle (r, 3.0f);
        }
        if (ticked)
        {
            g.setColour (highlighted ? Colour (0xff1a1206) : layout::amber);
            g.fillEllipse (r.getX() + 7.0f, r.getCentreY() - 3.0f, 6.0f, 6.0f);
        }
        g.setColour (highlighted ? Colour (0xff1a1206) : silkInk.withAlpha (active ? 1.0f : 0.4f));
        g.setFont (ticked ? barFont (13.0f) : getPopupMenuFont());
        g.drawText (text, r.withTrimmedLeft (20.0f).withTrimmedRight (6.0f), Justification::centredLeft, true);
    }
    void getIdealPopupMenuItemSize (const String& text, bool separator, int, int& w, int& h) override
    {
        h = separator ? 7 : 24;
        w = GlyphArrangement::getStringWidthInt (getPopupMenuFont(), text) + 40;
    }
    int getPopupMenuBorderSize() override { return 3; }

    // tick boxes: a small sunk window that lights amber
    void drawTickBox (Graphics& g, Component& c, float x, float y, float w, float h, bool ticked, bool enabled, bool hover, bool) override
    {
        const auto r = Rectangle<float> (x, y, w, h).reduced (1.0f);
        g.setColour (Colours::white.withAlpha (0.08f));
        g.fillRoundedRectangle (r.translated (0, 1.0f), 3.5f);
        g.setColour (Colour (0xff0b0b0c));
        g.fillRoundedRectangle (r, 3.5f);
        g.setColour (hover ? layout::amber.withAlpha (0.45f) : Colours::black.withAlpha (0.8f));
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        if (ticked)
        {
            const auto lit = r.reduced (r.getWidth() * 0.24f);
            g.setColour (layout::amber.withAlpha (enabled ? 0.25f : 0.1f));
            g.fillRoundedRectangle (lit.expanded (2.0f), 3.0f);
            g.setColour (enabled ? layout::amber : layout::amber.withAlpha (0.35f));
            g.fillRoundedRectangle (lit, 2.0f);
        }
        ignoreUnused (c);
    }
    void drawToggleButton (Graphics& g, ToggleButton& b, bool hover, bool down) override
    {
        const float box = jmin (16.0f, (float) b.getHeight() - 4.0f);
        drawTickBox (g, b, 4.0f, ((float) b.getHeight() - box) / 2.0f, box, box, b.getToggleState(), b.isEnabled(), hover, down);
        g.setColour (silkInk.withAlpha (b.isEnabled() ? 1.0f : 0.45f));
        g.setFont (barFont (13.0f, false));
        g.drawFittedText (b.getButtonText(), b.getLocalBounds().withTrimmedLeft (roundToInt (box) + 10).withTrimmedRight (2),
                          Justification::centredLeft, 2);
    }
    Font getLabelFont (Label& l) override
    {
        // the settings' labels in the bar's type, other labels as they were set
        return l.getFont().getTypefaceName() == Font::getDefaultSansSerifFontName() ? barFont (l.getFont().getHeight(), false) : l.getFont();
    }

    // alerts: the dialog's grey, a lit top edge, the text in silver
    void drawAlertBox (Graphics& g, AlertWindow& w, const Rectangle<int>& text, TextLayout& tl) override
    {
        const auto r = w.getLocalBounds().toFloat();
        g.setColour (findColour (AlertWindow::backgroundColourId));
        g.fillRoundedRectangle (r, 8.0f);
        g.setColour (layout::amber.withAlpha (0.8f));
        g.fillRect (r.getX() + 10.0f, 0.0f, r.getWidth() - 20.0f, 1.5f);
        g.setColour (findColour (AlertWindow::outlineColourId));
        g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
        g.setColour (silkInk);
        tl.draw (g, text.toFloat());
    }
    Font getAlertWindowTitleFont() override { return barFont (15.0f); }
    Font getAlertWindowMessageFont() override { return barFont (13.0f, false); }
    Font getAlertWindowFont() override { return barFont (13.0f, false); }

    // the Standalone's window: its title bar is the top of the same grey plastic
    void drawDocumentWindowTitleBar (DocumentWindow& window, Graphics& g, int w, int h, int, int, const Image*, bool) override
    {
        const auto r = Rectangle<float> ((float) w, (float) h);
        g.setGradientFill (ColourGradient (barBody.brighter (0.06f), 0, 0, barBody, 0, r.getBottom(), false));
        g.fillRect (r);
        g.setColour (Colours::white.withAlpha (0.10f));
        g.fillRect (r.withHeight (1.0f));
        g.setColour (Colours::black.withAlpha (0.35f));
        g.fillRect (r.withY (r.getBottom() - 1.0f).withHeight (1.0f));
        g.setFont (barFont (12.5f).withExtraKerningFactor (0.35f));
        g.setColour (window.isActiveWindow() ? silkInk : dimInk);
        g.drawText (window.getName().toUpperCase(), r.toNearestInt(), Justification::centred, true);
    }

    struct WindowButton final : public juce::Button
    {
        WindowButton (const String& name, int type) : juce::Button (name), kind (type) {}
        void paintButton (Graphics& g, bool hover, bool down) override
        {
            const float d = jmin ((float) getWidth(), (float) getHeight()) - 8.0f;
            const auto r = getLocalBounds().toFloat().withSizeKeepingCentre (d, d);
            drawCap (g, r, down, hover, d / 2.0f);
            const auto ink = hover ? (kind == DocumentWindow::closeButton ? Colour (0xffe8604a) : layout::amber) : dimInk;
            g.setColour (ink);
            const auto c = r.getCentre().translated (0, down ? 1.0f : 0.0f);
            const float a = d * 0.2f;
            if (kind == DocumentWindow::closeButton)
            {
                g.drawLine (c.x - a, c.y - a, c.x + a, c.y + a, 1.4f);
                g.drawLine (c.x + a, c.y - a, c.x - a, c.y + a, 1.4f);
            }
            else if (kind == DocumentWindow::minimiseButton)
                g.drawLine (c.x - a, c.y, c.x + a, c.y, 1.4f);
            else
                g.drawRect (Rectangle<float> (a * 2, a * 2).withCentre (c), 1.2f);
        }
        int kind;
    };
    juce::Button* createDocumentWindowButton (int type) override
    {
        const char* names[] = { "", "minimise", "maximise", "", "close" };
        return new WindowButton (type < 5 ? names[type] : "", type);
    }

    // a tooltip is laid out once, the same way for its size and for drawing it
    static TextLayout tooltipLayout (const String& text, float width)
    {
        AttributedString s;
        s.setJustification (Justification::topLeft);
        s.append (text, barFont (12.5f, false), silkInk);
        TextLayout tl;
        tl.createLayoutWithBalancedLineLengths (s, width);
        return tl;
    }
    static constexpr float tipPadX = 10.0f, tipPadY = 7.0f, tipMaxWidth = 340.0f;

    Rectangle<int> getTooltipBounds (const String& text, Point<int> at, Rectangle<int> parent) override
    {
        const auto tl = tooltipLayout (text, tipMaxWidth);
        const int w = (int) std::ceil (tl.getWidth() + tipPadX * 2.0f) + 2;
        const int h = (int) std::ceil (tl.getHeight() + tipPadY * 2.0f) + 2;
        auto r = Rectangle<int> (at.x > parent.getCentreX() ? at.x - (w + 12) : at.x + 24,
                                 at.y > parent.getCentreY() ? at.y - (h + 6) : at.y + 6, w, h);
        return r.constrainedWithin (parent);
    }

    void drawTooltip (Graphics& g, const String& text, int w, int h) override
    {
        const auto r = Rectangle<float> ((float) w, (float) h);
        g.setColour (findColour (TooltipWindow::backgroundColourId));
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (findColour (TooltipWindow::outlineColourId));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (layout::amber.withAlpha (0.8f));
        g.fillRect (r.getX() + 4.0f, 0.0f, r.getWidth() - 8.0f, 1.0f);
        const auto inner = r.reduced (tipPadX, tipPadY);
        tooltipLayout (text, jmax (10.0f, jmin (tipMaxWidth, inner.getWidth() + 1.0f))).draw (g, inner.withHeight (inner.getHeight() + 2.0f));
    }
};
BarLook& barLook()
{
    static BarLook l;
    return l;
}
} // namespace

Fm1Editor::Fm1Editor (Fm1Processor& p) : AudioProcessorEditor (p), proc (p), panel (p)
{
    setLookAndFeel (&barLook());
    addAndMakeVisible (panel);

    for (auto* c : std::initializer_list<Component*> { &loadButton, &exportButton, &mpeButton, &nameLabel, &channelLabel,
                                                       &clockLabel, &statusLabel, &channelBox, &clockBox })
        addAndMakeVisible (c);

    loadButton.setTooltip ("Open a .fwsc firmware package or a shared .fm1save setup");
    exportButton.setTooltip ("Share the running firmware, patches and settings as a .fm1save file");
    loadButton.onClick = [this] { loadFile(); };
    mpeButton.setClickingTogglesState (true);
    mpeButton.setTooltip ("MPE in. With a MIDI channel set, every note of an MPE controller goes to that channel and the "
                          "note played last bends, presses and slides it (per-note bend range 48, the firmware's 2). "
                          "With the channel \"as sent\", MPE goes through unchanged, for a firmware that speaks MPE itself.");
    mpeAttach = std::make_unique<AudioProcessorValueTreeState::ButtonAttachment> (proc.params, "mpe", mpeButton);
    exportButton.onClick = [this] { exportFile(); };

    nameLabel.setFont (barFont (13.5f));
    nameLabel.setColour (Label::textColourId, layout::amber);
    statusLabel.setFont (barFont (12.0f, false));
    statusLabel.setJustificationType (Justification::centredRight);
    for (auto* l : { &channelLabel, &clockLabel })
    {
        l->setFont (barFont (11.0f));
        l->setColour (Label::textColourId, silkInk);
        l->setJustificationType (Justification::centredRight);
    }
    channelLabel.setText ("MIDI CH", dontSendNotification);
    statusLabel.setMinimumHorizontalScale (0.6f);
    nameLabel.setMinimumHorizontalScale (0.7f);
    for (auto* l : { &channelLabel, &clockLabel })
        l->setMinimumHorizontalScale (0.7f);

    channelBox.addItem ("as sent", 1);
    for (int c = 1; c <= 16; ++c)
        channelBox.addItem (String (c), c + 1);
    channelAttach = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (proc.params, "channel", channelBox);

    for (int i = 0; i < 5; ++i)
        clockBox.addItem (String (Fm1Processor::clocks[i]) + " MHz", i + 1);
    clockBox.setTooltip ("The emulated FM-1's clock, 240 MHz as on the chip. Lower it if this computer can't keep up "
                         "with a heavy firmware (frames late); raise it if a firmware runs out of time. "
                         "Changing it restarts the firmware.");
    clockAttach = std::make_unique<AudioProcessorValueTreeState::ComboBoxAttachment> (proc.params, "clock", clockBox);

   #if JUCE_IOS
    // the screen (or the host's view) decides; the panel keeps its proportions inside
    constrainer.setSizeLimits (320, 200, 4096, 4096);
    setConstrainer (&constrainer);
    setResizable (true, false);
    setSize (1024, 600);
   #else
    const int w0 = 1024;
    constrainer.setFixedAspectRatio (layout::W / (layout::H + barHeight * layout::W / (float) w0));
    constrainer.setSizeLimits (640, 0, 2560, 2000);
    setConstrainer (&constrainer);
    setResizable (true, true);

    const int w = proc.editorWidth > 0 ? jlimit (640, 2560, proc.editorWidth) : w0;
    setSize (w, roundToInt ((float) w * layout::H / layout::W) + barHeight);
   #endif

    panel.refresh();
    startTimerHz (30);
    loadBundledFirmware();
}

void Fm1Editor::parentHierarchyChanged()
{
    // the Standalone: its window wears the panel's look too
    if (proc.wrapperType == AudioProcessor::wrapperType_Standalone)
        if (auto* w = findParentComponentOfClass<DocumentWindow>(); w != nullptr && w != styledWindow.getComponent())
        {
            styledWindow = w;
            // (the default too: the window's Options menu and the settings dialog are not its children)
            LookAndFeel::setDefaultLookAndFeel (&barLook());
            w->setLookAndFeel (&barLook());
            w->setBackgroundColour (layout::room);
            w->setTitleBarHeight (30);
            w->setTitleBarButtonsRequired (DocumentWindow::minimiseButton | DocumentWindow::closeButton, false);
            w->lookAndFeelChanged();
        }
}

Fm1Editor::~Fm1Editor()
{
    if (auto* w = styledWindow.getComponent())
    {
        w->setLookAndFeel (nullptr);
        LookAndFeel::setDefaultLookAndFeel (nullptr);
    }
    setLookAndFeel (nullptr);
}

void Fm1Editor::paint (Graphics& g)
{
    g.fillAll (layout::room);
    // the bar: a strip of the FM-1's matte grey plastic, lit from above, over the panel's shadow
    const auto bar = getLocalBounds().removeFromTop (safe.getTop() + barHeight).toFloat();
    g.setGradientFill (ColourGradient (barBody, 0, 0, barBodyLow, 0, bar.getBottom(), false));
    g.fillRect (bar);
    g.setColour (Colours::white.withAlpha (0.12f));
    g.fillRect (bar.withHeight (1.0f));
    g.setColour (Colours::black.withAlpha (0.8f));
    g.fillRect (bar.withY (bar.getBottom() - 1.0f).withHeight (1.0f));
    for (int i = 0; i < 6; ++i)
    {
        g.setColour (Colours::black.withAlpha (0.12f - (float) i * 0.02f));
        g.fillRect (bar.withY (bar.getBottom() + (float) i).withHeight (1.0f));
    }
    // the separators between the groups
    for (int x : separators)
    {
        g.setColour (Colours::black.withAlpha (0.35f));
        g.fillRect ((float) x, (float) safe.getTop() + 8.0f, 1.0f, (float) barHeight - 16.0f);
        g.setColour (Colours::white.withAlpha (0.08f));
        g.fillRect ((float) x + 1.0f, (float) safe.getTop() + 8.0f, 1.0f, (float) barHeight - 16.0f);
    }
}

void Fm1Editor::resized()
{
   #if JUCE_IOS
    // around the notch and the home indicator, when the editor covers the screen
    safe = {};
    if (auto* d = Desktop::getInstance().getDisplays().getPrimaryDisplay())
        if (getScreenBounds().contains (d->totalArea.reduced (2)) || proc.wrapperType == AudioProcessor::wrapperType_Standalone)
            safe = d->safeAreaInsets;
   #endif
    const auto inside = safe.subtractedFrom (getLocalBounds());
    auto bar = inside.withHeight (barHeight).reduced (10, 6);
    // a narrow window shortens the bar's texts rather than squeezing them
    const bool compact = bar.getWidth() < 880;
    loadButton.setButtonText (compact ? "Load..." : "Load firmware / save...");
    exportButton.setButtonText (compact ? "Export..." : "Export save...");
    channelLabel.setText (compact ? "CH" : "MIDI CH", dontSendNotification);
    loadButton.setBounds (bar.removeFromLeft (compact ? 76 : 190));
    bar.removeFromLeft (6);
    exportButton.setBounds (bar.removeFromLeft (compact ? 86 : 120));
    bar.removeFromLeft (8);
    separators[0] = bar.getX();
    bar.removeFromLeft (8);
    nameLabel.setBounds (bar.removeFromLeft (compact ? 100 : 170));
    clockBox.setBounds (bar.removeFromRight (92));
    clockLabel.setBounds (bar.removeFromRight (40));
    bar.removeFromRight (10);
    channelBox.setBounds (bar.removeFromRight (82));
    channelLabel.setBounds (bar.removeFromRight (compact ? 30 : 60));
    bar.removeFromRight (8);
    mpeButton.setBounds (bar.removeFromRight (52));
    bar.removeFromRight (8);
    separators[1] = bar.getRight();
    bar.removeFromRight (8);
    statusLabel.setBounds (bar);
    statusLabel.setVisible (bar.getWidth() > 120);

    // the panel keeps its proportions, centred in what is left
    const auto area = inside.withTrimmedTop (barHeight);
    const float k = jmin ((float) area.getWidth() / layout::W, (float) area.getHeight() / layout::H);
    panel.setBounds (area.withSizeKeepingCentre (roundToInt (layout::W * k), roundToInt (layout::H * k)));
    proc.editorWidth = getWidth();
}

void Fm1Editor::showMessage (const String& m, bool sticky)
{
    message = m;
    messageUntil = sticky ? 0.0 : Time::getMillisecondCounterHiRes() + 3000.0;
}

void Fm1Editor::autosave()
{
    // the Standalone keeps its last setup (firmware, its flash, settings, window) even when it is
    // not quit properly: JUCE saves it only on quitting, and iOS ends apps without asking
#if JucePlugin_Build_Standalone
    if (proc.wrapperType != AudioProcessor::wrapperType_Standalone)
        return;
    const double now = Time::getMillisecondCounterHiRes();

    String snapshot;
    for (auto* id : { "master", "channel", "clock", "mpe" })
        snapshot << params().getRawParameterValue (id)->load() << ";";
    if (auto* w = getTopLevelComponent())
        snapshot << w->getBounds().toString();
    snapshot << ";" << nameLabel.getText();
    if (snapshot != lastSnapshot)
    {
        lastSnapshot = snapshot;
        saveDueAt = now + 1500.0;
    }
    if (saveDueAt == 0 && now - lastSaveAt > 30000.0)
        saveDueAt = now; // the firmware's flash may have changed: the patches it saves

    if (saveDueAt == 0 || now < saveDueAt || busy)
        return;
    saveDueAt = 0;
    lastSaveAt = now;
    if (auto* holder = StandalonePluginHolder::getInstance())
    {
        holder->savePluginState();
        if (auto* settings = holder->settings.get())
        {
           #if ! JUCE_IOS
            if (auto* w = getTopLevelComponent())
            {
                settings->setValue ("windowX", w->getX());
                settings->setValue ("windowY", w->getY());
            }
           #endif
            if (auto* file = dynamic_cast<PropertiesFile*> (settings))
                file->saveIfNeeded();
        }
    }
#endif
}

void Fm1Editor::timerCallback()
{
    autosave();
    panel.refresh();
    const auto& s = panel.status;

    char name[256] {};
    fm1_firmware_name (proc.machine(), name, sizeof (name));
    const String n = String::fromUTF8 (name);
    nameLabel.setText (n.isEmpty() ? "no firmware" : n, dontSendNotification);
    exportButton.setEnabled (! busy && fm1_has_firmware (proc.machine()) != 0);
    loadButton.setEnabled (! busy);

    if (messageUntil > 0 && Time::getMillisecondCounterHiRes() >= messageUntil)
    {
        message.clear();
        messageUntil = 0;
    }

    String status;
    if (message.isNotEmpty())
        status = message;
    else if (s.booted == 0)
        status = "Open a .fwsc firmware or a shared .fm1save to start";
    else if (s.halted != 0)
        status = "The firmware stopped: an instruction the emulator does not know";
    else if (s.board == 1)
        status = "This is an SMK-37 firmware: it runs, but this panel is the FM-1's";
    else
    {
        const String late = s.late > 0 ? ", " + String ((int64) s.late) + " frames late in the last second" : String();
        status = s.speed > 0 ? "could run " + String (s.speed, 1) + "x real time" + late : "starting" + late;
    }
    statusLabel.setText (status, dontSendNotification);
}

void Fm1Editor::loadFile()
{
    chooser = std::make_unique<FileChooser> ("Open an FM-1 firmware or shared save",
                                             File::getSpecialLocation (File::userMusicDirectory),
                                             "*.fwsc;*.fm1save;*.json");
    chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
                          [this, safe = Component::SafePointer<Fm1Editor> (this)] (const FileChooser& fc)
                          {
                              if (safe == nullptr || fc.getURLResult().isEmpty())
                                  return;
                             #if JUCE_IOS
                              // the picker's file is the Files app's: copied in first
                              const auto url = fc.getURLResult();
                              auto copy = File::getSpecialLocation (File::tempDirectory)
                                              .getChildFile (url.getFileName().isNotEmpty() ? url.getFileName() : String ("firmware.fwsc"));
                              {
                                  copy.deleteFile();
                                  auto in = url.createInputStream (URL::InputStreamOptions (URL::ParameterHandling::inAddress));
                                  FileOutputStream out (copy);
                                  if (in == nullptr || ! out.openedOk() || out.writeFromInputStream (*in, -1) <= 0)
                                  {
                                      showMessage ("Could not read " + url.getFileName(), true);
                                      return;
                                  }
                              }
                              const auto path = copy.getFullPathName();
                             #else
                              const auto path = fc.getResult().getFullPathName();
                             #endif
                              busy = true;
                              showMessage ("Loading...", true);
                              const Fm1* m = proc.machine();
                              Thread::launch ([m, path, safe]
                                              {
                                                  char msg[512] {};
                                                  fm1_load_file (m, path.toRawUTF8(), msg, sizeof (msg));
                                                  const String text = String::fromUTF8 (msg);
                                                  MessageManager::callAsync ([safe, text]
                                                                             {
                                                                                 if (safe != nullptr)
                                                                                 {
                                                                                     safe->busy = false;
                                                                                     safe->showMessage (text, ! text.startsWith ("Loaded"));
                                                                                 }
                                                                             });
                                              });
                          });
}

void Fm1Editor::loadBundledFirmware()
{
#if FM1_BUNDLED_FIRMWARE
    // nothing loaded yet (a fresh install): the firmware that comes with the app, also left in
    // the app's folder in Files to load again later
    if (fm1_has_firmware (proc.machine()) != 0)
        return;
    const auto docs = File::getSpecialLocation (File::userDocumentsDirectory);
    const auto file = docs.getChildFile ("felucca-1.7.0.fwsc");
    if (! file.existsAsFile())
        file.replaceWithData (FirmwareData::felucca1_7_0_fwsc, (size_t) FirmwareData::felucca1_7_0_fwscSize);
    if (! file.existsAsFile())
        return;
    busy = true;
    showMessage ("Loading Felucca...", true);
    const Fm1* m = proc.machine();
    Thread::launch ([m, path = file.getFullPathName(), safe = Component::SafePointer<Fm1Editor> (this)]
                    {
                        char msg[512] {};
                        fm1_load_file (m, path.toRawUTF8(), msg, sizeof (msg));
                        const String text = String::fromUTF8 (msg);
                        MessageManager::callAsync ([safe, text]
                                                   {
                                                       if (safe != nullptr)
                                                       {
                                                           safe->busy = false;
                                                           safe->showMessage (text, ! text.startsWith ("Loaded"));
                                                       }
                                                   });
                    });
#endif
}

#if JUCE_IOS
void Fm1Editor::exportFile()
{
    // iOS: the save is written first, then the picker puts it where you choose (Files, iCloud...)
    char name[256] {};
    fm1_firmware_name (proc.machine(), name, sizeof (name));
    auto safeName = File::createLegalFileName (String::fromUTF8 (name));
    if (safeName.isEmpty())
        safeName = "FM-1";
    const auto file = File::getSpecialLocation (File::tempDirectory).getChildFile (safeName + ".fm1save");
    busy = true;
    showMessage ("Exporting live flash...", true);
    const Fm1* m = proc.machine();
    Thread::launch ([this, m, file, safe = Component::SafePointer<Fm1Editor> (this)]
                    {
                        char msg[512] {};
                        const bool ok = fm1_export_file (m, file.getFullPathName().toRawUTF8(), msg, sizeof (msg)) != 0;
                        const String text = String::fromUTF8 (msg);
                        MessageManager::callAsync ([this, safe, text, ok, file]
                        {
                            if (safe == nullptr)
                                return;
                            busy = false;
                            if (! ok)
                            {
                                showMessage (text, true);
                                return;
                            }
                            showMessage ({}, false);
                            chooser = std::make_unique<FileChooser> ("Export the FM-1 setup", file, "*.fm1save");
                            chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles,
                                                  [safe, file] (const FileChooser& fc)
                                                  {
                                                      if (safe != nullptr)
                                                          safe->showMessage (fc.getURLResult().isEmpty() ? "Export cancelled"
                                                                                                         : "Exported " + file.getFileName(), false);
                                                  });
                        });
                    });
}
#else
void Fm1Editor::exportFile()
{
    char name[256] {};
    fm1_firmware_name (proc.machine(), name, sizeof (name));
    auto safeName = File::createLegalFileName (String::fromUTF8 (name));
    if (safeName.isEmpty())
        safeName = "FM-1";

    chooser = std::make_unique<FileChooser> ("Export the FM-1 setup",
                                             File::getSpecialLocation (File::userMusicDirectory).getChildFile (safeName + ".fm1save"),
                                             "*.fm1save");
    chooser->launchAsync (FileBrowserComponent::saveMode | FileBrowserComponent::canSelectFiles
                              | FileBrowserComponent::warnAboutOverwriting,
                          [this, safe = Component::SafePointer<Fm1Editor> (this)] (const FileChooser& fc)
                          {
                              if (safe == nullptr || fc.getResult() == File())
                                  return;
                              const auto path = fc.getResult().getFullPathName();
                              busy = true;
                              showMessage ("Exporting live flash...", true);
                              const Fm1* m = proc.machine();
                              Thread::launch ([m, path, safe]
                                              {
                                                  char msg[512] {};
                                                  const bool ok = fm1_export_file (m, path.toRawUTF8(), msg, sizeof (msg)) != 0;
                                                  const String text = String::fromUTF8 (msg);
                                                  MessageManager::callAsync ([safe, text, ok]
                                                                             {
                                                                                 if (safe != nullptr)
                                                                                 {
                                                                                     safe->busy = false;
                                                                                     safe->showMessage (text, ! ok);
                                                                                 }
                                                                             });
                                              });
                          });
}
#endif
