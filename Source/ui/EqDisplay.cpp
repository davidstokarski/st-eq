#include "EqDisplay.h"
#include "../dsp/Sketch.h"
#include "BandIsland.h"

namespace steq
{

const float EqDisplay::displayRanges[4] = { 3.0f, 6.0f, 12.0f, 30.0f };
const float AnalyzerSettings::ranges[3] = { 60.0f, 90.0f, 120.0f };
const float AnalyzerSettings::tilts[4] = { 0.0f, 3.0f, 4.5f, 6.0f };

void AnalyzerSettings::load (const juce::ValueTree& ui)
{
    pre = (bool) ui.getProperty ("anPre", true);
    post = (bool) ui.getProperty ("anPost", true);
    sidechain = (bool) ui.getProperty ("anSc", false);
    rangeIndex = juce::jlimit (0, 2, (int) ui.getProperty ("anRange", 1));
    speedIndex = juce::jlimit (0, 2, (int) ui.getProperty ("anSpeed", 1));
    tiltIndex = juce::jlimit (0, 3, (int) ui.getProperty ("anTilt", 2));
    freeze = (bool) ui.getProperty ("anFreeze", false);
    grab = (bool) ui.getProperty ("anGrab", true);
    collisions = (bool) ui.getProperty ("anCollide", true);
    resolutionIndex = juce::jlimit (0, 3, (int) ui.getProperty ("anRes", 2));
}

const char* AnalyzerSettings::resolutionNames[4] = { "Low", "Medium", "High", "Maximum" };

void AnalyzerSettings::save (juce::ValueTree& ui) const
{
    ui.setProperty ("anPre", pre, nullptr);
    ui.setProperty ("anPost", post, nullptr);
    ui.setProperty ("anSc", sidechain, nullptr);
    ui.setProperty ("anRange", rangeIndex, nullptr);
    ui.setProperty ("anSpeed", speedIndex, nullptr);
    ui.setProperty ("anTilt", tiltIndex, nullptr);
    ui.setProperty ("anFreeze", freeze, nullptr);
    ui.setProperty ("anGrab", grab, nullptr);
    ui.setProperty ("anCollide", collisions, nullptr);
    ui.setProperty ("anRes", resolutionIndex, nullptr);
}

//==============================================================================
juce::String formatFrequency (float hz)
{
    if (hz >= 1000.0f) return juce::String (hz / 1000.0f, hz >= 10000.0f ? 1 : 2) + " kHz";
    return juce::String (hz, hz < 100.0f ? 1 : 0) + " Hz";
}

juce::String noteName (float hz)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const float midi = 69.0f + 12.0f * std::log2 (juce::jmax (1.0f, hz) / 440.0f);
    const int n = juce::roundToInt (midi);
    const int cents = juce::roundToInt ((midi - (float) n) * 100.0f);
    juce::String s = juce::String (names[((n % 12) + 12) % 12]) + juce::String (n / 12 - 1);
    if (cents != 0) s << (cents > 0 ? " +" : " ") << cents;
    return s;
}

float parseFrequency (const juce::String& text)
{
    const float v = st::params::parseHz (text);   // "1k", "250 Hz", notes: "A4", "C#3+13"
    return v > 0.0f ? v : -1.0f;
}

//==============================================================================
namespace
{
    /** "12 dB" box at the top of the EQ scale: yellow text on a dark box; click for the range menu. */
    struct RangeBox : juce::Component, juce::SettableTooltipClient
    {
        std::function<int()> getIndex;        // fixed choice, -1 = auto
        std::function<void (int)> setIndex;   // -1 = auto
        std::function<float()> getRange;

        RangeBox() { setTooltip ("EQ display range: Auto follows your bands (3 to 30 dB), or pick a fixed range"); setMouseCursor (juce::MouseCursor::PointingHandCursor); }

        void paint (juce::Graphics& g) override
        {
            const auto& t = st::Theme::get();
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (juce::Colours::black.withAlpha (isMouseOver() ? 0.55f : 0.4f));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (juce::Colours::white.withAlpha (0.07f));
            g.drawRoundedRectangle (r, 3.0f, 1.0f);
            g.setFont (st::fonts::semibold (10.5f));
            g.setColour (t.curve);
            g.drawText (juce::String (juce::roundToInt (getRange())) + " dB", r, juce::Justification::centred, false);
        }
        void mouseEnter (const juce::MouseEvent&) override { repaint(); }
        void mouseExit (const juce::MouseEvent&) override { repaint(); }
        void mouseDown (const juce::MouseEvent&) override
        {
            juce::PopupMenu m;
            m.addItem (100, "Auto", true, getIndex() < 0);
            m.addSeparator();
            for (int i = 0; i < 4; ++i)
                m.addItem (i + 1, juce::String ((int) EqDisplay::displayRanges[i]) + " dB", true, getIndex() == i);
            st::showMenu (m, this, [safe = juce::Component::SafePointer<RangeBox> (this)] (int r)
            {
                if (safe != nullptr && r > 0) { safe->setIndex (r == 100 ? -1 : r - 1); safe->repaint(); }
            });
        }
    };
}

EqDisplay::EqDisplay (StEqProcessor& p, bool isCompact) : proc (p), compact (isCompact)
{
    setOpaque (false);
    setWantsKeyboardFocus (true);
    analyzer.load (proc.uiState);
    if (compact) { analyzer.pre = false; analyzer.post = true; analyzer.sidechain = false; analyzer.freeze = false; analyzer.grab = false; }
    rangeIndex = juce::jlimit (0, 3, (int) proc.uiState.getProperty ("range", 2));
    autoRange = compact || (bool) proc.uiState.getProperty ("autoRange", true);
    displayRange = rangeTarget = autoRange ? 12.0f : displayRanges[rangeIndex];
    createAnalyzers();

    sketchButton.setMinimal (true);
    sketchButton.setClickingTogglesState (false);
    sketchButton.onClick = [this] { setSketchMode (! sketchMode); };
    addAndMakeVisible (sketchButton);

    piano = (bool) proc.uiState.getProperty ("piano", false);
    pianoButton.setMinimal (true);
    pianoButton.setClickingTogglesState (false);
    pianoButton.setToggleState (piano, juce::dontSendNotification);
    pianoButton.setOnColour (st::Theme::get().curve);
    pianoButton.onClick = [this]
    {
        piano = ! piano;
        pianoButton.setToggleState (piano, juce::dontSendNotification);
        proc.uiState.setProperty ("piano", piano, nullptr);
        gridDirty = true;
        repaint();
    };
    addAndMakeVisible (pianoButton);

    auto rb = std::make_unique<RangeBox>();
    rb->getIndex = [this] { return autoRange ? -1 : rangeIndex; };
    rb->setIndex = [this] (int i) { if (i < 0) setAutoRange(); else setDisplayRange (i); };
    rb->getRange = [this] { return displayRange; };
    rangeBox = std::move (rb);
    addAndMakeVisible (*rangeBox);
    if (compact) { sketchButton.setVisible (false); pianoButton.setVisible (false); piano = false; }

    clock = std::make_unique<st::FrameClock> (*this, [this] (double dt) { frame (dt); });
}

EqDisplay::~EqDisplay()
{
    if (compact) return;   // the instance's own editor owns its analyser feeds
    proc.preAnalyzer.setActive (false);
    proc.postAnalyzer.setActive (false);
    proc.scAnalyzer.setActive (false);
}

void EqDisplay::createAnalyzers()
{
    st::SpectrumAnalyzer::Options o;
    o.fftOrder = 11 + juce::jlimit (0, 3, analyzer.resolutionIndex);
    o.hopSize = juce::jmin (256, (1 << o.fftOrder) / 8);
    o.numDisplayPoints = 400;
    o.minHz = 8.0f;
    o.maxHz = 32000.0f;
    o.slopeDbPerOct = AnalyzerSettings::tilts[analyzer.tiltIndex];
    o.releaseDbPerSec = analyzer.speedIndex == 0 ? 14.0f : (analyzer.speedIndex == 1 ? 32.0f : 70.0f);
    o.attack = analyzer.speedIndex == 0 ? 0.3f : (analyzer.speedIndex == 1 ? 0.55f : 0.8f);
    o.smoothingPasses = 3;
    o.nonConsuming = compact;
    if (compact) { o.fftOrder = 12; o.hopSize = 1024; }

    preSpectrum = std::make_unique<st::SpectrumAnalyzer> (proc.preAnalyzer, o);
    postSpectrum = std::make_unique<st::SpectrumAnalyzer> (proc.postAnalyzer, o);
    scSpectrum = std::make_unique<st::SpectrumAnalyzer> (proc.scAnalyzer, o);
    if (compact) return;
    proc.preAnalyzer.setActive (analyzer.pre);
    proc.postAnalyzer.setActive (analyzer.post);
    proc.scAnalyzer.setActive (analyzer.sidechain);
}

void EqDisplay::setAnalyzer (const AnalyzerSettings& a)
{
    const bool rebuild = a.tiltIndex != analyzer.tiltIndex || a.speedIndex != analyzer.speedIndex || a.resolutionIndex != analyzer.resolutionIndex;
    if (! a.freeze) maxHold.clear();
    analyzer = a;
    if (compact) { gridDirty = true; repaint(); return; }
    analyzer.save (proc.uiState);
    if (rebuild) createAnalyzers();
    proc.preAnalyzer.setActive (a.pre);
    proc.postAnalyzer.setActive (a.post);
    proc.scAnalyzer.setActive (a.sidechain);
    if (! a.pre) preSpectrum->clear();
    if (! a.post) postSpectrum->clear();
    if (! a.sidechain) scSpectrum->clear();
    gridDirty = true;
    repaint();
}

void EqDisplay::setDisplayRange (int index)
{
    rangeIndex = juce::jlimit (0, 3, index);
    autoRange = false;
    if (! compact) { proc.uiState.setProperty ("range", rangeIndex, nullptr); proc.uiState.setProperty ("autoRange", false, nullptr); }
    displayRange = rangeTarget = displayRanges[rangeIndex];
    rangeChanged();
    if (onRangeChanged) onRangeChanged (rangeIndex);
}

void EqDisplay::setAutoRange()
{
    autoRange = true;
    if (! compact) proc.uiState.setProperty ("autoRange", true, nullptr);
    readParameters();
    rebuildCurves();
    rangeTarget = autoRangeTarget();
    if (onRangeChanged) onRangeChanged (-1);
    if (rangeBox != nullptr) rangeBox->repaint();
}

void EqDisplay::rangeChanged()
{
    gridDirty = curvesDirty = true;
    curveLayer = {};
    if (rangeBox != nullptr) rangeBox->repaint();
    repaint();
}

float EqDisplay::autoRangeTarget() const
{
    // what has to fit: every gain node and its dynamic range, and their summed curve (overlapping bands add up)
    float need = 0.0f;
    for (const auto& p : bands)
    {
        if (! p.used || ! st::dsp::eq::hasGain (p.shape)) continue;
        need = juce::jmax (need, std::abs (p.gain * gainScale));
        if (p.isDynamic()) need = juce::jmax (need, std::abs ((p.gain + p.dynRange) * gainScale));
    }
    // the combined curve of the gain shapes (bells, shelves, tilts) where they overlap; cuts, notches and band
    // passes always fall off the bottom, so they don't count
    for (int i = 0; i < kPoints; ++i)
    {
        if (freqs[(size_t) i] < 20.0f || freqs[(size_t) i] > 20000.0f) continue;
        float sum = 0.0f;
        for (int b = 0; b < kMaxBands; ++b)
        {
            const auto& p = bands[(size_t) b];
            if (p.isActive() && st::dsp::eq::hasGain (p.shape) && curves[(size_t) b].valid) sum += curves[(size_t) b].db[(size_t) i];
        }
        need = juce::jmax (need, std::abs (sum));
    }
    // a little headroom so nodes don't sit on the edge
    return juce::jlimit (minRange, maxRange, need * 1.15f + 0.5f);
}

bool EqDisplay::advanceRange (double seconds)
{
    float target = autoRange ? autoRangeTarget() : displayRanges[rangeIndex];
    if (autoRange)
    {
        const bool dragging = drag == Drag::bands || drag == Drag::dynRange || drag == Drag::value;
        if (target < rangeTarget)
        {
            // shrink only when the bands fit comfortably in a smaller view, and never mid-drag
            if (dragging || target > rangeTarget * 0.85f) target = rangeTarget;
        }
    }
    rangeTarget = target;
    if (std::abs (displayRange - rangeTarget) < 0.01f)
    {
        if (! juce::exactlyEqual (displayRange, rangeTarget)) { displayRange = rangeTarget; rangeChanged(); return true; }
        return false;
    }
    displayRange += (rangeTarget - displayRange) * (float) (1.0 - std::exp (-seconds * 9.0));
    rangeChanged();
    return true;
}

void EqDisplay::setSketchMode (bool on)
{
    if (on && ! flatView()) setView (View::front);   // sketching draws the gain curve
    sketchMode = on;
    sketchButton.setToggleState (on, juce::dontSendNotification);
    sketchButton.setOnColour (st::Theme::get().curve);
    sketchPoints.clear();
    setMouseCursor (on ? juce::MouseCursor::CrosshairCursor : juce::MouseCursor::NormalCursor);
    if (onSketchModeChanged) onSketchModeChanged (on);
    repaint();
}

//==============================================================================
float EqDisplay::xForHz (float hz) const noexcept
{
    return plot.getX() + plot.getWidth() * std::log (juce::jmax (1.0f, hz) / minHz) / std::log (maxHz / minHz);
}

float EqDisplay::hzForX (float x) const noexcept
{
    const float t = (x - plot.getX()) / juce::jmax (1.0f, plot.getWidth());
    return juce::jlimit (minHz, maxHz, minHz * std::pow (maxHz / minHz, t));
}

float EqDisplay::yForDb (float db) const noexcept
{
    const float r = displayRange;
    return eqArea.getCentreY() - db / r * eqArea.getHeight() * 0.5f;
}

float EqDisplay::dbForY (float y) const noexcept
{
    const float r = displayRange;
    return (eqArea.getCentreY() - y) / (eqArea.getHeight() * 0.5f) * r;
}

