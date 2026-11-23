// Use-case videos of the real editor (website): synthesized music runs through the processor while a scripted
// pointer works the UI through the components' own mouse handlers; frames (with a drawn cursor) go to ffmpeg.
//   StEqPluginTests --video <dir> [scene ...]
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ui/BandIsland.h"
#include <cstdio>

using namespace steq;

namespace
{
constexpr double sampleRate = 48000.0;
constexpr int fps = 30, blockSize = 1600;   // one audio block per video frame
constexpr float videoScale = 1.5f;          // 1560 x 930

//==============================================================================
/** Minimal RBJ biquad. */
struct Biquad
{
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void set (int type, float f, float q)   // 0 low pass, 1 band pass (0 dB peak), 2 high pass
    {
        const float w = juce::MathConstants<float>::twoPi * juce::jlimit (10.0f, (float) sampleRate * 0.45f, f) / (float) sampleRate;
        const float c = std::cos (w), al = std::sin (w) / (2.0f * q), a0 = 1.0f + al;
        if (type == 0) { b0 = (1 - c) * 0.5f; b1 = 1 - c; b2 = b0; }
        else if (type == 1) { b0 = al; b1 = 0; b2 = -al; }
        else { b0 = (1 + c) * 0.5f; b1 = -(1 + c); b2 = b0; }
        b0 /= a0; b1 /= a0; b2 /= a0; a1 = -2.0f * c / a0; a2 = (1.0f - al) / a0;
    }
    float operator() (float x) { const float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

/** Band-limited sawtooth (polyBLEP). */
struct Saw
{
    double phase = 0.0;
    float operator() (double hz)
    {
        const double dt = hz / sampleRate;
        phase += dt; if (phase >= 1.0) phase -= 1.0;
        double v = 2.0 * phase - 1.0;
        auto blep = [dt] (double t) { if (t < dt) { t /= dt; return t + t - t * t - 1.0; } if (t > 1.0 - dt) { t = (t - 1.0) / dt; return t * t + t + t + 1.0; } return 0.0; };
        return (float) (v - blep (phase));
    }
};

/** A small Schroeder room (four damped combs, two all-passes), different taps per side. */
struct Room
{
    struct Comb { std::vector<float> d; size_t i = 0; float lp = 0; float operator() (float x) { const float y = d[i]; lp += 0.35f * (y - lp); d[i] = x + lp * 0.8f; i = (i + 1) % d.size(); return y; } };
    struct AllPass { std::vector<float> d; size_t i = 0; float operator() (float x) { const float y = d[i]; d[i] = x + y * 0.5f; i = (i + 1) % d.size(); return y - x; } };
    Comb comb[2][4];
    AllPass ap[2][2];
    Room()
    {
        const int sizes[4] = { 1557, 1617, 1491, 1422 }, aps[2] = { 556, 441 };
        for (int s = 0; s < 2; ++s)
        {
            for (int k = 0; k < 4; ++k) comb[s][k].d.assign ((size_t) ((sizes[k] + s * 23) * 0.75), 0.0f);
            for (int k = 0; k < 2; ++k) ap[s][k].d.assign ((size_t) (aps[k] + s * 17), 0.0f);
        }
    }
    float operator() (int side, float x)
    {
        float y = 0.0f;
        for (auto& cb : comb[side]) y += cb (x * 0.25f);
        for (auto& a : ap[side]) y = a (y);
        return y;
    }
};

float midiHz (float m) { return 440.0f * std::pow (2.0f, (m - 69.0f) / 12.0f); }

/** A small band at 96 BPM (Am F C G): drums, bass, pad and a formant "vocal" with sibilance, mixable per scene. */
struct Music
{
    float vocal = 0, sib = 0, rumble = 0, box = 0, drums = 0, bass = 0, pad = 0;
    juce::Random rng { 42 };
    int64_t n = 0;

    static constexpr double beat = 60.0 / 96.0, bar = 4.0 * beat;
    Saw voice, bassOsc, padOsc[3][2];
    Room room;
    Biquad formant[3], sibBp, breathBp, boxBp, rumbleLp, rumbleLp2, bassLp, padLpL, padLpR, snareBp, hatHp, hatHp2, kickClick;
    float vocalSend = 0.0f, padSendL = 0.0f, padSendR = 0.0f;
    float voiceHz = 220.0f, glottal = 0.0f, kickPhase = 0.0f, snareTone = 0.0f, subPhase = 0.0f, humPhase = 0.0f;

    Music()
    {
        sibBp.set (1, 7200.0f, 1.4f); breathBp.set (1, 2200.0f, 0.8f); boxBp.set (1, 330.0f, 2.2f);
        rumbleLp.set (0, 55.0f, 0.9f); rumbleLp2.set (0, 55.0f, 0.9f); bassLp.set (0, 420.0f, 1.1f);
        padLpL.set (0, 1700.0f, 0.7f); padLpR.set (0, 1700.0f, 0.7f); snareBp.set (1, 2600.0f, 0.6f);
        hatHp.set (2, 7000.0f, 0.7f); hatHp2.set (2, 7000.0f, 0.7f); kickClick.set (1, 3500.0f, 1.0f);
    }

    void render (juce::AudioBuffer<float>& buf)
    {
        static const int chords[4][3] = { { 57, 60, 64 }, { 53, 57, 60 }, { 55, 60, 64 }, { 55, 59, 62 } };   // Am F C G
        static const int roots[4] = { 33, 29, 36, 31 };
        // melody: one note per beat (0 = rest), and the vowel of each
        static const int melody[4][4] = { { 76, 76, 74, 72 }, { 72, 69, 72, 74 }, { 76, 79, 76, 74 }, { 74, 71, 0, 0 } };
        static const float vowels[4][3] = { { 800, 1150, 2900 }, { 400, 1700, 2600 }, { 450, 800, 2830 }, { 330, 2100, 2900 } };
        static const float vowelGain[3] = { 1.0f, 0.55f, 0.3f };

        for (int i = 0; i < buf.getNumSamples(); ++i, ++n)
        {
            const double t = (double) n / sampleRate;
            const int barIdx = (int) (t / bar) % 4;
            const double inBar = std::fmod (t, bar);
            const int beatIdx = (int) (inBar / beat);
            const double inBeat = std::fmod (inBar, beat);
            const int eighth = (int) (inBar / (beat * 0.5));
            const double inEighth = std::fmod (inBar, beat * 0.5);
            float L = 0.0f, R = 0.0f;

            // --- drums
            if (drums > 0.0f)
            {
                double kt = -1.0;
                if (eighth == 0 || eighth == 4 || (eighth == 5 && barIdx % 2 == 1)) kt = inEighth;
                float d = 0.0f;
                if (kt >= 0.0)
                {
                    const float f = 46.0f + 115.0f * (float) std::exp (-kt * 32.0);
                    kickPhase += f / (float) sampleRate;
                    d += std::sin (juce::MathConstants<float>::twoPi * kickPhase) * (float) std::exp (-kt * 6.5) * 0.95f;
                    d += kickClick (rng.nextFloat() * 2.0f - 1.0f) * (float) std::exp (-kt * 250.0) * 0.6f;
                }
                if (beatIdx == 1 || beatIdx == 3)
                {
                    snareTone += 185.0f / (float) sampleRate;
                    d += std::sin (juce::MathConstants<float>::twoPi * snareTone) * (float) std::exp (-inBeat * 28.0) * 0.35f;
                    d += snareBp (rng.nextFloat() * 2.0f - 1.0f) * (float) std::exp (-inBeat * 15.0) * 1.1f;
                }
                const bool open = eighth == 7;
                const float hatEnv = (float) std::exp (-inEighth * (open ? 9.0 : 55.0)) * (eighth % 2 ? 0.32f : 0.2f);
                const float hat = hatHp2 (hatHp (rng.nextFloat() * 2.0f - 1.0f)) * hatEnv;
                L += (d + hat * 0.8f) * drums;
                R += (d + hat * 1.2f) * drums;
            }

            // --- bass: eighths on the root
            if (bass > 0.0f)
            {
                static const bool pattern[8] = { true, false, true, true, false, true, true, false };
                const float env = pattern[eighth] ? (float) (std::exp (-inEighth * 3.0) * juce::jmin (1.0, inEighth * 300.0)) : 0.0f;
                const float hz = midiHz ((float) roots[barIdx]);
                subPhase += hz / (float) sampleRate;
                const float b = bassLp (bassOsc (hz)) * 0.55f + std::sin (juce::MathConstants<float>::twoPi * subPhase) * 0.5f;
                L += b * env * bass; R += b * env * bass;
            }

            // --- pad: detuned saws, wide, slow swell per bar
            if (pad > 0.0f)
            {
                const float env = (float) (juce::jmin (1.0, inBar / 0.35) * juce::jmin (1.0, (bar - inBar) / 0.2));
                float pl = 0.0f, pr = 0.0f;
                for (int k = 0; k < 3; ++k)
                {
                    const float hz = midiHz ((float) chords[barIdx][k]);
                    pl += padOsc[k][0] (hz * 0.996);
                    pr += padOsc[k][1] (hz * 1.004);
                }
                padSendL = padLpL (pl) * 0.16f * env * pad;
                padSendR = padLpR (pr) * 0.16f * env * pad;
                L += padSendL; R += padSendR;
            }

            // --- vocal: glottal saw through three formants, vibrato, gliding pitch; sibilance on note onsets
            if (vocal > 0.0f || rumble > 0.0f)
            {
                const int note = melody[barIdx][beatIdx];
                float v = 0.0f;
                if (vocal > 0.0f)
                {
                    if (note > 0)
                    {
                        const float target = midiHz ((float) note - 12.0f);
                        voiceHz += (target - voiceHz) * 0.0025f;
                        const float vib = 1.0f + 0.006f * std::sin (juce::MathConstants<float>::twoPi * 5.4f * (float) t) * (float) juce::jmin (1.0, inBeat * 3.0);
                        const float env = (float) (juce::jmin (1.0, inBeat * 25.0) * juce::jmin (1.0, (beat - inBeat) * 12.0 + 0.35));
                        glottal += 0.25f * (voice (voiceHz * vib) - glottal);   // soften the source
                        const auto& fm = vowels[(barIdx * 4 + beatIdx) % 4];
                        for (int k = 0; k < 3; ++k)
                        {
                            formant[k].set (1, fm[k], k == 0 ? 6.0f : 9.0f);
                            v += formant[k] (glottal) * vowelGain[k];
                        }
                        v = v * 2.4f * env + breathBp (rng.nextFloat() * 2.0f - 1.0f) * 0.03f * env;
                    }
                    // "s" / "t" at the start of every other note, and a long "s" at the end of the phrase
                    const bool s = (note > 0 && beatIdx % 2 == 0 && inBeat < 0.14) || (barIdx == 3 && beatIdx == 2 && inBeat < 0.32);
                    if (s)
                    {
                        const double st = inBeat;
                        const float senv = (float) (juce::jmin (1.0, st * 60.0) * std::exp (-st * (barIdx == 3 ? 5.0 : 14.0)));
                        v += sibBp (rng.nextFloat() * 2.0f - 1.0f) * senv * 1.6f * sib;
                    }
                    v += boxBp (v) * box * 1.8f;
                    v *= vocal;
                }
                if (rumble > 0.0f)
                {
                    humPhase += 50.0f / (float) sampleRate;
                    v += (rumbleLp2 (rumbleLp (rng.nextFloat() * 2.0f - 1.0f)) * 3.5f + std::sin (juce::MathConstants<float>::twoPi * humPhase) * 0.05f) * rumble;
                }
                L += v; R += v;
                vocalSend = v;
            }
            // room on the vocal and pad
            L += room (0, vocalSend * 0.55f + padSendL * 0.4f) * 0.9f;
            R += room (1, vocalSend * 0.55f + padSendR * 0.4f) * 0.9f;
            vocalSend = padSendL = padSendR = 0.0f;

            buf.setSample (0, i, std::tanh (L * 0.8f) * 0.6f);
            buf.setSample (1, i, std::tanh (R * 0.8f) * 0.6f);
        }
    }
};

//==============================================================================
/** Sends mouse events straight to the components under a virtual pointer (in editor coordinates). */
struct Pointer
{
    juce::Component& root;
    juce::Point<float> pos { 700.0f, 300.0f }, downPos;
    juce::Component::SafePointer<juce::Component> hover, pressed;
    double pressFlash = -10.0;   // video time of the last press (cursor ripple)

    /** The deepest visible component under p that takes clicks (front-most first). */
    static juce::Component* deepest (juce::Component& c, juce::Point<float> local)
    {
        for (int i = c.getNumChildComponents(); --i >= 0;)
        {
            auto* child = c.getChildComponent (i);
            if (! child->isVisible()) continue;
            const auto q = child->getLocalPoint (&c, local);
            if (! child->getLocalBounds().toFloat().contains (q)) continue;
            if (auto* hit = deepest (*child, q)) return hit;
        }
        bool self = true, kids = true;
        c.getInterceptsMouseClicks (self, kids);
        return self && c.hitTest (juce::roundToInt (local.x), juce::roundToInt (local.y)) ? &c : nullptr;
    }
    juce::Component* at (juce::Point<float> p)
    {
        auto* c = deepest (root, p);
        return c != nullptr ? c : &root;
    }
    juce::MouseEvent ev (juce::Component* c, juce::ModifierKeys mods, int clicks = 1, bool dragged = false) const
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), c->getLocalPoint (&root, pos), mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                 c, c, now, c->getLocalPoint (&root, downPos), now, clicks, dragged);
    }
    void moveTo (juce::Point<float> p)
    {
        pos = p;
        if (pressed != nullptr) { pressed->mouseDrag (ev (pressed, juce::ModifierKeys::leftButtonModifier, 1, true)); return; }
        auto* c = at (p);
        if (c != hover.getComponent())
        {
            if (hover != nullptr) hover->mouseExit (ev (hover, {}));
            hover = c;
            c->mouseEnter (ev (c, {}));
        }
        c->mouseMove (ev (c, {}));
    }
    void down (int clicks = 1)
    {
        downPos = pos;
        pressed = at (pos);
        pressed->mouseDown (ev (pressed, juce::ModifierKeys::leftButtonModifier, clicks));
    }
    void up()
    {
        if (pressed != nullptr) pressed->mouseUp (ev (pressed, {}, 1, pos != downPos));
        pressed = nullptr;
    }
    void doubleClick()
    {
        down(); up(); down (2);
        if (pressed != nullptr) pressed->mouseDoubleClick (ev (pressed, {}, 2));
        up();
    }
    void wheel (float dy)
    {
        auto* c = at (pos);
        juce::MouseWheelDetails w { 0.0f, dy, false, false, false };
        c->mouseWheelMove (ev (c, {}), w);
    }
};

