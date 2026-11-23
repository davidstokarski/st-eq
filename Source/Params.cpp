#include "Params.h"

namespace steq
{

using namespace st::params;

float effectiveQ (const BandParams& b, float gainDb) noexcept
{
    if (! b.gainQ || b.shape != Shape::bell)
        return b.q;
    // Analog-style proportional Q: wider at small gains, narrower at large ones (equal at 12 dB).
    return juce::jlimit (0.025f, 40.0f, b.q * std::exp2 ((std::abs (gainDb) - 12.0f) / 12.0f));
}

st::dsp::EqBandSpec makeSpec (const BandParams& b, float gainScale, float dynDb) noexcept
{
    st::dsp::EqBandSpec s;
    s.shape = b.shape;
    s.freq = b.freq;
    s.gainDb = st::dsp::eq::hasGain (b.shape) ? (b.gain + dynDb) * gainScale : 0.0f;
    s.q = effectiveQ (b, (float) s.gainDb);
    s.order = slopeToOrder (b.slope);
    return s;
}

void ParamRefs::bind (juce::AudioProcessorValueTreeState& s)
{
    for (int b = 0; b < kMaxBands; ++b)
    {
        auto& r = bands[(size_t) b];
        auto id = [b] (const char* x) { return ids::band (b, x); };
        r.used.bind (s, id ("used"));         r.bypass.bind (s, id ("bypass"));
        r.shape.bind (s, id ("shape"));       r.freq.bind (s, id ("freq"));
        r.gain.bind (s, id ("gain"));         r.q.bind (s, id ("q"));
        r.slope.bind (s, id ("slope"));       r.place.bind (s, id ("place"));
        r.gq.bind (s, id ("gq"));
        r.dynRange.bind (s, id ("dyn_range")); r.dynThresh.bind (s, id ("dyn_thresh"));
        r.dynAuto.bind (s, id ("dyn_auto"));   r.dynAttack.bind (s, id ("dyn_attack"));
        r.dynRelease.bind (s, id ("dyn_release"));
        r.dynSc.bind (s, id ("dyn_sc"));       r.dynTrig.bind (s, id ("dyn_trig"));
        r.dynLo.bind (s, id ("dyn_lo"));       r.dynHi.bind (s, id ("dyn_hi"));
        r.dynBypass.bind (s, id ("dyn_bypass"));
        r.spectral.bind (s, id ("spectral"));  r.specDensity.bind (s, id ("spec_density"));
        r.specTilt.bind (s, id ("spec_tilt"));
        r.depth.bind (s, id ("depth"));
    }
    outGain.bind (s, ids::outGain);
    outPan.bind (s, ids::outPan);
    autoGain.bind (s, ids::autoGain);
    gainScale.bind (s, ids::gainScale);
    phaseInv.bind (s, ids::phaseInv);
    mode.bind (s, ids::mode);
    resolution.bind (s, ids::resolution);
    character.bind (s, ids::character);
    bypass.bind (s, ids::bypass);
    panMode.bind (s, ids::panMode);
    mode3d.bind (s, ids::mode3d);
}

BandParams ParamRefs::readBand (int b) const noexcept
{
    const auto& r = bands[(size_t) b];
    BandParams p;
    p.used = r.used.getBool();
    p.bypass = r.bypass.getBool();
    p.shape = (Shape) juce::jlimit (0, 9, r.shape.getInt());
    p.freq = r.freq.get();
    p.gain = r.gain.get();
    p.q = r.q.get();
    p.slope = juce::jlimit (0, 9, r.slope.getInt());
    p.place = (Placement) juce::jlimit (0, 4, r.place.getInt());
    p.gainQ = r.gq.getBool();
    p.dynRange = r.dynRange.get();
    p.dynThresh = r.dynThresh.get();
    p.dynAuto = r.dynAuto.getBool();
    p.attackMs = r.dynAttack.get();
    p.releaseMs = r.dynRelease.get();
    p.sidechain = r.dynSc.getBool();
    p.trigger = (Trigger) juce::jlimit (0, 1, r.dynTrig.getInt());
    p.trigLo = r.dynLo.get();
    p.trigHi = r.dynHi.get();
    p.dynBypass = r.dynBypass.getBool();
    p.spectral = r.spectral.getBool();
    p.specDensity = r.specDensity.get();
    p.specTilt = r.specTilt.getBool();
    p.depth = mode3d.getBool() ? juce::jlimit (0.0f, 100.0f, r.depth.get()) : 0.0f;
    return p;
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    {
        auto global = std::make_unique<juce::AudioProcessorParameterGroup> ("global", "Global", " | ");
        global->addChild (makeFloat (ids::outGain, "Output Gain", linear (-36.0f, 36.0f, 0.01f), 0.0f, dbAttributes()));
        global->addChild (makeFloat (ids::outPan, "Output Pan", linear (-100.0f, 100.0f, 0.01f), 0.0f, percentAttributes (true)));
        global->addChild (makeBool (ids::autoGain, "Auto Gain", false));
        global->addChild (makeFloat (ids::gainScale, "Gain Scale", linear (-100.0f, 200.0f, 0.01f), 100.0f, percentAttributes()));
        global->addChild (makeBool (ids::phaseInv, "Phase Invert", false));
        global->addChild (makeChoice (ids::mode, "Processing Mode", modeNames, 0));
        global->addChild (makeChoice (ids::resolution, "Linear Phase Resolution", resolutionNames, 1));
        global->addChild (makeChoice (ids::character, "Character", characterNames, 0));
        global->addChild (makeBool (ids::bypass, "Bypass", false));
        global->addChild (makeChoice (ids::panMode, "Pan Mode", { "Left / Right", "Mid / Side" }, 0));
        global->addChild (makeBool (ids::mode3d, "3D Mode", true));
        layout.add (std::move (global));
    }

    auto qAttributes = juce::AudioParameterFloatAttributes()
        .withStringFromValueFunction ([] (float v, int) { return juce::String (v, v < 1.0f ? 3 : 2); })
        .withValueFromStringFunction ([] (const juce::String& s) { return s.retainCharacters ("-.0123456789").getFloatValue(); });

    for (int b = 0; b < kMaxBands; ++b)
    {
        const auto n = "Band " + juce::String (b + 1) + " ";
        auto id = [b] (const char* s) { return ids::band (b, s); };
        auto group = std::make_unique<juce::AudioProcessorParameterGroup> ("band" + juce::String (b + 1), "Band " + juce::String (b + 1), " | ");

        group->addChild (makeBool (id ("used"), n + "Used", false));
        group->addChild (makeBool (id ("bypass"), n + "Bypass", false));
        group->addChild (makeChoice (id ("shape"), n + "Shape", shapeNames, 0));
        group->addChild (makeFloat (id ("freq"), n + "Frequency", logarithmic (10.0f, 30000.0f), 1000.0f, hzAttributes()));
        group->addChild (makeFloat (id ("gain"), n + "Gain", linear (-30.0f, 30.0f, 0.01f), 0.0f, dbAttributes (2)));
        group->addChild (makeFloat (id ("q"), n + "Q", logarithmic (0.025f, 40.0f), 1.0f, qAttributes));
        group->addChild (makeChoice (id ("slope"), n + "Slope", slopeNames, 1));
        group->addChild (makeChoice (id ("place"), n + "Stereo Placement", placementNames, 0));
        group->addChild (makeBool (id ("gq"), n + "Gain-Q Interaction", false));
        group->addChild (makeFloat (id ("dyn_range"), n + "Dynamic Range", linear (-30.0f, 30.0f, 0.01f), 0.0f, dbAttributes()));
        group->addChild (makeFloat (id ("dyn_thresh"), n + "Threshold", linear (-80.0f, 0.0f, 0.01f), -30.0f, dbAttributes()));
        group->addChild (makeBool (id ("dyn_auto"), n + "Auto Threshold", true));
        group->addChild (makeFloat (id ("dyn_attack"), n + "Attack", skewed (0.1f, 500.0f, 15.0f, 0.01f), 10.0f, msAttributes()));
        group->addChild (makeFloat (id ("dyn_release"), n + "Release", skewed (5.0f, 3000.0f, 200.0f, 0.1f), 150.0f, msAttributes()));
        group->addChild (makeBool (id ("dyn_sc"), n + "External Side Chain", false));
        group->addChild (makeChoice (id ("dyn_trig"), n + "Trigger", { "Band", "Free" }, 0));
        group->addChild (makeFloat (id ("dyn_lo"), n + "Trigger Low Cut", logarithmic (10.0f, 30000.0f), 100.0f, hzAttributes()));
        group->addChild (makeFloat (id ("dyn_hi"), n + "Trigger High Cut", logarithmic (10.0f, 30000.0f), 10000.0f, hzAttributes()));
        group->addChild (makeBool (id ("dyn_bypass"), n + "Dynamics Bypass", false));
        group->addChild (makeBool (id ("spectral"), n + "Spectral Dynamics", false));
        group->addChild (makeFloat (id ("spec_density"), n + "Spectral Density", linear (0.0f, 100.0f, 0.01f), 50.0f, percentAttributes()));
        group->addChild (makeBool (id ("spec_tilt"), n + "Spectral Tilt", true));
        group->addChild (makeFloat (id ("depth"), n + "Depth", linear (0.0f, 100.0f, 0.01f), 0.0f, percentAttributes()));
        layout.add (std::move (group));
    }

    return layout;
}

juce::Colour bandColour (int band)
{
    // A warm-leaning order of the series palette so neighbouring bands contrast.
    static const int order[] = { 6, 1, 4, 0, 7, 3, 5, 2 };
    return st::Theme::get().seriesColour (order[((band % 8) + 8) % 8]);
}

} // namespace steq