float EqDisplay::analyzerY (float db) const noexcept
{
    const float range = AnalyzerSettings::ranges[analyzer.rangeIndex];
    return juce::jmap (db, -range, 0.0f, plot.getBottom() - 18.0f, plot.getY() + 15.0f);
}

/** Cut filters: the dot sits on the curve at the cutoff, so its height shows the resonance. */
static float cutDotDb (float q, int slope)
{
    if (slopeToOrder (slope) < 2) return -3.01f;
    return 20.0f * std::log10 (q / 0.7071f) - 3.01f;
}
static float cutQForDb (float db) { return juce::jlimit (0.025f, 40.0f, 0.7071f * std::pow (10.0f, (db + 3.01f) / 20.0f)); }

float EqDisplay::dotDb (const BandParams& p) const noexcept
{
    switch (p.shape)
    {
        case Shape::bell: case Shape::lowShelf: case Shape::highShelf: return p.gain * gainScale;
        case Shape::tiltShelf:                                           return 0.5f * p.gain * gainScale;
        case Shape::lowCut: case Shape::highCut:                         return cutDotDb (p.q, p.slope);
        case Shape::flatTilt: case Shape::notch: case Shape::bandPass: case Shape::allPass: break;
    }
    return 0.0f;
}

void EqDisplay::applyVertical (int band, const BandParams& s, float dDb)
{
    const float scale = std::abs (gainScale) > 0.01f ? gainScale : 1.0f;
    switch (s.shape)
    {
        case Shape::bell: case Shape::lowShelf: case Shape::highShelf: case Shape::flatTilt:
            setPlain (band, "gain", juce::jlimit (-30.0f, 30.0f, s.gain + dDb / scale)); break;
        case Shape::tiltShelf:
            setPlain (band, "gain", juce::jlimit (-30.0f, 30.0f, s.gain + 2.0f * dDb / scale)); break;
        case Shape::lowCut: case Shape::highCut:
            if (slopeToOrder (s.slope) >= 2) setPlain (band, "q", cutQForDb (cutDotDb (s.q, s.slope) + dDb));
            break;
        case Shape::notch: case Shape::bandPass: case Shape::allPass:
            setPlain (band, "q", juce::jlimit (0.025f, 40.0f, s.q * std::exp2 (dDb / 4.0f))); break;
    }
}

juce::Point<float> EqDisplay::dotPosition (int b) const
{
    const auto& p = bands[(size_t) b];
    switch (getView())
    {
        case View::threeD: return project (p.freq, juce::jlimit (-displayRange, displayRange * 1.2f, dotDb (p)), p.depth * 0.01f);
        case View::top:    return { xForHz (p.freq), topY (p.depth * 0.01f) };
        case View::front:  break;
    }
    return { xForHz (p.freq), yForDb (dotDb (p)) };
}

float EqDisplay::getBandX (int b) const
{
    if (! juce::isPositiveAndBelow (b, kMaxBands)) return plot.getCentreX();
    return dotPosition (b).x;
}

float EqDisplay::curveYAt (float x) const
{
    const float t = juce::jlimit (0.0f, (float) (kPoints - 1), (x - xs[0]) / juce::jmax (1.0f, xs[kPoints - 1] - xs[0]) * (float) (kPoints - 1));
    const int i = juce::jmin ((int) t, kPoints - 2);
    const float f = t - (float) i;
    const auto& tot = totals[0];
    return yForDb (tot[(size_t) i] + (tot[(size_t) i + 1] - tot[(size_t) i]) * f);
}

int EqDisplay::bandAt (juce::Point<float> pt) const
{
    int best = -1;
    float bestD = 11.0f;
    for (int b = kMaxBands - 1; b >= 0; --b)
    {
        if (! bands[(size_t) b].used) continue;
        const auto dp = dotPosition (b);
        const float d = dp.getDistanceFrom (pt);
        if (d < bestD) { bestD = d; best = b; }
    }
    // 3D view seen from high up: a dot can sit close to its floor handle; the nearer one wins
    if (best >= 0)
        if (const int h = depthHandleAt (pt); h >= 0 && depthHandle (h).getDistanceFrom (pt) < bestD) return -1;
    return best;
}

juce::Rectangle<float> EqDisplay::dynHandleBounds (int b) const
{
    const auto& p = bands[(size_t) b];
    const auto dp = dotPosition (b);
    const float k = p.shape == Shape::tiltShelf ? 0.5f : 1.0f;
    const float y = p.shape == Shape::flatTilt ? yForDb (p.dynRange * gainScale) : yForDb ((p.gain + p.dynRange) * gainScale * k);
    return juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ dp.x, y });
}

int EqDisplay::dynHandleAt (juce::Point<float> pt) const
{
    if (! flatView()) return -1;
    for (int b = kMaxBands - 1; b >= 0; --b)
        if (bands[(size_t) b].isDynamic() && dynHandleBounds (b).expanded (4.0f).contains (pt))
            return b;
    return -1;
}

//==============================================================================
void EqDisplay::resized()
{
    // Pro-style layout: the display fills the window under the header; on the right an EQ scale (inside the
    // plot), an analyser scale and the output meter. 0 dB sits about a third down, so the band island (bottom
    // third) never covers the EQ range.
    auto r = getLocalBounds().toFloat();
    if (compact)
    {
        // no label strip: the plot runs 18 px past the bottom so everything drawn to "bottom - 18" fills the row
        plot = r.withHeight (r.getHeight() + 18.0f);
        meterArea = {};
        eqArea = juce::Rectangle<float> (plot.getX(), r.getY() + r.getHeight() * 0.1f, plot.getWidth(), r.getHeight() * 0.8f);
    }
    else
    {
        plot = juce::Rectangle<float> (r.getX(), r.getY() + 44.0f, r.getWidth() - 58.0f, r.getHeight() - 44.0f);
        meterArea = juce::Rectangle<float> (r.getRight() - 20.0f, plot.getY() + 30.0f, 12.0f, plot.getHeight() - 40.0f);
        const float h = plot.getHeight();
        eqArea = juce::Rectangle<float> (plot.getX(), plot.getY() + h * 0.075f, plot.getWidth(), h * 0.50f);
    }

    for (int i = 0; i < kPoints; ++i)
    {
        freqs[(size_t) i] = minHz * std::pow (maxHz / minHz, (float) i / (kPoints - 1));
        xs[(size_t) i] = xForHz (freqs[(size_t) i]);
    }
    sketchButton.setBounds (juce::roundToInt (plot.getX()) + 8, juce::roundToInt (plot.getBottom()) - 66, 22, 22);
    pianoButton.setBounds (juce::roundToInt (plot.getX()) + 8, juce::roundToInt (plot.getBottom()) - 42, 22, 22);
    if (rangeBox != nullptr) rangeBox->setBounds (juce::roundToInt (plot.getRight()) - 42, juce::roundToInt (plot.getY()) + 6, 40, 18);
    gridDirty = curvesDirty = true;
    for (auto& c : curves) c.valid = false;
}

bool EqDisplay::readParameters()
{
    bool changed = false;
    for (int b = 0; b < kMaxBands; ++b)
    {
        const auto p = proc.getBand (b);
        if (! (p == bands[(size_t) b])) { bands[(size_t) b] = p; changed = true; }
    }
    const float s = proc.refs.readGainScale();
    const int m = proc.refs.mode.getInt();
    const double sr = proc.getEngine().getSampleRate();
    if (! juce::exactlyEqual (s, gainScale) || m != procMode || ! juce::exactlyEqual (sr, sampleRate))
    {
        gainScale = s; procMode = m; sampleRate = sr;
        changed = true;
    }

    // selection may reference removed bands
    for (int i = selection.size(); --i >= 0;)
        if (! bands[(size_t) selection[i]].used) { selection.remove (i); changed = true; }
    if (primary >= 0 && ! bands[(size_t) primary].used)
    {
        primary = selection.isEmpty() ? -1 : selection.getLast();
        notifySelection();
    }
    if (changed)
    {
        // Whoever noticed the change (frame, mouse handler, test), the curves and their cached layer are stale.
        curvesDirty = true;
        curveLayer = {};
        repaint();
    }
    return changed;
}

void EqDisplay::computeBandCurve (int b, Curve& c)
{
    const auto& p = bands[(size_t) b];
    auto fill = [this] (std::array<float, kPoints>& out, const st::dsp::EqBandSpec& spec)
    {
        if (procMode == (int) ProcMode::zeroLatency)
        {
            // The digital response (what you hear). A high cut falls away at Nyquist, where the digital filter ends.
            // Other shapes (low cuts too: they pass everything up there) blend over the top of the band into the
            // analog shape (shifted to meet it), so the curve carries on past Nyquist without a kink.
            const auto a = st::dsp::eq::designAnalog (spec);
            const auto d = st::dsp::eq::designDigital (a, sampleRate);
            const bool cut = spec.shape == Shape::highCut;
            const double fA = 0.42 * sampleRate, fB = 0.4995 * sampleRate;
            auto digitalDb = [&] (double f) { return 20.0 * std::log10 (std::max (1.0e-9, std::abs (d.response (f, sampleRate)))); };
            const double offset = digitalDb (fA) - a.magnitudeDb (fA);
            for (int i = 0; i < kPoints; ++i)
            {
                const double f = freqs[(size_t) i];
                double y;
                if (cut)            y = f < fB ? digitalDb (f) : -200.0;
                else if (f <= fA)   y = digitalDb (f);
                else
                {
                    const double ana = a.magnitudeDb (f) + offset;
                    if (f >= fB) y = ana;
                    else
                    {
                        const double t = (f - fA) / (fB - fA), u = t * t * (3.0 - 2.0 * t);
                        y = (1.0 - u) * digitalDb (f) + u * ana;
                    }
                }
                out[(size_t) i] = (float) y;
            }
        }
        else
        {
            const auto a = st::dsp::eq::designAnalog (spec);
            for (int i = 0; i < kPoints; ++i)
                out[(size_t) i] = (float) a.magnitudeDb (freqs[(size_t) i]);
        }
    };

    const bool keyChanged = ! c.valid || ! (c.key == p) || ! juce::exactlyEqual (c.scale, gainScale) || c.mode != procMode
                            || ! juce::exactlyEqual (c.sr, sampleRate);
    if (keyChanged)
    {
        fill (c.db, makeSpec (p, gainScale, 0.0f));
        c.hasRange = p.isDynamic();
        if (c.hasRange) fill (c.rangeDb, makeSpec (p, gainScale, p.dynRange));
    }
    const float live = liveDyn[(size_t) b];
    if (c.hasRange && p.isSpectral() && proc.getEngine().isSpectralActive())
    {
        // spectral: static curve + the dynamic range scaled per frequency by the live amount
        const auto& am = specAmount[(size_t) b];
        constexpr int G = SpectralDynamics::kGrid;
        for (int i = 0; i < kPoints; ++i)
        {
            const float x = std::log (juce::jlimit (20.0f, 20000.0f, freqs[(size_t) i]) / 20.0f) / std::log (1000.0f) * (float) (G - 1);
            const int j = juce::jlimit (0, G - 2, (int) x);
            const float a = am[(size_t) j] + (x - (float) j) * (am[(size_t) j + 1] - am[(size_t) j]);
            c.liveDb[(size_t) i] = c.db[(size_t) i] + (c.rangeDb[(size_t) i] - c.db[(size_t) i]) * a;
        }
    }
    else if (std::abs (live) > 0.01f) fill (c.liveDb, makeSpec (p, gainScale, live));
    else c.liveDb = c.db;
    c.specVersion = specVersion[(size_t) b];

    c.key = p;
    c.scale = gainScale;
    c.dyn = live;
    c.mode = procMode;
    c.sr = sampleRate;
    c.valid = true;
}

void EqDisplay::rebuildCurves()
{
    bool hasMS = false, hasLR = false;
    for (int b = 0; b < kMaxBands; ++b)
    {
        const auto& p = bands[(size_t) b];
        auto& c = curves[(size_t) b];
        if (! p.used) { c.valid = false; continue; }
        if (! c.valid || ! (c.key == p) || ! juce::exactlyEqual (c.scale, gainScale) || c.mode != procMode
            || ! juce::exactlyEqual (c.sr, sampleRate) || std::abs (c.dyn - liveDyn[(size_t) b]) > 0.04f || c.specVersion != specVersion[(size_t) b])
            computeBandCurve (b, c);
        if (p.isActive())
        {
            hasMS |= p.place == Placement::mid || p.place == Placement::side;
            hasLR |= p.place == Placement::left || p.place == Placement::right;
        }
    }

    // One curve when every band is stereo; otherwise two: Mid / Side, Left / Right, or (mixed) the left and right
    // channel response to a centred source (stereo + left/right + mid bands).
    totalsCount = hasMS || hasLR ? 2 : 1;
    const bool mixed = hasMS && hasLR;
    totalsLabels[0] = hasLR ? "L" : (hasMS ? "M" : "");
    totalsLabels[1] = hasLR ? "R" : (hasMS ? "S" : "");

    // The yellow curve is what you hear: live dynamic gains included.
    for (auto& t : totals) t.fill (0.0f);
    for (int b = 0; b < kMaxBands; ++b)
    {
        const auto& p = bands[(size_t) b];
        if (! p.isActive()) continue;
        const auto& c = curves[(size_t) b];
        const auto pl = p.place;
        const bool toA = totalsCount == 1 || pl == Placement::stereo || pl == (hasLR ? Placement::left : Placement::mid) || (mixed && pl == Placement::mid);
        const bool toB = pl == Placement::stereo || pl == (hasLR ? Placement::right : Placement::side) || (mixed && pl == Placement::mid);
        for (int i = 0; i < kPoints; ++i)
        {
            if (toA) totals[0][(size_t) i] += c.liveDb[(size_t) i];
            if (toB) totals[1][(size_t) i] += c.liveDb[(size_t) i];
        }
    }
    curvesDirty = false;
}