/** A timeline of pointer moves, presses and waits. Targets are functions: they follow things that move. */
struct Script
{
    struct Step { double dur; std::function<void()> start; std::function<void (float)> tick; };
    std::vector<Step> steps;
    size_t index = 0;
    double stepTime = 0.0;
    bool started = false;

    bool done() const { return index >= steps.size(); }
    void advance (double dt)
    {
        stepTime += dt;
        while (! done())
        {
            auto& s = steps[index];
            if (! started) { if (s.start) s.start(); started = true; }
            const float u = s.dur > 0.0 ? (float) juce::jlimit (0.0, 1.0, stepTime / s.dur) : 1.0f;
            if (s.tick) s.tick (u);
            if (stepTime < s.dur) return;
            stepTime -= s.dur;
            ++index;
            started = false;
        }
    }
};

float ease (float u) { return u * u * (3.0f - 2.0f * u); }

/** Builds the script of a scene against a live editor. */
struct Director
{
    StEqEditor& ed;
    Pointer& ptr;
    Script& s;

    EqDisplay& display() { return ed.getDisplay(); }
    juce::Point<float> fromDisplay (juce::Point<float> p) { return ed.getLocalPoint (&display(), p); }
    juce::Point<float> at (float hz, float db) { return fromDisplay ({ display().xForHz (hz), display().yForDb (db) }); }
    juce::Point<float> onCurve (float hz) { return fromDisplay ({ display().xForHz (hz), display().getCurveY (hz) }); }
    juce::Point<float> dot() { return fromDisplay (display().dotPosition (display().getPrimary())); }

