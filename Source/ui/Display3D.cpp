#include "EqDisplay.h"

// EqDisplay: the 3D mode views (3D perspective and Top). The Front view is the normal display (EqDisplay.cpp).

namespace steq
{

namespace
{
    const char* viewNames[3] = { "3D", "TOP", "FRONT" };

    float uFor (float hz, float lo, float hi) noexcept { return std::log (juce::jmax (lo, hz) / lo) / std::log (hi / lo); }
}

//==============================================================================
EqDisplay::View EqDisplay::getView() const
{
    if (! proc.refs.is3D()) return View::front;
    return (View) juce::jlimit (0, 2, (int) proc.uiState.getProperty ("view3d", (int) View::front));
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

// The 3D camera: the box (frequency -1..1 across, gain up, depth 0..kBoxDepth back) seen from above and a little
// to the left, turned so the high end and the back recede; perspective, then fitted to the plot.
namespace
{
    constexpr float kBoxHalfH = 0.42f, kBoxDepth = 1.05f, kDist = 2.5f;
    constexpr float kMinYaw = -0.75f, kMaxYaw = 0.75f, kMinPitch = 0.0f, kMaxPitch = 1.3f;
}

juce::Point<float> EqDisplay::Camera::raw (float X, float Y, float Z) const noexcept
{
    const float pz = Z - kBoxDepth * 0.5f;
    const float x1 = X * cy - pz * sy, z1 = X * sy + pz * cy;
    const float y2 = Y * cp + z1 * sp, z2 = -Y * sp + z1 * cp;
    const float s = kDist / (kDist + z2);
    return { x1 * s, y2 * s };
}

bool EqDisplay::Camera::unproject (juce::Point<float> r, float Z, float& X, float& Y) const noexcept
{
    // raw = (x1, y2) * D / (D + z2) is linear in X, Y once multiplied out: a 2x2 system
    const float c = Z - kBoxDepth * 0.5f, a = r.x / kDist, b = r.y / kDist;
    const float m11 = cy - a * sy * cp, m12 = a * sp,      k1 = a * kDist + a * c * cy * cp + c * sy;
    const float m21 = sy * sp - b * sy * cp, m22 = cp + b * sp, k2 = b * kDist + b * c * cy * cp - c * cy * sp;
    const float det = m11 * m22 - m12 * m21;
    if (std::abs (det) < 1.0e-7f) return false;
    X = (k1 * m22 - m12 * k2) / det;
    Y = (m11 * k2 - m21 * k1) / det;
    return true;
}

float EqDisplay::getCameraYaw() const   { return juce::jlimit (kMinYaw, kMaxYaw, (float) (double) proc.uiState.getProperty ("camYaw", defaultYaw)); }
float EqDisplay::getCameraPitch() const { return juce::jlimit (kMinPitch, kMaxPitch, (float) (double) proc.uiState.getProperty ("camPitch", defaultPitch)); }

void EqDisplay::setCameraAngles (float yaw, float pitch)
{
    proc.uiState.setProperty ("camYaw", juce::jlimit (kMinYaw, kMaxYaw, yaw), nullptr);
    proc.uiState.setProperty ("camPitch", juce::jlimit (kMinPitch, kMaxPitch, pitch), nullptr);
    repaint();
}

juce::Point<float> EqDisplay::getGizmoCentre() const noexcept
{
    const auto vb = getViewButton (View::front);
    return { vb.getRight() - 24.0f, vb.getBottom() + 36.0f };
}

bool EqDisplay::gizmoAt (juce::Point<float> pt) const noexcept
{
    return ! compact && getView() == View::threeD && getGizmoCentre().getDistanceFrom (pt) < 26.0f;
}

const EqDisplay::Camera& EqDisplay::getCamera() const noexcept
{
    const float yaw = getCameraYaw(), pitch = getCameraPitch();
    if (camera.valid && camera.key == plot && juce::exactlyEqual (camera.keyYaw, yaw) && juce::exactlyEqual (camera.keyPitch, pitch)) return camera;
    camera.cy = std::cos (yaw); camera.sy = std::sin (yaw);
    camera.cp = std::cos (pitch); camera.sp = std::sin (pitch);
    const float H = plot.getHeight() - 18.0f, W = plot.getWidth();
    const auto target = compact ? juce::Rectangle<float> (plot.getX() + W * 0.04f, plot.getY() + H * 0.10f, W * 0.92f, H * 0.84f)
                                : juce::Rectangle<float> (plot.getX() + W * 0.07f, plot.getY() + H * 0.15f, W * 0.85f, H * 0.62f);
    float x0 = 1.0e9f, x1 = -1.0e9f, y0 = 1.0e9f, y1 = -1.0e9f;
    for (float X : { -1.0f, 1.0f })
        for (float Y : { -kBoxHalfH, kBoxHalfH * 1.2f })
            for (float Z : { 0.0f, kBoxDepth })
            {
                const auto p = camera.raw (X, Y, Z);
                x0 = juce::jmin (x0, p.x); x1 = juce::jmax (x1, p.x); y0 = juce::jmin (y0, p.y); y1 = juce::jmax (y1, p.y);
            }
    camera.kx = target.getWidth() / juce::jmax (1.0e-6f, x1 - x0);
    camera.ky = target.getHeight() / juce::jmax (1.0e-6f, y1 - y0);
    camera.ox = target.getX() - x0 * camera.kx;
    camera.oy = target.getBottom() + y0 * camera.ky;
    camera.key = plot;
    camera.keyYaw = yaw;
    camera.keyPitch = pitch;
    camera.valid = true;
    return camera;
}

juce::Point<float> EqDisplay::projectUnit (float u, float v, float depth) const noexcept
{
    const auto& c = getCamera();
    const auto p = c.raw (u * 2.0f - 1.0f, v * kBoxHalfH, juce::jlimit (0.0f, 1.0f, depth) * kBoxDepth);
    return { c.ox + p.x * c.kx, c.oy - p.y * c.ky };
}

juce::Point<float> EqDisplay::project (float hz, float db, float depth) const noexcept
{
    return projectUnit (uFor (hz, minHz, maxHz), db / juce::jmax (0.1f, displayRange), depth);
}

bool EqDisplay::planePoint (juce::Point<float> pt, float depth, float& hz, float& db) const noexcept
{
    const auto& c = getCamera();
    float X = 0.0f, Y = 0.0f;
    if (! c.unproject ({ (pt.x - c.ox) / c.kx, (c.oy - pt.y) / c.ky }, juce::jlimit (0.0f, 1.0f, depth) * kBoxDepth, X, Y)) return false;
    const float u = juce::jlimit (-1.0f, 2.0f, (X + 1.0f) * 0.5f), v = juce::jlimit (-20.0f, 20.0f, Y / kBoxHalfH);
    hz = minHz * std::pow (maxHz / minHz, u);
    db = v * displayRange;
    return true;
}

float EqDisplay::depthAlongFloor (float hz, juce::Point<float> pt) const noexcept
{
    auto dist = [&] (float w) { return project (hz, -displayRange, w).getDistanceSquaredFrom (pt); };
    float best = 0.0f, bestD = 1.0e18f;
    for (int i = 0; i <= 40; ++i)
        if (const float d = dist ((float) i / 40.0f); d < bestD) { bestD = d; best = (float) i / 40.0f; }
    float lo = juce::jmax (0.0f, best - 0.025f), hi = juce::jmin (1.0f, best + 0.025f);
    for (int i = 0; i < 24; ++i)
    {
        const float a = lo + (hi - lo) / 3.0f, b = hi - (hi - lo) / 3.0f;
        if (dist (a) < dist (b)) hi = b; else lo = a;
    }
    return juce::jlimit (0.0f, 1.0f, 0.5f * (lo + hi));
}

float EqDisplay::depthScale (float depth) const noexcept
{
    const float front = projectUnit (0.0f, 0.0f, 0.0f).getDistanceFrom (projectUnit (1.0f, 0.0f, 0.0f));
    return projectUnit (0.0f, 0.0f, depth).getDistanceFrom (projectUnit (1.0f, 0.0f, depth)) / juce::jmax (1.0f, front);
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
    float hz = 1000.0f, db = 0.0f;
    if (! planePoint (pt, 0.0f, hz, db)) return { pt.x, yForDb (0.0f) };
    return { xForHz (juce::jlimit (minHz, maxHz, hz)), yForDb (db) };
}

bool EqDisplay::pushWaterfall (double dt)
{
    // a new slice ~30 times a second; the oldest falls off the back
    waterfallClock += dt;
    if (waterfallClock < 1.0 / 30.0) return false;
    waterfallClock = 0.0;
    const auto& src = analyzer.post ? *postSpectrum : *preSpectrum;
    const int n = src.getNumPoints();
    if (n <= 0 || ! (analyzer.post || analyzer.pre)) { waterfallCount = 0; return false; }
    if ((int) waterfallHz.size() != n)
    {
        waterfallHz.resize ((size_t) n);
        for (int i = 0; i < n; ++i) waterfallHz[(size_t) i] = src.getFrequencyForPoint (i);
        waterfallCount = 0;
    }
    waterfallHead = (waterfallHead + 1) % kWaterfall;
    waterfall[(size_t) waterfallHead] = src.getLevelsDb();
    waterfallCount = juce::jmin (kWaterfall, waterfallCount + 1);
    return true;
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
    auto P = [this] (float hz, float db, float w) { return project (hz, db, w); };
    auto clampDb = [range] (float db) { return juce::jlimit (-range, range * 1.2f, db); };
    auto line = [&g] (juce::Point<float> a, juce::Point<float> b, float width = 1.0f) { g.drawLine ({ a, b }, width); };
    const juce::Colour ice (0xffa9dcee);   // the analyser waterfall

    g.saveState();
    g.reduceClipRegion (plot.withBottom (plot.getBottom() - (compact ? 0.0f : 2.0f)).toNearestInt());

    // floor and back wall
    {
        juce::Path floor;
        floor.startNewSubPath (P (minHz, -range, 0.0f));
        floor.lineTo (P (maxHz, -range, 0.0f));
        floor.lineTo (P (maxHz, -range, 1.0f));
        floor.lineTo (P (minHz, -range, 1.0f));
        floor.closeSubPath();
        const auto f0 = P (1000.0f, -range, 0.0f), f1 = P (1000.0f, -range, 1.0f);
        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.045f), f0.x, f0.y,
                                                 juce::Colours::white.withAlpha (0.012f), f1.x, f1.y, false));
        g.fillPath (floor);
        juce::Path wall;
        wall.startNewSubPath (P (minHz, -range, 1.0f));
        wall.lineTo (P (maxHz, -range, 1.0f));
        wall.lineTo (P (maxHz, range, 1.0f));
        wall.lineTo (P (minHz, range, 1.0f));
        wall.closeSubPath();
        g.setColour (juce::Colours::white.withAlpha (0.015f));
        g.fillPath (wall);
    }

    // fine grid: frequency lines along the floor and up the back wall, depth lines across the floor
    for (float decade : { 10.0f, 100.0f, 1000.0f, 10000.0f })
        for (int m = 1; m <= 9; ++m)
        {
            const float hz = decade * (float) m;
            if (hz < minHz || hz > maxHz) continue;
            const float a = m == 1 ? 0.075f : (m == 2 || m == 5 ? 0.045f : 0.022f);
            g.setColour (juce::Colours::white.withAlpha (a));
            line (P (hz, -range, 0.0f), P (hz, -range, 1.0f), 0.8f);
            g.setColour (juce::Colours::white.withAlpha (a * 0.6f));
            line (P (hz, -range, 1.0f), P (hz, range, 1.0f), 0.8f);
        }
    for (int k = 0; k <= 8; ++k)
    {
        const float w = (float) k / 8.0f;
        g.setColour (juce::Colours::white.withAlpha (k == 0 || k == 8 ? 0.10f : (k % 2 == 0 ? 0.04f : 0.022f)));
        line (P (minHz, -range, w), P (maxHz, -range, w), 0.8f);
    }

    // box edges: floor sides, the back wall's frame, the front left post (the dB scale) and the 0 dB plane's sides
    g.setColour (juce::Colours::white.withAlpha (0.09f));
    line (P (minHz, -range, 0.0f), P (minHz, -range, 1.0f));
    line (P (maxHz, -range, 0.0f), P (maxHz, -range, 1.0f));
    line (P (minHz, range, 1.0f), P (maxHz, range, 1.0f));
    line (P (minHz, range, 1.0f), P (minHz, -range, 1.0f));
    line (P (maxHz, range, 1.0f), P (maxHz, -range, 1.0f));
    g.setColour (juce::Colours::white.withAlpha (0.12f));
    line (P (minHz, -range, 0.0f), P (minHz, range, 0.0f));
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    line (P (minHz, 0.0f, 0.0f), P (minHz, 0.0f, 1.0f));
    line (P (maxHz, 0.0f, 0.0f), P (maxHz, 0.0f, 1.0f));

    // dB lines on the back wall (ticks on the front post); 0 dB brighter and across the front
    const float step = range <= 4.5f ? 1.0f : (range <= 9.0f ? 2.0f : (range <= 18.0f ? 3.0f : 6.0f));
    for (float db = -std::floor (range / step + 0.001f) * step; db <= range + 0.01f; db += step)
    {
        const bool zero = std::abs (db) < 0.01f;
        g.setColour (juce::Colours::white.withAlpha (zero ? 0.09f : 0.03f));
        line (P (minHz, db, 1.0f), P (maxHz, db, 1.0f), 0.8f);
        const auto a = P (minHz, db, 0.0f);
        g.setColour (juce::Colours::white.withAlpha (zero ? 0.3f : 0.16f));
        line (a, a.translated (-5.0f, 0.0f));
    }
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    line (P (minHz, 0.0f, 0.0f), P (maxHz, 0.0f, 0.0f));

    // --- what stands in the box, back to front: each band's curve at its depth and the analyser waterfall ---
    const int solo = proc.soloBand.load();
    const float aRange = AnalyzerSettings::ranges[analyzer.rangeIndex];
    const float topHz = juce::jmin (maxHz, (float) (sampleRate * 0.5) * 0.985f);
    constexpr float kWaterfallSpan = 0.36f;   // the waterfall covers the front of the floor
    auto sliceDb = [&] (float lv) { return -range + 2.0f * range * 0.68f * juce::jlimit (0.0f, 1.0f, 1.0f + lv / aRange); };

    // a slice: the spectrum stood up at a depth (decimated, keeping peaks)
    auto slicePath = [&] (const std::vector<float>& lv, const std::vector<float>& hz, float w, bool closed, int maxPoints)
    {
        juce::Path p;
        const int n = (int) juce::jmin (lv.size(), hz.size());
        int i0 = 0, i1 = n - 1;
        while (i0 < n && hz[(size_t) i0] < 20.0f) ++i0;
        while (i1 > i0 && hz[(size_t) i1] > topHz) --i1;
        if (i1 - i0 < 2) return p;
        const int stride = juce::jmax (1, (i1 - i0) / maxPoints);
        bool started = false;
        juce::Point<float> last;
        for (int i = i0; i <= i1; i += stride)
        {
            float m = -200.0f;
            for (int j = i; j < juce::jmin (i1 + 1, i + stride); ++j) m = juce::jmax (m, lv[(size_t) j]);
            const auto pt = P (hz[(size_t) i], sliceDb (m), w);
            if (! started)
            {
                if (closed) { p.startNewSubPath (P (hz[(size_t) i], -range, w)); p.lineTo (pt); }
                else p.startNewSubPath (pt);
                started = true;
            }
            else p.lineTo (pt);
            last = P (hz[(size_t) i], -range, w);
        }
        if (closed) { p.lineTo (last); p.closeSubPath(); }
        return p;
    };

    struct Layer { float depth; int kind; int index; };   // kind 0: waterfall slice (index = age), 1: band
    std::vector<Layer> layers;
    layers.reserve ((size_t) (kMaxBands + kWaterfall3D + 1));
    const bool showWaterfall = (analyzer.pre || analyzer.post) && waterfallCount > 0;
    if (showWaterfall)
        for (int age = 0; age < juce::jmin (waterfallCount, kWaterfall3D); ++age)
            layers.push_back ({ kWaterfallSpan * (float) age / (float) (kWaterfall3D - 1), 0, age });
    for (int b = 0; b < kMaxBands; ++b)
        if (bands[(size_t) b].used && curves[(size_t) b].valid) layers.push_back ({ bands[(size_t) b].depth * 0.01f, 1, b });
    std::stable_sort (layers.begin(), layers.end(), [] (const Layer& a, const Layer& b)
    {
        if (! juce::exactlyEqual (a.depth, b.depth)) return a.depth > b.depth;
        return a.kind < b.kind;   // at the same depth the band's curve stays in front of the spectrum
    });

    for (const auto& L : layers)
    {
        if (L.kind == 0)
        {
            const auto& lv = waterfall[(size_t) ((waterfallHead - L.index + kWaterfall) % kWaterfall)];
            if (lv.empty()) continue;
            const float age = (float) L.index / (float) (kWaterfall3D - 1), fade = std::pow (1.0f - age, 1.4f);
            const bool newest = L.index == 0;
            const int pts = newest ? 240 : 80;
            // the newest slice is a lit surface; every few older ones hide a little of what's behind (a terrain)
            if (newest)
            {
                const auto top = P (1000.0f, range * 0.36f, L.depth), bottom = P (1000.0f, -range, L.depth);
                g.setGradientFill (juce::ColourGradient (ice.withAlpha (0.15f), top.x, top.y, t.bg0.withAlpha (0.5f), bottom.x, bottom.y, false));
                g.fillPath (slicePath (lv, waterfallHz, L.depth, true, pts));
            }
            else if (L.index == 4 || L.index == 9)
            {
                g.setColour (t.bg0.withAlpha (0.22f * fade));
                g.fillPath (slicePath (lv, waterfallHz, L.depth, true, pts));
            }
            if (newest && analyzer.pre && analyzer.post)
            {
                // pre: a faint silhouette on the newest slice
                if ((int) preHz.size() != preSpectrum->getNumPoints())
                {
                    preHz.resize ((size_t) preSpectrum->getNumPoints());
                    for (int i = 0; i < (int) preHz.size(); ++i) preHz[(size_t) i] = preSpectrum->getFrequencyForPoint (i);
                }
                g.setColour (juce::Colours::white.withAlpha (0.05f));
                g.fillPath (slicePath (preSpectrum->getLevelsDb(), preHz, 0.0f, true, 160));
            }
            g.setColour (ice.withAlpha (newest ? 0.9f : 0.025f + 0.22f * fade * fade));
            g.strokePath (slicePath (lv, waterfallHz, L.depth, false, pts), juce::PathStrokeType (newest ? 1.3f : 0.8f));
            continue;
        }

        const int b = L.index;
        const auto& p = bands[(size_t) b];
        const auto& c = curves[(size_t) b];
        const float w = L.depth;
        const bool sel = selection.contains (b);
        const float s = selectAmt[(size_t) b].current, h = hoverAmt[(size_t) b].current;
        const auto col = (p.isActive() ? bandColour (b) : t.textFaint).withMultipliedAlpha (solo >= 0 && solo != b ? 0.3f : 1.0f);

        // a thin curve at the band's depth with a faint curtain down (or up) to its 0 dB line
        juce::Path curve, curtain;
        for (int i = 0; i < kPoints; i += 2)
        {
            const auto pt = P (freqs[(size_t) i], clampDb (c.db[(size_t) i]), w);
            if (i == 0) curve.startNewSubPath (pt); else curve.lineTo (pt);
        }
        curtain = curve;
        curtain.lineTo (P (freqs[(size_t) kPoints - 2], 0.0f, w));
        curtain.lineTo (P (freqs[0], 0.0f, w));
        curtain.closeSubPath();
        g.setColour (col.withAlpha ((p.isActive() ? 0.03f : 0.015f) + 0.11f * s + 0.05f * h));
        g.fillPath (curtain);
        g.setColour (col.withAlpha (sel ? 0.95f : (0.24f + 0.45f * h) * (p.isActive() ? 1.0f : 0.5f)));
        g.strokePath (curve, juce::PathStrokeType (sel ? 1.6f : 1.0f, juce::PathStrokeType::curved));
    }

    // the total curve runs through the bands' depths: a soft curtain to 0 dB, its shadow on the floor, a glow
    std::array<float, kPoints> dep {};
    curveDepths (dep);
    juce::Path total, shadow, curtain;
    for (int i = 0; i < kPoints; ++i)
    {
        const auto pt = P (freqs[(size_t) i], clampDb (totals[0][(size_t) i]), dep[(size_t) i]);
        const auto sh = P (freqs[(size_t) i], -range, dep[(size_t) i]);
        if (i == 0) { total.startNewSubPath (pt); shadow.startNewSubPath (sh); }
        else        { total.lineTo (pt); shadow.lineTo (sh); }
    }
    curtain = total;
    for (int i = kPoints - 1; i >= 0; i -= 3)
        curtain.lineTo (P (freqs[(size_t) i], 0.0f, dep[(size_t) i]));
    curtain.lineTo (P (freqs[0], 0.0f, dep[0]));
    curtain.closeSubPath();
    g.setColour (t.curve.withAlpha (0.045f));
    g.fillPath (curtain);
    g.setColour (t.curve.withAlpha (0.14f));
    g.strokePath (shadow, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved));
    g.setColour (t.curve.withAlpha (0.10f));
    g.strokePath (total, juce::PathStrokeType (7.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (juce::Colours::black.withAlpha (0.3f));
    g.strokePath (total, juce::PathStrokeType (3.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
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
        const float k = depthScale (p.depth * 0.01f);
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

    // labels: frequency along the front edge, dB up the front post, near / far along the right floor edge
    if (! compact)
    {
        g.setFont (st::fonts::medium (10.0f));
        g.setColour (t.textFaint);
        for (float hz : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
        {
            const auto pt = P (hz, -range, 0.0f);
            const auto label = hz >= 1000.0f ? juce::String ((int) (hz / 1000.0f)) + "k" : juce::String ((int) hz);
            g.drawText (label, juce::Rectangle<float> (pt.x - 20.0f, pt.y + 5.0f, 40.0f, 14.0f), juce::Justification::centred, false);
        }
        g.setFont (st::fonts::semibold (10.0f));
        for (float db = -std::floor (range / step + 0.001f) * step; db <= range + 0.01f; db += step)
        {
            const bool zero = std::abs (db) < 0.01f;
            const auto pt = P (minHz, db, 0.0f);
            g.setColour (t.curve.withAlpha (zero ? 0.95f : 0.75f));
            g.drawText (zero ? juce::String ("0") : (db > 0 ? "+" : "") + juce::String ((int) db),
                        juce::Rectangle<float> (pt.x - 38.0f, pt.y - 7.0f, 28.0f, 14.0f), juce::Justification::centredRight, false);
        }
        g.setFont (st::fonts::medium (10.0f));
        g.setColour (t.textDim);
        const auto nearPt = P (maxHz, -range, 0.0f), farPt = P (maxHz, -range, 1.0f);
        g.drawText ("near", juce::Rectangle<float> (nearPt.x + 6.0f, nearPt.y - 7.0f, 40.0f, 14.0f), juce::Justification::centredLeft, false);
        g.drawText ("far", juce::Rectangle<float> (farPt.x + 6.0f, farPt.y - 7.0f, 40.0f, 14.0f), juce::Justification::centredLeft, false);

        // axis gizmo under the view buttons: X (frequency), Y (gain), Z (depth, towards you); drag it to orbit
        const auto o = getGizmoCentre();
        const bool active = hoverGizmo || drag == Drag::orbit;
        g.setColour (juce::Colours::white.withAlpha (active ? 0.07f : 0.025f));
        g.fillEllipse (juce::Rectangle<float> (48.0f, 48.0f).withCentre (o));
        if (active)
        {
            g.setColour (t.curve.withAlpha (0.35f));
            g.drawEllipse (juce::Rectangle<float> (48.0f, 48.0f).withCentre (o), 1.0f);
        }
        auto dir = [] (juce::Point<float> a, juce::Point<float> b) { const auto d = b - a; return d / juce::jmax (0.001f, d.getDistanceFromOrigin()); };
        const auto c0 = projectUnit (0.5f, 0.0f, 0.5f);
        const juce::Point<float> ax[3] = { dir (c0, projectUnit (0.6f, 0.0f, 0.5f)), dir (c0, projectUnit (0.5f, 0.25f, 0.5f)),
                                           dir (c0, projectUnit (0.5f, 0.0f, 0.3f)) };
        const char* names[3] = { "X", "Y", "Z" };
        g.setFont (st::fonts::medium (9.0f));
        for (int i = 0; i < 3; ++i)
        {
            const auto e = o + ax[i] * 14.0f;
            g.setColour (juce::Colours::white.withAlpha (active ? 0.6f : 0.3f));
            line (o, e, active ? 1.3f : 1.0f);
            g.setColour (active ? t.textDim : t.textFaint);
            g.drawText (names[i], juce::Rectangle<float> (10.0f, 10.0f).withCentre (o + ax[i] * 21.0f), juce::Justification::centred, false);
        }
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

    // the analyser as a waterfall seen from above: recent spectra as ridgelines, the newest at the front (bottom),
    // older ones further back and fainter; each ridge hides a little of the ones behind it
    if ((analyzer.pre || analyzer.post) && waterfallCount > 0 && ! waterfallHz.empty())
    {
        const juce::Colour ice (0xffa9dcee);
        const float aRange = AnalyzerSettings::ranges[analyzer.rangeIndex];
        const float topHz = juce::jmin (maxHz, (float) (sampleRate * 0.5) * 0.985f);
        const int n = (int) waterfallHz.size();
        int i0 = 0, i1 = n - 1;
        while (i0 < n && waterfallHz[(size_t) i0] < 20.0f) ++i0;
        while (i1 > i0 && waterfallHz[(size_t) i1] > topHz) --i1;
        const float rowGap = area.getHeight() / (float) (kWaterfall - 1);
        const float amp = juce::jmin (rowGap * 4.0f, area.getHeight() * 0.4f);
        for (int age = juce::jmin (waterfallCount, kWaterfall) - 1; age >= 0 && i1 - i0 > 2; --age)
        {
            const auto& lv = waterfall[(size_t) ((waterfallHead - age + kWaterfall) % kWaterfall)];
            if ((int) lv.size() != n) continue;
            const bool newest = age == 0;
            const float base = area.getBottom() - rowGap * (float) age;
            const float fade = std::pow (1.0f - (float) age / (float) kWaterfall, 1.3f);
            const int stride = juce::jmax (1, (i1 - i0) / (newest ? 260 : 130));
            juce::Path ridge, body;
            float lastX = 0.0f;
            for (int i = i0; i <= i1; i += stride)
            {
                float m = -200.0f;
                for (int j = i; j < juce::jmin (i1 + 1, i + stride); ++j) m = juce::jmax (m, lv[(size_t) j]);
                const float x = xForHz (waterfallHz[(size_t) i]);
                const float y = base - amp * juce::jlimit (0.0f, 1.0f, 1.0f + m / aRange);
                if (i == i0) { ridge.startNewSubPath (x, y); body.startNewSubPath (x, base); body.lineTo (x, y); }
                else         { ridge.lineTo (x, y); body.lineTo (x, y); }
                lastX = x;
            }
            body.lineTo (lastX, base);
            body.closeSubPath();
            g.setColour (newest ? ice.withAlpha (0.10f) : t.bg0.withAlpha (0.5f * fade));
            g.fillPath (body);
            g.setColour (ice.withAlpha (newest ? 0.9f : 0.05f + 0.4f * fade * fade));
            g.strokePath (ridge, juce::PathStrokeType (newest ? 1.3f : 0.8f));
        }
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