//==============================================================================
void EqDisplay::frame (double dt)
{
    bool changed = readParameters();
    bool layersChanged = changed;
    if (proc.refs.is3D() != mode3d)
    {
        mode3d = ! mode3d;
        if (! mode3d) hoverDepthBand = -1;
        gridDirty = layersChanged = changed = true;
    }

    // solo changes dim the other bands
    if (const int solo = proc.soloBand.load(); solo != lastSoloBand) { lastSoloBand = solo; layersChanged = true; }

    // live dynamic gains
    for (int b = 0; b < kMaxBands; ++b)
    {
        const float d = bands[(size_t) b].isDynamic() && bands[(size_t) b].isActive()
                          ? proc.bandMeter (b).dynDb.load (std::memory_order_relaxed) : 0.0f;
        if (std::abs (d - liveDyn[(size_t) b]) > 0.04f) { liveDyn[(size_t) b] = d; layersChanged = true; }
        else if (! juce::exactlyEqual (d, liveDyn[(size_t) b]) && juce::exactlyEqual (d, 0.0f)) { liveDyn[(size_t) b] = 0.0f; layersChanged = true; }

        if (bands[(size_t) b].isSpectral() && bands[(size_t) b].isActive() && proc.getEngine().isSpectralActive())
        {
            auto& src = proc.getEngine().getSpectral().amount[(size_t) b];
            auto& dst = specAmount[(size_t) b];
            bool moved = false;
            for (size_t i = 0; i < dst.size(); ++i)
            {
                const float a = src[i].load (std::memory_order_relaxed);
                if (std::abs (a - dst[i]) > 0.01f || (juce::exactlyEqual (a, 0.0f) && ! juce::exactlyEqual (dst[i], 0.0f))) { dst[i] = a; moved = true; }
            }
            if (moved) { ++specVersion[(size_t) b]; layersChanged = true; }
        }
    }
    if (layersChanged) curvesDirty = true;
    if (curvesDirty) { rebuildCurves(); curveLayer = {}; changed = true; }

    // EQ scale: follows the bands (auto) or the fixed choice, animated
    if (! rangeReady)
    {
        rangeReady = true;
        displayRange = rangeTarget = autoRange ? autoRangeTarget() : displayRanges[rangeIndex];
        rangeChanged();
        changed = true;
    }
    else if (advanceRange (dt)) changed = true;

    // analyzer
    const bool frozen = grabFrozen;   // Freeze keeps the live analyser running and builds a maximum (below)
    if (! frozen)
    {
        if (analyzer.post)      changed |= postSpectrum->update (dt);
        if (analyzer.pre)       changed |= preSpectrum->update (dt);
        if (analyzer.sidechain) changed |= scSpectrum->update (dt);
        if (getView() != View::front) changed |= pushWaterfall (dt);
    }
    if (analyzer.freeze && analyzer.post)
    {
        const auto& lv = postSpectrum->getLevelsDb();
        if (maxHold.size() != lv.size()) maxHold.assign (lv.size(), -200.0f);
        for (size_t i = 0; i < lv.size(); ++i) maxHold[i] = juce::jmax (maxHold[i], lv[i]);
    }

    // animations
    for (int b = 0; b < kMaxBands; ++b)
    {
        hoverAmt[(size_t) b].setTarget (b == hoverBand || b == hoverDynBand ? 1.0f : 0.0f);
        selectAmt[(size_t) b].setTarget (selection.contains (b) ? 1.0f : 0.0f);
        layersChanged |= hoverAmt[(size_t) b].tick (dt, 18.0f);
        layersChanged |= selectAmt[(size_t) b].tick (dt, 14.0f);
    }
    const bool showPreview = mouseInside && hoverBand < 0 && hoverDynBand < 0 && hoverPeak < 0 && drag == Drag::none && ! sketchMode && entry == nullptr;
    previewAlpha.setTarget (showPreview ? 1.0f : 0.0f);
    changed |= previewAlpha.tick (dt, 12.0f);

    // output meter
    {
        auto r = proc.outputMeter.read();
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        for (size_t c = 0; c < 2; ++c)
        {
            const float pk = juce::Decibels::gainToDecibels (r.peak[c], -100.0f);
            const float nv = pk > meterDb[c] ? pk : juce::jmax (pk, meterDb[c] - 30.0f * (float) dt);
            changed |= std::abs (nv - meterDb[c]) > 0.1f;
            meterDb[c] = nv;
            if (pk >= meterHold[c]) { meterHold[c] = pk; holdTime[c] = now; }
            else if (now - holdTime[c] > 1.5) meterHold[c] = juce::jmax (-100.0f, meterHold[c] - 20.0f * (float) dt);
            if (pk > meterMax) { meterMax = pk; changed = true; }
        }
    }

    if (layersChanged) changed = true;
    if (layersChanged) curveLayer = {};

    if (onFrame) onFrame (dt);

    sinceRepaint += dt;
    if (changed || drag != Drag::none || sinceRepaint > 0.5)
    {
        sinceRepaint = 0.0;
        repaint();
    }
}

//==============================================================================
void EqDisplay::rebuildLayers (float ps)
{
    const int w = juce::roundToInt ((float) getWidth() * ps), h = juce::roundToInt ((float) getHeight() * ps);
    if (w <= 0 || h <= 0) return;

    if (gridDirty || gridLayer.isNull() || ! juce::exactlyEqual (ps, layerScale) || gridLayer.getWidth() != w)
    {
        gridLayer = juce::Image (juce::Image::ARGB, w, h, true);
        juce::Graphics g (gridLayer);
        g.addTransform (juce::AffineTransform::scale (ps));
        paintGrid (g);
        gridDirty = false;
    }
    if (curveLayer.isNull() || ! juce::exactlyEqual (ps, layerScale) || curveLayer.getWidth() != w)
    {
        curveLayer = juce::Image (juce::Image::ARGB, w, h, true);
        juce::Graphics g (curveLayer);
        g.addTransform (juce::AffineTransform::scale (ps));
        paintCurves (g);
    }
    layerScale = ps;
}

void EqDisplay::paint (juce::Graphics& g)
{
    const float ps = juce::jmax (1.0f, g.getInternalContext().getPhysicalPixelScaleFactor());
    if (curvesDirty) { rebuildCurves(); curveLayer = {}; }

    if (const auto view = getView(); view != View::front)
    {
        // 3D mode views: drawn directly (no layers), then the same dots / parameter display / meter
        if (view == View::threeD) paint3D (g); else paintTop (g);
        paintDots (g);
        paintReadout (g);
        if (! compact) paintMeter (g);
        if (drag == Drag::rubber && ! rubber.isEmpty())
        {
            const auto& t = st::Theme::get();
            g.setColour (t.curve.withAlpha (0.06f));
            g.fillRoundedRectangle (rubber, 3.0f);
            g.setColour (t.curve.withAlpha (0.35f));
            g.drawRoundedRectangle (rubber, 3.0f, 1.0f);
        }
        paintViewButtons (g);
        return;
    }

    rebuildLayers (ps);
    const auto all = getLocalBounds().toFloat();

    g.drawImage (gridLayer, all, juce::RectanglePlacement::stretchToFit);
    paintSpectrum (g);
    paintPreview (g);
    g.drawImage (curveLayer, all, juce::RectanglePlacement::stretchToFit);
    paintGrab (g);
    paintDynHandles (g);
    paintTriggerBar (g);
    paintDots (g);
    paintReadout (g);
    if (! compact) paintMeter (g);

    const auto& t = st::Theme::get();
    if (drag == Drag::rubber && ! rubber.isEmpty())
    {
        g.setColour (t.curve.withAlpha (0.06f));
        g.fillRoundedRectangle (rubber, 3.0f);
        g.setColour (t.curve.withAlpha (0.35f));
        g.drawRoundedRectangle (rubber, 3.0f, 1.0f);
    }

    if (sketchMode)
    {
        if (sketchPoints.size() > 1)
        {
            juce::Path p;
            p.startNewSubPath (sketchPoints.front());
            for (size_t i = 1; i < sketchPoints.size(); ++i) p.lineTo (sketchPoints[i]);
            st::paint::glowStroke (g, p, t.curve, 2.0f, 6.0f);
        }
        g.setFont (st::fonts::medium (12.0f));
        g.setColour (t.text.withAlpha (0.7f));
        g.drawText (sketchPoints.empty() ? "EQ Sketch: draw the curve you want (Esc to cancel)" : "Release to create bands",
                    juce::Rectangle<float> (plot.getX() + 44.0f, plot.getY() + 8.0f, 420.0f, 22.0f), juce::Justification::centredLeft, false);
    }

    bool any = false;
    for (auto& b : bands) any |= b.used;
    paintViewButtons (g);
    if (! any && ! sketchMode && ! compact)
    {
        g.setFont (st::fonts::medium (13.0f));
        g.setColour (t.text.withAlpha (0.45f));
        g.drawText ("Double-click or drag the curve to create a band", plot.withSizeKeepingCentre (500.0f, 20.0f).translated (0.0f, -60.0f),
                    juce::Justification::centred, false);
    }
}

void EqDisplay::paintGrid (juce::Graphics& g)
{
    const auto& t = st::Theme::get();
    const float top = plot.getY(), bottom = plot.getBottom() - 18.0f;

    // frequency lines: decades and the 2 / 5 lines a little brighter
    for (float decade : { 10.0f, 100.0f, 1000.0f, 10000.0f })
        for (int m = 1; m <= 9; ++m)
        {
            const float hz = decade * (float) m;
            if (hz < minHz || hz > maxHz) continue;
            const float x = std::round (xForHz (hz)) + 0.5f;
            const float a = m == 1 ? 0.075f : (m == 2 || m == 5 ? 0.05f : 0.025f);
            g.setColour (juce::Colours::white.withAlpha (a));
            g.fillRect (x - 0.5f, top, 1.0f, bottom - top);
        }

    // dB lines over the whole plot width; 0 dB brighter
    const float r = displayRange;
    const float step = r <= 4.5f ? 1.0f : (r <= 9.0f ? 2.0f : (r <= 18.0f ? 3.0f : 6.0f));
    for (float db = -std::floor (r / step + 0.001f) * step; db <= r + 0.01f; db += step)
    {
        const float y = std::round (yForDb (db)) + 0.5f;
        const bool zero = std::abs (db) < 0.01f;
        g.setColour (juce::Colours::white.withAlpha (zero ? 0.11f : 0.04f));
        g.fillRect (plot.getX(), y - 0.5f, plot.getWidth(), 1.0f);

        // EQ scale (yellow) inside the plot at the right edge (not in compact rows: the range box says it)
        if (compact) continue;
        g.setFont (st::fonts::semibold (10.0f));
        g.setColour (t.curve.withAlpha (zero ? 0.95f : 0.8f));
        const auto label = zero ? juce::String ("0") : (db > 0 ? "+" : "") + juce::String ((int) db);
        g.drawText (label, juce::Rectangle<float> (plot.getRight() - 32.0f, y - 7.0f, 28.0f, 14.0f), juce::Justification::centredRight, false);
    }

    // frequency labels along the bottom of the plot
    g.setFont (st::fonts::medium (10.0f));
    if (! compact)
    for (float hz : { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
    {
        const float x = xForHz (hz);
        const auto label = hz >= 1000.0f ? juce::String ((int) (hz / 1000.0f)) + "k" : juce::String ((int) hz);
        g.setColour (t.textFaint);
        g.drawText (label, juce::Rectangle<float> (x - 20.0f, plot.getBottom() - 17.0f, 40.0f, 14.0f), juce::Justification::centred, false);
    }

    if (piano) paintPiano (g);

    // analyser scale (grey) between the plot and the meter
    if (analyzer.pre || analyzer.post || analyzer.sidechain)
    {
        const float range = AnalyzerSettings::ranges[analyzer.rangeIndex];
        g.setFont (st::fonts::regular (9.0f));
        g.setColour (t.textFaint.withAlpha (0.8f));
        for (float db = -10.0f; db > -range + 1.0f; db -= 10.0f)
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (plot.getRight() + 2.0f, analyzerY (db) - 6.0f, 26.0f, 12.0f),
                        juce::Justification::centredRight, false);
    }
}

void EqDisplay::paintSpectrum (juce::Graphics& g)
{
    const auto& t = st::Theme::get();
    const float range = AnalyzerSettings::ranges[analyzer.rangeIndex];
    auto area = juce::Rectangle<float> (plot.getX(), analyzerY (0.0f), plot.getWidth(), analyzerY (-range) - analyzerY (0.0f));

    g.saveState();
    g.reduceClipRegion (plot.withRight (juce::jmin (plot.getRight(), xForHz ((float) (sampleRate * 0.5) * 0.985f))).toNearestInt());

    // pre: a darker filled area; post: light line with a faint fill; side chain: cool line
    if (analyzer.pre)
    {
        auto pre = preSpectrum->createPath (area, -range, 0.0f, minHz, maxHz, true);
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.fillPath (pre);
    }
    if (analyzer.sidechain)
    {
        auto sc = scSpectrum->createPath (area, -range, 0.0f, minHz, maxHz, false);
        g.setColour (t.seriesColour (1).withAlpha (0.55f));
        g.strokePath (sc, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved));
    }
    if (analyzer.post)
    {
        auto fill = postSpectrum->createPath (area, -range, 0.0f, minHz, maxHz, true);
        g.setColour (t.spectrum.withAlpha (0.08f));
        g.fillPath (fill);
        auto line = postSpectrum->createPath (area, -range, 0.0f, minHz, maxHz, false);
        g.setColour (t.spectrum.withAlpha (0.5f));
        g.strokePath (line, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved));
        if (analyzer.collisions && collisionRef != nullptr)
            paintCollisions (g, area, range);
    }
    if (analyzer.freeze && ! maxHold.empty())
    {
        // frozen maximum: a brighter outline over the live analyser
        juce::Path p;
        const float x0 = area.getX(), w = area.getWidth();
        for (int i = 0; i < (int) maxHold.size(); ++i)
        {
            const float x = x0 + w * std::log (postSpectrum->getFrequencyForPoint (i) / minHz) / std::log (maxHz / minHz);
            const float y = area.getY() + area.getHeight() * juce::jlimit (0.0f, 1.0f, -maxHold[(size_t) i] / range);
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }
        g.setColour (t.text.withAlpha (0.55f));
        g.strokePath (p, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved));
    }
    g.restoreState();
}