    template <typename Pred> juce::Component* findInPanel (Pred pred)
    {
        auto* panel = ed.getBandPanel();
        if (panel == nullptr) return nullptr;
        for (auto* c : panel->getChildren())
            if (c->isVisible() && pred (c)) return c;
        return nullptr;
    }
    st::Knob* knob (const juce::String& suffix)
    {
        return dynamic_cast<st::Knob*> (findInPanel ([&] (juce::Component* c)
        {
            auto* k = dynamic_cast<st::Knob*> (c);
            return k != nullptr && k->getParameter().paramID.endsWith ("_" + suffix);
        }));
    }
    juce::Point<float> centreOf (juce::Component* c)
    {
        return c == nullptr ? ptr.pos : ed.getLocalPoint (c->getParentComponent(), c->getBounds().toFloat().getCentre());
    }
    juce::Point<float> knobCentre (const char* suffix) { return centreOf (knob (suffix)); }
    juce::Point<float> gainRim()
    {
        auto* k = knob ("gain");
        if (k == nullptr) return ptr.pos;
        const float R = (float) k->getWidth() * 0.5f;
        const float outerR = R - juce::jmax (2.0f, R * 0.07f) - 1.5f, r = outerR * 0.87f, a = -0.75f;
        return knobCentre ("gain") + juce::Point<float> (std::sin (a) * r, -std::cos (a) * r);
    }
    juce::Point<float> shapeTab() { return centreOf (findInPanel ([] (juce::Component* c) { return dynamic_cast<ShapeButton*> (c) != nullptr; })); }
    juce::Point<float> islandButton (const juce::String& tip)
    {
        return centreOf (findInPanel ([&] (juce::Component* c) { auto* b = dynamic_cast<juce::Button*> (c); return b != nullptr && b->getTooltip() == tip; }));
    }

