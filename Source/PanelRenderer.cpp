#include "PanelRenderer.h"

#if FM1_HAS_ASSETS
 #include "BinaryData.h"
#endif

using namespace juce;
using namespace layout;

namespace
{
Font font (float size, bool bold = true)
{
    return Font (FontOptions ("Helvetica Neue", size, bold ? Font::bold : Font::plain)).withExtraKerningFactor (0.08f);
}

/** An image of `w` x `h` design units at `ppu` pixels each, with a Graphics already scaled. */
struct Canvas
{
    Canvas (float w, float h, float ppu)
        // (a software image: drawn at once, not when a native context gets round to flushing)
        : image (Image::ARGB, jmax (1, roundToInt (w * ppu)), jmax (1, roundToInt (h * ppu)), true, SoftwareImageType()),
          g (image)
    {
        g.addTransform (AffineTransform::scale (ppu));
        g.setImageResamplingQuality (Graphics::highResamplingQuality);
    }
    Image image;
    Graphics g;
};

/** A recessed area: dark floor, shadowed top edge, lit bottom lip. */
void recess (Graphics& g, Rectangle<float> r, float radius, Colour floor)
{
    Path p;
    p.addRoundedRectangle (r, radius);
    g.setColour (floor);
    g.fillPath (p);

    // inner shadow from the top
    {
        Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (p);
        for (int i = 0; i < 8; ++i)
        {
            g.setColour (Colours::black.withAlpha (0.10f - (float) i * 0.012f));
            g.drawRoundedRectangle (r.translated (0, (float) i * 0.8f + 1.0f).expanded (1.0f), radius, 2.0f);
        }
        g.setGradientFill (ColourGradient (Colours::black.withAlpha (0.35f), r.getX(), r.getY(),
                                           Colours::transparentBlack, r.getX(), r.getY() + 14.0f, false));
        g.fillRect (r.withHeight (14.0f));
    }
    // the lip: light on the bottom edge, dark on the top
    g.setGradientFill (ColourGradient (Colours::black.withAlpha (0.55f), 0, r.getY(),
                                       Colours::white.withAlpha (0.16f), 0, r.getBottom(), false));
    g.drawRoundedRectangle (r.expanded (0.6f), radius + 0.6f, 1.3f);
}

void addGrain (Image& img, float amount, uint32 seed)
{
    Image::BitmapData d (img, Image::BitmapData::readWrite);
    Random rnd ((int64) seed);
    for (int y = 0; y < d.height; ++y)
    {
        for (int x = 0; x < d.width; ++x)
        {
            auto c = d.getPixelColour (x, y);
            if (c.getAlpha() == 0)
                continue;
            const float n = (rnd.nextFloat() - 0.5f) * amount;
            auto adj = [n] (uint8 v) { return (uint8) jlimit (0, 255, (int) v + roundToInt (n * 255.0f)); };
            d.setPixelColour (x, y, Colour (adj (c.getRed()), adj (c.getGreen()), adj (c.getBlue()), c.getAlpha()));
        }
    }
}

Colour mix (Colour a, Colour b, float t) { return a.interpolatedWith (b, jlimit (0.0f, 1.0f, t)); }

/** LED duty -> brightness: a fully lit LED is on ~1 tick in 11. */
float bright (float duty) { return jlimit (0.0f, 1.0f, duty / 0.08f); }

/** The light an LED throws around a part. */
void bloom (Graphics& g, Rectangle<float> r, float radius, Colour c, float amount)
{
    if (amount <= 0.0f)
        return;
    for (int i = 6; i >= 1; --i)
    {
        g.setColour (c.withAlpha (amount * 0.05f * (7.0f - (float) i) / 6.0f));
        g.fillRoundedRectangle (r.expanded ((float) i * 2.2f), radius + (float) i * 2.2f);
    }
}
} // namespace

// ------------------------------------------------------------------------------------------