float collisionAmount (float ownDb, float otherDb) noexcept
{
    // both spectra strong at the same frequency (masking / build-up between the two tracks): the quieter of the
    // two must be well up in the analyser range, and the other track no more than ~12 dB below this one
    const float m = juce::jmin (ownDb, otherDb);
    const float t = juce::jlimit (0.0f, 1.0f, (m + 52.0f) / 16.0f);
    const float near = juce::jlimit (0.0f, 1.0f, (otherDb - ownDb + 12.0f) / 8.0f);
    return t * t * (3.0f - 2.0f * t) * near;
}

void EqDisplay::paintCollisions (juce::Graphics& g, juce::Rectangle<float> area, float range)
{
    const auto& own = postSpectrum->getLevelsDb();
    const auto& ref = *collisionRef;
    const int n = (int) juce::jmin (own.size(), ref.size());
    if (n < 2) return;

    // red, from the spectrum line down, strongest where both are loud; a red line over the top
    const auto red = st::Theme::get().danger;
    const float x0 = area.getX(), w = area.getWidth();
    auto xAt = [&] (int i) { return x0 + w * std::log (postSpectrum->getFrequencyForPoint (i) / minHz) / std::log (maxHz / minHz); };
    auto yAt = [&] (float db) { return area.getY() + area.getHeight() * juce::jlimit (0.0f, 1.0f, -db / range); };
    juce::Path line;
    bool open = false;
    for (int i = 0; i < n - 1; ++i)
    {
        const float c = collisionAmount (own[(size_t) i], ref[(size_t) i]);
        const float xa = xAt (i), xb = xAt (i + 1);
        if (c > 0.01f)
        {
            const float y = yAt (own[(size_t) i]);
            g.setGradientFill (juce::ColourGradient (red.withAlpha (0.42f * c), 0.0f, y, red.withAlpha (0.0f), 0.0f, y + 70.0f, false));
            g.fillRect (juce::Rectangle<float> (xa, y, xb - xa + 0.6f, area.getBottom() - y));
        }
        if (c > 0.3f) { if (! open) line.startNewSubPath (xa, yAt (own[(size_t) i])); else line.lineTo (xa, yAt (own[(size_t) i])); open = true; }
        else open = false;
    }
    g.setColour (red.withAlpha (0.85f));
    g.strokePath (line, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved));
}