    // --- steps
    void wait (double sec) { s.steps.push_back ({ sec, {}, {} }); }
    void call (std::function<void()> f) { s.steps.push_back ({ 0.0, std::move (f), {} }); }
    void move (std::function<juce::Point<float>()> target, double sec)
    {
        auto from = std::make_shared<juce::Point<float>>();
        s.steps.push_back ({ sec, [this, from] { *from = ptr.pos; }, [this, from, target] (float u)
        {
            const auto to = target();
            const auto a = *from + (to - *from) * ease (u);
            // a slight arc, like a hand
            const float lift = std::sin (u * juce::MathConstants<float>::pi) * juce::jmin (18.0f, from->getDistanceFrom (to) * 0.08f);
            ptr.moveTo (a.translated (0.0f, -lift));
        } });
    }
    /** Drag (press, move, release) from where the pointer is to a target, or by an offset. */
    void drag (std::function<juce::Point<float>()> target, double sec)
    {
        auto from = std::make_shared<juce::Point<float>>();
        s.steps.push_back ({ 0.12, [this, from] { *from = ptr.pos; ptr.pressFlash = now; ptr.down(); }, {} });
        s.steps.push_back ({ sec, [this, from] { *from = ptr.pos; }, [this, from, target] (float u) { ptr.moveTo (*from + (target() - *from) * ease (u)); } });
        s.steps.push_back ({ 0.1, [this] { ptr.up(); }, {} });
    }
    void dragBy (juce::Point<float> offset, double sec)
    {
        auto from = std::make_shared<juce::Point<float>>();
        s.steps.push_back ({ 0.12, [this, from] { *from = ptr.pos; ptr.pressFlash = now; ptr.down(); }, {} });
        s.steps.push_back ({ sec, {}, [this, from, offset] (float u) { ptr.moveTo (*from + offset * ease (u)); } });
        s.steps.push_back ({ 0.1, [this] { ptr.up(); }, {} });
    }
    void click() { s.steps.push_back ({ 0.15, [this] { ptr.pressFlash = now; ptr.down(); ptr.up(); }, {} }); }
    void doubleClick() { s.steps.push_back ({ 0.3, [this] { ptr.pressFlash = now; ptr.doubleClick(); }, {} }); }
    /** Scrolls over the shape tab until the selected band has `target` (one scroll every `interval`; up to six). */
    void scrollShapeTo (Shape target, double interval)
    {
        for (int i = 0; i < 6; ++i)
            s.steps.push_back ({ interval, [this, target]
            {
                auto& proc = dynamic_cast<StEqProcessor&> (ed.processor);
                const int b = display().getPrimary();
                if (b >= 0 && proc.getBand (b).shape != target && dynamic_cast<ShapeButton*> (ptr.at (ptr.pos)) != nullptr)
                    ptr.wheel (-0.12f);
            }, {} });
    }
    void wheel (float dy, int times, double interval)
    {
        for (int i = 0; i < times; ++i)
            s.steps.push_back ({ interval, [this, dy] { ptr.wheel (dy); }, {} });
    }

