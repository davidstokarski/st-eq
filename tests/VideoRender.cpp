// Use-case videos of the real editor (website), on the shell's video kit (st_shell/testing/st_video.h): a
// synthesized mix plays through the processor while a scripted pointer works the UI.
//   StEqPluginTests --video <dir> [scene ...]
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "ui/BandIsland.h"
#include <st_shell/testing/st_video.h>

using namespace steq;
using st::video::Director;
using st::video::Music;
using st::video::Scene;

namespace
{
/** ST EQ places: the display, the band island. */
struct Eq
{
    Director& d;
    StEqEditor& ed() const { return dynamic_cast<StEqEditor&> (d.editor); }
    StEqProcessor& proc() const { return dynamic_cast<StEqProcessor&> (d.processor); }
    EqDisplay& display() const { return ed().getDisplay(); }
    juce::Point<float> fromDisplay (juce::Point<float> p) const { return d.pointIn (display(), p); }
    juce::Point<float> at (float hz, float db) const { return fromDisplay ({ display().xForHz (hz), display().yForDb (db) }); }
    juce::Point<float> onCurve (float hz) const { return fromDisplay ({ display().xForHz (hz), display().getCurveY (hz) }); }
    juce::Point<float> dot() const { return fromDisplay (display().dotPosition (display().getPrimary())); }
    st::Knob* knob (const juce::String& suffix) const
    {
        return d.findType<st::Knob> ([&] (st::Knob& k) { return k.getParameter().paramID.endsWith ("_" + suffix); }, ed().getBandPanel());
    }
    juce::Point<float> knobCentre (const char* suffix) const { return d.centreOf (knob (suffix)); }
    juce::Point<float> gainRim() const
    {
        auto* k = knob ("gain");
        if (k == nullptr) return d.ptr.pos;
        const float R = (float) k->getWidth() * 0.5f;
        const float outerR = R - juce::jmax (2.0f, R * 0.07f) - 1.5f, r = outerR * 0.87f, a = -0.75f;
        return knobCentre ("gain") + juce::Point<float> (std::sin (a) * r, -std::cos (a) * r);
    }
    juce::Point<float> shapeTab() const { return d.centreOf (d.findType<ShapeButton> ({}, ed().getBandPanel())); }
    juce::Point<float> islandButton (const juce::String& tip) const
    {
        return d.centreOf (d.findType<juce::Button> ([&] (juce::Button& b) { return b.getTooltip() == tip; }, ed().getBandPanel()));
    }
    /** Scrolls over the shape tab until the selected band has `target` (one scroll every `interval`; up to six). */
    void scrollShapeTo (Shape target, double interval) const
    {
        for (int i = 0; i < 6; ++i)
            d.call ([this, target]
            {
                const int b = display().getPrimary();
                if (b >= 0 && proc().getBand (b).shape != target && dynamic_cast<ShapeButton*> (d.ptr.at (d.ptr.pos)) != nullptr)
                    d.ptr.wheel (-0.12f);
            }), d.wait (interval);
    }
};

void setParam (StEqProcessor& p, const juce::String& id, float plain) { st::video::setParam (p, id, plain); }

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

StEqProcessor& eq (juce::AudioProcessor& p) { return dynamic_cast<StEqProcessor&> (p); }

std::vector<Scene> scenes()
{
    std::vector<Scene> v;

    // 1. Vocal clean-up: rumble out, boxiness down, presence up
    v.push_back ({ "vocal-cleanup", 15.0, [] (juce::AudioProcessor& ap, Music& m)
    {
        auto& p = eq (ap);
        reset (p, false);
        m.vocal = 1.0f; m.sib = 0.35f; m.rumble = 0.55f; m.box = 1.0f; m.pad = 0.25f;
    }, [] (Director& dir)
    {
        auto e = std::make_shared<Eq> (Eq { dir });
        dir.wait (1.3);
        dir.move ([e] { return e->at (75.0f, 0.0f); }, 0.9);
        dir.doubleClick();
        dir.wait (0.35);
        dir.move ([e] { return e->shapeTab(); }, 0.8);
        e->scrollShapeTo (Shape::lowCut, 0.28);   // Bell -> Low Shelf -> High Shelf -> Low Cut
        dir.wait (0.3);
        dir.move ([e] { return e->dot(); }, 0.7);
        dir.drag ([e] { return e->at (100.0f, 0.0f); }, 0.6);
        dir.move ([e] { return e->onCurve (330.0f); }, 0.8);
        dir.doubleClick();
        dir.drag ([e] { return e->at (330.0f, -5.0f); }, 0.8);
        dir.move ([e] { return e->knobCentre ("q"); }, 0.8);
        dir.dragBy ({ 0.0f, -42.0f }, 0.7);
        dir.move ([e] { return e->onCurve (4800.0f); }, 0.9);
        dir.doubleClick();
        dir.drag ([e] { return e->at (4800.0f, 3.5f); }, 0.8);
        dir.move ([e] { return e->at (2000.0f, 9.0f); }, 1.0);
        dir.wait (2.0);
    } });

    // 2. Harshness only when it happens: a dynamic band on the sibilance
    v.push_back ({ "dynamic-deess", 15.0, [] (juce::AudioProcessor& ap, Music& m)
    {
        auto& p = eq (ap);
        reset (p, false);
        p.addBand (Shape::lowCut, 90.0f, 0.0f, 0.71f, 3);
        p.addBand (Shape::bell, 300.0f, -2.5f, 1.2f);
        const int b = p.addBand (Shape::bell, 5200.0f, 0.0f, 2.2f);
        select (p, b);
        m.vocal = 1.0f; m.sib = 1.6f; m.pad = 0.35f; m.drums = 0.18f; m.bass = 0.25f;
    }, [] (Director& dir)
    {
        auto e = std::make_shared<Eq> (Eq { dir });
        dir.wait (1.6);
        dir.move ([e] { return e->dot(); }, 0.8);
        dir.drag ([e] { return e->at (7200.0f, 0.0f); }, 0.8);
        dir.wait (0.3);
        dir.move ([e] { return e->gainRim(); }, 0.9);
        dir.dragBy ({ 4.0f, 48.0f }, 1.1);   // the ring: dynamic range -12 dB
        dir.wait (0.6);
        dir.move ([e] { return e->islandButton ("Show dynamics settings"); }, 0.8);
        dir.click();
        dir.move ([e] { return e->at (1500.0f, 8.0f); }, 1.0);
        dir.wait (5.5);
    } });

    // 3. Drum bus: weight, less mud, air
    v.push_back ({ "drum-bus", 14.0, [] (juce::AudioProcessor& ap, Music& m)
    {
        auto& p = eq (ap);
        reset (p, false);
        m.drums = 1.0f; m.bass = 0.8f;
    }, [] (Director& dir)
    {
        auto e = std::make_shared<Eq> (Eq { dir });
        dir.wait (1.2);
        dir.move ([e] { return e->onCurve (58.0f); }, 0.9);
        dir.doubleClick();
        dir.drag ([e] { return e->at (58.0f, 4.5f); }, 0.8);
        dir.wheel (0.15f, 3, 0.22);   // wider (wheel over the dot steps Q)
        dir.move ([e] { return e->onCurve (420.0f); }, 0.9);
        dir.doubleClick();
        dir.drag ([e] { return e->at (420.0f, -3.5f); }, 0.8);
        dir.move ([e] { return e->onCurve (9000.0f); }, 1.0);
        dir.doubleClick();
        dir.drag ([e] { return e->at (9000.0f, 3.0f); }, 0.7);
        dir.move ([e] { return e->shapeTab(); }, 0.9);
        e->scrollShapeTo (Shape::highShelf, 0.3);   // Bell -> Low Shelf -> High Shelf
        dir.move ([e] { return e->at (1200.0f, 9.0f); }, 1.0);
        dir.wait (2.0);
    } });

    // 4. 3D depth: push the pad's presence back, then look around
    v.push_back ({ "3d-depth", 15.0, [] (juce::AudioProcessor& ap, Music& m)
    {
        auto& p = eq (ap);
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
    }, [] (Director& dir)
    {
        auto e = std::make_shared<Eq> (Eq { dir });
        dir.wait (1.4);
        dir.move ([e] { return e->knobCentre ("depth"); }, 1.0);
        dir.dragBy ({ 0.0f, -150.0f }, 1.8);   // depth 0 -> ~60 %
        dir.wait (0.5);
        dir.move ([e] { return e->fromDisplay (e->display().getGizmoCentre()); }, 1.0);
        dir.dragBy ({ -70.0f, 22.0f }, 2.0);    // orbit
        dir.wait (0.8);
        dir.move ([e] { return e->fromDisplay (e->display().getViewButton (EqDisplay::View::top).getCentre()); }, 0.9);
        dir.click();
        dir.wait (2.2);
        dir.move ([e] { return e->fromDisplay (e->display().getViewButton (EqDisplay::View::threeD).getCentre()); }, 0.8);
        dir.click();
        dir.wait (1.6);
    } });
    return v;
}
}

int runVideoRender (int argc, char** argv);
int runVideoRender (int argc, char** argv)
{
    return st::video::run (argc, argv, "st-eq", [] { return std::make_unique<StEqProcessor>(); }, scenes());
}