void EqDisplay::paintCurves (juce::Graphics& g)
{
    const auto& t = st::Theme::get();
    const float y0 = yForDb (0.0f);
    const float r = displayRange;
    auto clampDb = [r] (float db) { return juce::jlimit (-r * 6.0f, r * 2.0f, db); };   // far enough to leave the plot (clipped)
    auto pathOf = [&] (const std::array<float, kPoints>& db)
    {
        juce::Path path;
        for (int i = 0; i < kPoints; ++i)
        {
            const float y = yForDb (clampDb (db[(size_t) i]));
            if (i == 0) path.startNewSubPath (xs[0], y); else path.lineTo (xs[(size_t) i], y);
        }
        return path;
    };

    g.saveState();
    g.reduceClipRegion (plot.withBottom (plot.getBottom() - 18.0f).toNearestInt());

    // band areas: unselected first, selected on top
    for (int pass = 0; pass < 2; ++pass)
        for (int b = 0; b < kMaxBands; ++b)
        {
            const auto& p = bands[(size_t) b];
            const auto& c = curves[(size_t) b];
            if (! p.used || ! c.valid) continue;
            const bool sel = selection.contains (b);
            if ((pass == 1) != sel) continue;

            const float s = selectAmt[(size_t) b].current, h = hoverAmt[(size_t) b].current;
            const int solo = proc.soloBand.load();
            const auto col = (p.isActive() ? bandColour (b) : t.textFaint).withMultipliedAlpha (solo >= 0 && solo != b ? 0.3f : 1.0f);
            const auto line = pathOf (c.db);

            juce::Path area;
            if (c.hasRange)
            {
                // dynamic band: the area between the static curve and the range curve
                area = line;
                for (int i = kPoints - 1; i >= 0; --i) area.lineTo (xs[(size_t) i], yForDb (clampDb (c.rangeDb[(size_t) i])));
                area.closeSubPath();
                g.setColour (col.withAlpha ((p.isActive() ? 0.30f : 0.12f) + 0.25f * s + 0.10f * h));
                g.fillPath (area);
            }
            else
            {
                area = line;
                area.lineTo (xs[kPoints - 1], y0);
                area.lineTo (xs[0], y0);
                area.closeSubPath();
                const float peakDb = c.db[(size_t) (std::max_element (c.db.begin(), c.db.end(), [] (float a, float b2) { return std::abs (a) < std::abs (b2); }) - c.db.begin())];
                const float yPeak = yForDb (clampDb (peakDb));
                const float a = (p.isActive() ? 0.11f : 0.04f) + 0.24f * s + 0.10f * h;
                g.setGradientFill (juce::ColourGradient (col.withAlpha (a), 0.0f, yPeak, col.withAlpha (a * 0.35f), 0.0f, y0, false));
                g.fillPath (area);
            }

            g.setColour (col.withAlpha (sel ? 0.95f : (0.35f + 0.4f * h) * (p.isActive() ? 1.0f : 0.5f)));
            g.strokePath (line, juce::PathStrokeType (sel ? 1.6f : 1.1f, juce::PathStrokeType::curved));
            if (c.hasRange)
            {
                g.setColour (col.withAlpha (sel ? 0.6f : 0.3f));
                g.strokePath (pathOf (c.rangeDb), juce::PathStrokeType (1.0f, juce::PathStrokeType::curved));
            }
        }

    // total curve(s): crisp yellow
    for (int k = totalsCount - 1; k >= 0; --k)
    {
        const auto path = pathOf (totals[(size_t) k]);
        // channel colours: stereo yellow; left white / right red; mid green / side blue
        const auto& lab = totalsLabels[k];
        const auto col = totalsCount == 1 ? t.curve
                       : lab == "L" ? juce::Colour (0xffeeeef2)
                       : lab == "R" ? juce::Colour (0xffef5350)
                       : lab == "M" ? juce::Colour (0xff5ccf72)
                       : juce::Colour (0xff4f9cf0);
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.strokePath (path, juce::PathStrokeType (3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (col);
        g.strokePath (path, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (totalsCount > 1)
        {
            const int i = (int) (kPoints * 0.86f);
            const float y = yForDb (clampDb (totals[(size_t) k][(size_t) i]));
            g.setFont (st::fonts::semibold (10.0f));
            g.setColour (col.withAlpha (0.9f));
            g.drawText (totalsLabels[k], juce::Rectangle<float> (xs[(size_t) i] - 6.0f, y - (k == 0 ? 18.0f : -4.0f), 14.0f, 14.0f),
                        juce::Justification::centred, false);
        }
    }
    g.restoreState();
}

void EqDisplay::paintPreview (juce::Graphics& g)
{
    if (previewAlpha.current < 0.01f) return;
    const auto& t = st::Theme::get();
    const auto pos = mousePos;
    const auto shape = shapeForPosition (pos, false);

    BandParams b;
    b.used = true;
    b.shape = shape;
    b.freq = hzForX (pos.x);
    b.gain = st::dsp::eq::hasGain (shape) ? juce::jlimit (-30.0f, 30.0f, dbForY (pos.y)) : 0.0f;
    b.q = shape == Shape::notch ? 6.0f : (shape == Shape::bell ? 1.0f : 0.7071f);
    b.slope = shape == Shape::lowCut || shape == Shape::highCut ? 3 : 1;
    const auto a = st::dsp::eq::designAnalog (makeSpec (b, 1.0f));

    juce::Path path;
    const float r = displayRange;
    for (int i = 0; i < kPoints; i += 2)
    {
        // far enough to leave the plot (clipped), like the band curves: cuts must keep falling, not flatten
        const float y = yForDb (juce::jlimit (-r * 6.0f, r * 2.0f, (float) a.magnitudeDb (freqs[(size_t) i])));
        if (i == 0) path.startNewSubPath (xs[0], y); else path.lineTo (xs[(size_t) i], y);
    }
    juce::Path dashed;
    const float dashes[] = { 3.0f, 4.0f };
    juce::PathStrokeType (1.0f).createDashedStroke (dashed, path, dashes, 2);
    g.saveState();
    g.reduceClipRegion (plot.withBottom (plot.getBottom() - 18.0f).toNearestInt());
    g.setColour (t.curve.withAlpha (0.28f * previewAlpha.current));
    g.fillPath (dashed);
    g.restoreState();
}

void EqDisplay::paintDynHandles (juce::Graphics& g)
{
    // ▲▼ marker at the end of each dynamic band's range (drag it to change the range)
    for (int b = 0; b < kMaxBands; ++b)
    {
        const auto& p = bands[(size_t) b];
        if (! p.used || ! p.isDynamic()) continue;
        const auto hb = dynHandleBounds (b);
        const float h = hoverAmt[(size_t) b].current;
        const auto c = hb.getCentre();
        const auto col = juce::Colours::white.withAlpha (0.55f + 0.4f * juce::jmax (h, b == hoverDynBand ? 1.0f : 0.0f));
        juce::Path tri;
        tri.addTriangle (c.x - 3.5f, c.y - 1.5f, c.x + 3.5f, c.y - 1.5f, c.x, c.y - 5.5f);
        tri.addTriangle (c.x - 3.5f, c.y + 1.5f, c.x + 3.5f, c.y + 1.5f, c.x, c.y + 5.5f);
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillPath (tri, juce::AffineTransform::translation (0.0f, 1.0f));
        g.setColour (col);
        g.fillPath (tri);
    }
}

void EqDisplay::paintDots (juce::Graphics& g)
{
    for (int pass = 0; pass < 2; ++pass)
        for (int b = 0; b < kMaxBands; ++b)
        {
            const auto& p = bands[(size_t) b];
            if (! p.used) continue;
            const bool sel = selection.contains (b);
            if ((pass == 1) != sel) continue;

            const auto c = dotPosition (b);
            const float s = selectAmt[(size_t) b].current, h = hoverAmt[(size_t) b].current;
            const auto col = p.bypass ? bandColour (b).withSaturation (0.15f).darker (0.3f) : bandColour (b);

            // soft glow when hovered / selected
            if (s + h > 0.01f)
            {
                g.setColour (col.withAlpha (0.22f * juce::jmax (s, h)));
                g.fillEllipse (juce::Rectangle<float> (22.0f, 22.0f).withCentre (c));
            }

            const float r = 4.8f + 2.6f * s + 0.8f * h;
            const auto dot = juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c);
            g.setColour (juce::Colours::black.withAlpha (0.5f));
            g.fillEllipse (dot.expanded (1.0f).translated (0.0f, 0.8f));

            if (s > 0.01f)
            {
                // selected: white ring around the band colour
                g.setColour (juce::Colours::white.withAlpha (0.95f));
                g.fillEllipse (dot);
                g.setColour (col);
                g.fillEllipse (dot.reduced (2.2f * s + 0.01f));
            }
            else
            {
                g.setColour (col);
                g.fillEllipse (dot);
                g.setColour (juce::Colours::white.withAlpha (0.25f));
                g.drawEllipse (dot.reduced (0.5f), 0.8f);
            }

            if (p.place != Placement::stereo)
            {
                static const char* letters[] = { "", "L", "R", "M", "S" };
                g.setFont (st::fonts::bold (9.0f));
                g.setColour (col.brighter (0.4f));
                g.drawText (letters[(int) p.place], juce::Rectangle<float> (12.0f, 12.0f).withCentre (c.translated (r + 6.0f, -r - 4.0f)),
                            juce::Justification::centred, false);
            }
        }
}

EqDisplay::Readout EqDisplay::readoutLayout (int b) const
{
    Readout r;
    if (! juce::isPositiveAndBelow (b, kMaxBands) || ! bands[(size_t) b].used) return r;
    const auto& p = bands[(size_t) b];
    const auto c = dotPosition (b);

    std::array<Part, 4> parts { Part::freq, Part::none, Part::none, Part::none };
    int n = 1;
    if (st::dsp::eq::hasGain (p.shape)) parts[(size_t) n++] = Part::gain;
    if (st::dsp::eq::hasQ (p.shape)) parts[(size_t) n++] = Part::q;
    const bool range = p.isDynamic() && (drag == Drag::dynRange || hoverDynBand == b);
    if (range) ++n;   // range line (display only)

    const float lineH = 14.0f, buttonsH = 20.0f, w = 96.0f;
    const float h = buttonsH + lineH * (float) n + 8.0f;
    auto box = juce::Rectangle<float> (w, h).withPosition (c.x + 16.0f, c.y - h * 0.5f);
    if (box.getRight() > plot.getRight() - 34.0f) box.setX (c.x - 16.0f - w);
    box.setY (juce::jlimit (plot.getY() + 4.0f, plot.getBottom() - h - 20.0f, box.getY()));

    r.box = box;
    r.lines = n;
    r.parts = parts;
    r.buttons = box.withHeight (buttonsH).reduced (4.0f, 2.0f);
    auto row = r.buttons;
    const float bw = row.getWidth() / 4.0f;
    r.shape = row.removeFromLeft (bw);
    r.bypass = row.removeFromLeft (bw);
    r.solo = row.removeFromLeft (bw);
    r.remove = row;
    for (int i = 0; i < n; ++i)
        r.values[(size_t) i] = juce::Rectangle<float> (box.getX() + 4.0f, box.getY() + buttonsH + 3.0f + lineH * (float) i, w - 8.0f, lineH);
    return r;
}

juce::Rectangle<float> EqDisplay::getReadoutPart (int band, const juce::String& part) const
{
    const auto r = readoutLayout (band);
    if (part == "shape") return r.shape;
    if (part == "bypass") return r.bypass;
    if (part == "solo") return r.solo;
    if (part == "remove") return r.remove;
    const auto want = part == "freq" ? Part::freq : (part == "gain" ? Part::gain : Part::q);
    for (int i = 0; i < r.lines; ++i)
        if (r.parts[(size_t) i] == want) return r.values[(size_t) i];
    return {};
}

EqDisplay::Part EqDisplay::partAt (juce::Point<float> pos) const
{
    if (readoutBand < 0) return Part::none;
    const auto r = readoutLayout (readoutBand);
    if (! r.box.contains (pos)) return Part::none;
    if (r.shape.contains (pos)) return Part::shape;
    if (r.bypass.contains (pos)) return Part::bypass;
    if (r.solo.contains (pos)) return Part::solo;
    if (r.remove.contains (pos)) return Part::remove;
    for (int i = 0; i < r.lines; ++i)
        if (r.values[(size_t) i].contains (pos)) return r.parts[(size_t) i];
    return Part::none;
}

void EqDisplay::paintReadout (juce::Graphics& g)
{
    int b = drag == Drag::bands || drag == Drag::dynRange ? primary : (hoverDynBand >= 0 ? hoverDynBand : hoverBand);
    if (drag == Drag::value || drag == Drag::solo) b = readoutBand;
    if (entry != nullptr) b = entryBand;
    if (! juce::isPositiveAndBelow (b, kMaxBands) || ! bands[(size_t) b].used) return;

    const auto& t = st::Theme::get();
    const auto& p = bands[(size_t) b];
    const auto r = readoutLayout (b);
    const auto box = r.box;
    const auto col = bandColour (b);
    const bool interactive = b == readoutBand;

    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillRoundedRectangle (box.translated (0.0f, 2.0f), 5.0f);
    g.setColour (t.bg1.withAlpha (0.95f));
    g.fillRoundedRectangle (box, 5.0f);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.drawRoundedRectangle (box.reduced (0.5f), 5.0f, 1.0f);
    g.setColour (col.withAlpha (0.55f));
    g.fillRoundedRectangle (box.withWidth (2.5f).reduced (0.0f, 6.0f).translated (1.5f, 0.0f), 1.0f);

    // quick buttons: shape, bypass, solo (hold), delete
    auto iconCol = [&] (Part part, bool on, juce::Colour onCol)
    {
        const bool hot = interactive && (hoverPart == part || (part == Part::solo && drag == Drag::solo));
        auto c2 = t.textDim.withAlpha (0.8f).interpolatedWith (t.text, hot ? 1.0f : 0.0f);
        return on ? onCol : c2;
    };
    {
        const auto gl = r.shape.withSizeKeepingCentre (14.0f, 10.0f);
        g.setColour (iconCol (Part::shape, false, {}));
        g.strokePath (shapeGlyph (p.shape, gl), juce::PathStrokeType (1.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    st::icons::draw (g, st::icons::Icon::power, r.bypass.withSizeKeepingCentre (13.0f, 13.0f), iconCol (Part::bypass, p.bypass, t.warning), 1.4f);
    st::icons::draw (g, st::icons::Icon::headphones, r.solo.withSizeKeepingCentre (13.0f, 13.0f), iconCol (Part::solo, proc.soloBand.load() == b, t.warning), 1.4f);
    st::icons::draw (g, st::icons::Icon::close, r.remove.withSizeKeepingCentre (11.0f, 11.0f), iconCol (Part::remove, false, {}), 1.4f);
    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.drawHorizontalLine (juce::roundToInt (r.buttons.getBottom() + 1.0f), box.getX() + 6.0f, box.getRight() - 6.0f);

    // values
    const auto f = st::fonts::medium (11.0f);
    g.setFont (f);
    for (int i = 0; i < r.lines; ++i)
    {
        const auto part = r.parts[(size_t) i];
        juce::String text;
        switch (part)
        {
            case Part::freq: text = formatFrequency (p.freq); break;
            case Part::gain: text = st::params::formatDb (p.gain * gainScale, 1); break;
            case Part::q:    text = "Q " + juce::String (p.q, p.q < 10.0f ? 2 : 1); break;
            case Part::none: case Part::shape: case Part::bypass: case Part::solo: case Part::remove:
                text = "Range " + st::params::formatDb (p.dynRange * gainScale, 1); break;
        }
        const bool hot = interactive && part != Part::none && (hoverPart == part || (drag == Drag::value && valuePart == part));
        const auto vr = r.values[(size_t) i];
        if (hot)
        {
            g.setColour (juce::Colours::white.withAlpha (0.07f));
            g.fillRoundedRectangle (vr, 3.0f);
        }
        g.setColour (hot ? t.text : (i == 0 ? t.text.withAlpha (0.92f) : t.textDim));
        g.drawText (text, vr.withTrimmedLeft (6.0f), juce::Justification::centredLeft, false);
        if (part == Part::freq)
        {
            g.setFont (st::fonts::regular (9.5f));
            g.setColour (t.textFaint);
            g.drawText (noteName (p.freq), vr.withTrimmedRight (4.0f), juce::Justification::centredRight, false);
            g.setFont (f);
        }
    }
}

void EqDisplay::paintMeter (juce::Graphics& g)
{
    const auto& t = st::Theme::get();
    auto area = meterArea;

    g.setFont (st::fonts::semibold (9.0f));
    g.setColour (meterMax > 0.0f ? t.danger : t.textDim);
    g.drawText (meterMax <= -99.0f ? juce::String ("-inf") : juce::String (meterMax, 1),
                juce::Rectangle<float> (area.getX() - 10.0f, plot.getY() + 6.0f, area.getWidth() + 14.0f, 14.0f), juce::Justification::centred, false);

    constexpr float lo = -60.0f, hi = 6.0f;
    auto yFor = [&] (float db) { const float n = juce::jlimit (0.0f, 1.0f, (db - lo) / (hi - lo)); return area.getBottom() - std::pow (n, 1.35f) * area.getHeight(); };
    const float bw = (area.getWidth() - 2.0f) * 0.5f;
    const auto green = t.seriesColour (0).interpolatedWith (t.warning, 0.35f);
    for (int c = 0; c < 2; ++c)
    {
        auto bar = juce::Rectangle<float> (area.getX() + (float) c * (bw + 2.0f), area.getY(), bw, area.getHeight());
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRect (bar);
        const float y = yFor (meterDb[(size_t) c]);
        if (y < bar.getBottom() - 0.5f)
        {
            juce::ColourGradient grad (green.darker (0.25f), 0.0f, bar.getBottom(), t.warning, 0.0f, yFor (0.0f), false);
            g.setGradientFill (grad);
            g.fillRect (bar.withTop (y));
            if (meterDb[(size_t) c] > 0.0f)
            {
                g.setColour (t.danger);
                g.fillRect (bar.withTop (y).withBottom (yFor (0.0f)));
            }
        }
        if (meterHold[(size_t) c] > lo)
        {
            g.setColour (meterHold[(size_t) c] > 0.0f ? t.danger : t.warning);
            g.fillRect (bar.getX(), yFor (meterHold[(size_t) c]) - 0.5f, bar.getWidth(), 1.5f);
        }
    }
}

void EqDisplay::paintGrab (juce::Graphics& g)
{
    if (! grabFrozen || peaks.empty()) return;
    const auto& t = st::Theme::get();
    for (int i = 0; i < (int) peaks.size(); ++i)
    {
        const auto& pk = peaks[(size_t) i];
        const bool hot = i == hoverPeak;
        const float r = hot ? 5.0f : 3.0f;
        g.setColour (t.spectrum.withAlpha (hot ? 0.95f : 0.45f));
        g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre ({ pk.x, pk.y }), hot ? 1.6f : 1.0f);
        if (hot)
        {
            g.setFont (st::fonts::semibold (10.5f));
            g.setColour (t.text);
            g.drawText (formatFrequency (pk.hz) + "  " + noteName (pk.hz), juce::Rectangle<float> (140.0f, 16.0f).withCentre ({ pk.x, pk.y - 16.0f }),
                        juce::Justification::centred, false);
        }
    }
}

void EqDisplay::paintPiano (juce::Graphics& g)
{
    // keyboard strip just above the frequency labels; C keys labelled
    const float h = 12.0f, y = plot.getBottom() - 18.0f - h;
    const bool black[12] = { false, true, false, true, false, false, true, false, true, false, true, false };
    for (int pass = 0; pass < 2; ++pass)
        for (int n = 21; n <= 108; ++n)   // 88 keys, A0 .. C8
        {
            const bool isBlack = black[n % 12];
            if ((pass == 1) != isBlack) continue;
            const float f0 = 440.0f * std::exp2 (((float) n - 69.5f) / 12.0f), f1 = 440.0f * std::exp2 (((float) n - 68.5f) / 12.0f);
            if (f1 < minHz || f0 > maxHz) continue;
            auto key = juce::Rectangle<float> (xForHz (f0), y, xForHz (f1) - xForHz (f0), isBlack ? h * 0.62f : h).reduced (isBlack ? 0.0f : 0.25f, 0.0f);
            g.setColour (isBlack ? st::Theme::get().bg0 : juce::Colours::white.withAlpha (0.16f));
            g.fillRect (key);
            if (n % 12 == 0 && key.getWidth() > 2.5f)
            {
                g.setFont (st::fonts::regular (7.5f));
                g.setColour (st::Theme::get().textFaint);
                g.drawText ("C" + juce::String (n / 12 - 1), key.withY (y - 10.0f).withHeight (10.0f).expanded (6.0f, 0.0f), juce::Justification::centred, false);
            }
        }
}

int EqDisplay::triggerHandleAt (juce::Point<float> pt) const
{
    if (! flatView()) return 0;
    if (triggerBand < 0 || triggerBarY < 0.0f || ! bands[(size_t) triggerBand].used || bands[(size_t) triggerBand].trigger != Trigger::free) return 0;
    const auto& p = bands[(size_t) triggerBand];
    if (std::abs (pt.y - triggerBarY) > 9.0f) return 0;
    if (std::abs (pt.x - xForHz (p.trigLo)) < 9.0f) return 1;
    if (std::abs (pt.x - xForHz (p.trigHi)) < 9.0f) return 2;
    return 0;
}

void EqDisplay::paintTriggerBar (juce::Graphics& g)
{
    if (triggerBand < 0 || triggerBarY < 0.0f) return;
    const auto& p = bands[(size_t) triggerBand];
    if (! p.used || p.trigger != Trigger::free) return;
    const auto col = bandColour (triggerBand);
    const float x0 = xForHz (juce::jmin (p.trigLo, p.trigHi)), x1 = xForHz (juce::jmax (p.trigLo, p.trigHi));
    const float y = triggerBarY;

    // light column over the trigger range, the bar and two half-round grips
    g.setGradientFill (juce::ColourGradient (col.withAlpha (0.0f), 0.0f, plot.getY(), col.withAlpha (0.10f), 0.0f, y, false));
    g.fillRect (juce::Rectangle<float> (x0, plot.getY(), x1 - x0, y - plot.getY()));
    g.setColour (juce::Colours::white.withAlpha (0.18f));
    g.fillRect (plot.getX(), y - 0.5f, plot.getWidth(), 1.0f);
    g.setColour (col);
    g.fillRoundedRectangle (juce::Rectangle<float> (x0, y - 2.0f, x1 - x0, 4.0f), 2.0f);
    for (float x : { x0, x1 })
    {
        auto grip = juce::Rectangle<float> (12.0f, 16.0f).withCentre ({ x, y });
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRoundedRectangle (grip.translated (0.0f, 1.0f), 5.0f);
        g.setColour (st::Theme::get().knobDark);
        g.fillRoundedRectangle (grip, 5.0f);
        g.setColour (juce::Colours::white.withAlpha (0.25f));
        g.drawRoundedRectangle (grip.reduced (0.5f), 5.0f, 1.0f);
    }
}

void EqDisplay::updatePeaks()
{
    peaks.clear();
    if (! analyzer.post) return;
    const auto& lv = postSpectrum->getLevelsDb();
    const int n = (int) lv.size();
    for (int i = 4; i < n - 4; ++i)
    {
        const float v = lv[(size_t) i];
        bool isMax = true;
        for (int k = -4; k <= 4 && isMax; ++k) if (k != 0 && lv[(size_t) (i + k)] > v) isMax = false;
        if (! isMax) continue;
        float lo = v;
        for (int k = -14; k <= 14; ++k) lo = juce::jmin (lo, lv[(size_t) juce::jlimit (0, n - 1, i + k)]);
        if (v - lo < 5.0f || v < -AnalyzerSettings::ranges[analyzer.rangeIndex] + 6.0f) continue;
        const float hz = postSpectrum->getFrequencyForPoint (i);
        if (hz < 20.0f || hz > 20000.0f) continue;
        peaks.push_back ({ hz, v, xForHz (hz), analyzerY (v) });
    }
}

//==============================================================================
void EqDisplay::select (int band, bool add)
{
    if (! add) selection.clearQuick();
    if (band >= 0) selection.addIfNotAlreadyThere (band);
    primary = band >= 0 ? band : (selection.isEmpty() ? -1 : selection.getLast());
    notifySelection();
}

void EqDisplay::setSelection (const juce::Array<int>& s, int p)
{
    selection = s;
    primary = p;
    notifySelection();
}

void EqDisplay::notifySelection()
{
    juce::StringArray ids;
    for (int b : selection) ids.add (juce::String (b));
    proc.uiState.setProperty ("selection", ids.joinIntoString (","), nullptr);
    proc.uiState.setProperty ("primary", primary, nullptr);
    curveLayer = {};
    if (onSelectionChanged) onSelectionChanged();
    repaint();
}

void EqDisplay::beginGesture (int band, const char* suffix)
{
    proc.bandParam (band, suffix).beginChangeGesture();
    gestureBands.add (band * 64 + ids::bandSuffixes.indexOf (suffix));
}

void EqDisplay::setPlain (int band, const char* suffix, float v)
{
    auto& p = proc.bandParam (band, suffix);
    p.setValueNotifyingHost (p.convertTo0to1 (v));
}

void EqDisplay::endGestures()
{
    for (int code : gestureBands)
        proc.bandParam (code / 64, ids::bandSuffixes[code % 64]).endChangeGesture();
    gestureBands.clear();
}

Shape EqDisplay::shapeForPosition (juce::Point<float> pos, bool fromCurve) const
{
    const float hz = hzForX (pos.x);
    if (fromCurve)
        return hz < 45.0f ? Shape::lowShelf : (hz > 11000.0f ? Shape::highShelf : Shape::bell);
    if (hz < 22.0f) return Shape::lowCut;
    if (hz > 17000.0f) return Shape::highCut;
    if (dbForY (pos.y) < -displayRange * 1.08f) return Shape::notch;
    return Shape::bell;
}

int EqDisplay::createBandAt (juce::Point<float> pos, bool fromCurve, bool dynamic, bool spectral)
{
    const auto shape = shapeForPosition (pos, fromCurve);
    const float hz = hzForX (pos.x);
    float gain = 0.0f;
    if (! fromCurve && st::dsp::eq::hasGain (shape))
        gain = juce::jlimit (-30.0f, 30.0f, dbForY (pos.y) / (std::abs (gainScale) > 0.01f ? gainScale : 1.0f));
    const float q = shape == Shape::notch ? 6.0f : (shape == Shape::bell ? 1.0f : 0.7071f);
    const int slope = shape == Shape::lowCut || shape == Shape::highCut ? 3 : 1;

    proc.undoManager.beginNewTransaction ("Add band");
    const int b = proc.addBand (shape, hz, std::round (gain * 10.0f) / 10.0f, q, slope, Placement::stereo, false);
    if (b >= 0 && (dynamic || spectral) && st::dsp::eq::hasGain (shape))
        proc.setBandValue (b, "dyn_range", -6.0f);
    if (b >= 0 && spectral && st::dsp::eq::hasGain (shape))
        proc.setBandValue (b, "spectral", 1.0f);
    readParameters();
    return b;
}

void EqDisplay::beginBandDrag (const juce::MouseEvent& e)
{
    drag = Drag::bands;
    dragStart = e.position;
    dragDbAccum = 0.0f;
    lastDragY = e.position.y;
    lastDragPos = e.position;
    axisLocked = false;
    dragStarts.clear();
    proc.undoManager.beginNewTransaction ("Move bands");
    for (int b : selection)
    {
        const auto& p = bands[(size_t) b];
        dragStarts.push_back ({ b, p.freq, p.gain, p.q, p.dynRange, p });
        beginGesture (b, "freq");
        beginGesture (b, "gain");
        beginGesture (b, "q");
        if (getView() == View::top) beginGesture (b, "depth");
    }
}

void EqDisplay::applyBandDrag (const juce::MouseEvent& e)
{
    const float fine = e.mods.isShiftDown() ? 0.15f : 1.0f;
    float dx = (e.position.x - dragStart.x) * fine;
    float dy = (e.position.y - dragStart.y) * fine;

    if (e.mods.isAltDown())
    {
        if (! axisLocked && (std::abs (dx) > 4.0f || std::abs (dy) > 4.0f)) { axisLocked = true; lockHorizontal = std::abs (dx) > std::abs (dy); }
        if (axisLocked) { if (lockHorizontal) dy = 0.0f; else dx = 0.0f; }
    }
    else axisLocked = false;

    const auto view = getView();
    const juce::Rectangle<float> span (plot.getX(), eqArea.getY(), plot.getWidth(), eqArea.getHeight());
    const float octPerPx = std::log2 (maxHz / minHz) / juce::jmax (1.0f, span.getWidth());
    float ratio = std::exp2 (dx * octPerPx);

    // 3D view: frequency and gain under the mouse on the (first) dragged band's plane
    bool onPlane = false;
    float planeStepDb = 0.0f;
    if (view == View::threeD && ! dragStarts.empty())
    {
        const float w = dragStarts.front().params.depth * 0.01f;
        float h0, d0, h1, d1, hp, dp;
        if (planePoint (dragStart, w, h0, d0) && planePoint (e.position, w, h1, d1) && planePoint (lastDragPos, w, hp, dp))
        {
            onPlane = true;
            ratio = axisLocked && ! lockHorizontal ? 1.0f : std::exp2 (fine * std::log2 (juce::jmax (1.0f, h1) / juce::jmax (1.0f, h0)));
            planeStepDb = axisLocked && lockHorizontal ? 0.0f : fine * (d1 - dp);
        }
        lastDragPos = e.position;
    }

    if (view == View::top)
    {
        // Top view: across = frequency, up = depth (Cmd: Q)
        const float dDepth = -dy / juce::jmax (1.0f, topArea().getHeight()) * 100.0f;
        for (auto& s : dragStarts)
        {
            setPlain (s.band, "freq", juce::jlimit (10.0f, 30000.0f, s.freq * ratio));
            if (e.mods.isCommandDown()) setPlain (s.band, "q", juce::jlimit (0.025f, 40.0f, s.q * std::exp2 (-dy / 50.0f)));
            else                        setPlain (s.band, "depth", juce::jlimit (0.0f, 100.0f, s.params.depth + dDepth));
        }
        return;
    }

    // vertical: accumulate dB from each mouse step at the scale shown at that moment, so a scale that grows
    // while you drag past the edge never feeds back into the gain
    const float pxPerDb = span.getHeight() * 0.5f / displayRange;
    if (onPlane) dragDbAccum += planeStepDb;
    else
    {
        dragDbAccum -= (e.position.y - lastDragY) * fine / pxPerDb;
        if (std::abs (dy) < 0.001f) dragDbAccum = 0.0f;   // axis locked horizontally, or back at the start
    }
    lastDragY = e.position.y;
    const float dDb = dragDbAccum;
    const bool qMode = e.mods.isCommandDown();

    const bool snap = piano && ! e.mods.isShiftDown();
    for (auto& s : dragStarts)
    {
        float f = juce::jlimit (10.0f, 30000.0f, s.freq * ratio);
        if (snap) f = 440.0f * std::exp2 (std::round (12.0f * std::log2 (f / 440.0f)) / 12.0f);
        setPlain (s.band, "freq", juce::jlimit (10.0f, 30000.0f, f));
        if (qMode)
            setPlain (s.band, "q", juce::jlimit (0.025f, 40.0f, s.q * std::exp2 (-dy / 50.0f)));
        else
            applyVertical (s.band, s.params, dDb);
    }
}

void EqDisplay::removeSelected()
{
    if (selection.isEmpty()) return;
    proc.undoManager.beginNewTransaction ("Delete bands");
    const auto toRemove = selection;
    selection.clearQuick();
    primary = -1;
    for (int b : toRemove) proc.removeBand (b);
    readParameters();
    notifySelection();
}

void EqDisplay::splitSelected (bool midSide)
{
    const auto a = midSide ? Placement::mid : Placement::left;
    const auto bPlace = midSide ? Placement::side : Placement::right;
    proc.undoManager.beginNewTransaction ("Split bands");
    juce::Array<int> made;
    for (int t : selection)
    {
        if (bands[(size_t) t].place != Placement::stereo) continue;
        const auto copies = proc.pasteBands (proc.bandsToText ({ t }), false);
        proc.setBandValue (t, "place", (float) (int) a);
        made.add (t);
        for (int c : copies) { proc.setBandValue (c, "place", (float) (int) bPlace); made.add (c); }
    }
    readParameters();
    if (! made.isEmpty()) setSelection (made, made.getFirst());
}

//==============================================================================
void EqDisplay::mouseMove (const juce::MouseEvent& e)
{
    mousePos = e.position;
    mouseInside = plot.contains (e.position);

    // the parameter display stays while the mouse is over it (so its values and buttons can be used)
    const bool overReadout = readoutBand >= 0 && readoutLayout (readoutBand).box.expanded (6.0f).contains (e.position);
    const auto part = overReadout ? partAt (e.position) : Part::none;
    if (part != hoverPart) { hoverPart = part; repaint(); }
    int hb = overReadout ? readoutBand : bandAt (e.position);
    if (! overReadout && hb < 0 && readoutBand >= 0 && readoutLayout (readoutBand).box.expanded (6.0f).getUnion (
            juce::Rectangle<float> (1.0f, 1.0f).withCentre (dotPosition (readoutBand))).contains (e.position))
        hb = readoutBand;   // crossing the gap between the dot and its display
    readoutBand = hb;
    const int hd = hb < 0 ? dynHandleAt (e.position) : -1;
    const int hdd = hb < 0 ? depthHandleAt (e.position) : -1;
    if (hdd != hoverDepthBand) { hoverDepthBand = hdd; setMouseCursor (hdd >= 0 ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor); }
    if (const bool hg = hb < 0 && gizmoAt (e.position); hg != hoverGizmo)
    {
        hoverGizmo = hg;
        setMouseCursor (hg ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }

    // spectrum grab: freeze the analyser while hovering near its peaks
    int hp = -1;
    const bool canGrab = analyzer.grab && analyzer.post && hb < 0 && hd < 0 && ! sketchMode && mouseInside && flatView();
    if (canGrab)
    {
        if (! grabFrozen) { grabFrozen = true; updatePeaks(); }
        float best = 16.0f;
        for (int i = 0; i < (int) peaks.size(); ++i)
        {
            const auto& pk = peaks[(size_t) i];
            const float d = std::abs (pk.x - e.position.x);
            if (d < best && std::abs (pk.y - e.position.y) < 30.0f) { best = d; hp = i; }
        }
        // only stay frozen while the mouse is close to the spectrum
        if (hp < 0)
        {
            bool nearSpectrum = false;
            for (auto& pk : peaks) nearSpectrum |= std::abs (pk.x - e.position.x) < 60.0f && std::abs (pk.y - e.position.y) < 60.0f;
            if (! nearSpectrum) { grabFrozen = false; peaks.clear(); }
        }
    }
    else if (grabFrozen) { grabFrozen = false; peaks.clear(); }

    if (hb != hoverBand || hd != hoverDynBand || hp != hoverPeak)
    {
        hoverBand = hb; hoverDynBand = hd; hoverPeak = hp;
        setMouseCursor (sketchMode ? juce::MouseCursor::CrosshairCursor
                      : (part == Part::freq || part == Part::gain || part == Part::q) ? juce::MouseCursor::UpDownResizeCursor
                      : (hd >= 0 || hdd >= 0) ? juce::MouseCursor::UpDownResizeCursor
                      : (hb >= 0 || hp >= 0) ? juce::MouseCursor::PointingHandCursor
                      : juce::MouseCursor::NormalCursor);
        curveLayer = {};
    }
    repaint();
}

void EqDisplay::mouseExit (const juce::MouseEvent&)
{
    mouseInside = false;
    if (drag == Drag::none) { readoutBand = -1; hoverPart = Part::none; }
    hoverBand = hoverDynBand = hoverPeak = hoverDepthBand = -1;
    hoverGizmo = false;
    if (grabFrozen) { grabFrozen = false; peaks.clear(); }
    curveLayer = {};
    repaint();
}

void EqDisplay::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    if (entry != nullptr) commitEntry (false);
    dragStart = e.position;

    if (const int vb = viewButtonAt (e.position); vb >= 0)
    {
        if (! e.mods.isPopupMenu()) setView ((View) vb);
        return;
    }
    if (gizmoAt (e.position) && ! e.mods.isPopupMenu())
    {
        // 3D view: drag the axis gizmo to turn (across) and tilt (up / down) the camera
        drag = Drag::orbit;
        orbitYaw0 = getCameraYaw();
        orbitPitch0 = getCameraPitch();
        return;
    }

    if (const auto part = partAt (e.position); part != Part::none && ! e.mods.isPopupMenu())
    {
        const int rb = readoutBand;
        readParameters();
        const auto p = bands[(size_t) rb];
        switch (part)
        {
            case Part::freq: case Part::gain: case Part::q:
            {
                drag = Drag::value;
                valuePart = part;
                const char* id = part == Part::freq ? "freq" : (part == Part::gain ? "gain" : "q");
                valueStart = part == Part::freq ? p.freq : (part == Part::gain ? p.gain : p.q);
                proc.undoManager.beginNewTransaction ("Band value");
                beginGesture (rb, id);
                if (! selection.contains (rb)) select (rb);
                return;
            }
            case Part::shape:
                st::showMenu (makeShapeMenu ((int) p.shape), nullptr, [this, rb] (int r) { if (r > 0) { proc.undoManager.beginNewTransaction ("Shape"); proc.setBandValue (rb, "shape", (float) (r - 1)); } }, true);
                return;
            case Part::bypass:
                proc.undoManager.beginNewTransaction ("Bypass band");
                proc.setBandValue (rb, "bypass", p.bypass ? 0.0f : 1.0f);
                return;
            case Part::remove:
                setSelection ({ rb }, rb);
                removeSelected();
                readoutBand = -1;
                hoverPart = Part::none;
                return;
            case Part::solo:
                // click and hold to listen; drag sideways to move the band, up / down for its width
                drag = Drag::solo;
                proc.soloTrigger = false;
                proc.soloBand = rb;
                valueStart = p.freq;
                valueStart2 = p.q;
                proc.undoManager.beginNewTransaction ("Solo drag");
                beginGesture (rb, "freq");
                if (st::dsp::eq::hasQ (p.shape)) beginGesture (rb, "q");
                curveLayer = {};
                repaint();
                return;
            case Part::none:
                break;
        }
    }

    const int b = bandAt (e.position);
    const int dh = b < 0 ? dynHandleAt (e.position) : -1;

    if (e.mods.isPopupMenu())
    {
        if (b >= 0) { if (! selection.contains (b)) select (b); showBandMenu (b); }
        else showBackgroundMenu();
        return;
    }

    if (const int th = triggerHandleAt (e.position); th > 0)
    {
        drag = th == 1 ? Drag::triggerLo : Drag::triggerHi;
        proc.undoManager.beginNewTransaction ("Trigger range");
        beginGesture (triggerBand, th == 1 ? "dyn_lo" : "dyn_hi");
        return;
    }

    if (sketchMode)
    {
        drag = Drag::sketch;
        sketchPoints.clear();
        sketchPoints.push_back (e.position);
        if (curvesDirty) rebuildCurves();
        sketchBase = totals[0];
        sketchBands.clear();
        lastSketchFit = juce::Time::getMillisecondCounterHiRes();
        proc.undoManager.beginNewTransaction ("EQ Sketch");
        return;
    }

    if (dh >= 0)
    {
        if (! selection.contains (dh)) select (dh);
        drag = Drag::dynRange;
        dragDbAccum = 0.0f;
        lastDragY = e.position.y;
        dragStarts = { { dh, bands[(size_t) dh].freq, bands[(size_t) dh].gain, bands[(size_t) dh].q, bands[(size_t) dh].dynRange, bands[(size_t) dh] } };
        proc.undoManager.beginNewTransaction ("Dynamic range");
        beginGesture (dh, "dyn_range");
        return;
    }

    if (b >= 0)
    {
        if (e.mods.isAltDown() && e.mods.isCommandDown())
        {
            select (b);
            st::showMenu (makeShapeMenu ((int) bands[(size_t) b].shape), nullptr, [this, b] (int r) { if (r > 0) { proc.undoManager.beginNewTransaction ("Shape"); proc.setBandValue (b, "shape", (float) (r - 1)); } }, true);
            return;
        }
        if (e.mods.isAltDown() && e.mods.isShiftDown())
        {
            select (b);
            juce::PopupMenu m;
            for (int i = 0; i < slopeNames.size(); ++i) m.addItem (i + 1, slopeNames[i], true, bands[(size_t) b].slope == i);
            st::showMenu (m, nullptr, [this, b] (int r) { if (r > 0) { proc.undoManager.beginNewTransaction ("Slope"); proc.setBandValue (b, "slope", (float) (r - 1)); } }, true);
            return;
        }
        if (e.mods.isAltDown())
        {
            // Alt-click toggles bypass (on mouse-up, if the mouse didn't move); Alt-drag constrains the axis.
            pendingAltClick = b;
            if (! selection.contains (b)) select (b);
            beginBandDrag (e);
            return;
        }
        if (e.mods.isCommandDown())
        {
            if (selection.contains (b)) { selection.removeFirstMatchingValue (b); primary = selection.isEmpty() ? -1 : selection.getLast(); notifySelection(); }
            else select (b, true);
            return;
        }
        if (e.mods.isShiftDown() && primary >= 0 && ! selection.contains (b))
        {
            const float f0 = juce::jmin (bands[(size_t) primary].freq, bands[(size_t) b].freq);
            const float f1 = juce::jmax (bands[(size_t) primary].freq, bands[(size_t) b].freq);
            for (int k = 0; k < kMaxBands; ++k)
                if (bands[(size_t) k].used && bands[(size_t) k].freq >= f0 && bands[(size_t) k].freq <= f1) selection.addIfNotAlreadyThere (k);
            primary = b;
            notifySelection();
            return;
        }
        if (! selection.contains (b)) select (b);
        else { primary = b; notifySelection(); }
        beginBandDrag (e);
        return;
    }

    // 3D view: drag a dot's shadow on the floor to move the band back / forward
    if (const int db3 = depthHandleAt (e.position); db3 >= 0)
    {
        if (! selection.contains (db3)) select (db3);
        drag = Drag::depth;
        depthGrabOffset = depthHandle (db3) - e.position;
        const auto& p = bands[(size_t) db3];
        dragStarts = { { db3, p.freq, p.gain, p.q, p.dynRange, p } };
        proc.undoManager.beginNewTransaction ("Depth");
        beginGesture (db3, "depth");
        return;
    }

    if (! plot.contains (e.position)) return;

    // spectrum grab: create a bell on the peak and drag its gain
    // (created only once the mouse actually moves, so a click or double-click on the line doesn't add a band)
    if (hoverPeak >= 0 && grabFrozen && flatView())
    {
        drag = Drag::pendingGrab;
        pendingGrabHz = peaks[(size_t) hoverPeak].hz;
        pendingDynamic = e.mods.isAltDown();
        pendingSpectral = e.mods.isAltDown() && e.mods.isShiftDown();
        return;
    }

    // drag the curve to create a band
    if (flatView() && std::abs (curveYAt (e.position.x) - e.position.y) < 7.0f)
    {
        drag = Drag::pendingCurve;
        pendingDynamic = e.mods.isAltDown();
        pendingSpectral = e.mods.isAltDown() && e.mods.isShiftDown();
        return;
    }

    // rubber band selection
    drag = Drag::rubber;
    rubber = {};
    rubberBase = e.mods.isCommandDown() || e.mods.isShiftDown() ? selection : juce::Array<int>();
    if (rubberBase.isEmpty() && ! selection.isEmpty()) { selection.clearQuick(); primary = -1; notifySelection(); }
}

void EqDisplay::mouseDrag (const juce::MouseEvent& e)
{
    mousePos = e.position;
    if (drag == Drag::value && readoutBand >= 0)
    {
        const float dy = dragStart.y - e.position.y;
        const float fine = e.mods.isShiftDown() ? 0.2f : 1.0f;
        if (valuePart == Part::freq)      setPlain (readoutBand, "freq", juce::jlimit (minHz, maxHz, valueStart * std::exp2 (dy * fine / 60.0f)));
        else if (valuePart == Part::gain) setPlain (readoutBand, "gain", juce::jlimit (-30.0f, 30.0f, valueStart + dy * fine * 0.1f));
        else if (valuePart == Part::q)    setPlain (readoutBand, "q", juce::jlimit (0.025f, 40.0f, valueStart * std::exp2 (dy * fine / 80.0f)));
        readParameters();
        return;
    }
    if (drag == Drag::solo && readoutBand >= 0)
    {
        const float fine = e.mods.isShiftDown() ? 0.2f : 1.0f;
        const float x = xForHz (valueStart) + (e.position.x - dragStart.x) * fine;
        setPlain (readoutBand, "freq", juce::jlimit (minHz, maxHz, hzForX (x)));
        if (st::dsp::eq::hasQ (bands[(size_t) readoutBand].shape))
            setPlain (readoutBand, "q", juce::jlimit (0.025f, 40.0f, valueStart2 * std::exp2 ((dragStart.y - e.position.y) * fine / 80.0f)));
        readParameters();
        return;
    }
    if ((drag == Drag::pendingCurve || drag == Drag::pendingGrab) && e.position.getDistanceFrom (dragStart) > 3.0f)
    {
        const bool grab = drag == Drag::pendingGrab;
        drag = Drag::none;
        const int nb = createBandAt (grab ? juce::Point<float> (xForHz (pendingGrabHz), yForDb (0.0f)) : dragStart, true, pendingDynamic, pendingSpectral);
        if (nb < 0) return;
        if (grab)
        {
            proc.setBandValue (nb, "shape", (float) (int) Shape::bell);
            proc.setBandValue (nb, "q", 4.0f);
            readParameters();
            grabFrozen = false; peaks.clear(); hoverPeak = -1;
        }
        select (nb);
        auto startEvent = e.withNewPosition (dragStart);
        beginBandDrag (startEvent);
    }

    if (pendingAltClick >= 0 && e.position.getDistanceFrom (dragStart) > 3.0f)
        pendingAltClick = -1;

    switch (drag)
    {
        case Drag::pendingCurve:
        case Drag::pendingGrab:
        case Drag::value:
        case Drag::solo:
            break;

        case Drag::triggerLo:
        case Drag::triggerHi:
            setPlain (triggerBand, drag == Drag::triggerLo ? "dyn_lo" : "dyn_hi", hzForX (e.position.x));
            break;

        case Drag::bands:
            applyBandDrag (e);
            break;

        case Drag::orbit:
        {
            const float fine = e.mods.isShiftDown() ? 0.2f : 1.0f;
            setCameraAngles (orbitYaw0 + (e.position.x - dragStart.x) * 0.006f * fine,
                             orbitPitch0 + (e.position.y - dragStart.y) * 0.006f * fine);
            break;
        }

        case Drag::depth:
            setPlain (dragStarts.front().band, "depth",
                      100.0f * depthAlongFloor (bands[(size_t) dragStarts.front().band].freq, e.position + depthGrabOffset));
            break;

        case Drag::dynRange:
        {
            const auto& s = dragStarts.front();
            const float pxPerDb = eqArea.getHeight() * 0.5f / displayRange;
            const float fine = e.mods.isShiftDown() ? 0.15f : 1.0f;
            const float scale = std::abs (gainScale) > 0.01f ? gainScale : 1.0f;
            dragDbAccum -= (e.position.y - lastDragY) * fine / pxPerDb / scale;
            lastDragY = e.position.y;
            setPlain (s.band, "dyn_range", juce::jlimit (-30.0f, 30.0f, s.range + dragDbAccum));
            break;
        }

        case Drag::rubber:
        {
            rubber = juce::Rectangle<float> (dragStart, e.position);
            auto sel = rubberBase;
            for (int b = 0; b < kMaxBands; ++b)
                if (bands[(size_t) b].used && rubber.contains (dotPosition (b))) sel.addIfNotAlreadyThere (b);
            if (sel != selection) { selection = sel; primary = sel.isEmpty() ? -1 : sel.getLast(); notifySelection(); }
            repaint();
            break;
        }

        case Drag::sketch:
        {
            auto pt = e.position.withX (juce::jlimit (plot.getX(), plot.getRight(), e.position.x));
            // keep the stroke a function of x: drop points the pen moved back over
            while (! sketchPoints.empty() && sketchPoints.back().x >= pt.x && sketchPoints.size() > 1
                   && (pt.x - sketchPoints.front().x) * (sketchPoints.back().x - sketchPoints.front().x) >= 0.0f
                   && std::abs (pt.x - sketchPoints.front().x) < std::abs (sketchPoints.back().x - sketchPoints.front().x))
                sketchPoints.pop_back();
            if (sketchPoints.empty() || std::abs (pt.x - sketchPoints.back().x) >= 1.0f) sketchPoints.push_back (pt);
            // progressive: refit the bands a few times a second while drawing
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (now - lastSketchFit > 110.0 && sketchPoints.size() >= 6) { lastSketchFit = now; refitSketch(); }
            repaint();
            break;
        }

        case Drag::none:
            break;
    }
}

void EqDisplay::mouseUp (const juce::MouseEvent&)
{
    if (drag == Drag::sketch) finishSketch();
    if (drag == Drag::solo) { proc.soloBand = -1; curveLayer = {}; }
    valuePart = Part::none;
    endGestures();
    if (pendingAltClick >= 0)
    {
        const int b = pendingAltClick;
        pendingAltClick = -1;
        proc.undoManager.beginNewTransaction ("Bypass band");
        proc.setBandValue (b, "bypass", bands[(size_t) b].bypass ? 0.0f : 1.0f);
    }
    drag = Drag::none;
    rubber = {};
    dragStarts.clear();
    repaint();
}

void EqDisplay::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (sketchMode) return;
    if (gizmoAt (e.position)) { setCameraAngles (defaultYaw, defaultPitch); return; }
    if (const auto part = partAt (e.position); part == Part::freq || part == Part::gain || part == Part::q)
    {
        showEntry (readoutBand, part == Part::freq ? 0 : (part == Part::gain ? 1 : 2));
        return;
    }
    const int b = bandAt (e.position);
    if (b >= 0)
    {
        showEntry (b, 0);
        return;
    }
    if (! plot.contains (e.position) || dynHandleAt (e.position) >= 0 || depthHandleAt (e.position) >= 0 || viewButtonAt (e.position) >= 0
        || gizmoAt (e.position)) return;
    if (! flatView())
    {
        // 3D: a band at the front plane where clicked; Top: at the clicked frequency and depth, 0 dB
        float depth = 0.0f;
        const auto pt = frontPointFor (e.position, depth);
        const int nb = createBandAt (pt, false, e.mods.isAltDown(), e.mods.isAltDown() && e.mods.isShiftDown());
        if (nb < 0) return;
        if (depth > 0.0f) proc.setBandValue (nb, "depth", depth * 100.0f);
        readParameters();
        select (nb);
        return;
    }
    const int nb = createBandAt (e.position, false, e.mods.isAltDown(), e.mods.isAltDown() && e.mods.isShiftDown());
    if (nb >= 0) select (nb);
}

void EqDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (const auto part = partAt (e.position); part == Part::freq || part == Part::gain || part == Part::q)
    {
        const float d = (std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.0f : 1.0f) * (e.mods.isShiftDown() ? 0.25f : 1.0f);
        const auto& p = bands[(size_t) readoutBand];
        proc.undoManager.beginNewTransaction ("Wheel");
        if (part == Part::freq)      proc.setBandValue (readoutBand, "freq", juce::jlimit (minHz, maxHz, p.freq * std::exp2 (d * 0.5f)));
        else if (part == Part::gain) proc.setBandValue (readoutBand, "gain", juce::jlimit (-30.0f, 30.0f, p.gain + d * 3.0f));
        else                         proc.setBandValue (readoutBand, "q", juce::jlimit (0.025f, 40.0f, p.q * std::exp2 (d)));
        readParameters();
        return;
    }
    int b = bandAt (e.position);
    juce::Array<int> targets;
    if (b >= 0 && ! selection.contains (b)) targets.add (b);
    else if (b >= 0 || ! compact) targets = selection;
    if (targets.isEmpty())
    {
        // nothing to adjust here: let the parent (e.g. the instance list) scroll
        juce::Component::mouseWheelMove (e, w);
        return;
    }

    const float dy = (std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : -w.deltaX) * (w.isReversed ? -1.0f : 1.0f);
    const float fine = e.mods.isShiftDown() ? 0.25f : 1.0f;
    proc.undoManager.beginNewTransaction ("Wheel");

    for (int t : targets)
    {
        const auto& p = bands[(size_t) t];
        if (e.mods.isAltDown() && e.mods.isCommandDown() && st::dsp::eq::hasGain (p.shape))
        {
            proc.setBandValue (t, "gain", juce::jlimit (-30.0f, 30.0f, p.gain + dy * 3.0f * fine));
            proc.setBandValue (t, "dyn_range", juce::jlimit (-30.0f, 30.0f, p.dynRange - dy * 3.0f * fine));
        }
        else if (e.mods.isAltDown() && st::dsp::eq::hasGain (p.shape))
            proc.setBandValue (t, "dyn_range", juce::jlimit (-30.0f, 30.0f, p.dynRange + dy * 4.0f * fine));
        else if (e.mods.isCommandDown() && st::dsp::eq::hasGain (p.shape))
            proc.setBandValue (t, "gain", juce::jlimit (-30.0f, 30.0f, p.gain + dy * 4.0f * fine));
        else if ((p.shape == Shape::lowCut || p.shape == Shape::highCut) && ! e.mods.isShiftDown())
        {
            const int step = dy > 0.0f ? 1 : -1;
            proc.setBandValue (t, "slope", (float) juce::jlimit (0, 9, p.slope + step));
        }
        else
            proc.setBandValue (t, "q", juce::jlimit (0.025f, 40.0f, p.q * std::exp2 (dy * 1.5f * fine)));
    }
}

bool EqDisplay::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) { removeSelected(); return true; }
    if (key == juce::KeyPress::escapeKey)
    {
        if (sketchMode) { setSketchMode (false); return true; }
        if (! selection.isEmpty()) { select (-1); return true; }
        return false;
    }
    if (mods.isCommandDown() && juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) key.getKeyCode()) == 'A')
    {
        juce::Array<int> all;
        for (int b = 0; b < kMaxBands; ++b) if (bands[(size_t) b].used) all.add (b);
        setSelection (all, all.isEmpty() ? -1 : all.getFirst());
        return true;
    }
    if (mods.isCommandDown() && juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) key.getKeyCode()) == 'C' && ! selection.isEmpty())
    {
        juce::SystemClipboard::copyTextToClipboard (proc.bandsToText (selection));
        return true;
    }
    if (mods.isCommandDown() && juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) key.getKeyCode()) == 'V')
    {
        const auto added = proc.pasteBands (juce::SystemClipboard::getTextFromClipboard());
        if (! added.isEmpty()) { readParameters(); setSelection (added, added.getLast()); }
        return true;
    }
    return false;
}