    double now = 0.0;   // video time, kept current by the renderer
};

//==============================================================================
struct Scene
{
    juce::String name;
    double seconds;
    std::function<void (StEqProcessor&, Music&)> setup;
    std::function<void (Director&)> script;
};

void setParam (StEqProcessor& p, const juce::String& id, float plain)
{
    auto& prm = p.param (id);
    prm.setValueNotifyingHost (prm.convertTo0to1 (plain));
}

void reset (StEqProcessor& p, bool threeD)
{
    p.presets.loadPreset (0);
    for (int b = 0; b < kMaxBands; ++b) p.removeBand (b);
    setParam (p, ids::mode3d, threeD ? 1.0f : 0.0f);
    p.uiState.setProperty ("view3d", 2, nullptr);   // front
    p.uiState.setProperty ("selection", "", nullptr);
    p.uiState.setProperty ("primary", -1, nullptr);
    p.uiState.setProperty ("dynExpanded", false, nullptr);
    p.uiState.setProperty ("piano", false, nullptr);
}

void select (StEqProcessor& p, int band)
{
    p.uiState.setProperty ("selection", juce::String (band), nullptr);
    p.uiState.setProperty ("primary", band, nullptr);
}

std::vector<Scene> scenes()
{
    std::vector<Scene> v;

    // 1. Vocal clean-up: rumble out, boxiness down, presence up
    v.push_back ({ "vocal-cleanup", 15.0, [] (StEqProcessor& p, Music& m)
    {
        reset (p, false);
        m.vocal = 1.0f; m.sib = 0.35f; m.rumble = 0.55f; m.box = 1.0f; m.pad = 0.25f;
    }, [] (Director& d)
    {
        d.wait (1.3);
        d.move ([&d] { return d.at (75.0f, 0.0f); }, 0.9);
        d.doubleClick();
        d.wait (0.35);
        d.move ([&d] { return d.shapeTab(); }, 0.8);
        d.scrollShapeTo (Shape::lowCut, 0.28);   // Bell -> Low Shelf -> High Shelf -> Low Cut
        d.wait (0.3);
        d.move ([&d] { return d.dot(); }, 0.7);
        d.drag ([&d] { return d.at (100.0f, 0.0f); }, 0.6);
        d.move ([&d] { return d.onCurve (330.0f); }, 0.8);
        d.doubleClick();
        d.drag ([&d] { return d.at (330.0f, -5.0f); }, 0.8);
        d.move ([&d] { return d.knobCentre ("q"); }, 0.8);
        d.dragBy ({ 0.0f, -42.0f }, 0.7);
        d.move ([&d] { return d.onCurve (4800.0f); }, 0.9);
        d.doubleClick();
        d.drag ([&d] { return d.at (4800.0f, 3.5f); }, 0.8);
        d.move ([&d] { return d.at (2000.0f, 9.0f); }, 1.0);
        d.wait (2.0);
    } });

    // 2. Harshness only when it happens: a dynamic band on the sibilance
    v.push_back ({ "dynamic-deess", 15.0, [] (StEqProcessor& p, Music& m)
    {
        reset (p, false);
        p.addBand (Shape::lowCut, 90.0f, 0.0f, 0.71f, 3);
        p.addBand (Shape::bell, 300.0f, -2.5f, 1.2f);
        const int b = p.addBand (Shape::bell, 5200.0f, 0.0f, 2.2f);
        select (p, b);
        m.vocal = 1.0f; m.sib = 1.6f; m.pad = 0.35f; m.drums = 0.18f; m.bass = 0.25f;
    }, [] (Director& d)
    {
        d.wait (1.6);
        d.move ([&d] { return d.dot(); }, 0.8);
        d.drag ([&d] { return d.at (7200.0f, 0.0f); }, 0.8);
        d.wait (0.3);
        d.move ([&d] { return d.gainRim(); }, 0.9);
        d.dragBy ({ 4.0f, 48.0f }, 1.1);   // the ring: dynamic range -12 dB
        d.wait (0.6);
        d.move ([&d] { return d.islandButton ("Show dynamics settings"); }, 0.8);
        d.click();
        d.move ([&d] { return d.at (1500.0f, 8.0f); }, 1.0);
        d.wait (5.5);
    } });

    // 3. Drum bus: weight, less mud, air
    v.push_back ({ "drum-bus", 14.0, [] (StEqProcessor& p, Music& m)
    {
        reset (p, false);
        m.drums = 1.0f; m.bass = 0.8f;
    }, [] (Director& d)
    {
        d.wait (1.2);
        d.move ([&d] { return d.onCurve (58.0f); }, 0.9);
        d.doubleClick();
        d.drag ([&d] { return d.at (58.0f, 4.5f); }, 0.8);
        d.wheel (0.15f, 3, 0.22);   // wider (wheel over the dot steps Q)
        d.move ([&d] { return d.onCurve (420.0f); }, 0.9);
        d.doubleClick();
        d.drag ([&d] { return d.at (420.0f, -3.5f); }, 0.8);
        d.move ([&d] { return d.onCurve (9000.0f); }, 1.0);
        d.doubleClick();
        d.drag ([&d] { return d.at (9000.0f, 3.0f); }, 0.7);
        d.move ([&d] { return d.shapeTab(); }, 0.9);
        d.scrollShapeTo (Shape::highShelf, 0.3);   // Bell -> Low Shelf -> High Shelf
        d.move ([&d] { return d.at (1200.0f, 9.0f); }, 1.0);
        d.wait (2.0);
    } });

    // 4. 3D depth: push the pad's presence back, then look around
    v.push_back ({ "3d-depth", 15.0, [] (StEqProcessor& p, Music& m)
    {
        reset (p, true);
        p.uiState.setProperty ("view3d", 0, nullptr);
        p.uiState.setProperty ("camYaw", (double) EqDisplay::defaultYaw, nullptr);
        p.uiState.setProperty ("camPitch", (double) EqDisplay::defaultPitch, nullptr);
        p.uiState.setProperty ("autoRange", true, nullptr);
        p.addBand (Shape::lowCut, 45.0f, 0.0f, 0.71f, 3);
        p.addBand (Shape::bell, 280.0f, -2.5f, 1.0f);
        const int b = p.addBand (Shape::bell, 2400.0f, 4.0f, 0.9f);
        const int hs = p.addBand (Shape::highShelf, 9000.0f, 2.0f, 0.7f);
        setParam (p, ids::band (hs, "depth"), 30.0f);
        select (p, b);
        m.pad = 1.0f; m.vocal = 0.6f; m.sib = 0.3f; m.drums = 0.45f; m.bass = 0.5f;
    }, [] (Director& d)
    {
        d.wait (1.4);
        d.move ([&d] { return d.knobCentre ("depth"); }, 1.0);
        d.dragBy ({ 0.0f, -150.0f }, 1.8);   // depth 0 -> ~60 %
        d.wait (0.5);
        d.move ([&d] { return d.fromDisplay (d.display().getGizmoCentre()); }, 1.0);
        d.dragBy ({ -70.0f, 22.0f }, 2.0);    // orbit
        d.wait (0.8);
        d.move ([&d] { return d.fromDisplay (d.display().getViewButton (EqDisplay::View::top).getCentre()); }, 0.9);
        d.click();
        d.wait (2.2);
        d.move ([&d] { return d.fromDisplay (d.display().getViewButton (EqDisplay::View::threeD).getCentre()); }, 0.8);
        d.click();
        d.wait (1.6);
    } });
    return v;
}

