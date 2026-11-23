#include "EqDisplay.h"

// EqDisplay: the 3D mode views (3D perspective and Top). The Front view is the normal display (EqDisplay.cpp).

namespace steq
{

namespace
{
    constexpr float kBackScale = 0.6f;   // the back plane's size relative to the front one (perspective)
    const char* viewNames[3] = { "3D", "TOP", "FRONT" };

    float uFor (float hz, float lo, float hi) noexcept { return std::log (juce::jmax (lo, hz) / lo) / std::log (hi / lo); }
}

//==============================================================================
EqDisplay::View EqDisplay::getView() const
{
    if (! proc.refs.is3D()) return View::front;
    return (View) juce::jlimit (0, 2, (int) proc.uiState.getProperty ("view3d", 0));
}

void EqDisplay::setView (View v)
{
    proc.uiState.setProperty ("view3d", (int) v, nullptr);
    if (v != View::front && sketchMode) setSketchMode (false);
    readoutBand = hoverBand = hoverDepthBand = -1;
    hoverPart = Part::none;
    gridDirty = true;
    curveLayer = {};
    repaint();
}

juce::Rectangle<float> EqDisplay::planeAt (float w) const noexcept
{
    // front plane low and wide, back plane smaller and higher: the floor (-range) recedes into the screen
    const float H = plot.getHeight() - 18.0f, W = plot.getWidth();
    // a low camera: the floor is a shallow band, depth mostly shrinks things towards the middle
    const float f1 = compact ? 0.30f : 0.26f, f2 = compact ? 0.94f : 0.76f, b2 = compact ? 0.72f : 0.56f;
    const juce::Rectangle<float> front (plot.getX() + W * 0.07f, plot.getY() + H * f1, W * 0.82f, H * (f2 - f1));
    const float bw = front.getWidth() * kBackScale, bh = front.getHeight() * kBackScale;
    const juce::Rectangle<float> back (front.getCentreX() - bw * 0.5f, plot.getY() + H * b2 - bh, bw, bh);

    // perspective: equal depth steps get smaller towards the back
    const float a = 1.0f / kBackScale - 1.0f;
    const float s = 1.0f / (1.0f + juce::jlimit (0.0f, 1.0f, w) * a);
    const float t = (1.0f - s) / (1.0f - kBackScale);
    return { front.getX() + (back.getX() - front.getX()) * t, front.getY() + (back.getY() - front.getY()) * t,
             front.getWidth() + (back.getWidth() - front.getWidth()) * t, front.getHeight() + (back.getHeight() - front.getHeight()) * t };
}

juce::Point<float> EqDisplay::project (float hz, float db, float depth) const noexcept
{
    const auto r = planeAt (depth);
    return { r.getX() + uFor (hz, minHz, maxHz) * r.getWidth(), r.getCentreY() - db / displayRange * r.getHeight() * 0.5f };
}

float EqDisplay::depthForFloorY (float y) const noexcept
{
    const float f = planeAt (0.0f).getBottom(), b = planeAt (1.0f).getBottom();
    const float t = juce::jlimit (0.0f, 1.0f, (y - f) / (b - f));
    const float s = 1.0f - t * (1.0f - kBackScale);
    return juce::jlimit (0.0f, 1.0f, (1.0f / s - 1.0f) / (1.0f / kBackScale - 1.0f));
}

juce::Rectangle<float> EqDisplay::topArea() const noexcept
{
    const float H = plot.getHeight() - 18.0f;
    return { plot.getX(), plot.getY() + H * 0.12f, plot.getWidth(), H * (compact ? 0.76f : 0.56f) };
}

float EqDisplay::topY (float depth) const noexcept
{
    const auto a = topArea();
    return a.getBottom() - juce::jlimit (0.0f, 1.0f, depth) * a.getHeight();
}

float EqDisplay::topDepth (float y) const noexcept
{
    const auto a = topArea();
    return juce::jlimit (0.0f, 1.0f, (a.getBottom() - y) / juce::jmax (1.0f, a.getHeight()));
}

juce::Point<float> EqDisplay::depthHandle (int b) const
{
    const auto& p = bands[(size_t) b];
    return project (p.freq, -displayRange, p.depth * 0.01f);
}

int EqDisplay::depthHandleAt (juce::Point<float> pt) const
{
    if (getView() != View::threeD) return -1;
    int best = -1;
    float bestD = 9.0f;
    for (int b = kMaxBands - 1; b >= 0; --b)
    {
        if (! bands[(size_t) b].used) continue;
        const float d = depthHandle (b).getDistanceFrom (pt);
        if (d < bestD) { bestD = d; best = b; }
    }
    return best;
}

juce::Rectangle<float> EqDisplay::getViewButton (View v) const
{
    if (compact) return {};
    const float w = 42.0f, h = 18.0f;
    const float x0 = plot.getRight() - 42.0f - 10.0f - 3.0f * w;
    return { x0 + (float) (int) v * w, plot.getY() + 6.0f, w - 2.0f, h };
}

int EqDisplay::viewButtonAt (juce::Point<float> pt) const
{
    if (compact || ! proc.refs.is3D()) return -1;
    for (int v = 0; v < 3; ++v)
        if (getViewButton ((View) v).contains (pt)) return v;
    return -1;
}

void EqDisplay::curveDepths (std::array<float, kPoints>& out) const
{
    for (int i = 0; i < kPoints; ++i)
    {
        float num = 0.0f, den = 0.0f;
        for (int b = 0; b < kMaxBands; ++b)
        {
            const auto& p = bands[(size_t) b];
            const auto& c = curves[(size_t) b];
            if (! p.isActive() || ! c.valid) continue;
            // weight: what the band visibly does (a cut's -200 dB past Nyquist mustn't drag the curve to its plane)
            const float w = juce::jmin (std::abs (c.db[(size_t) i]), displayRange * 1.2f);
            num += w * p.depth * 0.01f;
            den += w;
        }
        // where no band acts the curve comes back to the front
        out[(size_t) i] = juce::jlimit (0.0f, 1.0f, num / (den + 0.35f));
    }
}

juce::Point<float> EqDisplay::frontPointFor (juce::Point<float> pt, float& depth) const
{
    depth = 0.0f;
    if (getView() == View::top)
    {
        depth = topDepth (pt.y);
        return { pt.x, yForDb (0.0f) };
    }
    const auto r = planeAt (0.0f);
    const float u = juce::jlimit (0.0f, 1.0f, (pt.x - r.getX()) / juce::jmax (1.0f, r.getWidth()));
    const float hz = minHz * std::pow (maxHz / minHz, u);
    const float db = (r.getCentreY() - pt.y) / (r.getHeight() * 0.5f) * displayRange;
    return { xForHz (hz), yForDb (db) };
}

//==============================================================================
void EqDisplay::paintViewButtons (juce::Graphics& g)
{
    if (compact || ! proc.refs.is3D()) return;
    const auto& t = st::Theme::get();
    const auto cur = getView();
    g.setFont (st::fonts::semibold (10.0f));
    for (int v = 0; v < 3; ++v)
    {
        const auto r = getViewButton ((View) v);
        const bool sel = (int) cur == v, over = r.contains (mousePos);
        g.setColour (sel ? t.curve.withAlpha (0.16f) : juce::Colours::white.withAlpha (over ? 0.09f : 0.04f));
        g.fillRoundedRectangle (r, 3.0f);
        if (sel)
        {
            g.setColour (t.curve.withAlpha (0.45f));
            g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);
        }
        g.setColour (sel ? t.curve : (over ? t.text : t.textDim));
        g.drawText (viewNames[v], r, juce::Justification::centred, false);
    }
}

