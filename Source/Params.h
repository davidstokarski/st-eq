#pragma once
#include <st_shell/st_shell.h>

namespace steq
{

constexpr int kMaxBands = 24;

using Shape = st::dsp::EqShape;
enum class Placement { stereo, left, right, mid, side };
enum class ProcMode { zeroLatency, naturalPhase, linearPhase };
enum class Character { clean, subtle, warm };
enum class Trigger { band, free };

inline const juce::StringArray shapeNames { "Bell", "Low Shelf", "Low Cut", "High Shelf", "High Cut", "Notch", "Band Pass", "Tilt Shelf", "Flat Tilt", "All Pass" };
inline const juce::StringArray slopeNames { "6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "30 dB/oct", "36 dB/oct", "48 dB/oct", "72 dB/oct", "96 dB/oct", "Brickwall" };
inline const juce::StringArray placementNames { "Stereo", "Left", "Right", "Mid", "Side" };
inline const juce::StringArray modeNames { "Zero Latency", "Natural Phase", "Linear Phase" };
inline const juce::StringArray resolutionNames { "Low", "Medium", "High", "Very High", "Maximum" };
inline const juce::StringArray characterNames { "Clean", "Subtle", "Warm" };

/** Slope index -> filter order (6 dB/oct per order). Brickwall = order 20 (120 dB/oct). */
inline int slopeToOrder (int slopeIndex)
{
    static constexpr int orders[] = { 1, 2, 3, 4, 5, 6, 8, 12, 16, 20 };
    return orders[juce::jlimit (0, 9, slopeIndex)];
}

namespace ids
{
    inline const juce::String outGain   = "out_gain";
    inline const juce::String outPan    = "out_pan";
    inline const juce::String autoGain  = "auto_gain";
    inline const juce::String gainScale = "gain_scale";
    inline const juce::String phaseInv  = "phase_inv";
    inline const juce::String mode      = "proc_mode";
    inline const juce::String resolution = "lp_res";
    inline const juce::String character = "character";
    inline const juce::String bypass    = "global_bypass";
    inline const juce::String panMode   = "pan_mode";
    inline const juce::String mode3d    = "mode_3d";

    // Per-band parameter suffixes. Full ID: "b<n>_<suffix>", n = 1..24
    inline const juce::StringArray bandSuffixes {
        "used", "bypass", "shape", "freq", "gain", "q", "slope", "place", "gq",
        "dyn_range", "dyn_thresh", "dyn_auto", "dyn_attack", "dyn_release", "dyn_sc", "dyn_trig", "dyn_lo", "dyn_hi", "dyn_bypass",
        "spectral", "spec_density", "spec_tilt", "depth"
    };

    inline juce::String band (int b, const juce::String& suffix) { return "b" + juce::String (b + 1) + "_" + suffix; }
}

/** Plain values of one band, read from the parameters (audio thread, worker thread or UI). */
struct BandParams
{
    bool used = false, bypass = false;
    Shape shape = Shape::bell;
    float freq = 1000.0f, gain = 0.0f, q = 1.0f;
    int slope = 1;
    Placement place = Placement::stereo;
    bool gainQ = false;
    float dynRange = 0.0f, dynThresh = -30.0f;
    bool dynAuto = true;
    float attackMs = 10.0f, releaseMs = 150.0f;
    bool sidechain = false;
    Trigger trigger = Trigger::band;
    float trigLo = 100.0f, trigHi = 10000.0f;
    bool dynBypass = false;
    bool spectral = false, specTilt = true;
    float specDensity = 50.0f;   // %
    /** 3D mode: how far back the band sits (0..100 %). Effective value: 0 while 3D mode is off. */
    float depth = 0.0f;

    bool isActive() const noexcept { return used && ! bypass; }
    bool isDynamic() const noexcept { return used && ! dynBypass && std::abs (dynRange) > 0.05f && st::dsp::eq::hasGain (shape); }
    /** Spectral dynamics: the dynamic gain is applied per frequency (linear phase, STFT). */
    bool isSpectral() const noexcept { return spectral && isDynamic(); }
    /** 3D mode with some depth: boosts move back (wider, small room reflections), cuts move to the sides. */
    bool hasDepth() const noexcept { return depth > 0.05f; }
    bool operator== (const BandParams& o) const noexcept
    {
        using juce::exactlyEqual;
        return used == o.used && bypass == o.bypass && shape == o.shape && exactlyEqual (freq, o.freq) && exactlyEqual (gain, o.gain)
            && exactlyEqual (q, o.q) && slope == o.slope && place == o.place && gainQ == o.gainQ && exactlyEqual (dynRange, o.dynRange)
            && exactlyEqual (dynThresh, o.dynThresh) && dynAuto == o.dynAuto && exactlyEqual (attackMs, o.attackMs)
            && exactlyEqual (releaseMs, o.releaseMs) && sidechain == o.sidechain && trigger == o.trigger
            && exactlyEqual (trigLo, o.trigLo) && exactlyEqual (trigHi, o.trigHi) && dynBypass == o.dynBypass
            && spectral == o.spectral && specTilt == o.specTilt && exactlyEqual (specDensity, o.specDensity)
            && exactlyEqual (depth, o.depth);
    }
};

/** Filter spec for a band. `dynDb` is added to the gain (dynamic EQ), gainScale (1 = 100 %) scales gains. */
st::dsp::EqBandSpec makeSpec (const BandParams&, float gainScale, float dynDb = 0.0f) noexcept;

/** Effective Q including analog-style gain-Q interaction. */
float effectiveQ (const BandParams&, float gainDb) noexcept;

/** Cached raw-value pointers for all band + global parameters (thread-safe atomic reads). */
struct ParamRefs
{
    struct Band
    {
        st::params::Ref used, bypass, shape, freq, gain, q, slope, place, gq,
                        dynRange, dynThresh, dynAuto, dynAttack, dynRelease, dynSc, dynTrig, dynLo, dynHi, dynBypass,
                        spectral, specDensity, specTilt, depth;
    };
    std::array<Band, kMaxBands> bands;
    st::params::Ref outGain, outPan, autoGain, gainScale, phaseInv, mode, resolution, character, bypass, panMode, mode3d;

    void bind (juce::AudioProcessorValueTreeState&);
    /** The band's values; depth is 0 while 3D mode is off. */
    BandParams readBand (int b) const noexcept;
    bool is3D() const noexcept { return mode3d.getBool(); }
    float readGainScale() const noexcept { return gainScale.get() * 0.01f; }
};

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

/** Band colour (by slot), from the shared series palette. */
juce::Colour bandColour (int band);

} // namespace steq