//==============================================================================
void drawCursor (juce::Graphics& g, juce::Point<float> p, bool pressed, float ripple)
{
    if (ripple > 0.0f && ripple < 1.0f)
    {
        const float r = 6.0f + 16.0f * ripple;
        g.setColour (juce::Colours::white.withAlpha (0.45f * (1.0f - ripple)));
        g.drawEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (p), 1.5f);
    }
    juce::Path arrow;
    arrow.startNewSubPath (0, 0); arrow.lineTo (0, 17); arrow.lineTo (4.2f, 13.2f); arrow.lineTo (7.2f, 19.8f);
    arrow.lineTo (9.8f, 18.7f); arrow.lineTo (6.9f, 12.3f); arrow.lineTo (12.4f, 12.3f); arrow.closeSubPath();
    const auto tf = juce::AffineTransform::scale (pressed ? 0.9f : 1.0f).translated (p);
    g.setColour (juce::Colours::black.withAlpha (0.35f));
    g.fillPath (arrow, tf.translated (0.8f, 1.6f));
    g.setColour (juce::Colours::black);
    g.fillPath (arrow, tf);
    g.setColour (juce::Colours::white);
    g.strokePath (arrow, juce::PathStrokeType (1.3f, juce::PathStrokeType::mitered), tf);
}