void EqDisplay::paint3D (juce::Graphics& g)
{
    const auto& t = st::Theme::get();
    const float range = displayRange;
    const auto F = planeAt (0.0f), B = planeAt (1.0f);
    auto P = [this] (float hz, float db, float w) { return project (hz, db, w); };
    auto clampDb = [range] (float db) { return juce::jlimit (-range, range * 1.2f, db); };
    auto line = [&g] (juce::Point<float> a, juce::Point<float> b, float width = 1.0f) { g.drawLine ({ a, b }, width); };

    g.saveState();
    g.reduceClipRegion (plot.withBottom (plot.getBottom() - (compact ? 0.0f : 2.0f)).toNearestInt());

    // floor and back wall
    {
        juce::Path floor;
        floor.startNewSubPath (F.getBottomLeft());
        floor.lineTo (F.getBottomRight());
        floor.lineTo (B.getBottomRight());
        floor.lineTo (B.getBottomLeft());
        floor.closeSubPath();
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.05f), 0.0f, F.getBottom(),
                                                 juce::Colours::white.withAlpha (0.015f), 0.0f, B.getBottom(), false));
        g.fillPath (floor);
        g.setColour (juce::Colours::white.withAlpha (0.02f));
        g.fillRect (B);
    }

    // frequency lines along the floor and up the back wall
    for (float decade : { 10.0f, 100.0f, 1000.0f, 10000.0f })
        for (int m = 1; m <= 9; ++m)
        {
            const float hz = decade * (float) m;
            if (hz < minHz || hz > maxHz) continue;
            const float a = m == 1 ? 0.08f : (m == 2 || m == 5 ? 0.05f : 0.025f);
            g.setColour (juce::Colours::white.withAlpha (a));
            line (P (hz, -range, 0.0f), P (hz, -range, 1.0f));
            g.setColour (juce::Colours::white.withAlpha (a * 0.7f));
            line (P (hz, -range, 1.0f), P (hz, range, 1.0f));
        }

    // depth lines across the floor; floor edges; the 0 dB plane's edges
    for (float w : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        g.setColour (juce::Colours::white.withAlpha (w == 0.0f || w == 1.0f ? 0.10f : 0.045f));
        line (P (minHz, -range, w), P (maxHz, -range, w));
    }
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    line (P (minHz, -range, 0.0f), P (minHz, -range, 1.0f));
    line (P (maxHz, -range, 0.0f), P (maxHz, -range, 1.0f));
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    line (P (minHz, 0.0f, 0.0f), P (minHz, 0.0f, 1.0f));
    line (P (maxHz, 0.0f, 0.0f), P (maxHz, 0.0f, 1.0f));
    line (P (minHz, range, 1.0f), P (maxHz, range, 1.0f));
    line (P (minHz, range, 1.0f), P (minHz, -range, 1.0f));
    line (P (maxHz, range, 1.0f), P (maxHz, -range, 1.0f));

    // dB lines on the back wall; 0 dB brighter (and across the front)
    const float step = range <= 4.5f ? 1.0f : (range <= 9.0f ? 2.0f : (range <= 18.0f ? 3.0f : 6.0f));
    for (float db = -std::floor (range / step + 0.001f) * step; db <= range + 0.01f; db += step)
    {
        const bool zero = std::abs (db) < 0.01f;
        g.setColour (juce::Colours::white.withAlpha (zero ? 0.10f : 0.035f));
        line (P (minHz, db, 1.0f), P (maxHz, db, 1.0f));
    }
    g.setColour (juce::Colours::white.withAlpha (0.07f));
    line (P (minHz, 0.0f, 0.0f), P (maxHz, 0.0f, 0.0f));

    // spectrum on the back wall
    if (analyzer.pre || analyzer.post)
    {
        const float aRange = AnalyzerSettings::ranges[analyzer.rangeIndex];
        const float nyquist = (float) (sampleRate * 0.5) * 0.985f;
        auto wallPath = [&] (const st::SpectrumAnalyzer& s, bool closed)
        {
            const auto& lv = s.getLevelsDb();
            juce::Path p;
            bool started = false;
            float lastX = B.getX();
            for (int i = 0; i < (int) lv.size(); ++i)
            {
                const float hz = s.getFrequencyForPoint (i);
                if (hz < minHz || hz > juce::jmin (maxHz, nyquist)) continue;
                const float x = B.getX() + uFor (hz, minHz, maxHz) * B.getWidth();
                const float y = B.getBottom() - B.getHeight() * juce::jlimit (0.0f, 1.0f, 1.0f + lv[(size_t) i] / aRange);
                if (! started) { p.startNewSubPath (x, closed ? B.getBottom() : y); if (closed) p.lineTo (x, y); started = true; }
                else p.lineTo (x, y);
                lastX = x;
            }
            if (closed && started) { p.lineTo (lastX, B.getBottom()); p.closeSubPath(); }
            return p;
        };
        if (analyzer.pre)
        {
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.fillPath (wallPath (*preSpectrum, true));
        }
        if (analyzer.post)
        {
            g.setColour (t.spectrum.withAlpha (0.09f));
            g.fillPath (wallPath (*postSpectrum, true));
            g.setColour (t.spectrum.withAlpha (0.45f));
            g.strokePath (wallPath (*postSpectrum, false), juce::PathStrokeType (1.0f, juce::PathStrokeType::curved));
        }
    }

    // each band's curve standing at its depth, back to front
    std::array<int, kMaxBands> order {};
    for (int b = 0; b < kMaxBands; ++b) order[(size_t) b] = b;
    std::stable_sort (order.begin(), order.end(), [this] (int a, int b) { return bands[(size_t) a].depth > bands[(size_t) b].depth; });
    const int solo = proc.soloBand.load();
    for (int b : order)
    {
        const auto& p = bands[(size_t) b];
        const auto& c = curves[(size_t) b];
        if (! p.used || ! c.valid) continue;
        const float w = p.depth * 0.01f;
        const bool sel = selection.contains (b);
        const float s = selectAmt[(size_t) b].current, h = hoverAmt[(size_t) b].current;
        const auto col = (p.isActive() ? bandColour (b) : t.textFaint).withMultipliedAlpha (solo >= 0 && solo != b ? 0.3f : 1.0f);

        juce::Path curve;
        for (int i = 0; i < kPoints; i += 2)
        {
            const auto pt = P (freqs[(size_t) i], clampDb (c.db[(size_t) i]), w);
            if (i == 0) curve.startNewSubPath (pt); else curve.lineTo (pt);
        }
        auto area = curve;
        area.lineTo (P (freqs[(size_t) kPoints - 2], 0.0f, w));
        area.lineTo (P (freqs[0], 0.0f, w));
        area.closeSubPath();
        g.setColour (col.withAlpha ((p.isActive() ? 0.10f : 0.04f) + 0.20f * s + 0.08f * h));
        g.fillPath (area);
        g.setColour (col.withAlpha (sel ? 0.9f : (0.35f + 0.4f * h) * (p.isActive() ? 1.0f : 0.5f)));
        g.strokePath (curve, juce::PathStrokeType (sel ? 1.5f : 1.0f, juce::PathStrokeType::curved));
    }

    // the total curve runs through the bands' depths; its shadow on the floor
    std::array<float, kPoints> dep {};
    curveDepths (dep);
    juce::Path total, shadow;
    for (int i = 0; i < kPoints; ++i)
    {
        const auto pt = P (freqs[(size_t) i], clampDb (totals[0][(size_t) i]), dep[(size_t) i]);
        const auto sh = P (freqs[(size_t) i], -range, dep[(size_t) i]);
        if (i == 0) { total.startNewSubPath (pt); shadow.startNewSubPath (sh); }
        else        { total.lineTo (pt); shadow.lineTo (sh); }
    }
    g.setColour (t.curve.withAlpha (0.18f));
    g.strokePath (shadow, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved));
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.strokePath (total, juce::PathStrokeType (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (t.curve);
    g.strokePath (total, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // stems from each dot down to its depth handle on the floor
    for (int b = 0; b < kMaxBands; ++b)
    {
        const auto& p = bands[(size_t) b];
        if (! p.used) continue;
        const auto col = bandColour (b);
        const auto dp = dotPosition (b), fh = depthHandle (b);
        const float hv = juce::jmax (hoverAmt[(size_t) b].current, b == hoverDepthBand ? 1.0f : 0.0f, selectAmt[(size_t) b].current);
        juce::Path stem, dashed;
        stem.startNewSubPath (dp);
        stem.lineTo (fh);
        const float dashes[] = { 2.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, stem, dashes, 2);
        g.setColour (col.withAlpha (0.25f + 0.35f * hv));
        g.fillPath (dashed);
        // the handle: a flat disc on the floor (scaled with the distance)
        const float k = planeAt (p.depth * 0.01f).getWidth() / F.getWidth();
        const auto disc = juce::Rectangle<float> ((11.0f + 3.0f * hv) * k, (5.0f + 1.5f * hv) * k).withCentre (fh);
        g.setColour (col.withAlpha (0.45f + 0.45f * hv));
        g.fillEllipse (disc);
        if (b == hoverDepthBand)
        {
            g.setColour (juce::Colours::white.withAlpha (0.7f));
            g.drawEllipse (disc.expanded (1.5f), 1.0f);
        }
    }
    g.restoreState();

    // labels: frequency along the front edge, dB at the front plane's left, depth along the right floor edge
    if (! compact)
    {
        g.setFont (st::fonts::medium (10.0f));
        g.setColour (t.textFaint);
        for (float hz : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
        {
            const auto pt = P (hz, -range, 0.0f);
            const auto label = hz >= 1000.0f ? juce::String ((int) (hz / 1000.0f)) + "k" : juce::String ((int) hz);
            g.drawText (label, juce::Rectangle<float> (pt.x - 20.0f, pt.y + 4.0f, 40.0f, 14.0f), juce::Justification::centred, false);
        }
        g.setFont (st::fonts::semibold (10.0f));
        for (float db = -std::floor (range / step + 0.001f) * step; db <= range + 0.01f; db += step)
        {
            const bool zero = std::abs (db) < 0.01f;
            const auto pt = P (minHz, db, 0.0f);
            g.setColour (t.curve.withAlpha (zero ? 0.95f : 0.75f));
            g.drawText (zero ? juce::String ("0") : (db > 0 ? "+" : "") + juce::String ((int) db),
                        juce::Rectangle<float> (pt.x - 34.0f, pt.y - 7.0f, 28.0f, 14.0f), juce::Justification::centredRight, false);
        }
        g.setFont (st::fonts::medium (10.0f));
        g.setColour (t.textDim);
        for (float w : { 0.0f, 0.5f, 1.0f })
        {
            const auto pt = P (maxHz, -range, w);
            g.drawText (juce::String (juce::roundToInt (w * 100.0f)), juce::Rectangle<float> (pt.x + 6.0f, pt.y - 7.0f, 30.0f, 14.0f),
                        juce::Justification::centredLeft, false);
        }
        const auto mid = P (maxHz, -range, 0.5f);
        g.setFont (st::fonts::semibold (9.0f));
        g.setColour (t.textFaint);
        g.drawText ("DEPTH", juce::Rectangle<float> (mid.x + 6.0f, mid.y + 6.0f, 40.0f, 12.0f), juce::Justification::centredLeft, false);
    }
}

void EqDisplay::paintTop (juce::Graphics& g)
{
    const auto& t = st::Theme::get();
    const auto area = topArea();
    const float range = displayRange;

    g.saveState();
    g.reduceClipRegion (plot.toNearestInt());

    g.setColour (juce::Colours::white.withAlpha (0.02f));
    g.fillRect (area);

    // frequency lines; depth lines (front at the bottom)
    for (float decade : { 10.0f, 100.0f, 1000.0f, 10000.0f })
        for (int m = 1; m <= 9; ++m)
        {
            const float hz = decade * (float) m;
            if (hz < minHz || hz > maxHz) continue;
            const float x = std::round (xForHz (hz)) + 0.5f;
            g.setColour (juce::Colours::white.withAlpha (m == 1 ? 0.075f : (m == 2 || m == 5 ? 0.05f : 0.025f)));
            g.fillRect (x - 0.5f, area.getY(), 1.0f, area.getHeight());
        }
    for (float w : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
    {
        const float y = std::round (topY (w)) + 0.5f;
        g.setColour (juce::Colours::white.withAlpha (w == 0.0f || w == 1.0f ? 0.10f : 0.045f));
        g.fillRect (area.getX(), y - 0.5f, area.getWidth(), 1.0f);
    }

    // each band: a strip at its depth, as thick and bright as the band acts at each frequency
    const int solo = proc.soloBand.load();
    for (int pass = 0; pass < 2; ++pass)
        for (int b = 0; b < kMaxBands; ++b)
        {
            const auto& p = bands[(size_t) b];
            const auto& c = curves[(size_t) b];
            if (! p.used || ! c.valid) continue;
            const bool sel = selection.contains (b);
            if ((pass == 1) != sel) continue;
            const float y = topY (p.depth * 0.01f);
            const float s = selectAmt[(size_t) b].current, h = hoverAmt[(size_t) b].current;
            const auto col = (p.isActive() ? bandColour (b) : t.textFaint).withMultipliedAlpha (solo >= 0 && solo != b ? 0.3f : 1.0f);

            // thickness: how much the band does at each frequency (full at 12 dB, or half the scale if larger)
            const float full = juce::jmax (6.0f, juce::jmin (12.0f, range * 0.5f));
            juce::Path strip;
            for (int i = 0; i < kPoints; i += 2)
            {
                const float a = juce::jlimit (0.0f, 1.0f, std::abs (c.db[(size_t) i]) / full);
                const float pt = y - (0.5f + 12.0f * a);
                if (i == 0) strip.startNewSubPath (xs[0], pt); else strip.lineTo (xs[(size_t) i], pt);
            }
            for (int i = kPoints - 2; i >= 0; i -= 2)
            {
                const float a = juce::jlimit (0.0f, 1.0f, std::abs (c.db[(size_t) i]) / full);
                strip.lineTo (xs[(size_t) i], y + 0.5f + 12.0f * a);
            }
            strip.closeSubPath();
            g.setColour (col.withAlpha ((p.isActive() ? 0.22f : 0.08f) + 0.25f * s + 0.1f * h));
            g.fillPath (strip);
            g.setColour (col.withAlpha (sel ? 0.85f : 0.4f + 0.3f * h));
            g.strokePath (strip, juce::PathStrokeType (sel ? 1.2f : 0.8f));
        }

    // the total curve's depth across the spectrum
    std::array<float, kPoints> dep {};
    curveDepths (dep);
    juce::Path path;
    for (int i = 0; i < kPoints; ++i)
    {
        const float y = topY (dep[(size_t) i]);
        if (i == 0) path.startNewSubPath (xs[0], y); else path.lineTo (xs[(size_t) i], y);
    }
    g.setColour (t.curve.withAlpha (0.85f));
    g.strokePath (path, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved));
    g.restoreState();

    if (! compact)
    {
        g.setFont (st::fonts::medium (10.0f));
        g.setColour (t.textFaint);
        for (float hz : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
        {
            const float x = xForHz (hz);
            const auto label = hz >= 1000.0f ? juce::String ((int) (hz / 1000.0f)) + "k" : juce::String ((int) hz);
            g.drawText (label, juce::Rectangle<float> (x - 20.0f, area.getBottom() + 18.0f, 40.0f, 14.0f), juce::Justification::centred, false);
        }
        g.setColour (t.textDim);
        for (float w : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            g.drawText (juce::String (juce::roundToInt (w * 100.0f)), juce::Rectangle<float> (plot.getRight() - 34.0f, topY (w) - 7.0f, 28.0f, 14.0f),
                        juce::Justification::centredRight, false);
        g.setFont (st::fonts::semibold (9.0f));
        g.setColour (t.textFaint);
        g.drawText ("DEPTH", juce::Rectangle<float> (plot.getRight() - 52.0f, area.getY() - 16.0f, 46.0f, 12.0f), juce::Justification::centredRight, false);
    }
}

} // namespace steq