//==============================================================================
void EqDisplay::showBandMenu (int b)
{
    const auto p = bands[(size_t) b];
    const auto targets = selection.contains (b) ? selection : juce::Array<int> { b };
    juce::PopupMenu shape, slope, place, menu;

    auto setAll = [this, targets] (const char* suffix, float v, const char* name)
    {
        proc.undoManager.beginNewTransaction (name);
        for (int t : targets) proc.setBandValue (t, suffix, v);
    };

    for (int i = 0; i < shapeNames.size(); ++i)
        shape.addItem (juce::PopupMenu::Item (shapeNames[i]).setTicked ((int) p.shape == i).setImage (shapeMenuIcon ((Shape) i))
                           .setAction ([setAll, i] { setAll ("shape", (float) i, "Shape"); }));
    for (int i = 0; i < slopeNames.size(); ++i)
        slope.addItem (juce::PopupMenu::Item (slopeNames[i]).setTicked (p.slope == i).setAction ([setAll, i] { setAll ("slope", (float) i, "Slope"); }));
    for (int i = 0; i < placementNames.size(); ++i)
        place.addItem (juce::PopupMenu::Item (placementNames[i]).setTicked ((int) p.place == i).setImage (placementMenuIcon ((Placement) i)).setAction ([setAll, i] { setAll ("place", (float) i, "Stereo placement"); }));

    menu.addSectionHeader ("Band " + juce::String (b + 1) + (targets.size() > 1 ? " (+" + juce::String (targets.size() - 1) + " selected)" : juce::String()));
    menu.addSubMenu ("Shape", shape);
    if (st::dsp::eq::hasSlope (p.shape)) menu.addSubMenu ("Slope", slope);
    menu.addSubMenu ("Stereo Placement", place);
    menu.addSeparator();
    menu.addItem (juce::PopupMenu::Item ("Bypass").setTicked (p.bypass).setAction ([setAll, p] { setAll ("bypass", p.bypass ? 0.0f : 1.0f, "Bypass"); }));
    menu.addItem (juce::PopupMenu::Item ("Solo").setTicked (proc.soloBand.load() == b).setAction ([this, b] { proc.soloBand = proc.soloBand.load() == b ? -1 : b; }));
    if (st::dsp::eq::hasGain (p.shape))
    {
        if (std::abs (p.dynRange) > 0.05f)
            menu.addItem (juce::PopupMenu::Item ("Clear Dynamics").setAction ([setAll] { setAll ("dyn_range", 0.0f, "Clear dynamics"); }));
        else
            menu.addItem (juce::PopupMenu::Item ("Make Dynamic").setAction ([setAll] { setAll ("dyn_range", -6.0f, "Make dynamic"); }));
        menu.addItem (juce::PopupMenu::Item ("Spectral Dynamics").setTicked (p.spectral && std::abs (p.dynRange) > 0.05f).setAction ([this, targets, p]
        {
            const bool on = ! (p.spectral && std::abs (p.dynRange) > 0.05f);
            proc.undoManager.beginNewTransaction (on ? "Make spectral" : "Spectral off");
            for (int t : targets)
            {
                if (on && std::abs (bands[(size_t) t].dynRange) <= 0.05f) proc.setBandValue (t, "dyn_range", -6.0f);
                proc.setBandValue (t, "spectral", on ? 1.0f : 0.0f);
            }
        }));
        menu.addItem (juce::PopupMenu::Item ("Invert Gain").setAction ([this, targets]
        {
            proc.undoManager.beginNewTransaction ("Invert gain");
            for (int t : targets) proc.setBandValue (t, "gain", -bands[(size_t) t].gain);
        }));
    }
    if (p.shape == Shape::bell)
        menu.addItem (juce::PopupMenu::Item ("Gain-Q Interaction").setTicked (p.gainQ).setAction ([setAll, p] { setAll ("gq", p.gainQ ? 0.0f : 1.0f, "Gain-Q"); }));
    if (p.place == Placement::stereo)
    {
        menu.addItem (juce::PopupMenu::Item ("Split into Left / Right").setAction ([this] { splitSelected (false); }));
        menu.addItem (juce::PopupMenu::Item ("Split into Mid / Side").setAction ([this] { splitSelected (true); }));
    }
    menu.addSeparator();
    menu.addItem (juce::PopupMenu::Item ("Copy").setAction ([this, targets] { juce::SystemClipboard::copyTextToClipboard (proc.bandsToText (targets)); }));
    menu.addItem (juce::PopupMenu::Item (targets.size() > 1 ? "Delete Bands" : "Delete Band").setAction ([this] { removeSelected(); }));
    st::showMenu (menu, nullptr, {}, true);
}