struct Renderer : juce::Timer
{
    StEqProcessor& p;
    juce::File dir;
    std::vector<Scene> list;
    size_t index = 0;
    std::unique_ptr<Music> music;
    std::unique_ptr<juce::AudioProcessorEditor> ed;
    std::unique_ptr<Pointer> ptr;
    std::unique_ptr<Script> script;
    std::unique_ptr<Director> director;
    FILE* pipe = nullptr;
    int frame = 0, preroll = 0;
    double wallStart = 0.0;

    Renderer (StEqProcessor& proc, juce::File d, std::vector<Scene> s) : p (proc), dir (d), list (std::move (s)) {}

    void begin()
    {
        auto& sc = list[index];
        music = std::make_unique<Music>();
        sc.setup (p, *music);
        p.applyModeChange();
        ed.reset (p.createEditor());
        ed->setSize (StEqEditor::designWidth, StEqEditor::designHeight);
        auto& e = *dynamic_cast<StEqEditor*> (ed.get());
        ptr = std::make_unique<Pointer> (Pointer { e, {}, {}, {}, {} });
        ptr->pos = e.getLocalPoint (&e.getDisplay(), juce::Point<float> (e.getDisplay().xForHz (1500.0f), e.getDisplay().yForDb (8.0f)));
        script = std::make_unique<Script>();
        director = std::make_unique<Director> (Director { e, *ptr, *script });
        sc.script (*director);
        frame = 0;
        preroll = fps * 2;   // the analyser fills before recording starts

        const int w = juce::roundToInt ((float) StEqEditor::designWidth * videoScale), h = juce::roundToInt ((float) StEqEditor::designHeight * videoScale);
        const auto out = dir.getChildFile ("st-eq-" + sc.name + ".mp4");
        out.deleteFile();
        const auto cmd = "ffmpeg -y -loglevel error -f rawvideo -pix_fmt bgra -s " + juce::String (w) + "x" + juce::String (h) + " -r " + juce::String (fps)
                       + " -i - -an -c:v libx264 -preset slow -crf 22 -pix_fmt yuv420p -movflags +faststart \"" + out.getFullPathName() + "\"";
        pipe = popen (cmd.toRawUTF8(), "w");
        wallStart = juce::Time::getMillisecondCounterHiRes();
        std::cout << "rendering " << sc.name << "\n";
    }