void PanelSkin::prepare (float pixelsPerUnit)
{
    pixelsPerUnit = jlimit (0.5f, 4.0f, pixelsPerUnit);
    if (std::abs (pixelsPerUnit - ppu) < 0.01f && background.isValid())
        return;
    ppu = pixelsPerUnit;

#if FM1_HAS_ASSETS
    if (! photo.isValid())
        for (auto name : { "fm1_top_png", "fm1_top_jpg" })
        {
            int size = 0;
            if (auto* data = BinaryData::getNamedResource (name, size))
            {
                photo = ImageFileFormat::loadFrom (data, (size_t) size);
                if (photo.isValid())
                    break;
            }
        }
#endif

    background = renderBackground();
    lcdGrid = renderLcdGrid();
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        rotor[i] = renderRotor (knobs[i]);
        light[i] = renderLight (knobs[i]);
        faces[i] = {};
        if (photo.isValid())
            splitFace (faces[i], rotor[i]);
    }
}

void PanelSkin::splitFace (Face& f, const Image& img) const
{
    const int n = jmin (img.getWidth(), img.getHeight());
    f.n = n;
    f.detail.assign ((size_t) (n * n * 3), 0.0f);
    f.light.assign ((size_t) (n * n), 1.0f);
    f.alpha.assign ((size_t) (n * n), 0);
    f.radius = (float) n / 2.0f - 2.5f;
    f.turned = Image (Image::ARGB, n, n, true, SoftwareImageType());
    f.angle = 1.0e9f;

    const Image::BitmapData d (img, Image::BitmapData::readOnly);
    const float c = ((float) n - 1.0f) / 2.0f, s = (float) n / 2.0f;
    std::vector<float> lum ((size_t) (n * n), 0.0f);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
        {
            const auto col = d.getPixelColour (x, y);
            const size_t i = (size_t) (y * n + x);
            f.alpha[i] = col.getAlpha();
            f.detail[i * 3 + 0] = col.getFloatRed();
            f.detail[i * 3 + 1] = col.getFloatGreen();
            f.detail[i * 3 + 2] = col.getFloatBlue();
            lum[i] = 0.3f * col.getFloatRed() + 0.59f * col.getFloatGreen() + 0.11f * col.getFloatBlue();
        }

    // the light: a smooth quadratic over the face, fitted to its luminance, the pointer and
    // other markings (far from the fit) left out
    auto inside = [&] (int x, int y) { return juce::square ((float) x - c) + juce::square ((float) y - c) <= juce::square (f.radius); };
    double coef[6] { 0.3, 0, 0, 0, 0, 0 };
    auto basis = [&] (int x, int y, double* b)
    {
        const double u = ((double) x - c) / s, v = ((double) y - c) / s;
        b[0] = 1; b[1] = u; b[2] = v; b[3] = u * u; b[4] = u * v; b[5] = v * v;
    };
    auto eval = [&] (int x, int y) { double b[6]; basis (x, y, b); double r = 0; for (int k = 0; k < 6; ++k) r += coef[k] * b[k]; return r; };
    double sigma = 1.0e9;
    for (int pass = 0; pass < 4; ++pass)
    {
        double A[6][7] {};
        double err = 0; int count = 0;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                if (! inside (x, y)) continue;
                const double l = lum[(size_t) (y * n + x)];
                if (pass > 0 && std::abs (l - eval (x, y)) > 2.0 * sigma) continue;
                double b[6]; basis (x, y, b);
                for (int r = 0; r < 6; ++r) { for (int k = 0; k < 6; ++k) A[r][k] += b[r] * b[k]; A[r][6] += b[r] * l; }
                ++count;
            }
        if (count < 12) break;
        // Gauss-Jordan
        for (int col = 0; col < 6; ++col)
        {
            int piv = col;
            for (int r = col + 1; r < 6; ++r) if (std::abs (A[r][col]) > std::abs (A[piv][col])) piv = r;
            if (std::abs (A[piv][col]) < 1.0e-12) break;
            std::swap (A[col], A[piv]);
            for (int r = 0; r < 6; ++r)
            {
                if (r == col) continue;
                const double m = A[r][col] / A[col][col];
                for (int k = col; k < 7; ++k) A[r][k] -= m * A[col][k];
            }
        }
        for (int k = 0; k < 6; ++k) coef[k] = A[k][6] / A[k][k];
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (inside (x, y)) { const double e = lum[(size_t) (y * n + x)] - eval (x, y); err += e * e; }
        sigma = std::sqrt (err / count) + 0.004;
    }

    // the markings: the face divided by its light, around a mid level so that turning them under
    // a different light changes nothing but the light
    double mean = 0; int count = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            if (inside (x, y)) { mean += eval (x, y); ++count; }
    mean = count > 0 ? mean / count : 0.3;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
        {
            const size_t i = (size_t) (y * n + x);
            const float l = jmax (0.02f, (float) eval (x, y));
            f.light[i] = l;
            for (int k = 0; k < 3; ++k)
                f.detail[i * 3 + (size_t) k] *= (float) mean / l;
        }
    for (auto& l : f.light) l /= (float) mean;
}