void EqDisplay::showBackgroundMenu()
{
    juce::PopupMenu menu, range;
    const auto clip = juce::SystemClipboard::getTextFromClipboard();
    juce::Array<int> all;
    for (int b = 0; b < kMaxBands; ++b) if (bands[(size_t) b].used) all.add (b);

    menu.addItem (juce::PopupMenu::Item ("Paste Bands").setEnabled (clip.startsWith ("<STEQBands")).setAction ([this, clip]
    {
        const auto added = proc.pasteBands (clip);
        if (! added.isEmpty()) { readParameters(); setSelection (added, added.getLast()); }
    }));
    menu.addItem (juce::PopupMenu::Item ("Copy All Bands").setEnabled (! all.isEmpty()).setAction ([this, all] { juce::SystemClipboard::copyTextToClipboard (proc.bandsToText (all)); }));
    menu.addItem (juce::PopupMenu::Item ("Select All Bands").setEnabled (! all.isEmpty()).setAction ([this, all] { setSelection (all, all.isEmpty() ? -1 : all.getFirst()); }));
    menu.addItem (juce::PopupMenu::Item ("Delete All Bands").setEnabled (! all.isEmpty()).setAction ([this, all] { selection = all; removeSelected(); }));
    menu.addSeparator();
    menu.addItem (juce::PopupMenu::Item ("EQ Sketch").setAction ([this] { setSketchMode (true); }));
    menu.addItem (juce::PopupMenu::Item ("EQ Match...").setAction ([this] { if (onMatchRequested) onMatchRequested(); }));
    range.addItem (juce::PopupMenu::Item ("Auto").setTicked (autoRange).setAction ([this] { setAutoRange(); }));
    range.addSeparator();
    for (int i = 0; i < 4; ++i)
        range.addItem (juce::PopupMenu::Item (juce::String ((int) displayRanges[i]) + " dB").setTicked (! autoRange && rangeIndex == i).setAction ([this, i] { setDisplayRange (i); }));
    menu.addSubMenu ("Display Range", range);
    st::showMenu (menu, nullptr, {}, true);
}