    void finish()
    {
        if (pipe != nullptr) pclose (pipe);
        pipe = nullptr;
        const double wall = (juce::Time::getMillisecondCounterHiRes() - wallStart) / 1000.0;
        std::cout << "  " << frame << " frames, " << juce::String (wall, 1) << " s wall for " << juce::String ((double) (frame + fps * 2) / fps, 1) << " s video\n";
        director.reset(); script.reset(); ptr.reset();
        ed.reset();
        ++index;
    }

    void timerCallback() override
    {
        if (ed == nullptr)
        {
            if (index >= list.size()) { stopTimer(); juce::JUCEApplicationBase::quit(); return; }
            begin();
        }

        juce::AudioBuffer<float> buf (2, blockSize);
        juce::MidiBuffer midi;
        music->render (buf);
        p.processBlock (buf, midi);

        if (preroll > 0) { --preroll; return; }

        const double t = (double) frame / fps;
        director->now = t;
        script->advance (1.0 / fps);

        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, videoScale);
        {
            juce::Graphics g (img);
            g.addTransform (juce::AffineTransform::scale (videoScale));
            const float ripple = (float) ((t - ptr->pressFlash) / 0.35);
            drawCursor (g, ptr->pos, ptr->pressed != nullptr, ripple);
        }
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::readOnly);
        for (int y = 0; y < bd.height; ++y)
            fwrite (bd.getLinePointer (y), 4, (size_t) bd.width, pipe);

        if (++frame >= juce::roundToInt (list[index].seconds * fps)) finish();
    }
};

juce::File videoDir;
juce::StringArray videoOnly;

struct VideoApp : juce::JUCEApplicationBase
{
    std::unique_ptr<StEqProcessor> proc;
    std::unique_ptr<Renderer> renderer;

    const juce::String getApplicationName() override { return "StEqVideos"; }
    const juce::String getApplicationVersion() override { return "1"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void anotherInstanceStarted (const juce::String&) override {}
    void suspended() override {}
    void resumed() override {}
    void systemRequestedQuit() override { quit(); }
    void unhandledException (const std::exception*, const juce::String&, int) override {}
    void shutdown() override { renderer.reset(); proc.reset(); }

    void initialise (const juce::String&) override
    {
        proc = std::make_unique<StEqProcessor>();
        proc->setPlayConfigDetails (2, 2, sampleRate, blockSize);
        proc->prepareToPlay (sampleRate, blockSize);
        videoDir.createDirectory();
        auto all = scenes();
        std::vector<Scene> chosen;
        for (auto& s : all)
            if (videoOnly.isEmpty() || videoOnly.contains (s.name)) chosen.push_back (s);
        renderer = std::make_unique<Renderer> (*proc, videoDir, std::move (chosen));
        renderer->startTimerHz (fps);
    }
};

juce::JUCEApplicationBase* createVideoApp() { return new VideoApp(); }
}

int runVideoRender (int argc, char** argv);
int runVideoRender (int argc, char** argv)
{
    juce::StringArray args (argv + 1, argc - 1);
    const int i = args.indexOf ("--video");
    videoDir = juce::File (args[i + 1]);
    for (int k = i + 2; k < args.size(); ++k) videoOnly.add (args[k]);
    juce::JUCEApplicationBase::createInstance = &createVideoApp;
    return juce::JUCEApplicationBase::main (argc, (const char**) argv);
}