const Image& PanelSkin::turnFace (const Face& f, float angle) const
{
    if (std::abs (angle - f.angle) < 1.0e-4f)
        return f.turned;
    f.angle = angle;
    const int n = f.n;
    const float c = ((float) n - 1.0f) / 2.0f, ca = std::cos (angle), sa = std::sin (angle);
    Image::BitmapData out (f.turned, Image::BitmapData::writeOnly);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
        {
            const size_t i = (size_t) (y * n + x);
            if (f.alpha[i] == 0) { out.setPixelColour (x, y, Colours::transparentBlack); continue; }
            // where this pixel was before the turn, kept on the face
            float u = (float) x - c, v = (float) y - c;
            float sx = ca * u + sa * v, sy = -sa * u + ca * v;
            const float rr = std::sqrt (sx * sx + sy * sy);
            if (rr > f.radius) { sx *= f.radius / rr; sy *= f.radius / rr; }
            sx += c; sy += c;
            const int x0 = jlimit (0, n - 2, (int) std::floor (sx)), y0 = jlimit (0, n - 2, (int) std::floor (sy));
            const float fx = jlimit (0.0f, 1.0f, sx - (float) x0), fy = jlimit (0.0f, 1.0f, sy - (float) y0);
            float rgb[3];
            for (int k = 0; k < 3; ++k)
            {
                auto at = [&] (int xx, int yy) { return f.detail[(size_t) (yy * n + xx) * 3 + (size_t) k]; };
                const float top = at (x0, y0) + (at (x0 + 1, y0) - at (x0, y0)) * fx;
                const float bot = at (x0, y0 + 1) + (at (x0 + 1, y0 + 1) - at (x0, y0 + 1)) * fx;
                rgb[k] = jlimit (0.0f, 1.0f, (top + (bot - top) * fy) * f.light[i]);
            }
            out.setPixelColour (x, y, Colour::fromFloatRGBA (rgb[0], rgb[1], rgb[2], f.alpha[i] / 255.0f));
        }
    return f.turned;
}