//==============================================================================
namespace
{
    /** Value entry box: Return commits, Tab commits and moves to the next field, Esc cancels. */
    struct EntryEditor : juce::TextEditor
    {
        std::function<void()> onTab;
        bool keyPressed (const juce::KeyPress& k) override
        {
            if (k == juce::KeyPress::tabKey) { if (onTab) onTab(); return true; }
            return juce::TextEditor::keyPressed (k);
        }
    };
}

void EqDisplay::showEntry (int b, int field)
{
    const auto& p = bands[(size_t) b];
    if (field == 1 && ! st::dsp::eq::hasGain (p.shape)) field = 2;
    if (field == 2 && ! st::dsp::eq::hasQ (p.shape)) field = 0;

    auto ed = std::make_unique<EntryEditor>();
    ed->onTab = [this] { commitEntry (true); };
    entry = std::move (ed);
    entryBand = b;
    entryField = field;
    const auto& t = st::Theme::get();
    entry->setFont (st::fonts::semibold (12.0f));
    entry->setJustification (juce::Justification::centred);
    entry->setColour (juce::TextEditor::backgroundColourId, t.bg1);
    entry->setColour (juce::TextEditor::textColourId, t.text);
    entry->setColour (juce::TextEditor::outlineColourId, bandColour (b).withAlpha (0.7f));
    entry->setColour (juce::TextEditor::focusedOutlineColourId, bandColour (b));
    entry->setText (field == 0 ? formatFrequency (p.freq) : field == 1 ? juce::String (p.gain, 1) : juce::String (p.q, 2), false);
    entry->selectAll();
    const auto c = dotPosition (b);
    entry->setBounds (juce::Rectangle<int> (96, 22).withCentre ({ juce::roundToInt (c.x), juce::roundToInt (c.y) - 52 }));
    entry->onReturnKey = [this] { commitEntry (false); };
    entry->onEscapeKey = [this] { juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<EqDisplay> (this)] { if (safe != nullptr) safe->entry.reset(); }); };
    entry->onFocusLost = [this] { commitEntry (false); };
    addAndMakeVisible (*entry);
    entry->grabKeyboardFocus();
}

void EqDisplay::commitEntry (bool moveNext)
{
    if (entry == nullptr) return;
    const auto text = entry->getText();
    const int b = entryBand, field = entryField;
    // retire the editor now (so nothing commits it twice) but delete it later: we may be inside its callback
    entry->onFocusLost = nullptr;
    entry->setVisible (false);
    retiredEntry = std::move (entry);
    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<EqDisplay> (this)] { if (safe != nullptr) safe->retiredEntry.reset(); });

    proc.undoManager.beginNewTransaction ("Type value");
    if (field == 0)
    {
        const float hz = parseFrequency (text);
        if (hz > 0.0f) proc.setBandValue (b, "freq", juce::jlimit (10.0f, 30000.0f, hz));
    }
    else if (field == 1)
        proc.setBandValue (b, "gain", juce::jlimit (-30.0f, 30.0f, st::params::parseDb (text)));   // also "2x", "50%"
    else
    {
        const float q = text.retainCharacters (".0123456789").getFloatValue();
        if (q > 0.0f) proc.setBandValue (b, "q", juce::jlimit (0.025f, 40.0f, q));
    }
    if (moveNext)
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<EqDisplay> (this), b, field]
        {
            if (safe != nullptr) { safe->readParameters(); safe->showEntry (b, (field + 1) % 3); }
        });
}

void EqDisplay::refitSketch()
{
    if (sketchPoints.size() < 4) return;

    // Target = drawn curve minus the curve as it was before this stroke (bands are added on top).
    auto pts = sketchPoints;
    std::sort (pts.begin(), pts.end(), [] (auto& a, auto& b) { return a.x < b.x; });
    auto baseAt = [this] (float x)
    {
        const auto it = std::lower_bound (xs.begin(), xs.end(), x);
        const int i = juce::jlimit (1, kPoints - 1, (int) (it - xs.begin()));
        const float t = juce::jlimit (0.0f, 1.0f, (x - xs[(size_t) i - 1]) / juce::jmax (1.0e-3f, xs[(size_t) i] - xs[(size_t) i - 1]));
        return sketchBase[(size_t) i - 1] + t * (sketchBase[(size_t) i] - sketchBase[(size_t) i - 1]);
    };
    std::vector<std::pair<float, float>> target;
    const float scale = std::abs (gainScale) > 0.01f ? gainScale : 1.0f;
    for (auto& p : pts)
    {
        const float hz = hzForX (p.x);
        if (target.empty() || hz > target.back().first * 1.001f)
            target.push_back ({ hz, (dbForY (p.y) - baseAt (p.x)) / scale });
    }

    // replace this stroke's bands (one undo transaction for the whole stroke)
    for (int b : sketchBands) proc.removeBand (b);
    sketchBands.clear();
    int free = 0;
    for (int b = 0; b < kMaxBands; ++b) free += proc.getBand (b).used ? 0 : 1;
    for (auto& f : sketch::fit (target, juce::jmin (8, free)))
    {
        const int b = proc.addBand (f.shape, f.freq, f.gain, f.q, f.slope, Placement::stereo, false);
        if (b >= 0) sketchBands.add (b);
    }
    readParameters();
}

void EqDisplay::finishSketch()
{
    if (sketchPoints.size() >= 4) refitSketch();
    sketchPoints.clear();
    const auto added = sketchBands;
    sketchBands.clear();
    setSketchMode (false);
    if (! added.isEmpty()) setSelection (added, added.getLast());
}

} // namespace steq