Image PanelSkin::renderBackground() const
{
    Canvas c (W, H, ppu);
    auto& g = c.g;

    if (photo.isValid())
    {
        // the cut-out photo on the desk, with the shadow it throws
        g.fillAll (room);
        const Rectangle<float> body (20, 20, W - 40, H - 40);
        for (int i = 14; i >= 1; --i)
        {
            g.setColour (Colours::black.withAlpha (0.04f));
            g.fillRoundedRectangle (body.translated (0, 8).expanded ((float) i * 1.6f), 48.0f + (float) i);
        }
        g.setOpacity (1.0f); // (drawImage takes the current colour's opacity)
        g.drawImage (photo, Rectangle<float> (0, 0, W, H), RectanglePlacement::stretchToFit);
        return c.image;
    }

    g.fillAll (room);
    const Rectangle<float> body (14, 14, W - 28, H - 28);

    // the device's shadow on the desk
    for (int i = 12; i >= 1; --i)
    {
        g.setColour (Colours::black.withAlpha (0.035f));
        g.fillRoundedRectangle (body.translated (0, 6).expanded ((float) i * 1.4f), 30.0f + (float) i);
    }

    // the body: a soft top light on matte plastic
    {
        Path p;
        p.addRoundedRectangle (body, 30.0f);
        g.setGradientFill (ColourGradient (bodyTop, 0, body.getY(), bodyBot, 0, body.getBottom(), false));
        g.fillPath (p);
        Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (p);
        ColourGradient sheen (Colours::white.withAlpha (0.10f), W * 0.3f, -60.0f,
                              Colours::transparentWhite, W * 0.3f, H * 0.55f, true);
        g.setGradientFill (sheen);
        g.fillRect (body);
    }
    // the rounded edge: lit along the top, shaded along the bottom
    g.setGradientFill (ColourGradient (Colours::white.withAlpha (0.35f), 0, body.getY(),
                                       Colours::black.withAlpha (0.45f), 0, body.getBottom(), false));
    g.drawRoundedRectangle (body.reduced (1.0f), 29.0f, 2.0f);
    g.setColour (Colours::black.withAlpha (0.6f));
    g.drawRoundedRectangle (body, 30.0f, 1.0f);

    // the screen's bezel: black glass over the panel
    {
        auto bz = bezel;
        g.setColour (Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (bz.translated (0, 3).expanded (2), 24.0f);
        g.setGradientFill (ColourGradient (Colour (0xff141416), 0, bz.getY(), Colour (0xff09090a), 0, bz.getBottom(), false));
        g.fillRoundedRectangle (bz, 22.0f);
        g.setColour (Colours::white.withAlpha (0.10f));
        g.drawRoundedRectangle (bz.reduced (0.5f), 22.0f, 1.0f);
        g.setColour (Colours::black);
        g.fillRect (lcd.expanded (3));
    }

    // trays: OCT, the button block, the keyboard
    recess (g, octTray, 14.0f, tray);
    recess (g, buttonTray, 18.0f, tray);
    recess (g, keyTray, 40.0f, tray);

    // knob wells: a ring of shadow the knob stands in
    for (auto& k : knobs)
    {
        const Rectangle<float> well (k.cx - k.r - 5, k.cy - k.r - 5, (k.r + 5) * 2, (k.r + 5) * 2);
        g.setGradientFill (ColourGradient (Colours::black.withAlpha (0.55f), k.cx, k.cy - k.r,
                                           Colours::white.withAlpha (0.10f), k.cx, k.cy + k.r, false));
        g.fillEllipse (well);
        for (int i = 8; i >= 1; --i)
        {
            g.setColour (Colours::black.withAlpha (0.05f));
            g.fillEllipse (Rectangle<float> (k.cx - k.r, k.cy - k.r, k.r * 2, k.r * 2).translated (1.5f, 4.0f).expanded ((float) i * 0.8f));
        }

        // the scale around the knob: detent ticks for encoders, a 270-degree arc for the pot
        g.setColour (silk.withAlpha (0.55f));
        if (k.enc < 0)
        {
            for (int t = 0; t <= 10; ++t)
            {
                const float a = degreesToRadians (-135.0f + 27.0f * (float) t);
                const float r0 = k.r + 9.0f, r1 = k.r + (t % 5 == 0 ? 15.0f : 12.5f);
                g.drawLine (k.cx + std::sin (a) * r0, k.cy - std::cos (a) * r0,
                            k.cx + std::sin (a) * r1, k.cy - std::cos (a) * r1, 1.6f);
            }
        }

        g.setColour (silk);
        g.setFont (font (15.0f));
        g.drawText (k.label, Rectangle<float> (k.cx - 80, k.cy - k.r - 34, 160, 18), Justification::centred);
    }

    // shadows under the buttons and the keys
    for (auto& b : buttons)
    {
        g.setColour (Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (b.r.translated (0.5f, 3.0f).expanded (0.5f), 13.0f);
    }
    for (auto& k : keys)
    {
        g.setColour (Colours::black.withAlpha (0.5f));
        g.fillRoundedRectangle (k.r.translated (0.5f, 4.0f).expanded (0.5f), k.r.getWidth() / 2.0f);
    }

    // the model name under the screen
    g.setColour (Colour (0xff9a978f));
    g.setFont (font (13.0f));
    g.drawText ("F M - 1", Rectangle<float> (bezel.getCentreX() - 100, bezel.getBottom() + 8, 200, 16), Justification::centred);

    addGrain (c.image, 0.025f, 0xf31);
    return c.image;
}

Image PanelSkin::renderRotor (const Knob& k) const
{
    if (photo.isValid())
    {
        // only the flat top face turns: the skirt is seen in perspective, and turning it with
        // the face would turn its shaded side round the knob as well
        const float size = k.tr * 2.0f + 2.0f, c0 = size / 2.0f;
        Canvas c (size, size, ppu);
        auto& g = c.g;
        Path face;
        face.addEllipse (c0 - k.tr, c0 - k.tr, k.tr * 2, k.tr * 2);
        g.reduceClipRegion (face);
        g.setOpacity (1.0f);
        g.drawImage (photo, Rectangle<float> (c0 - k.tx, c0 - k.ty, W, H));
        return c.image;
    }

    const float size = k.r * 2.0f + 4.0f;
    Canvas c (size, size, ppu);
    auto& g = c.g;
    const float cx = size / 2.0f, cy = size / 2.0f, r = k.r;

    // knurled skirt
    const int ridges = 36;
    for (int i = 0; i < ridges; ++i)
    {
        const float a0 = MathConstants<float>::twoPi * (float) i / (float) ridges;
        const float a1 = MathConstants<float>::twoPi * ((float) i + 0.5f) / (float) ridges;
        Path wedge;
        wedge.addPieSegment (cx - r, cy - r, r * 2, r * 2, a0, a1, 0.0f);
        g.setColour (Colour (0xff3a3d42));
        g.fillPath (wedge);
        Path wedge2;
        wedge2.addPieSegment (cx - r, cy - r, r * 2, r * 2, a1, a0 + MathConstants<float>::twoPi / (float) ridges, 0.0f);
        g.setColour (Colour (0xff2a2c30));
        g.fillPath (wedge2);
    }
    g.setColour (Colours::black.withAlpha (0.6f));
    g.drawEllipse (cx - r, cy - r, r * 2, r * 2, 1.0f);

    // the cap
    const float rc = r * 0.74f;
    g.setGradientFill (ColourGradient (Colour (0xff4a4d53), cx, cy - rc, Colour (0xff2f3236), cx, cy + rc, false));
    g.fillEllipse (cx - rc, cy - rc, rc * 2, rc * 2);
    // concentric machining on the cap
    for (float rr = rc * 0.2f; rr < rc; rr += 1.3f)
    {
        g.setColour (Colours::white.withAlpha (0.018f));
        g.drawEllipse (cx - rr, cy - rr, rr * 2, rr * 2, 0.5f);
    }
    g.setColour (Colours::black.withAlpha (0.5f));
    g.drawEllipse (cx - rc, cy - rc, rc * 2, rc * 2, 1.0f);

    // the pointer, pointing up
    Path ptr;
    ptr.addRoundedRectangle (cx - 1.8f, cy - rc + 3.0f, 3.6f, rc * 0.62f, 1.8f);
    g.setColour (Colour (0xfff1ede4));
    g.fillPath (ptr);
    return c.image;
}

Image PanelSkin::renderLight (const Knob& k) const
{
    const float size = k.r * 2.0f + 4.0f;
    Canvas c (size, size, ppu);
    if (photo.isValid())
        return c.image; // the photo has its own light

    auto& g = c.g;
    const float cx = size / 2.0f, cy = size / 2.0f, r = k.r;
    // the room's light stays put while the knob turns: a highlight up left, shade down right
    ColourGradient hl (Colours::white.withAlpha (0.22f), cx - r * 0.45f, cy - r * 0.55f,
                       Colours::transparentWhite, cx - r * 0.45f + r * 0.9f, cy - r * 0.55f + r * 0.9f, true);
    g.setGradientFill (hl);
    g.fillEllipse (cx - r, cy - r, r * 2, r * 2);
    ColourGradient sh (Colours::transparentBlack, cx, cy, Colours::black.withAlpha (0.35f), cx + r, cy + r, true);
    g.setGradientFill (sh);
    g.fillEllipse (cx - r, cy - r, r * 2, r * 2);
    // a specular arc on the skirt's top edge
    Path arc;
    arc.addCentredArc (cx, cy, r - 1.2f, r - 1.2f, 0.0f, -1.1f, 0.7f, true);
    g.setColour (Colours::white.withAlpha (0.25f));
    g.strokePath (arc, PathStrokeType (1.4f));
    return c.image;
}

Image PanelSkin::renderLcdGrid() const
{
    // the gaps between the screen's pixels, and its glass
    Canvas c (lcd.getWidth(), lcd.getHeight(), ppu);
    auto& g = c.g;
    if (ppu >= 1.2f)
    {
        g.setColour (Colours::black.withAlpha (0.22f));
        const float step = lcd.getWidth() / 240.0f, line = jmin (0.45f, 0.9f / ppu);
        for (int i = 1; i < 240; ++i)
        {
            g.fillRect ((float) i * step - line / 2, 0.0f, line, lcd.getHeight());
            g.fillRect (0.0f, (float) i * step - line / 2, lcd.getWidth(), line);
        }
    }
    ColourGradient glare (Colours::white.withAlpha (0.07f), 0, 0, Colours::transparentWhite, lcd.getWidth() * 0.6f, lcd.getHeight() * 0.6f, false);
    g.setGradientFill (glare);
    g.fillRect (Rectangle<float> (0, 0, lcd.getWidth(), lcd.getHeight()));
    // a faint vignette at the glass's edge
    for (int i = 0; i < 6; ++i)
    {
        g.setColour (Colours::black.withAlpha (0.06f));
        g.drawRoundedRectangle (Rectangle<float> (0, 0, lcd.getWidth(), lcd.getHeight()).reduced ((float) i + 0.5f), lcdCorner - (float) i, 1.0f);
    }
    return c.image;
}

// ------------------------------------------------------------------------------------------

void PanelSkin::paintButton (Graphics& g, const Button& b, bool down, float led) const
{
    const Colour tint = b.id == 12 ? green : amber;
    const float off = down ? 2.0f : 0.0f;
    auto r = b.r.translated (0, off);

    if (photo.isValid())
    {
        if (down)
        {
            g.setColour (Colours::black.withAlpha (0.28f));
            g.fillRoundedRectangle (b.r, 13.0f);
        }
        bloom (g, b.r, 13.0f, tint, led);
        g.setColour (tint.withAlpha (led * 0.45f));
        g.fillRoundedRectangle (b.r, 13.0f);
        return;
    }

    bloom (g, r, 13.0f, tint, led);
    g.setGradientFill (ColourGradient (mix (btnTop, tint, led * 0.85f), 0, r.getY(),
                                       mix (btnBot, tint, led * 0.7f), 0, r.getBottom(), false));
    g.fillRoundedRectangle (r, 13.0f);
    // the cap's top bevel
    g.setGradientFill (ColourGradient (Colours::white.withAlpha (down ? 0.06f : 0.16f), 0, r.getY(),
                                       Colours::transparentWhite, 0, r.getY() + 10.0f, false));
    g.drawRoundedRectangle (r.reduced (0.7f), 12.5f, 1.4f);
    g.setColour (Colours::black.withAlpha (0.55f));
    g.drawRoundedRectangle (r, 13.0f, 0.8f);

    g.setColour (led > 0.5f ? Colour (0xff2a1a03) : btnInk);
    g.setFont (font (13.0f));
    g.drawText (b.label, r, Justification::centred);
}

void PanelSkin::paintKey (Graphics& g, const Key& k, bool down, float led) const
{
    const float rad = k.r.getWidth() / 2.0f;
    const float heat = down ? 0.55f : led * 0.6f;

    if (photo.isValid())
    {
        // pressed: the silicone sinks into its shadow; lit: the light bar glows through
        if (down)
        {
            g.setColour (Colours::black.withAlpha (0.32f));
            g.fillRoundedRectangle (k.r.reduced (1.5f), rad - 1.5f);
        }
        const float glow = jmax (led, down ? 0.35f : 0.0f);
        if (glow > 0.0f)
        {
            const auto s = k.stripe;
            for (int i = 7; i >= 1; --i)
            {
                g.setColour (amber.withAlpha (glow * 0.06f * (8.0f - (float) i) / 7.0f));
                g.fillRoundedRectangle (s.expanded ((float) i * 2.4f), s.getWidth() / 2.0f + (float) i * 2.4f);
            }
            g.setColour (mix (Colour (0xfffff1d6), amber, 0.55f).withAlpha (0.35f + 0.6f * glow));
            g.fillRoundedRectangle (s, s.getWidth() / 2.0f);
        }
        return;
    }

    auto r = k.r.translated (0, down ? 3.0f : 0.0f);
    bloom (g, r, rad, amber, led);
    g.setGradientFill (ColourGradient (mix (keyTop, amber, heat), 0, r.getY(),
                                       mix (keyBot, amber, heat * 0.8f), 0, r.getBottom(), false));
    g.fillRoundedRectangle (r, rad);

    // soft-touch silicone: a broad dull highlight across the upper half
    ColourGradient sheen (Colours::white.withAlpha (0.11f), r.getCentreX(), r.getY() + rad * 0.6f,
                          Colours::transparentWhite, r.getCentreX(), r.getY() + rad * 2.0f, true);
    g.setGradientFill (sheen);
    g.fillRoundedRectangle (r.reduced (3.0f), rad - 3.0f);

    // the ridge down the middle
    const auto line = mix (keyLine, Colour (0xffffe2b0), down ? 1.0f : led);
    const auto s = k.stripe.translated (0, down ? 3.0f : 0.0f);
    g.setColour (Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (s.translated (0, 1.0f), s.getWidth() / 2.0f);
    g.setColour (line);
    g.fillRoundedRectangle (s, s.getWidth() / 2.0f);

    g.setColour (Colours::black.withAlpha (0.6f));
    g.drawRoundedRectangle (r, rad, 0.9f);
}

void PanelSkin::paint (Graphics& g, const Frame& f) const
{
    g.setImageResamplingQuality (Graphics::highResamplingQuality);
    g.setOpacity (1.0f);
    g.drawImage (background, Rectangle<float> (0, 0, W, H));

    // the screen: the bezel's whole glass, its corners rounded like the bezel's
    {
        Graphics::ScopedSaveState s (g);
        Path glass;
        glass.addRoundedRectangle (lcd, lcdCorner);
        g.reduceClipRegion (glass);
        g.setColour (Colour (0xff050506));
        g.fillRect (lcd);
        if (f.lcd != nullptr && f.lcdOn)
        {
            Graphics::ScopedSaveState s2 (g);
            g.setImageResamplingQuality (Graphics::lowResamplingQuality); // its pixels stay square
            g.setOpacity (1.0f);
            g.drawImage (*f.lcd, lcd);
        }
        g.setOpacity (1.0f);
        g.drawImage (lcdGrid, lcd);
    }

    // knobs: the rotor turns, the room's light does not
    for (size_t i = 0; i < knobs.size(); ++i)
    {
        const auto& k = knobs[i];
        const bool ph = photo.isValid();
        // (a photo turns the top face about its own centre, a rendered knob turns whole)
        const float px = ph ? k.tx : k.cx, py = ph ? k.ty : k.cy;
        const float size = ph ? k.tr * 2.0f + 2.0f : k.r * 2.0f + 4.0f;
        const auto box = Rectangle<float> (px - size / 2, py - size / 2, size, size);
        {
            Graphics::ScopedSaveState s (g);
            // (the photographed MASTER already points somewhere: turn it from there)
            const float a = f.knobAngle[i] - (ph && (int) i == potIndex ? potPhotoAngle : 0.0f);
            g.setOpacity (1.0f);
            if (ph && faces[i].n > 0)
            {
                // the markings turn, the light on the face stays where the photo has it
                g.drawImage (turnFace (faces[i], a), box);
            }
            else
            {
                g.addTransform (AffineTransform::rotation (a, px, py));
                g.drawImage (rotor[i], box);
            }
        }
        if (! ph)
        {
            g.setOpacity (1.0f);
            g.drawImage (light[i], box);
        }
    }

    for (auto& b : buttons)
        paintButton (g, b, f.down != nullptr && f.down[b.id], f.leds != nullptr ? bright (f.leds[b.id]) : 0.0f);
    for (auto& k : keys)
        if (! k.black)
            paintKey (g, k, f.down != nullptr && f.down[k.id], f.leds != nullptr ? bright (f.leds[k.id]) : 0.0f);
    for (auto& k : keys)
        if (k.black)
            paintKey (g, k, f.down != nullptr && f.down[k.id], f.leds != nullptr ? bright (f.leds[k.id]) : 0.0f);
}
